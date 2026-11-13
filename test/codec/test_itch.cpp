// Tests for codec/itch.*
//
// No framework. A test here is: build a byte array by hand, decode it, assert
// the fields. That needs nothing a framework would provide, and nothing in this
// file is ever linked into the trading binary -- so its performance is irrelevant.
//
// Two kinds of check, and they catch different things:
//   static_assert    compile time, zero cost. Layout: sizeof, offsets, lengths.
//   CHECK            run time. Decoded VALUES from known bytes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "codec/itch.hpp"

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

} // namespace

// ---------------------------------------------------------------------------
// Layout -- compile time. These fire at BUILD time, cost nothing, and ship
// nothing. A typo'd length constant is caught here, not three phases later.
// ---------------------------------------------------------------------------
namespace mm::itch
{
// offsets must sum to the declared length for every message
static_assert(AddOrder::price + 4 == AddOrder::length);
static_assert(AddOrderWithMPID::mpid + 4 == AddOrderWithMPID::length);
static_assert(OrderExecuted::match_number + 8 == OrderExecuted::length);
static_assert(OrderExecutedWithPrice::execution_price + 4 == OrderExecutedWithPrice::length);
static_assert(OrderCancel::cancelled_shares + 4 == OrderCancel::length);
static_assert(OrderDelete::order_ref + 8 == OrderDelete::length);
static_assert(OrderReplace::price + 4 == OrderReplace::length);
static_assert(RMessage::inverse_indicator + 1 == RMessage::length);
static_assert(RMessage::stock_ticker == MessageHeader::size);
static_assert(RMessage::stock_locate == MessageHeader::stock_locate);

// every message body starts after the common 11-byte header
static_assert(MessageHeader::size == 11);
static_assert(AddOrder::order_ref == MessageHeader::size);
static_assert(OrderDelete::order_ref == MessageHeader::size);

// (E) and (C) share the fields we read -- that's why one decoder serves both
static_assert(OrderExecuted::order_ref == OrderExecutedWithPrice::order_ref);
static_assert(OrderExecuted::executed_shares == OrderExecutedWithPrice::executed_shares);

static_assert(sizeof(Side) == 1); // we asked for : std::uint8_t
} // namespace mm::itch

// ---------------------------------------------------------------------------
// Values -- run time.
// ---------------------------------------------------------------------------
namespace
{

void test_read_be()
{
    // big-endian on the wire -> native on read
    const std::byte buf[] = {
        std::byte{0x00}, std::byte{0x01},                                   // 1
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0xF4}, // 500
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x3D}, std::byte{0x5C}, std::byte{0xB4}, std::byte{0xC1}, // 1029485761
    };

    CHECK_EQ(mm::itch::read_be16(buf, 0), 1u);
    CHECK_EQ(mm::itch::read_be32(buf, 2), 500u);
    CHECK_EQ(mm::itch::read_be64(buf, 6), 1029485761u);
}

// (A) Add Order, 36 bytes, hand-built. This is the test that actually verifies the
// OFFSETS -- a round-trip against our own encoder would pass even with wrong offsets,
// since both sides read the same constants.
void test_decode_add_order()
{
    std::byte msg[mm::itch::AddOrder::length]{};

    msg[0] = std::byte{'A'}; // msg type
    msg[1] = std::byte{0x00};
    msg[2] = std::byte{0x2A}; // stock_locate = 42
    // bytes 3-4 tracking, 5-10 timestamp: left zero, we don't read them

    // order_ref = 1029485761 at offset 11 (8 bytes, big-endian)
    msg[11] = std::byte{0x00};
    msg[12] = std::byte{0x00};
    msg[13] = std::byte{0x00};
    msg[14] = std::byte{0x00};
    msg[15] = std::byte{0x3D};
    msg[16] = std::byte{0x5C};
    msg[17] = std::byte{0xB4};
    msg[18] = std::byte{0xC1};

    msg[19] = std::byte{'B'}; // side

    // shares = 500 at offset 20 (4 bytes)
    msg[20] = std::byte{0x00};
    msg[21] = std::byte{0x00};
    msg[22] = std::byte{0x01};
    msg[23] = std::byte{0xF4};

    // stock = "AAPL    " at offset 24 (8 bytes) -- not read, but set it anyway
    msg[24] = std::byte{'A'};
    msg[25] = std::byte{'A'};
    msg[26] = std::byte{'P'};
    msg[27] = std::byte{'L'};
    msg[28] = std::byte{' '};
    msg[29] = std::byte{' '};
    msg[30] = std::byte{' '};
    msg[31] = std::byte{' '};

    // price = 175.2500 -> 1752500 at offset 32 (4 bytes, 4 implied decimals)
    msg[32] = std::byte{0x00};
    msg[33] = std::byte{0x1A};
    msg[34] = std::byte{0xBD};
    msg[35] = std::byte{0xB4};

    const auto f = mm::itch::detail::decode_add_order(msg);

    CHECK_EQ(f.order_ref, 1029485761u);
    CHECK_EQ(f.shares, 500u);
    CHECK_EQ(f.price, 1752500u); // integer, 4 implied decimals. never a float.
    CHECK(f.side == mm::Side::Buy);

    msg[19] = std::byte{'S'};
    CHECK(mm::itch::detail::decode_add_order(msg).side == mm::Side::Sell);

    // a byte that is neither B nor S still yields a defined side (Buy), never garbage
    msg[19] = std::byte{'X'};
    CHECK(mm::itch::detail::decode_add_order(msg).side == mm::Side::Buy);
}

// (D) Order Delete, 19 bytes. One field -- which is the point: the quantity to
// remove is NOT on the wire, it comes from the order map.
void test_decode_delete_order()
{
    std::byte msg[mm::itch::OrderDelete::length]{};

    msg[0]  = std::byte{'D'};
    msg[11] = std::byte{0x00};
    msg[12] = std::byte{0x00};
    msg[13] = std::byte{0x00};
    msg[14] = std::byte{0x00};
    msg[15] = std::byte{0x3D};
    msg[16] = std::byte{0x5C};
    msg[17] = std::byte{0xB4};
    msg[18] = std::byte{0xC1};

    const auto f = mm::itch::detail::decode_delete_order(msg);
    CHECK_EQ(f.order_ref, 1029485761u);
}

// (R) Stock Directory, 39 bytes. Offsets typed in by hand (1 = locate, 11 = ticker), not
// taken from RMessage, so a wrong constant can't hide behind itself.
void put_r_message(std::byte* msg, std::uint16_t locate, const char (&ticker)[9])
{
    msg[0] = std::byte{'R'};
    msg[1] = static_cast<std::byte>(locate >> 8);
    msg[2] = static_cast<std::byte>(locate & 0xFF);
    for (std::size_t i = 0; i < 8; ++i)
    {
        msg[11 + i] = static_cast<std::byte>(ticker[i]);
    }
}

constexpr std::array<char, 8> AAPL{'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
constexpr std::array<char, 8> MSFT{'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '};

void test_decode_r_message()
{
    std::byte msg[mm::itch::RMessage::length]{};
    put_r_message(msg, 14, "AAPL    ");

    const auto f = mm::itch::detail::decode_r_message(msg);
    CHECK_EQ(f.stock_locate, 14u);
    CHECK(f.stock_ticker == AAPL);

    put_r_message(msg, 0x1234, "MSFT    ");
    CHECK_EQ(mm::itch::detail::decode_r_message(msg).stock_locate, 0x1234u);
}

void test_next_r_message_stops_at_first_non_r()
{
    // [R AAPL | R MSFT | D] = 39 + 39 + 19
    std::byte datagram[39 + 39 + 19]{};
    put_r_message(datagram, 14, "AAPL    ");
    put_r_message(datagram + 39, 22, "MSFT    ");
    datagram[78] = std::byte{'D'};

    std::span<const std::byte> packet{datagram};
    std::size_t                offset = 0;
    mm::itch::RMessageFields   r{};

    CHECK(mm::itch::next_r_message(packet, offset, r));
    CHECK_EQ(offset, 39u);
    CHECK_EQ(r.stock_locate, 14u);
    CHECK(r.stock_ticker == AAPL);

    CHECK(mm::itch::next_r_message(packet, offset, r));
    CHECK_EQ(offset, 78u);
    CHECK_EQ(r.stock_locate, 22u);
    CHECK(r.stock_ticker == MSFT);

    CHECK(!mm::itch::next_r_message(packet, offset, r));
    CHECK_EQ(offset, 78u); // left pointing AT the D
}

void test_next_r_message_truncated_r_drops_the_tail()
{
    // a full R, then an R cut off after 20 bytes
    std::byte datagram[39 + 20]{};
    put_r_message(datagram, 14, "AAPL    ");
    datagram[39] = std::byte{'R'};

    std::span<const std::byte> packet{datagram};
    std::size_t                offset = 0;
    mm::itch::RMessageFields   r{};

    CHECK(mm::itch::next_r_message(packet, offset, r));
    CHECK(!mm::itch::next_r_message(packet, offset, r));
    CHECK_EQ(offset, packet.size());

    CHECK(!mm::itch::next_r_message(packet, offset, r)); // already at the end
    CHECK_EQ(offset, packet.size());
}

} // namespace

int main()
{
    test_read_be();
    test_decode_add_order();
    test_decode_delete_order();
    test_decode_r_message();
    test_next_r_message_stops_at_first_non_r();
    test_next_r_message_truncated_r_drops_the_tail();

    // TODO: the same for (E), (X), (U). (U) matters most -- two adjacent uint64 order
    // refs at offsets 11 and 19, so a transposition is silent and would corrupt the map.
    // TODO: transcribe the expected bytes from the official Nasdaq TotalView-ITCH 5.0
    // spec rather than from our own offset constants, or these only prove self-consistency.

    if (failures == 0)
    {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
