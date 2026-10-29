#pragma once

#include <cstdint>

// Common 11-byte prefix on every ITCH message. All per-message bodies start at 11.
struct MessageHeader
{
    static constexpr int msg_type = 0; // byte 0
    static constexpr int stock_locate = 1; // byte 1/2
    static constexpr int tracking_num = 3; // byte 3/4
    static constexpr int timestamp = 5; // byte 5-10, 6 bytes, ns since midnight

    static constexpr int size = 11; // where every body begins
};

// (A) Add Order
struct AddOrder
{
    static constexpr int order_ref = 11; // 8 bytes
    static constexpr int side = 19; // 1 byte char
    static constexpr int shares = 20; // 4 byte amount
    static constexpr int stock = 24; // 8 byte chars
    static constexpr int price = 32; // 4 int, with 4 decimals (amt/10000)

    static constexpr int length = 36;
};

// (E) Order Executed. No price field -- the trade happened at the resting
// order's price, which we already hold in the order map.
struct OrderExecuted
{
    static constexpr int order_ref = 11; // 8 bytes
    static constexpr int executed_shares = 19; // 4 bytes
    static constexpr int match_number = 23; // 8 bytes

    static constexpr int length = 31;
};

// (C) Order Executed With Price. execution_price is trade-reporting info only;
// the book level to decrement still comes from the order map.
struct OrderExecutedWithPrice
{
    static constexpr int order_ref = 11; // 8 bytes
    static constexpr int executed_shares = 19; // 4 bytes
    static constexpr int match_number = 23; // 8 bytes
    static constexpr int printable = 31; // 1 byte char, 'Y'/'N'
    static constexpr int execution_price = 32; // 4 int, 4 implied decimals

    static constexpr int length = 36;
};

// (X) Order Cancel. Partial -- cancelled_shares is how much to remove.
struct OrderCancel
{
    static constexpr int order_ref = 11; // 8 bytes
    static constexpr int cancelled_shares = 19; // 4 bytes

    static constexpr int length = 23;
};

// (D) Order Delete. No quantity field: the amount to remove is the order's
// remaining shares, which only the order map knows.
struct OrderDelete
{
    static constexpr int order_ref = 11; // 8 bytes

    static constexpr int length = 19;
};

// (U) Order Replace. Two order refs. Delete the original's full remaining from
// its old level, erase that ref, then insert new_order_ref at the new price.
// All later messages use the new ref.
struct OrderReplace
{
    static constexpr int original_order_ref = 11; // 8 bytes
    static constexpr int new_order_ref = 19; // 8 bytes
    static constexpr int shares = 27; // 4 bytes
    static constexpr int price = 31; // 4 int, 4 implied decimals

    static constexpr int length = 35;
};

