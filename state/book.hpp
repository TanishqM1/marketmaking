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
class Book
{
  public:
    OrderMap                       orders_;
    static constexpr std::uint16_t ARRAY_SIZE = 4096;
    static constexpr std::uint16_t NO_LEVEL   = ARRAY_SIZE;
    std::uint32_t                  bids_[ARRAY_SIZE + 1]{};
    std::uint32_t                  asks_[ARRAY_SIZE + 1]{};
    std::uint16_t                  best_bid_ = NO_LEVEL;
    std::uint16_t                  best_ask_ = NO_LEVEL;
    std::uint32_t                  low_;

    explicit Book(std::uint32_t tick)
    {
        std::uint32_t prev_close = tick / 100;
        if (prev_close < 2048)
        {
            low_ = 0;
        }
        else
        {
            low_ = prev_close - 2048;
        }
    };
};
} // namespace mm::state
