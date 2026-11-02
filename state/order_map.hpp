#pragma once

#include "types/types.hpp"
#include <cstddef>
#include <cstdint>

namespace mm::state
{

struct OrderEntry
{
    std::uint64_t ref;       // key
    std::uint32_t remaining; // this order's remaining amount.
    std::uint16_t tick;      // price (tick index)
    std::uint16_t book_id;   // stock_locate
    mm::Side      side;      // buy or sell
    std::uint8_t  state;     // EMPTY / OCCUPIED / TOMBSTONE
};

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
    static constexpr std::uint64_t EMPTY =
        0; // placeholder value. if we are probing and we hit this, we know to STOP probing!
    void        insert(const OrderEntry& e);
    OrderEntry* find(std::uint64_t ref);
    void        erase(std::uint64_t ref);
    // using 128 because that's what the cache line boundaries for apple m4 silicon is. its just
    // what I got mane.
  private:
    alignas(128) OrderEntry
        slots_[CAPACITY]{}; // align on cache line boundaries, for the length of our CAPACITY.
    std::size_t count_{0};  // how many are CURRENTLY used ?
    std::size_t used_{0};   // how many have been used in TOTAL ?
};
} // namespace mm::state