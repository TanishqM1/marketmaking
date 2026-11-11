#pragma once

#include "types/types.hpp"
#include <cstddef>
#include <cstdint>

namespace mm::state
{

// must be above the struct that uses it..
enum class slotState : std::uint8_t
{
    // 0/1/2
    EMPTY,
    OCCUPIED,
    TOMBSTONE
};

struct OrderEntry
{
    std::uint64_t ref;       // key
    std::uint32_t remaining; // this order's remaining amount.
    std::uint32_t abs_tick;  // absolute tick, not price
    mm::Side      side;      // buy or sell
    slotState     state;     // EMPTY / OCCUPIED / TOMBSTONE (0/1/2)
    std::uint8_t  pad[14];   // PADDING to be compliant with cache lines.
};

static_assert(sizeof(OrderEntry) == 32);

class OrderMap
{
    // this decides HOW MUCH space we allocate up front to our probe chains.
    // something cool that claude shows:
    // ─────────────┬───────────────────────┐
    // │ load factor │ avg probes per lookup │
    // ├─────────────┼───────────────────────┤
    // │ 0.25        │ 1.2                   │
    // ├─────────────┼───────────────────────┤
    // │ 0.50        │ 1.5                   │
    // ├─────────────┼───────────────────────┤
    // │ 0.70        │ 2.2                   │
    // ├─────────────┼───────────────────────┤
    // │ 0.90        │ 5.5                   │
    // ├─────────────┼───────────────────────┤
    // │ 0.95        │ 10.5                  │
    // └─────────────┴───────────────────────┘
    // essentially, past .70, when we are doing multple lookups. It also has to be
    // a multiple of 2.

    // 2^(16) uses 2MB, and stores all 32,000 possible orders ALL in L2 Cache!
    // TODO make this part dynamic, so we can chose different capacties at compile time.

    // static = class owns this variable, ONE copy.
    // constexpr = evaluate at compile time instead of runtime, to save memory.

    // ==== WHAT THIS IS ====
    // - one big array, that will contigously store 'OrderEntry' structs.
    // - we compute index using 'ref' (the key) by doing (ref & capacity) -> basic hashing
    // replacement.
    //      incrase there is a hash collision, we use linear probing (increment) until there is an
    //      open slot. This is popular because we ALREADY have access to that memory via the cache
    //      line. mimght as well store it there lol.

    // there are 3 paths:
    // - INSERT(ref) -> find the index it belongs to. if the it's empty, store what we need there!
    // - FIND(ref) -> hash as needed. if ref != what we find, linearly probe until we find it. if we
    // end up at an empty slot in the address, we know there was an error in the original ref.
    //
  public:
    static constexpr std::size_t CAPACITY = 65536;    // 2^16 TOTAL capacity
    static constexpr std::size_t MASK = CAPACITY - 1; // (ref & mask) is GUARANTEED to be in range.
    static_assert((CAPACITY & MASK) == 0);

    [[nodiscard]] bool insert(const OrderEntry& e) noexcept
    {
        // logic is a bit different. we need to probe until we find a EMPTY (insert) or a TOMBSTONE
        // (mark and continue until we find an empty).
        std::size_t idx        = (e.ref & MASK);
        std::size_t last_valid = CAPACITY;

        for (std::size_t i = 0; i < CAPACITY; i++)
        {
            if ((slots_[idx].state == slotState::OCCUPIED) && (slots_[idx].ref == e.ref))
            {
                return false;
            }
            if (slots_[idx].state == slotState::EMPTY)
            {
                count_++;
                if (last_valid != CAPACITY)
                {
                    slots_[last_valid]       = e;
                    slots_[last_valid].state = slotState::OCCUPIED;
                    return true;
                }
                else
                {
                    used_++;
                    slots_[idx]       = e;
                    slots_[idx].state = slotState::OCCUPIED;
                    return true;
                }
            }
            if (slots_[idx].state == slotState::TOMBSTONE)
            {
                if (last_valid == CAPACITY)
                {
                    last_valid = idx;
                }
            }
            // otherwise increment idx!
            idx = (idx + 1) & MASK;
        }
        // if we are not at CAPACITY, then we can add to last_valid
        if (last_valid != CAPACITY)
        {
            count_++;
            slots_[last_valid]       = e;
            slots_[last_valid].state = slotState::OCCUPIED;
            return true;
        }
        return false;
    }

    OrderEntry* find(std::uint64_t ref) noexcept
    {
        std::size_t idx = find_idx(ref);
        if (idx == CAPACITY)
        {
            return nullptr;
        }
        return &slots_[idx];
    }

    void erase(std::uint64_t ref) noexcept
    {
        std::size_t idx = find_idx(ref);
        if (idx == CAPACITY)
        {
            return;
        }
        slots_[idx].state = slotState::TOMBSTONE;
        count_--;
    }

    // delete DIRECTLY with a pointer to the order (no linear probing or mask search ever needed.)
    // cases like delete() where we must find, then ERASE, might as well only probe once.
    void erase(OrderEntry* p) noexcept
    {
        p->state = slotState::TOMBSTONE;
        count_--;
    }

    // APIs to fetch and retrieve count_ and used_ sizing.
    // TESTS ONLY. no code is generated.
    std::size_t size() const noexcept { return count_; }

    std::size_t used() const noexcept { return used_; }

    // using 128 because that's what the cache line boundaries for apple m4 silicon is. its just
    // what I got mane.
  private:
    //   this reads as making an ARRAY of sizeof("ORDERENTRY") CAPACITY. aka 65536 or "CAPACITY"
    //   orders!
    alignas(128) OrderEntry
        slots_[CAPACITY]{}; // align on cache line boundaries, for the length of our CAPACITY.
    // empty "{}" also means all indicies of our OrderMap start with "0". (for linear probing)
    std::size_t count_{0}; // how many are CURRENTLY used ?
    std::size_t used_{0};  // how many have been used in TOTAL ?

    // helper for find() and erase(). const = function will NOT modidt object state. noexcept =
    // function will NOT throw an exception.
    std::size_t find_idx(std::uint64_t ref) const noexcept
    {
        // hash to find initial slot
        // while taken, walk +=1
        std::size_t idx = (ref & MASK);
        // in this function & loop, a tombstone does not tell us anything, because the ref COULD be
        // on the other side.
        for (std::size_t i = 0; i < CAPACITY; i++)
        {
            // this means it has NOT been touched, the ref is NOT past this point.
            if (slots_[idx].state == slotState::EMPTY)
            {
                return CAPACITY;
            }
            // found it!
            if ((slots_[idx].state == slotState::OCCUPIED) && (slots_[idx].ref == ref))
            {
                return idx;
            }
            // Automatically handles WRAPPING
            idx = (idx + 1) & MASK;
        }
        return CAPACITY;
    }
};
} // namespace mm::state