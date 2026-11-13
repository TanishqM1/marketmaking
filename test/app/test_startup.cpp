// Tests for app/startup.*
//
// Everything except run_startup() itself, which only adds the socket loop around
// handle_startup_packet(). Datagrams are hand-built byte arrays, no network.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>

#include "app/startup.hpp"

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

using mm::app::Market;

constexpr std::array<char, 8> AAPL{'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
constexpr std::array<char, 8> MSFT{'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '};
constexpr std::array<char, 8> TSLA{'T', 'S', 'L', 'A', ' ', ' ', ' ', ' '};

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

void load_three_symbols(Market& market)
{
    std::istringstream in{"AAPL,175.23\nMSFT,410.15\nTSLA,250\n"};
    mm::app::load_config(market, in);
}

bool config_throws(const std::string& text)
{
    Market             market;
    std::istringstream in{text};
    try
    {
        mm::app::load_config(market, in);
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
    return false;
}

std::uint32_t price_of(const std::string& price_text)
{
    Market             market;
    std::istringstream in{"AAPL," + price_text + "\n"};
    mm::app::load_config(market, in);
    return market.config.at(AAPL).prev_close;
}

bool price_rejected(const std::string& price_text)
{
    return config_throws("AAPL," + price_text + "\n");
}

std::string config_error_message(const std::string& text)
{
    Market             market;
    std::istringstream in{text};
    try
    {
        mm::app::load_config(market, in);
    }
    catch (const std::runtime_error& e)
    {
        return e.what();
    }
    return "";
}

std::string startup_error_message(Market& market, std::span<const std::byte> packet)
{
    try
    {
        mm::app::handle_startup_packet(market, packet);
    }
    catch (const std::runtime_error& e)
    {
        return e.what();
    }
    return "";
}

// ---------------------------------------------------------------------------
// prices: dollars text -> ITCH integer units (4 implied decimals). No float anywhere.
// ---------------------------------------------------------------------------
void test_config_prices()
{
    CHECK_EQ(price_of("175.23"), 1752300u);
    CHECK_EQ(price_of("175"), 1750000u);
    CHECK_EQ(price_of("175.2"), 1752000u);
    CHECK_EQ(price_of("175.2500"), 1752500u);
    CHECK_EQ(price_of("0.0001"), 1u);
    CHECK_EQ(price_of("0"), 0u);
    CHECK_EQ(price_of("429496.7295"), 4294967295u); // UINT32_MAX

    CHECK(price_rejected("429496.7296")); // one past UINT32_MAX
    CHECK(price_rejected("429497"));
    CHECK(price_rejected("99999999999999999999")); // overflows uint64 in from_chars
    CHECK(price_rejected("175.23456"));            // a 5th decimal would be truncated
    CHECK(price_rejected(""));
    CHECK(price_rejected("."));
    CHECK(price_rejected("175."));
    CHECK(price_rejected(".5"));
    CHECK(price_rejected("-1"));
    CHECK(price_rejected("+1"));
    CHECK(price_rejected("1.-5"));
    CHECK(price_rejected("abc"));
    CHECK(price_rejected("1.2.3"));
    CHECK(price_rejected("1e3"));
}

// ---------------------------------------------------------------------------
// load_config
// ---------------------------------------------------------------------------
void test_load_config()
{
    Market             market;
    std::istringstream in{"# ticker,prev_close\n\nAAPL,175.23\nMSFT,410.15\nNVDALONG,1\n"};
    mm::app::load_config(market, in);

    CHECK_EQ(market.config.size(), 3u);
    CHECK_EQ(market.config.at(AAPL).prev_close, 1752300u); // padded to 8, so it matches the wire
    CHECK_EQ(market.config.at(MSFT).prev_close, 4101500u);
    CHECK_EQ(market.config.at(AAPL).book, Market::NOT_TRADED);
    CHECK(market.config.contains(std::array<char, 8>{'N', 'V', 'D', 'A', 'L', 'O', 'N', 'G'}));
    CHECK(market.books.capacity() >= 3); // room for every book reserved up front
}

void test_load_config_rejects_bad_lines()
{
    CHECK(config_throws("AAPL 175.23\n"));         // no comma
    CHECK(config_throws("TOOLONGXX,1\n"));         // 9-char ticker
    CHECK(config_throws(",1\n"));                  // empty ticker
    CHECK(config_throws("AAPL,\n"));               // empty price
    CHECK(config_throws("AAPL,abc\n"));            // not a number
    CHECK(config_throws("AAPL,1\nAAPL,2\n"));      // duplicate
    CHECK(config_throws(" AAPL,175.23\n"));        // space before ticker
    CHECK(config_throws("AAPL ,175.23\n"));        // space after ticker
    CHECK(config_throws("AA PL,175.23\n"));        // space inside ticker
    CHECK(config_throws("AAPL\t,175.23\n"));       // tab in ticker
    CHECK(config_throws("AAPL, 175.23\n"));        // space before price
    CHECK(config_throws("AAPL,175.23 \n"));        // space after price
    CHECK(config_throws("AAPL,175.23\r\n"));       // Windows line ending
    CHECK(config_throws("AAPL,1\n\r\n"));          // Windows blank line
    CHECK(!config_throws("# only a comment\n\n")); // empty config is allowed

    // the error names the line and quotes it
    CHECK(config_error_message("AAPL,1\nMSFT ,2\n") == "config line 2: bad entry \"MSFT ,2\"");
    CHECK(config_error_message("AAPL,1\nAAPL,2\n") == "config line 2: duplicate ticker \"AAPL,2\"");
}

// ---------------------------------------------------------------------------
// Market / handle_startup_packet
// ---------------------------------------------------------------------------
void test_new_market()
{
    Market market;
    CHECK_EQ(market.books.size(), 0u);
    CHECK_EQ(market.locate_to_book[0], Market::NOT_TRADED);
    CHECK_EQ(market.locate_to_book[65535], Market::NOT_TRADED);
    CHECK(!market.seen_r_message);
}

void test_full_startup_sequence()
{
    Market market;
    load_three_symbols(market);
    const auto* books = market.books.data();

    // 1) before any R: a datagram that doesn't start with R is dropped whole
    std::byte early[12]{};
    early[0] = std::byte{'S'};
    CHECK(!mm::app::handle_startup_packet(market, early));
    CHECK(!market.seen_r_message);
    CHECK_EQ(market.books.size(), 0u);

    // 2) [R AAPL 14 | R ZZZZ 7]: ZZZZ isn't in the config, so it gets no book
    std::byte first[39 + 39]{};
    put_r_message(first, 14, "AAPL    ");
    put_r_message(first + 39, 7, "ZZZZ    ");
    CHECK(!mm::app::handle_startup_packet(market, first));
    CHECK(market.seen_r_message);
    CHECK_EQ(market.books.size(), 1u);
    CHECK_EQ(market.locate_to_book[14], 0u);
    CHECK_EQ(market.locate_to_book[7], Market::NOT_TRADED);

    // 3) [R MSFT 22 | A ...]: the A ends startup and everything from it on is the leftover
    std::byte last[39 + 36]{};
    put_r_message(last, 22, "MSFT    ");
    last[39] = std::byte{'A'};
    CHECK(mm::app::handle_startup_packet(market, last));

    CHECK_EQ(market.books.size(), 2u);
    CHECK_EQ(market.locate_to_book[22], 1u);
    CHECK_EQ(market.config.at(AAPL).book, 0u);
    CHECK_EQ(market.config.at(MSFT).book, 1u);
    CHECK_EQ(market.config.at(TSLA).book, Market::NOT_TRADED); // never got an R -> warned about

    CHECK(market.leftover.data() == last + 39);
    CHECK_EQ(market.leftover.size(), 36u);
    CHECK(market.leftover[0] == std::byte{'A'});

    // reserve() covered every symbol, so adding books never reallocated (no book moved)
    CHECK(market.books.data() == books);

    // the book was built from the config's prev_close: $175.23 is inside its window
    market.books[0].add(1, mm::Side::Buy, 1752300, 100);
    CHECK_EQ(market.books[0].best_bid().tick, 17523u);
    CHECK_EQ(market.books[0].best_bid().qty, 100u);
}

void test_non_r_packet_after_r_is_all_leftover()
{
    Market market;
    load_three_symbols(market);

    std::byte r_only[39]{};
    put_r_message(r_only, 14, "AAPL    ");
    CHECK(!mm::app::handle_startup_packet(market, r_only)); // whole packet was R: keep reading

    std::byte orders[19 + 19]{};
    orders[0]  = std::byte{'D'};
    orders[19] = std::byte{'D'};
    CHECK(mm::app::handle_startup_packet(market, orders));
    CHECK(market.leftover.data() == orders);
    CHECK_EQ(market.leftover.size(), sizeof(orders));
}

void test_duplicate_r_message_throws()
{
    // same locate for two tickers
    {
        Market market;
        load_three_symbols(market);
        std::byte packet[39 + 39]{};
        put_r_message(packet, 14, "AAPL    ");
        put_r_message(packet + 39, 14, "MSFT    ");
        CHECK(startup_error_message(market, packet) == "duplicate R message: MSFT     locate 14");
    }
    // same ticker on two locates
    {
        Market market;
        load_three_symbols(market);
        std::byte packet[39 + 39]{};
        put_r_message(packet, 14, "AAPL    ");
        put_r_message(packet + 39, 30, "AAPL    ");
        CHECK(startup_error_message(market, packet) == "duplicate R message: AAPL     locate 30");
    }
    // a ticker outside the config is skipped, never a duplicate
    {
        Market market;
        load_three_symbols(market);
        std::byte packet[39 + 39]{};
        put_r_message(packet, 7, "ZZZZ    ");
        put_r_message(packet + 39, 8, "ZZZZ    ");
        CHECK(startup_error_message(market, packet).empty());
    }
}

void test_truncated_r_is_dropped_and_startup_continues()
{
    Market market;
    load_three_symbols(market);

    std::byte packet[39 + 20]{};
    put_r_message(packet, 14, "AAPL    ");
    packet[39] = std::byte{'R'};
    CHECK(!mm::app::handle_startup_packet(market, packet));
    CHECK_EQ(market.books.size(), 1u);
    CHECK(market.leftover.empty());
}

} // namespace

int main()
{
    test_config_prices();
    test_load_config();
    test_load_config_rejects_bad_lines();
    test_new_market();
    test_full_startup_sequence();
    test_non_r_packet_after_r_is_all_leftover();
    test_duplicate_r_message_throws();
    test_truncated_r_is_dropped_and_startup_continues();

    if (failures == 0)
    {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
