// Tests for codec/itch.*
//
// No framework. A test here is: build a byte array by hand, decode it, assert
// the fields. That needs nothing a framework would provide, and nothing in this
// file is ever linked into the trading binary -- so its performance is irrelevant.
//
// Two kinds of check, and they catch different things:
//   static_assert    compile time, zero cost. Layout: sizeof, offsets, lengths.
//   CHECK            run time. Decoded VALUES from known bytes.

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
        std::byte{0x00}, std::byte{0x01},                                    // 1
        std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0xF4},  // 500
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x3D}, std::byte{0x5C}, std::byte{0xB4}, std::byte{0xC1},  // 1029485761
    };

    CHECK_EQ(mm::itch::read_be16(buf, 0), 1u);
    CHECK_EQ(mm::itch::read_be32(buf, 2), 500u);
    CHECK_EQ(mm::itch::read_be64(buf, 6), 1029485761u);
}

} // namespace

int main()
{
    test_read_be();

    // TODO: decode assertions per message type. Needs the decoders reachable --
    // they're in an anonymous namespace in itch.cpp right now, so only
    // decode_packet is linkable. See the note in the handoff.

    if (failures == 0)
    {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
