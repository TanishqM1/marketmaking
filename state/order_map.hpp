#pragma once

#include "types/types.hpp"
#include <cstddef>
#include <cstdint>

namespace mm::state
{

struct OrderEntry
{
    static constexpr std::uint32_t TICK_BITS = 26;

    std::uint64_t ref;
    std::uint32_t remaining;
    // this is assigning BITS, not BYTES
    // abs_tick = 26 bits, side = 1 bit, occupied = 1 bit. total is 28 bits, which pads to 32 bits,
    // which fits in 4 bytes
    std::uint32_t abs_tick : TICK_BITS;
    mm::Side      side : 1;
    std::uint32_t occupied : 1;
};

static_assert(sizeof(OrderEntry) == 16);
static_assert(static_cast<std::uint8_t>(mm::Side::Sell) == 1);
static_assert(OrderEntry{}.occupied == 0);

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
        // probe until we find an EMPTY (insert there). a matching ref on the way = duplicate.
        std::size_t idx = (e.ref & MASK);

        for (std::size_t i = 0; i < CAPACITY; i++)
        {
            if (!slots_[idx].occupied)
            {
                count_++;
                slots_[idx]          = e;
                slots_[idx].occupied = 1;
                return true;
            }
            if (slots_[idx].ref == e.ref)
            {
                return false;
            }
            // otherwise increment idx!
            idx = (idx + 1) & MASK;
        }
        // table full
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
        shift_delete(idx);
    }

    // delete DIRECTLY with a pointer to the order (no second search for it).
    // cases like delete() where we must find, then ERASE, might as well only probe once.
    // WARNING: erase moves other entries, so any OrderEntry* held across an erase is stale.
    void erase(OrderEntry* p) noexcept { shift_delete(static_cast<std::size_t>(p - slots_)); }

    // API to fetch count_.
    // TESTS ONLY. no code is generated.
    std::size_t size() const noexcept { return count_; }

    // using 128 because that's what the cache line boundaries for apple m4 silicon is. its just
    // what I got mane.
  private:
    //   this reads as making an ARRAY of sizeof("ORDERENTRY") CAPACITY. aka 65536 or "CAPACITY"
    //   orders!
    alignas(128) OrderEntry
        slots_[CAPACITY]{}; // align on cache line boundaries, for the length of our CAPACITY.
    // empty "{}" also means all indicies of our OrderMap start with "0". (for linear probing)
    std::size_t count_{0}; // how many are CURRENTLY used ?

    // backward-shift deletion: empty the slot, then slide later entries in the cluster back
    // into the gap, so no EMPTY ever sits between an entry and its home slot. keeps every
    // probe chain short forever (tombstones never cleared, and filled the table).
    // an entry at j may move into the hole only if the hole is in [home, j) -- i.e. moving
    // it doesn't put it BEFORE its own home slot, where find() would never look.
    void shift_delete(std::size_t hole) noexcept
    {
        std::size_t j = hole;
        for (std::size_t i = 0; i < CAPACITY; i++)
        {
            j = (j + 1) & MASK;
            if (!slots_[j].occupied)
            {
                break;
            }
            std::size_t home = slots_[j].ref & MASK;
            if (((j - home) & MASK) >= ((j - hole) & MASK))
            {
                slots_[hole] = slots_[j];
                hole         = j;
            }
        }
        slots_[hole] = OrderEntry{};
        count_--;
    }

    // helper for find() and erase(). const = function will NOT modidt object state. noexcept =
    // function will NOT throw an exception.
    std::size_t find_idx(std::uint64_t ref) const noexcept
    {
        // hash to find initial slot
        // while taken, walk +=1
        std::size_t idx = (ref & MASK);
        for (std::size_t i = 0; i < CAPACITY; i++)
        {
            // this means it has NOT been touched, the ref is NOT past this point.
            if (!slots_[idx].occupied)
            {
                return CAPACITY;
            }
            // found it!
            if (slots_[idx].ref == ref)
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