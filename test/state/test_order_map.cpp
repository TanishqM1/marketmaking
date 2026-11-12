// Tests for state/order_map.*
//
// Deliberately knows NOTHING about markets — no ITCH, no prices, no book. It just
// hammers the container: insert, find, erase, collisions, probe chains, backward-shift delete.
// That isolation is the point: when a book bug shows up later, these tests tell you
// whether the map is implicated.
//
// NOTE: will not LINK until insert/find/erase have bodies. The undefined-symbol list
// is the todo list.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unordered_map>

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

OrderEntry make_entry(std::uint64_t ref, std::uint32_t remaining = 100, std::uint32_t tick = 17525,
                      mm::Side side = mm::Side::Buy)
{
    return OrderEntry{.ref = ref, .remaining = remaining, .abs_tick = tick, .side = side};
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
    auto     e = make_entry(1029485761, 500, 17525, mm::Side::Sell);
    CHECK(m.insert(e));

    OrderEntry* got = m.find(1029485761);
    CHECK(got != nullptr);
    if (!got)
        return;
    CHECK_EQ(got->ref, 1029485761ull);
    CHECK_EQ(got->remaining, 500u);
    CHECK_EQ(got->abs_tick, 17525u);
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
    // THE erase test, and the one most likely to fail on a first implementation.
    // a, b, c all share a home slot -> stored at idx, idx+1, idx+2.
    // Erase `a` (the FIRST). If erase just blanks the slot to EMPTY without shifting b and c
    // back, find(b) stops there and reports "not in the map" even though b is one slot on.
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
    CHECK(m.find(b) != nullptr); // <- breaks if erase leaves a gap in the chain
    CHECK(m.find(c) != nullptr);
    if (m.find(b))
        CHECK_EQ(m.find(b)->remaining, 2u);
    if (m.find(c))
        CHECK_EQ(m.find(c)->remaining, 3u);
}

void test_erase_then_reinsert()
{
    // An erased slot must be WRITABLE again.
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
    // Insert/erase repeatedly on colliding refs. Every erase shifts the chain back, so
    // the table never fills up with dead slots.
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

void test_monotonic_refs_never_degrade()
{
    // THE regression test for tombstones. ITCH refs only go UP, so `ref & MASK` sweeps every
    // slot in turn. With tombstones, after ~65k adds no EMPTY slot was left and every insert /
    // missing-ref find scanned all 65,536 slots. Here: 4 full sweeps with only 16 live orders,
    // then 20,000 misses. With tombstones this takes ~1.3 billion slot reads (seconds);
    // with backward shift it is instant. If this test is slow, tombstones are back.
    OrderMap            m;
    constexpr std::uint64_t N    = 4 * OrderMap::CAPACITY;
    constexpr std::uint64_t LIVE = 16;
    for (std::uint64_t ref = 1; ref <= N; ++ref)
    {
        auto e = make_entry(ref);
        CHECK(m.insert(e));
        if (ref > LIVE)
            m.erase(ref - LIVE);
    }
    CHECK_EQ(m.size(), LIVE);
    for (std::uint64_t i = 0; i < 20000; ++i)
        CHECK(m.find(N + 1 + i * 7) == nullptr); // never inserted
    for (std::uint64_t ref = N - LIVE + 1; ref <= N; ++ref)
        CHECK(m.find(ref) != nullptr);           // the live ones survived every shift
}

void test_shift_keeps_entry_before_its_home()
{
    // Cluster: a (home H) at H, b (home H) at H+1, c (home H+1) at H+2.
    // Erase a: b must move back to H (its home), and c must move to H+1 (its home).
    // Then erase b's NEW slot via ref: c must stay at H+1 -- moving it to H would put it
    // before its home, where find() never looks.
    OrderMap            m;
    const std::uint64_t H = 777;
    const std::uint64_t a = colliding(H, 0);
    const std::uint64_t b = colliding(H, 1);
    const std::uint64_t c = colliding(H + 1, 1);
    auto ea = make_entry(a, 1);
    auto eb = make_entry(b, 2);
    auto ec = make_entry(c, 3);
    CHECK(m.insert(ea));
    CHECK(m.insert(eb));
    CHECK(m.insert(ec));

    m.erase(a);
    CHECK(m.find(a) == nullptr);
    CHECK(m.find(b) != nullptr);
    CHECK(m.find(c) != nullptr);

    m.erase(b);
    CHECK(m.find(b) == nullptr);
    CHECK(m.find(c) != nullptr);
    if (m.find(c))
        CHECK_EQ(m.find(c)->remaining, 3u);
    CHECK_EQ(m.size(), 1u);
}

void test_random_ops_match_reference()
{
    // Random insert/erase/find against std::unordered_map as the reference answer.
    // Refs are packed into a few home slots near the END of the table so clusters are long,
    // overlap, and wrap past index 65535 -> 0: every branch of the shift gets exercised.
    OrderMap                                     m;
    std::unordered_map<std::uint64_t, std::uint32_t> ref_model;
    std::uint64_t                                rng = 0x9E3779B97F4A7C15ull;
    auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };

    for (int step = 0; step < 200000; ++step)
    {
        // 8 home slots around the wrap point x 8 refs each = 64 possible keys
        std::uint64_t home = (OrderMap::CAPACITY - 4 + next() % 8) & OrderMap::MASK;
        std::uint64_t key  = colliding(home, next() % 8);
        std::uint64_t op   = next() % 3;

        if (op == 0)
        {
            auto val = static_cast<std::uint32_t>(step);
            auto e   = make_entry(key, val);
            bool ok  = m.insert(e);
            bool was_absent = ref_model.find(key) == ref_model.end();
            CHECK(ok == was_absent);
            if (was_absent)
                ref_model[key] = val;
        }
        else if (op == 1)
        {
            m.erase(key);
            ref_model.erase(key);
        }
        else
        {
            OrderEntry* p  = m.find(key);
            auto        it = ref_model.find(key);
            CHECK((p != nullptr) == (it != ref_model.end()));
            if (p && it != ref_model.end())
                CHECK_EQ(p->remaining, it->second);
        }
    }
    CHECK_EQ(m.size(), ref_model.size());
    for (const auto& [key, val] : ref_model)
    {
        OrderEntry* p = m.find(key);
        CHECK(p != nullptr);
        if (p)
            CHECK_EQ(p->remaining, val);
    }
}

} // namespace

void test_ref_zero_is_a_valid_key()
{
    // ITCH can send ref 0, so EMPTY lives in its own bit, not in ref == 0
    OrderMap m;
    CHECK(m.find(0) == nullptr);
    CHECK(m.insert(make_entry(0, 77)));
    CHECK(m.find(0) != nullptr);
    if (m.find(0))
        CHECK_EQ(m.find(0)->remaining, 77u);
    CHECK(!m.insert(make_entry(0, 5))); // duplicate
    m.erase(std::uint64_t{0});
    CHECK(m.find(0) == nullptr);
    CHECK_EQ(m.size(), 0u);
}

void test_packed_fields_round_trip()
{
    // the largest possible tick (UINT32_MAX price / 100) must not bleed into the side/occupied bits
    constexpr std::uint32_t max_tick = 0xFFFFFFFFu / 100;
    OrderMap                m;
    CHECK(m.insert(make_entry(1, 10, max_tick, mm::Side::Sell)));
    CHECK(m.insert(make_entry(2, 10, max_tick, mm::Side::Buy)));
    CHECK(m.insert(make_entry(3, 10, 0, mm::Side::Sell)));
    OrderEntry* a = m.find(1);
    OrderEntry* b = m.find(2);
    OrderEntry* c = m.find(3);
    CHECK(a && b && c);
    if (!(a && b && c))
        return;
    CHECK_EQ(a->abs_tick, max_tick);
    CHECK(a->side == mm::Side::Sell);
    CHECK_EQ(b->abs_tick, max_tick);
    CHECK(b->side == mm::Side::Buy);
    CHECK_EQ(c->abs_tick, 0u);
    CHECK(c->side == mm::Side::Sell);
    CHECK(a->occupied && b->occupied && c->occupied);
}

// ---------------------------------------------------------------------------
// Layout — compile time, zero cost, catches a bad entry size at BUILD time.
// ---------------------------------------------------------------------------
namespace mm::state
{
// 128-byte cache line / 16-byte entry = exactly 8 per line, never straddling.
static_assert(sizeof(OrderEntry) == 16, "OrderEntry must divide the 128B cache line");
static_assert(128 % sizeof(OrderEntry) == 0, "entries would straddle cache lines");
static_assert(alignof(OrderEntry) <= 128);

// A non-power-of-two CAPACITY makes `ref & MASK` silently wrong rather than failing.
static_assert((OrderMap::CAPACITY & OrderMap::MASK) == 0, "CAPACITY must be a power of two");
static_assert(OrderMap::MASK == OrderMap::CAPACITY - 1);
static_assert(OrderEntry{}.occupied == 0, "zero-init of slots_ depends on 0 meaning EMPTY");
} // namespace mm::state

int main()
{
    test_ref_zero_is_a_valid_key();
    test_packed_fields_round_trip();
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
    test_monotonic_refs_never_degrade();
    test_shift_keeps_entry_before_its_home();
    test_random_ops_match_reference();

    if (failures == 0)
    {
        std::printf("all order_map tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
