// Tests for state/book.hpp
//
// Only the read side exists so far (constructor + best_bid/best_ask). Handler tests
// (add/execute/cancel/remove/replace) get added here as each one is written.

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

    if (failures == 0)
    {
        std::printf("all book tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
