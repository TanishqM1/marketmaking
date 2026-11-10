#pragma once
#include "state/order_map.hpp"

// need: order map
// bid/ask arrays
// best bid/ask indicies
// low_ (low_ + N = high) so, low_ = center tick (base tick / 100)-2048
// handlers from the market feed (add, execute, cancel, remove, replace)
// empty state for best bid/ask (edge case)
// helper functions to linear scan for best bid/ask

namespace mm::state
{
struct TickLevel
{
    std::uint32_t tick;
    std::uint32_t qty;
};

class Book
{
  public:
    static constexpr std::uint16_t ARRAY_SIZE = 4096;
    static constexpr std::uint16_t NO_LEVEL   = ARRAY_SIZE;
    static constexpr std::uint32_t TICK_SIZE  = 100; // $0.01 in ITCH's 4-decimal price units

    explicit Book(std::uint32_t prev_close) noexcept
    {
        // centre the window on yesterday's close: $175.23 -> 1752300 -> tick 17523
        std::uint32_t center = prev_close / TICK_SIZE;

        // a cheap stock ($5 -> tick 500) would underflow to ~4 billion, so clamp to 0
        if (center < ARRAY_SIZE / 2)
        {
            low_ = 0;
        }
        else
        {
            low_ = center - ARRAY_SIZE / 2;
        }
    }

    // qty == 0 -> that side is empty and `tick` is meaningless.
    [[nodiscard]] TickLevel best_bid() const noexcept
    {
        return {low_ + best_bid_, bids_[best_bid_]};
    }

    [[nodiscard]] TickLevel best_ask() const noexcept
    {
        return {low_ + best_ask_, asks_[best_ask_]};
    }

  private:
    OrderMap      orders_;
    std::uint32_t bids_[ARRAY_SIZE + 1]{}; // [ARRAY_SIZE] is a guard slot, always 0
    std::uint32_t asks_[ARRAY_SIZE + 1]{};
    std::uint16_t best_bid_ = NO_LEVEL;
    std::uint16_t best_ask_ = NO_LEVEL;
    std::uint32_t low_{0}; // absolute tick at ladder index 0
};
} // namespace mm::state