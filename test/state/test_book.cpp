// Tests for state/book.hpp
//
// Covers the constructor, best_bid/best_ask, add, execute, cancel, the scans (driven through
// execute/cancel, since they're private), remove and replace.

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

// ---- scans ----------------------------------------------------------------

void test_best_bid_empties_scans_down()
{
    // the scan must skip the empty ticks between 175.23 and 175.20
    static Book b{1752300};
    b.add(1, Side::Buy, 1752300, 100);
    b.add(2, Side::Buy, 1752000, 40);
    b.execute(1, 100);
    CHECK_EQ(b.best_bid().tick, 17520u);
    CHECK_EQ(b.best_bid().qty, 40u);
}

void test_best_ask_empties_scans_up()
{
    static Book b{1752300};
    b.add(1, Side::Sell, 1752500, 30);
    b.add(2, Side::Sell, 1753000, 70);
    b.cancel(1, 30);
    CHECK_EQ(b.best_ask().tick, 17530u);
    CHECK_EQ(b.best_ask().qty, 70u);
}

void test_last_level_empties_side_goes_empty()
{
    // nothing left to find: the scan runs off the window and parks on NO_LEVEL
    static Book b{1752300};
    b.add(1, Side::Buy, 1752300, 10);
    b.add(2, Side::Sell, 1752500, 10);
    b.execute(1, 10);
    b.execute(2, 10);
    CHECK_EQ(b.best_bid().qty, 0u);
    CHECK_EQ(b.best_ask().qty, 0u);
    CHECK_EQ(b.best_bid().tick, 17523u - 2048u + Book::NO_LEVEL);
    CHECK_EQ(b.best_ask().tick, 17523u - 2048u + Book::NO_LEVEL);

    // and the side must be usable again afterwards
    b.add(3, Side::Buy, 1752100, 5);
    CHECK_EQ(b.best_bid().tick, 17521u);
    CHECK_EQ(b.best_bid().qty, 5u);
}

void test_non_best_level_empties_no_move()
{
    // emptying a worse level must leave the best alone
    static Book b{1752300};
    b.add(1, Side::Buy, 1752300, 10);
    b.add(2, Side::Buy, 1752000, 40);
    b.execute(2, 40);
    CHECK_EQ(b.best_bid().tick, 17523u);
    CHECK_EQ(b.best_bid().qty, 10u);
}

void test_bid_scan_from_index_zero_no_wrap()
{
    // $5 stock -> low_ = 0. A bid at tick 0 sits on ladder index 0; emptying it must
    // end on NO_LEVEL, not wrap the uint16 cursor to 65535.
    static Book b{50000};
    b.add(1, Side::Buy, 0, 10);
    CHECK_EQ(b.best_bid().tick, 0u);
    b.execute(1, 10);
    CHECK_EQ(b.best_bid().qty, 0u);
    CHECK_EQ(b.best_bid().tick, 0u + Book::NO_LEVEL);
}

void test_ask_scan_at_top_of_window()
{
    // best ask on the last real index (4095): the scan starts at 4096 = the guard
    static Book b{50000};
    b.add(1, Side::Sell, 4095 * Book::TICK_SIZE, 10);
    CHECK_EQ(b.best_ask().tick, 4095u);
    b.cancel(1, 10);
    CHECK_EQ(b.best_ask().qty, 0u);
    CHECK_EQ(b.best_ask().tick, 0u + Book::NO_LEVEL);
}

// ---- remove / replace -----------------------------------------------------

void test_remove_takes_full_remaining()
{
    // D carries no qty: the book must pull whatever is left (60 after a partial fill)
    static Book b{1752300};
    b.add(1, Side::Buy, 1752300, 100);
    b.add(2, Side::Buy, 1752300, 50);
    b.execute(1, 40);
    b.remove(1);
    CHECK_EQ(b.best_bid().qty, 50u);
    b.remove(1); // already gone: no-op
    CHECK_EQ(b.best_bid().qty, 50u);
}

void test_remove_best_scans()
{
    static Book b{1752300};
    b.add(1, Side::Sell, 1752500, 30);
    b.add(2, Side::Sell, 1752800, 70);
    b.remove(1);
    CHECK_EQ(b.best_ask().tick, 17528u);
    CHECK_EQ(b.best_ask().qty, 70u);
    b.remove(42); // unknown ref
    CHECK_EQ(b.best_ask().qty, 70u);
}

void test_replace_moves_price_and_keeps_side()
{
    // U has no side on the wire: a sell must come back as a sell at the new price
    static Book b{1752300};
    b.add(1, Side::Sell, 1752500, 30);
    b.replace(1, 2, 1752700, 20);
    CHECK_EQ(b.best_ask().tick, 17527u);
    CHECK_EQ(b.best_ask().qty, 20u);
    CHECK_EQ(b.best_bid().qty, 0u);
}

void test_replace_new_ref_live_old_ref_dead()
{
    // all later messages use the new ref; the old one must be gone
    static Book b{1752300};
    b.add(1, Side::Buy, 1752300, 100);
    b.replace(1, 2, 1752300, 80);
    CHECK_EQ(b.best_bid().qty, 80u);
    b.execute(1, 10); // old ref: no-op
    CHECK_EQ(b.best_bid().qty, 80u);
    b.execute(2, 30); // new ref works
    CHECK_EQ(b.best_bid().qty, 50u);
}

void test_replace_unknown_ref_is_noop()
{
    // never saw the original -> side unknown -> must NOT guess and add
    static Book b{1752300};
    b.replace(42, 43, 1752300, 80);
    CHECK_EQ(b.best_bid().qty, 0u);
    CHECK_EQ(b.best_ask().qty, 0u);
}

void test_replace_into_and_out_of_window()
{
    static Book b{1752300};
    b.add(1, Side::Buy, 1752300, 10);
    b.replace(1, 2, 9999900, 10); // moves out of the window: ladder empties
    CHECK_EQ(b.best_bid().qty, 0u);
    b.replace(2, 3, 1752100, 10); // back in: found in the map, re-enters the ladder
    CHECK_EQ(b.best_bid().tick, 17521u);
    CHECK_EQ(b.best_bid().qty, 10u);
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
    test_best_bid_empties_scans_down();
    test_best_ask_empties_scans_up();
    test_last_level_empties_side_goes_empty();
    test_non_best_level_empties_no_move();
    test_bid_scan_from_index_zero_no_wrap();
    test_ask_scan_at_top_of_window();
    test_remove_takes_full_remaining();
    test_remove_best_scans();
    test_replace_moves_price_and_keeps_side();
    test_replace_new_ref_live_old_ref_dead();
    test_replace_unknown_ref_is_noop();
    test_replace_into_and_out_of_window();

    if (failures == 0)
    {
        std::printf("all book tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
