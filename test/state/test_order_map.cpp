// Tests for state/order_map.*
//
// Deliberately knows NOTHING about markets — no ITCH, no prices, no book. It just
// hammers the container: insert, find, erase, collisions, probe chains, tombstones.
// That isolation is the point: when a book bug shows up later, these tests tell you
// whether the map is implicated.
//
// NOTE: will not LINK until insert/find/erase have bodies. The undefined-symbol list
// is the todo list.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "state/order_map.hpp"

namespace
{
int failures = 0;

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                            \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do                                                                                             \
    {                                                                                              \
        auto va_ = (a);                                                                            \
        auto vb_ = (b);                                                                            \
        if (va_ != vb_)                                                                            \
        {                                                                                          \
            std::printf("FAIL %s:%d  %s == %s  (got %llu, want %llu)\n", __FILE__, __LINE__, #a,   \
                        #b, static_cast<unsigned long long>(va_),                                  \
                        static_cast<unsigned long long>(vb_));                                     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

using mm::state::OrderEntry;
using mm::state::OrderMap;

OrderEntry make_entry(std::uint64_t ref, std::uint32_t remaining = 100, std::uint16_t tick = 17525,
                      std::uint16_t book = 42, mm::Side side = mm::Side::Buy)
{
    OrderEntry e{};
    e.ref       = ref;
    e.remaining = remaining;
    e.tick      = tick;
    e.book_id   = book;
    e.side      = side;
    return e;
}

// Two refs that land on the same slot: differ by exactly CAPACITY, so
// (a & MASK) == (b & MASK). This is how every collision test below is built.
constexpr std::uint64_t colliding(std::uint64_t base, std::uint64_t n)
{
    return base + n * OrderMap::CAPACITY;
}

// ---------------------------------------------------------------------------

void test_empty_map_finds_nothing()
{
    OrderMap m;
    CHECK(m.find(1) == nullptr);
    CHECK(m.find(0) == nullptr); // a zeroed slot (ref 0) must not read as a hit
    CHECK(m.find(0xFFFF'FFFF'FFFF'FFFFull) == nullptr);
}

void test_insert_then_find()
{
    OrderMap m;
    auto     e = make_entry(1029485761, 500, 17525, 42, mm::Side::Sell);
    CHECK(m.insert(e));

    OrderEntry* got = m.find(1029485761);
    CHECK(got != nullptr);
    if (!got)
        return;
    CHECK_EQ(got->ref, 1029485761ull);
    CHECK_EQ(got->remaining, 500u);
    CHECK_EQ(got->tick, 17525u);
    CHECK_EQ(got->book_id, 42u);
    CHECK(got->side == mm::Side::Sell);
}

void test_find_returns_mutable_pointer()
{
    // find() must hand back a pointer INTO the table, not a copy -- the book
    // decrements `remaining` in place rather than paying a second lookup.
    OrderMap m;
    auto     e = make_entry(777, 500);
    CHECK(m.insert(e));

    OrderEntry* p = m.find(777);
    CHECK(p != nullptr);
    if (!p)
        return;
    p->remaining -= 200;

    CHECK_EQ(m.find(777)->remaining, 300u); // the write must have landed in the table
}

void test_collision_both_findable()
{
    // THE test for linear probing. Both refs hash to the same slot; the second must
    // probe forward on insert, and find() must walk past the first to reach it.
    OrderMap            m;
    const std::uint64_t a = colliding(5313, 0);
    const std::uint64_t b = colliding(5313, 1);
    CHECK_EQ(a & OrderMap::MASK, b & OrderMap::MASK); // same home slot

    auto ea = make_entry(a, 111);
    auto eb = make_entry(b, 222);
    CHECK(m.insert(ea));
    CHECK(m.insert(eb));

    CHECK(m.find(a) != nullptr);
    CHECK(m.find(b) != nullptr);
    if (m.find(a) && m.find(b))
    {
        CHECK_EQ(m.find(a)->remaining, 111u); // must not return the wrong entry
        CHECK_EQ(m.find(b)->remaining, 222u);
    }
}

void test_long_probe_chain()
{
    // 64 refs all on one home slot. Exercises a 64-long chain and wrap-around
    // arithmetic if the home slot is near the end of the array.
    OrderMap            m;
    constexpr int       N    = 64;
    const std::uint64_t base = OrderMap::CAPACITY - 3; // deliberately near the end

    for (int i = 0; i < N; ++i)
    {
        auto e = make_entry(colliding(base, static_cast<std::uint64_t>(i)),
                            static_cast<std::uint32_t>(i));
        CHECK(m.insert(e));
    }
    for (int i = 0; i < N; ++i)
    {
        OrderEntry* p = m.find(colliding(base, static_cast<std::uint64_t>(i)));
        CHECK(p != nullptr);
        if (p)
            CHECK_EQ(p->remaining, static_cast<std::uint32_t>(i));
    }
}

void test_erase_then_not_found()
{
    OrderMap m;
    auto     e = make_entry(12345);
    CHECK(m.insert(e));
    CHECK(m.find(12345) != nullptr);

    m.erase(12345);
    CHECK(m.find(12345) == nullptr);
}

void test_erase_middle_of_chain()
{
    // THE tombstone test, and the one most likely to fail on a first implementation.
    // a, b, c all share a home slot -> stored at idx, idx+1, idx+2.
    // Erase `a` (the FIRST). If erase blanks the slot to EMPTY, find(b) stops there
    // and reports "not in the map" even though b is one slot further on.
    OrderMap            m;
    const std::uint64_t a = colliding(900, 0);
    const std::uint64_t b = colliding(900, 1);
    const std::uint64_t c = colliding(900, 2);

    auto ea = make_entry(a, 1);
    auto eb = make_entry(b, 2);
    auto ec = make_entry(c, 3);
    CHECK(m.insert(ea));
    CHECK(m.insert(eb));
    CHECK(m.insert(ec));

    m.erase(a);

    CHECK(m.find(a) == nullptr);
    CHECK(m.find(b) != nullptr); // <- breaks if erase writes EMPTY instead of a tombstone
    CHECK(m.find(c) != nullptr);
    if (m.find(b))
        CHECK_EQ(m.find(b)->remaining, 2u);
    if (m.find(c))
        CHECK_EQ(m.find(c)->remaining, 3u);
}

void test_erase_then_reinsert()
{
    // A tombstoned slot must still be WRITABLE -- it's free for insert, just not
    // terminating for find.
    OrderMap m;
    auto     e1 = make_entry(555, 10);
    CHECK(m.insert(e1));
    m.erase(555);

    auto e2 = make_entry(555, 20);
    CHECK(m.insert(e2));

    CHECK(m.find(555) != nullptr);
    if (m.find(555))
        CHECK_EQ(m.find(555)->remaining, 20u);
}

void test_reinsert_does_not_duplicate()
{
    // Inserting a ref that's already present must be REJECTED and leave the original
    // untouched -- otherwise erase() removes one copy and find() returns the stale other.
    OrderMap m;
    auto     e1 = make_entry(888, 10);
    CHECK(m.insert(e1));
    auto e2 = make_entry(888, 20);
    CHECK(!m.insert(e2)); // duplicate -> false

    CHECK(m.find(888) != nullptr);
    if (m.find(888))
        CHECK_EQ(m.find(888)->remaining, 10u); // original kept, not overwritten

    m.erase(888);
    CHECK(m.find(888) == nullptr);
}

void test_churn()
{
    // Insert/erase repeatedly on colliding refs. With tombstones this is what slowly
    // fills the table; with backward-shift deletion it should run forever.
    // If this hangs, probing has no EMPTY slot left to terminate on.
    OrderMap m;
    for (int round = 0; round < 500; ++round)
    {
        for (int i = 0; i < 20; ++i)
        {
            auto e = make_entry(colliding(4096, static_cast<std::uint64_t>(i)),
                                static_cast<std::uint32_t>(round));
            CHECK(m.insert(e));
        }
        for (int i = 0; i < 20; ++i)
        {
            m.erase(colliding(4096, static_cast<std::uint64_t>(i)));
        }
    }
    CHECK(m.find(colliding(4096, 0)) == nullptr);

    // and the table must still work afterwards
    auto e = make_entry(99999, 42);
    CHECK(m.insert(e));
    CHECK(m.find(99999) != nullptr);
    if (m.find(99999))
        CHECK_EQ(m.find(99999)->remaining, 42u);
}

void test_many_distinct_refs()
{
    // 8192 unrelated refs = 12.5% load factor. Everything must still be findable,
    // and nothing must collide into a wrong answer.
    OrderMap      m;
    constexpr int N = 8192;
    for (int i = 0; i < N; ++i)
    {
        auto e = make_entry(1'000'000ull + static_cast<std::uint64_t>(i) * 7919,
                            static_cast<std::uint32_t>(i));
        CHECK(m.insert(e));
    }
    for (int i = 0; i < N; ++i)
    {
        OrderEntry* p = m.find(1'000'000ull + static_cast<std::uint64_t>(i) * 7919);
        CHECK(p != nullptr);
        if (p)
            CHECK_EQ(p->remaining, static_cast<std::uint32_t>(i));
    }
}

void test_erase_absent_is_safe()
{
    // Normal at start of day: cancels/deletes arrive for orders whose Add we never saw.
    // Must not corrupt the table or crash.
    OrderMap m;
    m.erase(42);
    auto e = make_entry(42, 7);
    CHECK(m.insert(e));
    m.erase(99);                  // different ref, same table
    CHECK(m.find(42) != nullptr); // the real entry survived
}

} // namespace

// ---------------------------------------------------------------------------
// Layout — compile time, zero cost, catches a bad entry size at BUILD time.
// ---------------------------------------------------------------------------
namespace mm::state
{
// 128-byte cache line / 32-byte entry = exactly 4 per line, never straddling.
// If this fires, add padding to OrderEntry.
static_assert(sizeof(OrderEntry) == 32, "OrderEntry must divide the 128B cache line");
static_assert(128 % sizeof(OrderEntry) == 0, "entries would straddle cache lines");
static_assert(alignof(OrderEntry) <= 128);

// A non-power-of-two CAPACITY makes `ref & MASK` silently wrong rather than failing.
static_assert((OrderMap::CAPACITY & OrderMap::MASK) == 0, "CAPACITY must be a power of two");
static_assert(OrderMap::MASK == OrderMap::CAPACITY - 1);
static_assert(static_cast<std::uint8_t>(slotState::EMPTY) == 0,
              "zero-init of slots_ depends on EMPTY being 0");
} // namespace mm::state

int main()
{
    test_empty_map_finds_nothing();
    test_insert_then_find();
    test_find_returns_mutable_pointer();
    test_collision_both_findable();
    test_long_probe_chain();
    test_erase_then_not_found();
    test_erase_middle_of_chain();
    test_erase_then_reinsert();
    test_reinsert_does_not_duplicate();
    test_churn();
    test_many_distinct_refs();
    test_erase_absent_is_safe();

    if (failures == 0)
    {
        std::printf("all order_map tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
