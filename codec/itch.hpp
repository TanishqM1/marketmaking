#pragma once

#include "types/types.hpp"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

// Common 11-byte prefix on every ITCH message. All per-message bodies start at 11.
namespace mm::itch
{

struct MessageHeader
{
    static constexpr std::size_t msg_type     = 0; // byte 0
    static constexpr std::size_t stock_locate = 1; // byte 1/2
    static constexpr std::size_t tracking_num = 3; // byte 3/4
    static constexpr std::size_t timestamp    = 5; // byte 5-10, 6 bytes, ns since midnight

    static constexpr std::size_t size = 11; // where every body begins
};

enum class MsgType : char
{
    AddOrder               = 'A',
    OrderExecuted          = 'E',
    OrderExecutedWithPrice = 'C',
    OrderCancel            = 'X',
    OrderDelete            = 'D',
    OrderReplace           = 'U',
};

// (A) Add Order
struct AddOrder
{
    static constexpr std::size_t order_ref = 11; // 8 bytes
    static constexpr std::size_t side      = 19; // 1 byte char
    static constexpr std::size_t shares    = 20; // 4 byte amount
    static constexpr std::size_t stock     = 24; // 8 byte chars
    static constexpr std::size_t price     = 32; // 4 byte int, with 4 decimals (amt/10000)

    static constexpr std::size_t length = 36;
};

struct AddOrderFields
{
    std::uint64_t order_ref;
    Side          side;
    std::uint32_t shares;
    std::uint32_t price;
};

// (E) Order Executed. No price field -- the trade happened at the resting
// order's price, which we already hold in the order map.
struct OrderExecuted

{
    static constexpr std::size_t order_ref       = 11; // 8 bytes
    static constexpr std::size_t executed_shares = 19; // 4 bytes
    static constexpr std::size_t match_number    = 23; // 8 bytes

    static constexpr std::size_t length = 31;
};

struct OrderExecutedFields
{
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
};

// (C) Order Executed With Price. execution_price is trade-reporting info only;
// the book level to decrement still comes from the order map.
struct OrderExecutedWithPrice

{
    static constexpr std::size_t order_ref       = 11; // 8 bytes
    static constexpr std::size_t executed_shares = 19; // 4 bytes
    static constexpr std::size_t match_number    = 23; // 8 bytes
    static constexpr std::size_t printable       = 31; // 1 byte char, 'Y'/'N'
    static constexpr std::size_t execution_price = 32; // 4 byte int, 4 implied decimals

    static constexpr std::size_t length = 36;
};

enum class Printable : std::uint8_t
{
    Yes,
    No
};

struct OrderExecutedWithPriceFields
{
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    Printable     printable;
    std::uint32_t execution_price;
};

// (X) Order Cancel. Partial -- cancelled_shares is how much to remove.
struct OrderCancel
{
    static constexpr std::size_t order_ref        = 11; // 8 bytes
    static constexpr std::size_t cancelled_shares = 19; // 4 bytes

    static constexpr std::size_t length = 23;
};

struct OrderCancelFields
{
    std::uint64_t order_ref;
    std::uint32_t cancelled_shares;
};

// (D) Order Delete. No quantity field: the amount to remove is the order's
// remaining shares, which only the order map knows.
struct OrderDelete
{
    static constexpr std::size_t order_ref = 11; // 8 bytes

    static constexpr std::size_t length = 19;
};

// (U) Order Replace. Two order refs. Delete the original's full remaining from
// its old level, erase that ref, then insert new_order_ref at the new price.
// All later messages use the new ref.
struct OrderReplace

{
    static constexpr std::size_t original_order_ref = 11; // 8 bytes
    static constexpr std::size_t new_order_ref      = 19; // 8 bytes
    static constexpr std::size_t shares             = 27; // 4 bytes
    static constexpr std::size_t price              = 31; // 4 byte int, 4 implied decimals

    static constexpr std::size_t length = 35;
};

struct OrderReplaceFields
{
    std::uint64_t original_order_ref;
    std::uint64_t new_order_ref;
    std::uint32_t shares;
    std::uint32_t price;
};

void decode_packet(std::span<const std::byte> packet);

std::uint16_t read_be16(const std::byte* packet, std::size_t offset);
std::uint32_t read_be32(const std::byte* packet, std::size_t offset);
std::uint64_t read_be64(const std::byte* packet, std::size_t offset);
} // namespace mm::itch