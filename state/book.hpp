#pragma once
#include <bit>

#include "state/order_map.hpp"

// need: order map
// bid/ask arrays
// best bid/ask indicies
// low_ (low_ + N = high) so, low_ = center tick (base price / 100)-2048
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
    static_assert(UINT32_MAX / TICK_SIZE < (std::uint32_t{1} << OrderEntry::TICK_BITS));
    static constexpr std::uint32_t WORDS =
        ARRAY_SIZE /
        64; // largest CPU-fitting integer, so we fit out bitmap into these chunks to process them

    explicit Book(std::uint32_t prev_close) noexcept
    {
        // centre the window on yesterday's close: $175.23 -> 1752300 -> tick 17523
        // price in, tick out
        std::uint32_t center_tick = prev_close / TICK_SIZE;

        // a cheap stock ($5 -> tick 500) would underflow to ~4 billion, so clamp to 0
        if (center_tick < ARRAY_SIZE / 2)
        {
            low_ = 0;
        }
        else
        {
            low_ = center_tick - ARRAY_SIZE / 2;
        }
    }

    // ITCH price, not tick
    void add(std::uint64_t ref, mm::Side side, std::uint32_t price, std::uint32_t qty) noexcept
    {
        std::uint32_t tick = price / TICK_SIZE;
        OrderEntry    order{.ref = ref, .remaining = qty, .abs_tick = tick, .side = side};
        std::uint32_t idx = tick - low_;

        // insertion error
        if (!orders_.insert(order))
        {
            return;
        }
        // oob error
        if (idx >= ARRAY_SIZE)
        {
            return;
        }
        if (side == mm::Side::Buy)
        {
            bids_[idx] += qty;
            set_bit(bid_bits_, idx);
            if (idx > best_bid_ || best_bid_ == NO_LEVEL)
            {
                best_bid_ = static_cast<std::uint16_t>(idx);
            }
        }
        else
        {
            asks_[idx] += qty;
            set_bit(ask_bits_, idx);
            if (idx < best_ask_)
            {
                best_ask_ = static_cast<std::uint16_t>(idx);
            }
        }
    }

    void execute(std::uint64_t ref, std::uint32_t qty) noexcept
    {
        // order lookup from ordermap.
        OrderEntry* order = orders_.find(ref);
        // order not found
        if (order == nullptr)
        {
            return;
        }
        reduce(order, qty);
    }

    void cancel(std::uint64_t ref, std::uint32_t qty) noexcept
    {
        OrderEntry* order = orders_.find(ref);

        if (order == nullptr)
        {
            return;
        }
        reduce(order, qty);
    }

    void remove(std::uint64_t ref) noexcept
    {
        // find order, simply remove its entire qty
        OrderEntry* order = orders_.find(ref);

        if (order == nullptr)
        {
            return;
        }
        reduce(order, (*order).remaining);
    }

    // delete (remove) + add
    void replace(std::uint64_t original_ref, std::uint64_t new_ref, std::uint32_t price,
                 std::uint32_t qty) noexcept
    {
        // find order, first remove it, then add it back.
        OrderEntry* order = orders_.find(original_ref);

        if (order == nullptr)
        {
            return;
        }
        mm::Side side = (*order).side;
        reduce(order, (*order).remaining);
        add(new_ref, side, price, qty);
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
    alignas(128) std::uint32_t bids_[ARRAY_SIZE + 1]{}; // [ARRAY_SIZE] is a guard slot, always 0
    alignas(128) std::uint32_t asks_[ARRAY_SIZE + 1]{};
    std::uint16_t best_bid_ = NO_LEVEL;
    std::uint16_t best_ask_ = NO_LEVEL;
    std::uint32_t low_{0}; // absolute tick at ladder index 0
    std::uint64_t bid_bits_[WORDS]{};
    std::uint64_t ask_bits_[WORDS]{};

    void reduce(OrderEntry* order, std::uint32_t qty) noexcept
    {
        // if we want to reduce for more than we have, well, just reduce
        // with what we can (which is order.remaining).
        if (qty > (*order).remaining)
        {
            qty = (*order).remaining;
        }

        // parse side and idx.
        mm::Side      side = (*order).side;
        std::uint32_t idx  = (*order).abs_tick - low_;

        // subtract the qty that we decided on (basically qty is min(qty, order.remaining))
        (*order).remaining -= qty;
        // if the order has nothing left, remove it from ordermap
        if ((*order).remaining == 0)
        {
            orders_.erase(order);
        }
        // if the tick (idx) is not in our bids_ or asks_, then return (its out of window)
        if (idx >= ARRAY_SIZE)
        {
            return;
        }

        // else, remove the qty as needed from our bids|asks array! in case the best ask/bid
        // was affected, call the helper function to find the new one.
        if (side == mm::Side::Buy)
        {
            bids_[idx] -= qty;
            if (bids_[idx] == 0)
            {
                clear_bit(bid_bits_, idx);
                if (idx == best_bid_)
                {
                    update_best_bid_down();
                }
            }
        }
        else
        {
            asks_[idx] -= qty;
            if (asks_[idx] == 0)
            {
                clear_bit(ask_bits_, idx);
                if (idx == best_ask_)
                {
                    update_best_ask_up();
                }
            }
        }
    }

    static void set_bit(std::uint64_t (&bits)[WORDS], std::uint32_t idx) noexcept
    {
        bits[idx / 64] |= std::uint64_t{1} << (idx % 64);
    }

    static void clear_bit(std::uint64_t (&bits)[WORDS], std::uint32_t idx) noexcept
    {
        bits[idx / 64] &= ~(std::uint64_t{1} << (idx % 64));
    }

    void update_best_bid_down() noexcept
    {
        std::uint32_t word = best_bid_ / 64;
        while (bid_bits_[word] == 0)
        {
            if (word == 0)
            {
                best_bid_ = NO_LEVEL;
                return;
            }
            --word;
        }
        std::uint32_t bit = 63 - static_cast<std::uint32_t>(std::countl_zero(bid_bits_[word]));
        best_bid_         = static_cast<std::uint16_t>(word * 64 + bit);
    }

    void update_best_ask_up() noexcept
    {
        std::uint32_t word = best_ask_ / 64;
        while (word < WORDS && ask_bits_[word] == 0)
        {
            ++word;
        }
        if (word == WORDS)
        {
            best_ask_ = NO_LEVEL;
            return;
        }
        std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(ask_bits_[word]));
        best_ask_         = static_cast<std::uint16_t>(word * 64 + bit);
    }
};
} // namespace mm::state