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
#include <memory>
#include <span>
#include <vector>

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

// what decode_packet writes into: the books, the locate table, the shared order map.
// the map is 64 MB, so it lives on the heap. nothing is bound until bind() is called, so the
// framing tests below (locate 0 everywhere) touch no book.
struct Books
{
    std::vector<mm::state::Book>                  books;
    std::array<std::uint16_t, mm::state::LOCATES> locate_to_book;
    std::unique_ptr<mm::state::OrderMap> orders = std::make_unique<mm::state::OrderMap>();

    Books() { locate_to_book.fill(mm::state::NOT_TRADED); }

    void bind(std::uint16_t locate, std::uint32_t prev_close)
    {
        locate_to_book[locate] = static_cast<std::uint16_t>(books.size());
        books.emplace_back(prev_close);
    }

    bool decode(std::span<const std::byte> packet)
    {
        return mm::itch::decode_packet(packet, books, locate_to_book, *orders);
    }
};

// framing: ok / bad packet.
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
    CHECK(Books{}.decode(datagram));
}

// an empty packet has nothing wrong with it
void test_decode_packet_empty_is_ok()
{
    CHECK(Books{}.decode({}));
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
    CHECK(Books{}.decode(datagram));
}

// [A | Z...]: 'Z' is not ITCH, so the rest of the packet is dropped
void test_decode_packet_unknown_type_fails()
{
    std::byte datagram[36 + 40]{};
    datagram[0]  = std::byte{'A'};
    datagram[36] = std::byte{'Z'};
    CHECK(!Books{}.decode(datagram));
}

// 0xC1 is negative as a signed char. indexing MSG_LENGTH with it unconverted reads before
// the array. only caught with libc++ hardening (_LIBCPP_HARDENING_MODE) or ASan on; without
// them a missing cast can still pass.
void test_decode_packet_high_byte_type_fails()
{
    std::byte datagram[40]{};
    datagram[0] = std::byte{0xC1};
    CHECK(!Books{}.decode(datagram));
}

// [A | P cut to 20 bytes]: the skip runs past the end
void test_decode_packet_truncated_skip_fails()
{
    std::byte datagram[36 + 20]{};
    datagram[0]  = std::byte{'A'};
    datagram[36] = std::byte{'P'};
    CHECK(!Books{}.decode(datagram));
}

// [A cut to 20 bytes]: a decoded type that doesn't fit
void test_decode_packet_truncated_add_fails()
{
    std::byte datagram[20]{};
    datagram[0] = std::byte{'A'};
    CHECK(!Books{}.decode(datagram));
}

// [A | 5 bytes]: a tail too short to hold a header
void test_decode_packet_short_tail_fails()
{
    std::byte datagram[36 + 5]{};
    datagram[0]  = std::byte{'A'};
    datagram[36] = std::byte{'D'};
    CHECK(!Books{}.decode(datagram));
}

// ---------------------------------------------------------------------------
// Routing: decoded messages land in the right book. Offsets typed in by hand from the
// spec (11 = ref, 19 = side/shares, ...), not taken from the structs.
// ---------------------------------------------------------------------------

void put_be(std::vector<std::byte>& p, std::size_t at, std::uint64_t value, std::size_t bytes)
{
    for (std::size_t i = 0; i < bytes; ++i)
    {
        p[at + i] = static_cast<std::byte>(value >> (8 * (bytes - 1 - i)));
    }
}

// appends one zeroed message of `length` bytes with its type and locate filled in
std::size_t start(std::vector<std::byte>& p, char type, std::size_t length, std::uint16_t locate)
{
    std::size_t at = p.size();
    p.resize(at + length);
    p[at] = static_cast<std::byte>(type);
    put_be(p, at + 1, locate, 2);
    return at;
}

// (A) 36 / (F) 40: ref 11, side 19, shares 20, price 32
void put_add(std::vector<std::byte>& p, char type, std::uint16_t locate, std::uint64_t ref,
             char side, std::uint32_t shares, std::uint32_t price)
{
    std::size_t at = start(p, type, type == 'F' ? 40 : 36, locate);
    put_be(p, at + 11, ref, 8);
    p[at + 19] = static_cast<std::byte>(side);
    put_be(p, at + 20, shares, 4);
    put_be(p, at + 32, price, 4);
}

// (E) 31 / (C) 36 / (X) 23: ref 11, shares 19
void put_reduce(std::vector<std::byte>& p, char type, std::uint16_t locate, std::uint64_t ref,
                std::uint32_t shares)
{
    std::size_t length = type == 'E' ? 31 : type == 'C' ? 36 : 23;
    std::size_t at     = start(p, type, length, locate);
    put_be(p, at + 11, ref, 8);
    put_be(p, at + 19, shares, 4);
}

// (D) 19: ref 11
void put_delete(std::vector<std::byte>& p, std::uint16_t locate, std::uint64_t ref)
{
    std::size_t at = start(p, 'D', 19, locate);
    put_be(p, at + 11, ref, 8);
}

// (U) 35: original ref 11, new ref 19, shares 27, price 31
void put_replace(std::vector<std::byte>& p, std::uint16_t locate, std::uint64_t original_ref,
                 std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price)
{
    std::size_t at = start(p, 'U', 35, locate);
    put_be(p, at + 11, original_ref, 8);
    put_be(p, at + 19, new_ref, 8);
    put_be(p, at + 27, shares, 4);
    put_be(p, at + 31, price, 4);
}

constexpr std::uint16_t AAPL_LOCATE = 13;
constexpr std::uint32_t AAPL_CLOSE  = 1752300; // $175.23
constexpr std::uint16_t MSFT_LOCATE = 22;
constexpr std::uint32_t MSFT_CLOSE  = 4000000; // $400.00

// A Buy 100 @ $175.10 -> tick 17510 x 100. a price/shares swap would give tick 1 x 1751000.
void test_route_add_sets_best_bid()
{
    Books b;
    b.bind(AAPL_LOCATE, AAPL_CLOSE);
    std::vector<std::byte> p;
    put_add(p, 'A', AAPL_LOCATE, 5, 'B', 100, 1751000);

    CHECK(b.decode(p));
    CHECK_EQ(b.books[0].best_bid().tick, 17510u);
    CHECK_EQ(b.books[0].best_bid().qty, 100u);
    CHECK_EQ(b.books[0].best_ask().qty, 0u);
    CHECK((*b.orders).find(5) != nullptr);
}

// F is an add too, here on the sell side
void test_route_add_with_mpid_sets_best_ask()
{
    Books b;
    b.bind(AAPL_LOCATE, AAPL_CLOSE);
    std::vector<std::byte> p;
    put_add(p, 'F', AAPL_LOCATE, 7, 'S', 300, 1752000);

    CHECK(b.decode(p));
    CHECK_EQ(b.books[0].best_ask().tick, 17520u);
    CHECK_EQ(b.books[0].best_ask().qty, 300u);
}

// [A 100 | E 40 | C 10 | X 20 | D] in one packet: 100 -> 60 -> 50 -> 30 -> gone
void test_route_reduces_then_deletes()
{
    Books b;
    b.bind(AAPL_LOCATE, AAPL_CLOSE);
    std::vector<std::byte> p;
    put_add(p, 'A', AAPL_LOCATE, 5, 'B', 100, 1751000);
    put_reduce(p, 'E', AAPL_LOCATE, 5, 40);
    CHECK(b.decode(p));
    CHECK_EQ(b.books[0].best_bid().qty, 60u);

    p.clear();
    put_reduce(p, 'C', AAPL_LOCATE, 5, 10);
    put_reduce(p, 'X', AAPL_LOCATE, 5, 20);
    CHECK(b.decode(p));
    CHECK_EQ(b.books[0].best_bid().qty, 30u);

    p.clear();
    put_delete(p, AAPL_LOCATE, 5);
    CHECK(b.decode(p));
    CHECK_EQ(b.books[0].best_bid().qty, 0u);
    CHECK((*b.orders).find(5) == nullptr);
}

// U 5 -> 6 at $175.05 x 200: old level empties, new ref lives at the new price.
// a swap of the two refs (offsets 11 and 19) would leave ref 5 alive.
void test_route_replace_moves_the_order()
{
    Books b;
    b.bind(AAPL_LOCATE, AAPL_CLOSE);
    std::vector<std::byte> p;
    put_add(p, 'A', AAPL_LOCATE, 5, 'B', 100, 1751000);
    put_replace(p, AAPL_LOCATE, 5, 6, 200, 1750500);

    CHECK(b.decode(p));
    CHECK_EQ(b.books[0].best_bid().tick, 17505u);
    CHECK_EQ(b.books[0].best_bid().qty, 200u);
    CHECK((*b.orders).find(5) == nullptr);
    CHECK((*b.orders).find(6) != nullptr);

    p.clear();
    put_delete(p, AAPL_LOCATE, 6);
    CHECK(b.decode(p));
    CHECK_EQ(b.books[0].best_bid().qty, 0u);
}

// locate 99 was never bound: the add is skipped, nothing reaches the map
void test_route_untraded_locate_is_ignored()
{
    Books b;
    b.bind(AAPL_LOCATE, AAPL_CLOSE);
    std::vector<std::byte> p;
    put_add(p, 'A', 99, 9, 'B', 100, 1751000);

    CHECK(b.decode(p));
    CHECK_EQ((*b.orders).size(), 0u);
    CHECK_EQ(b.books[0].best_bid().qty, 0u);
}

// AAPL on locate 13 -> book 0, MSFT on locate 22 -> book 1. each add lands in its own book,
// and an execute finds its order through the shared map.
void test_route_two_books_stay_separate()
{
    Books b;
    b.bind(AAPL_LOCATE, AAPL_CLOSE);
    b.bind(MSFT_LOCATE, MSFT_CLOSE);
    std::vector<std::byte> p;
    put_add(p, 'A', AAPL_LOCATE, 5, 'B', 100, 1751000);
    put_add(p, 'A', MSFT_LOCATE, 8, 'B', 50, 3999000);
    put_reduce(p, 'E', MSFT_LOCATE, 8, 20);

    CHECK(b.decode(p));
    CHECK_EQ(b.books[0].best_bid().tick, 17510u);
    CHECK_EQ(b.books[0].best_bid().qty, 100u);
    CHECK_EQ(b.books[1].best_bid().tick, 39990u);
    CHECK_EQ(b.books[1].best_bid().qty, 30u);
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
    test_route_add_sets_best_bid();
    test_route_add_with_mpid_sets_best_ask();
    test_route_reduces_then_deletes();
    test_route_replace_moves_the_order();
    test_route_untraded_locate_is_ignored();
    test_route_two_books_stay_separate();

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
