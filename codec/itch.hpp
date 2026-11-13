#pragma once

#include "types/types.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Common 11-byte prefix on every ITCH message. All per-message bodies start at 11.
namespace mm::itch
{

inline std::uint16_t read_be16(const std::byte* packet, std::size_t offset)
{
    std::uint16_t temp;
    std::memcpy(&temp, &packet[offset], sizeof(temp));
    return std::byteswap(temp);
}

inline std::uint32_t read_be32(const std::byte* packet, std::size_t offset)
{
    std::uint32_t temp;
    std::memcpy(&temp, &packet[offset], sizeof(temp));
    return std::byteswap(temp);
}

inline std::uint64_t read_be64(const std::byte* packet, std::size_t offset)
{
    std::uint64_t temp;
    std::memcpy(&temp, &packet[offset], sizeof(temp));
    return std::byteswap(temp);
}

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
    AddOrderWithMPID       = 'F',
    OrderExecuted          = 'E',
    OrderExecutedWithPrice = 'C',
    OrderCancel            = 'X',
    OrderDelete            = 'D',
    OrderReplace           = 'U',
    RMessage               = 'R',
};

// (R) R Message

struct RMessage
{
    static constexpr std::size_t stock_locate = 1;
    static constexpr std::size_t stock_ticker = 11; // 8 bytes
    static constexpr std::size_t length       = 39;
};

struct RMessageFields
{
    std::uint16_t       stock_locate = 1;
    std::array<char, 8> stock_ticker;
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

// (F) Add Order with MPID. Identical layout to (A) plus a 4-byte MPID at offset 36,
// so AddOrder's field offsets all apply. MPID is market-maker identity -- we don't care.
// ONLY the length differs. Same as the (E)/(C) pair: reuse the decoder, never the length.
struct AddOrderWithMPID
{
    static constexpr std::size_t mpid = 36; // 4 byte chars, unused

    static constexpr std::size_t length = 40;
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

// TODO: DELETE THIS. (C) printable is never read -- it is a tape concern, not a book
// concern. It also has the same latent bug Side had: enumerators are 0/1 but the wire
// carries 'Y'/'N' (89/78), so any static_cast from the byte silently yields an
// out-of-enumeration value. If it is ever needed, give it the ASCII values.
enum class Printable : std::uint8_t
{
    Yes,
    No
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

struct OrderDeleteFields
{
    std::uint64_t order_ref;
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

// Per-message decoders. `detail` means "do not call directly" -- these assume the message
// already fits, because decode_packet bounds-checks before dispatching. Exposed only so
// tests can feed them hand-written bytes and assert the decoded fields.
namespace detail
{
AddOrderFields      decode_add_order(const std::byte* packet);
OrderExecutedFields decode_executed_order(const std::byte* packet);
OrderCancelFields   decode_cancel_order(const std::byte* packet);
OrderDeleteFields   decode_delete_order(const std::byte* packet);
OrderReplaceFields  decode_replace_order(const std::byte* packet);
RMessageFields      decode_r_message(const std::byte* packet);
} // namespace detail

// TODO: DELETE these three. They are redundant -- the inline definitions at the top of
// this file already declare them. Keeping both invites changing one signature and not
// the other.
std::uint16_t read_be16(const std::byte* packet, std::size_t offset);
std::uint32_t read_be32(const std::byte* packet, std::size_t offset);
std::uint64_t read_be64(const std::byte* packet, std::size_t offset);
} // namespace mm::itch