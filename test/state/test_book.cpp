// Tests for state/book.hpp
//
// Covers the constructor, best_bid/best_ask, add, execute and cancel. remove/replace and
// the scan helpers get tests as they're written -- until the scans exist, no test empties
// the best level while a worse level still holds qty.

#include <cstdio>

#include "state/book.hpp"

namespace
{
int failures = 0;

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

using mm::Side;
using mm::state::Book;

// Books are ~2 MB (the order map), so they live in static storage, never on the stack --
// same as production. One per test so no state leaks between them.

void test_new_book_is_empty()
{
    static Book b{1752300}; // prev close $175.23
    CHECK_EQ(b.best_bid().qty, 0u);
    CHECK_EQ(b.best_ask().qty, 0u);
}

void test_window_centred_on_prev_close()
{
    // $175.23 -> tick 17523 -> low_ = 17523 - 2048. An empty side's cursor is NO_LEVEL,
    // so its reported tick is low_ + NO_LEVEL -- which exposes low_ without a getter.
    static Book b{1752300};
    CHECK_EQ(b.best_bid().tick, 17523u - 2048u + Book::NO_LEVEL);
    CHECK_EQ(b.best_ask().tick, 17523u - 2048u + Book::NO_LEVEL);
}

void test_cheap_stock_window_clamps_to_zero()
{
    // $5.00 -> tick 500 < 2048. Unclamped, 500 - 2048 wraps to ~4.29e9.
    static Book b{50000};
    CHECK_EQ(b.best_bid().tick, 0u + Book::NO_LEVEL);
}

void test_window_boundary()
{
    // Exactly at the clamp boundary: tick 2048 -> low_ = 0, not a wrap.
    static Book b{204800};
    CHECK_EQ(b.best_bid().tick, 0u + Book::NO_LEVEL);
}

// ---- add ------------------------------------------------------------------
// Window for prev close $175.23: low_ = 15475. Prices are ITCH 4-decimal ($175.23 = 1752300).

void test_add_first_bid_sets_best()
{
    // the bid cursor starts at NO_LEVEL; the first bid must still move it
    static Book b{1752300};
    b.add(1, Side::Buy, 1752000, 100);
    CHECK_EQ(b.best_bid().tick, 17520u);
    CHECK_EQ(b.best_bid().qty, 100u);
    CHECK_EQ(b.best_ask().qty, 0u); // other side untouched
}

void test_add_bids_better_worse_same()
{
    static Book b{1752300};
    b.add(1, Side::Buy, 1752000, 100); // 175.20
    b.add(2, Side::Buy, 1752300, 50);  // better -> cursor moves up
    b.add(3, Side::Buy, 1751000, 10);  // worse  -> cursor stays
    b.add(4, Side::Buy, 1752300, 5);   // same level -> sums
    CHECK_EQ(b.best_bid().tick, 17523u);
    CHECK_EQ(b.best_bid().qty, 55u);
}

void test_add_asks_better_worse_same()
{
    static Book b{1752300};
    b.add(1, Side::Sell, 1753000, 70); // 175.30
    b.add(2, Side::Sell, 1752500, 30); // better (lower) -> cursor moves down
    b.add(3, Side::Sell, 1754000, 10); // worse  -> cursor stays
    b.add(4, Side::Sell, 1752500, 2);  // same level -> sums
    CHECK_EQ(b.best_ask().tick, 17525u);
    CHECK_EQ(b.best_ask().qty, 32u);
}

void test_add_out_of_window_skips_ladder()
{
    // $999.99 is far above the window: map only, the book must still look empty
    static Book b{1752300};
    b.add(1, Side::Buy, 9999900, 999);
    b.add(2, Side::Sell, 100, 999); // $0.01, below the window (wraps on unsigned subtract)
    CHECK_EQ(b.best_bid().qty, 0u);
    CHECK_EQ(b.best_ask().qty, 0u);
}

void test_add_duplicate_ref_ignored()
{
    // insert rejects the dup, so the ladder must not count it either
    static Book b{1752300};
    b.add(1, Side::Buy, 1752300, 50);
    b.add(1, Side::Buy, 1752300, 77);
    CHECK_EQ(b.best_bid().qty, 50u);
}

// ---- execute / cancel -----------------------------------------------------

void test_execute_partial_then_full()
{
    static Book b{1752300};
    b.add(1, Side::Buy, 1752300, 100);
    b.add(2, Side::Buy, 1752300, 50);
    b.execute(1, 40);
    CHECK_EQ(b.best_bid().qty, 110u);
    b.execute(1, 60); // order 1 fully filled -> erased, level keeps order 2
    CHECK_EQ(b.best_bid().qty, 50u);
    b.execute(1, 10); // ref 1 is gone: must not touch the level
    CHECK_EQ(b.best_bid().qty, 50u);
}

void test_execute_more_than_remaining_clamps()
{
    // a bad feed value must not wrap remaining / the level to ~4 billion
    static Book b{1752300};
    b.add(1, Side::Sell, 1752500, 30);
    b.execute(1, 500);
    CHECK_EQ(b.best_ask().qty, 0u);
}

void test_execute_unknown_ref_is_noop()
{
    static Book b{1752300};
    b.add(1, Side::Sell, 1752500, 30);
    b.execute(42, 5);
    CHECK_EQ(b.best_ask().qty, 30u);
}

void test_execute_out_of_window_leaves_ladder()
{
    // the order is in the map but was never in the ladder -> nothing to subtract
    static Book b{1752300};
    b.add(1, Side::Buy, 9999900, 999);
    b.add(2, Side::Buy, 1752300, 50);
    b.execute(1, 999);
    CHECK_EQ(b.best_bid().qty, 50u);
}

void test_cancel_partial_then_full()
{
    static Book b{1752300};
    b.add(1, Side::Sell, 1752500, 100);
    b.cancel(1, 30);
    CHECK_EQ(b.best_ask().qty, 70u);
    b.cancel(1, 70); // to zero -> erased
    CHECK_EQ(b.best_ask().qty, 0u);
    b.cancel(1, 1); // gone: no-op, no wrap
    CHECK_EQ(b.best_ask().qty, 0u);
}

} // namespace

// Guard slot must exist: the empty-side read indexes [NO_LEVEL].
static_assert(Book::NO_LEVEL == Book::ARRAY_SIZE);
static_assert((Book::ARRAY_SIZE & (Book::ARRAY_SIZE - 1)) == 0, "ARRAY_SIZE must be a power of two");

int main()
{
    test_new_book_is_empty();
    test_window_centred_on_prev_close();
    test_cheap_stock_window_clamps_to_zero();
    test_window_boundary();
    test_add_first_bid_sets_best();
    test_add_bids_better_worse_same();
    test_add_asks_better_worse_same();
    test_add_out_of_window_skips_ladder();
    test_add_duplicate_ref_ignored();
    test_execute_partial_then_full();
    test_execute_more_than_remaining_clamps();
    test_execute_unknown_ref_is_noop();
    test_execute_out_of_window_leaves_ladder();
    test_cancel_partial_then_full();

    if (failures == 0)
    {
        std::printf("all book tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
