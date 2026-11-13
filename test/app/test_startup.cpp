// Tests for app/startup.*
//
// Everything except run_startup() itself, which only adds the socket loop around
// on_startup_datagram(). Datagrams are hand-built byte arrays, no network.

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

using mm::app::Config;
using mm::app::NOT_TRADED;
using mm::app::StartupState;
using mm::app::Ticker;

constexpr Ticker AAPL{'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
constexpr Ticker MSFT{'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '};
constexpr Ticker TSLA{'T', 'S', 'L', 'A', ' ', ' ', ' ', ' '};

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

Config three_symbols()
{
    std::istringstream in{"AAPL,175.23\nMSFT,410.15\nTSLA,250\n"};
    return mm::app::parse_config(in);
}

bool config_throws(const std::string& text)
{
    std::istringstream in{text};
    try
    {
        mm::app::parse_config(in);
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// parse_price: dollars text -> ITCH integer units (4 implied decimals). No float anywhere.
// ---------------------------------------------------------------------------
void test_parse_price()
{
    CHECK_EQ(mm::app::parse_price("175.23").value_or(0), 1752300u);
    CHECK_EQ(mm::app::parse_price("175").value_or(0), 1750000u);
    CHECK_EQ(mm::app::parse_price("175.2").value_or(0), 1752000u);
    CHECK_EQ(mm::app::parse_price("175.2500").value_or(0), 1752500u);
    CHECK_EQ(mm::app::parse_price("0.0001").value_or(0), 1u);
    CHECK_EQ(mm::app::parse_price("0").value_or(1), 0u);
    CHECK_EQ(mm::app::parse_price("429496.7295").value_or(0), 4294967295u); // UINT32_MAX

    CHECK(!mm::app::parse_price("429496.7296")); // one past UINT32_MAX
    CHECK(!mm::app::parse_price("429497"));
    CHECK(!mm::app::parse_price("99999999999999999999")); // overflows uint64 in from_chars
    CHECK(!mm::app::parse_price("175.23456"));            // a 5th decimal would be truncated
    CHECK(!mm::app::parse_price(""));
    CHECK(!mm::app::parse_price("."));
    CHECK(!mm::app::parse_price("175."));
    CHECK(!mm::app::parse_price(".5"));
    CHECK(!mm::app::parse_price("-1"));
    CHECK(!mm::app::parse_price("+1"));
    CHECK(!mm::app::parse_price("1.-5"));
    CHECK(!mm::app::parse_price("abc"));
    CHECK(!mm::app::parse_price("1.2.3"));
    CHECK(!mm::app::parse_price("1e3"));
}

// ---------------------------------------------------------------------------
// parse_config
// ---------------------------------------------------------------------------
void test_parse_config()
{
    std::istringstream in{"# ticker,prev_close\n\n  AAPL , 175.23 \r\nMSFT,410.15\nNVDALONG,1\n"};
    Config             config = mm::app::parse_config(in);

    CHECK_EQ(config.size(), 3u);
    CHECK_EQ(config.at(AAPL).prev_close, 1752300u); // padded to 8, so it matches the wire
    CHECK_EQ(config.at(MSFT).prev_close, 4101500u);
    CHECK_EQ(config.at(AAPL).book, NOT_TRADED);
    CHECK(config.contains(Ticker{'N', 'V', 'D', 'A', 'L', 'O', 'N', 'G'})); // exactly 8 fits
}

void test_parse_config_rejects_bad_lines()
{
    CHECK(config_throws("AAPL 175.23\n"));         // no comma
    CHECK(config_throws("TOOLONGXX,1\n"));         // 9-char ticker
    CHECK(config_throws(",1\n"));                  // empty ticker
    CHECK(config_throws("AAPL,\n"));               // empty price
    CHECK(config_throws("AAPL,abc\n"));            // not a number
    CHECK(config_throws("AAPL,1\nAAPL,2\n"));      // duplicate
    CHECK(!config_throws("# only a comment\n\n")); // empty config is allowed
}

// ---------------------------------------------------------------------------
// begin_startup / on_startup_datagram
// ---------------------------------------------------------------------------
void test_begin_startup()
{
    StartupState state = mm::app::begin_startup(three_symbols());
    CHECK_EQ(state.market.books.size(), 0u);
    CHECK(state.market.books.capacity() >= 3);
    CHECK_EQ(state.market.locate_to_book[0], NOT_TRADED);
    CHECK_EQ(state.market.locate_to_book[65535], NOT_TRADED);
    CHECK(!state.seen_r);
}

void test_full_startup_sequence()
{
    StartupState state = mm::app::begin_startup(three_symbols());
    const auto*  books = state.market.books.data();

    // 1) before any R: a datagram that doesn't start with R is dropped whole
    std::byte early[12]{};
    early[0] = std::byte{'S'};
    CHECK(!mm::app::on_startup_datagram(state, early));
    CHECK(!state.seen_r);
    CHECK_EQ(state.market.books.size(), 0u);

    // 2) [R AAPL 14 | R ZZZZ 7]: ZZZZ isn't in the config, so it gets no book
    std::byte first[39 + 39]{};
    put_r_message(first, 14, "AAPL    ");
    put_r_message(first + 39, 7, "ZZZZ    ");
    CHECK(!mm::app::on_startup_datagram(state, first));
    CHECK(state.seen_r);
    CHECK_EQ(state.market.books.size(), 1u);
    CHECK_EQ(state.market.locate_to_book[14], 0u);
    CHECK_EQ(state.market.locate_to_book[7], NOT_TRADED);

    // 3) [R MSFT 22 | R AAPL 30 | A ...]: AAPL already has a book, so locate 30 is ignored;
    //    the A ends startup and everything from it on is the leftover
    std::byte last[39 + 39 + 36]{};
    put_r_message(last, 22, "MSFT    ");
    put_r_message(last + 39, 30, "AAPL    ");
    last[78] = std::byte{'A'};
    CHECK(mm::app::on_startup_datagram(state, last));

    CHECK_EQ(state.market.books.size(), 2u);
    CHECK_EQ(state.market.locate_to_book[22], 1u);
    CHECK_EQ(state.market.locate_to_book[30], NOT_TRADED);
    CHECK(state.market.tickers.at(0) == AAPL);
    CHECK(state.market.tickers.at(1) == MSFT);
    CHECK_EQ(state.config.at(AAPL).book, 0u);
    CHECK_EQ(state.config.at(MSFT).book, 1u);
    CHECK_EQ(state.config.at(TSLA).book, NOT_TRADED); // never got an R -> warned about

    CHECK(state.market.leftover.data() == last + 78);
    CHECK_EQ(state.market.leftover.size(), 36u);
    CHECK(state.market.leftover[0] == std::byte{'A'});

    // reserve() covered every symbol, so binding never reallocated (no book moved)
    CHECK(state.market.books.data() == books);

    // the book was built from the config's prev_close: $175.23 is inside its window
    state.market.books[0].add(1, mm::Side::Buy, 1752300, 100);
    CHECK_EQ(state.market.books[0].best_bid().tick, 17523u);
    CHECK_EQ(state.market.books[0].best_bid().qty, 100u);
}

void test_non_r_datagram_after_r_is_all_leftover()
{
    StartupState state = mm::app::begin_startup(three_symbols());

    std::byte r_only[39]{};
    put_r_message(r_only, 14, "AAPL    ");
    CHECK(!mm::app::on_startup_datagram(state, r_only)); // whole datagram was R: keep reading

    std::byte orders[19 + 19]{};
    orders[0]  = std::byte{'D'};
    orders[19] = std::byte{'D'};
    CHECK(mm::app::on_startup_datagram(state, orders));
    CHECK(state.market.leftover.data() == orders);
    CHECK_EQ(state.market.leftover.size(), sizeof(orders));
}

void test_duplicate_locate_is_ignored()
{
    StartupState state = mm::app::begin_startup(three_symbols());

    std::byte datagram[39 + 39]{};
    put_r_message(datagram, 14, "AAPL    ");
    put_r_message(datagram + 39, 14, "MSFT    ");
    CHECK(!mm::app::on_startup_datagram(state, datagram));

    CHECK_EQ(state.market.books.size(), 1u);
    CHECK_EQ(state.market.locate_to_book[14], 0u); // first R wins
    CHECK_EQ(state.config.at(MSFT).book, NOT_TRADED);
}

void test_truncated_r_is_dropped_and_startup_continues()
{
    StartupState state = mm::app::begin_startup(three_symbols());

    std::byte datagram[39 + 20]{};
    put_r_message(datagram, 14, "AAPL    ");
    datagram[39] = std::byte{'R'};
    CHECK(!mm::app::on_startup_datagram(state, datagram));
    CHECK_EQ(state.market.books.size(), 1u);
    CHECK(state.market.leftover.empty());
}

} // namespace

int main()
{
    test_parse_price();
    test_parse_config();
    test_parse_config_rejects_bad_lines();
    test_begin_startup();
    test_full_startup_sequence();
    test_non_r_datagram_after_r_is_all_leftover();
    test_duplicate_locate_is_ignored();
    test_truncated_r_is_dropped_and_startup_continues();

    if (failures == 0)
    {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
