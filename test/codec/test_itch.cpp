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

// MSG_LENGTH against the Nasdaq TotalView-ITCH 5.0 spec (rev. Apr 28 2023), typed in by
// hand, not taken from the structs, so a wrong constant can't hide behind itself.
static_assert(MSG_LENGTH['A'] == 36);
static_assert(MSG_LENGTH['F'] == 40);
static_assert(MSG_LENGTH['E'] == 31);
static_assert(MSG_LENGTH['C'] == 36);
static_assert(MSG_LENGTH['X'] == 23);
static_assert(MSG_LENGTH['D'] == 19);
static_assert(MSG_LENGTH['U'] == 35);
static_assert(MSG_LENGTH['R'] == 39);
static_assert(MSG_LENGTH['S'] == 12);
static_assert(MSG_LENGTH['H'] == 25);
static_assert(MSG_LENGTH['Y'] == 20);
static_assert(MSG_LENGTH['L'] == 26);
static_assert(MSG_LENGTH['V'] == 35);
static_assert(MSG_LENGTH['W'] == 12);
static_assert(MSG_LENGTH['K'] == 28);
static_assert(MSG_LENGTH['J'] == 35);
static_assert(MSG_LENGTH['h'] == 21);
static_assert(MSG_LENGTH['P'] == 44);
static_assert(MSG_LENGTH['Q'] == 40);
static_assert(MSG_LENGTH['B'] == 19);
static_assert(MSG_LENGTH['I'] == 50);
static_assert(MSG_LENGTH['N'] == 20);
static_assert(MSG_LENGTH['O'] == 48);

// exactly those 23 types are known; every other byte is 0
constexpr std::size_t known_types()
{
    std::size_t n = 0;
    for (std::uint8_t length : MSG_LENGTH)
    {
        n += length != 0;
    }
    return n;
}
static_assert(known_types() == 23);
static_assert(MSG_LENGTH['Z'] == 0);
static_assert(MSG_LENGTH[0] == 0);
static_assert(MSG_LENGTH[0xC1] == 0);
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

// decode_packet's only output so far (routing is #5) is ok / bad packet.
// before MSG_LENGTH, every one of these spun forever in default. ctest's TIMEOUT turns a
// hang into a failure.

// [A | S | P | D] = 36 + 12 + 44 + 19: skipped types sit between decoded ones
void test_decode_packet_skips_known_types()
{
    std::byte datagram[36 + 12 + 44 + 19]{};
    datagram[0]            = std::byte{'A'};
    datagram[36]           = std::byte{'S'};
    datagram[36 + 12]      = std::byte{'P'};
    datagram[36 + 12 + 44] = std::byte{'D'};
    CHECK(mm::itch::decode_packet(std::span<const std::byte>{datagram}));
}

// an empty packet has nothing wrong with it
void test_decode_packet_empty_is_ok()
{
    CHECK(mm::itch::decode_packet(std::span<const std::byte>{}));
}

// one of every skipped type, back to back
void test_decode_packet_skips_every_skipped_type()
{
    constexpr char types[] = {'S', 'R', 'H', 'Y', 'L', 'V', 'W', 'K',
                              'J', 'h', 'P', 'Q', 'B', 'I', 'N', 'O'};
    std::byte      datagram[12 + 39 + 25 + 20 + 26 + 35 + 12 + 28 + 35 + 21 + 44 + 40 + 19 + 50 +
                            20 + 48]{};
    std::size_t    at = 0;
    for (char type : types)
    {
        datagram[at] = static_cast<std::byte>(type);
        at += mm::itch::MSG_LENGTH[static_cast<std::uint8_t>(type)];
    }
    CHECK_EQ(at, sizeof(datagram));
    CHECK(mm::itch::decode_packet(std::span<const std::byte>{datagram}));
}

// [A | Z...]: 'Z' is not ITCH, so the rest of the packet is dropped
void test_decode_packet_unknown_type_fails()
{
    std::byte datagram[36 + 40]{};
    datagram[0]  = std::byte{'A'};
    datagram[36] = std::byte{'Z'};
    CHECK(!mm::itch::decode_packet(std::span<const std::byte>{datagram}));
}

// 0xC1 is negative as a signed char. indexing MSG_LENGTH with it unconverted reads before
// the array. only caught with libc++ hardening (_LIBCPP_HARDENING_MODE) or ASan on; without
// them a missing cast can still pass.
void test_decode_packet_high_byte_type_fails()
{
    std::byte datagram[40]{};
    datagram[0] = std::byte{0xC1};
    CHECK(!mm::itch::decode_packet(std::span<const std::byte>{datagram}));
}

// [A | P cut to 20 bytes]: the skip runs past the end
void test_decode_packet_truncated_skip_fails()
{
    std::byte datagram[36 + 20]{};
    datagram[0]  = std::byte{'A'};
    datagram[36] = std::byte{'P'};
    CHECK(!mm::itch::decode_packet(std::span<const std::byte>{datagram}));
}

// [A cut to 20 bytes]: a decoded type that doesn't fit
void test_decode_packet_truncated_add_fails()
{
    std::byte datagram[20]{};
    datagram[0] = std::byte{'A'};
    CHECK(!mm::itch::decode_packet(std::span<const std::byte>{datagram}));
}

// [A | 5 bytes]: a tail too short to hold a header
void test_decode_packet_short_tail_fails()
{
    std::byte datagram[36 + 5]{};
    datagram[0]  = std::byte{'A'};
    datagram[36] = std::byte{'D'};
    CHECK(!mm::itch::decode_packet(std::span<const std::byte>{datagram}));
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
    test_decode_packet_skips_known_types();
    test_decode_packet_empty_is_ok();
    test_decode_packet_skips_every_skipped_type();
    test_decode_packet_unknown_type_fails();
    test_decode_packet_high_byte_type_fails();
    test_decode_packet_truncated_skip_fails();
    test_decode_packet_truncated_add_fails();
    test_decode_packet_short_tail_fails();

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
