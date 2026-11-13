// in this file we need to

// 1) load our config, store ticker : prev_close
// 2) read all our "R Messages"
// 3) create books for all of them
// 4) make a locate_to_book structure.

// TODO: parse and update rest of functions in this file.
// how tf is handle_startup_packet working ???

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "app/startup.hpp"
#include "codec/itch.hpp"

namespace mm::app
{
namespace
{
// ITCH Price scale
// so $175.23 = 1752300
constexpr std::uint64_t ITCH_UNITS_PER_DOLLAR = 10000;

// read price ,
bool parse_digits(std::string_view text, std::uint64_t& value)
{
    if (text.empty())
    {
        return false;
    }
    const char*            end    = text.data() + text.size();
    std::from_chars_result result = std::from_chars(text.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

// error helper function (golang style lol)
std::runtime_error config_line_error(std::size_t line_number, const char* reason,
                                     std::string_view line)
{
    return std::runtime_error("config line " + std::to_string(line_number) + ": " + reason + " \"" +
                              std::string(line) + "\"");
}

void bind_r_message(Market& market, const itch::RMessageFields& r_message)
{
    // find corresponding book to ticker from our preloaded config
    auto symbol = market.config.find(r_message.stock_ticker);
    // OOB
    if (symbol == market.config.end())
    {
        return;
    }
    // book already exists
    // already taken pt.2
    if ((*symbol).second.book != Market::NOT_TRADED ||
        market.locate_to_book[r_message.stock_locate] != Market::NOT_TRADED)
    {
        std::string ticker{r_message.stock_ticker.data(), r_message.stock_ticker.size()};
        throw std::runtime_error("duplicate R message: " + ticker + " locate " +
                                 std::to_string(r_message.stock_locate));
    }

    std::uint16_t book = static_cast<std::uint16_t>(market.books.size());
    market.books.emplace_back((*symbol).second.prev_close);
    market.locate_to_book[r_message.stock_locate] = book;
    (*symbol).second.book                         = book;
}

// helper function to see what tickers are unused/not reported by exchange!
void report_missing_tickers(const Market& market)
{
    for (const auto& entry : market.config)
    {
        if (entry.second.book == Market::NOT_TRADED)
        {
            std::string_view ticker{entry.first.data(), entry.first.size()};
            std::cerr << "startup: no R message for " << ticker << '\n';
        }
    }
}
} // namespace

void load_config(Market& market, std::istream& in)
{
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(in, line))
    {
        line_number++;
        std::string_view text = line;
        if (text.empty() || text.front() == '#')
        {
            continue;
        }

        std::size_t comma = text.find(',');
        if (comma == std::string_view::npos)
        {
            throw config_line_error(line_number, "bad entry", line);
        }
        std::string_view ticker_text = text.substr(0, comma);
        std::string_view price_text  = text.substr(comma + 1);

        std::array<char, 8> ticker;
        if (ticker_text.empty() || ticker_text.size() > ticker.size() ||
            ticker_text.find_first_of(" \t\r") != std::string_view::npos)
        {
            throw config_line_error(line_number, "bad entry", line);
        }
        ticker.fill(' ');
        std::memcpy(ticker.data(), ticker_text.data(), ticker_text.size());

        if (market.config.contains(ticker))
        {
            throw config_line_error(line_number, "duplicate ticker", line);
        }

        std::size_t   dot      = price_text.find('.');
        std::uint64_t dollars  = 0;
        std::uint64_t decimals = 0;
        if (!parse_digits(price_text.substr(0, dot), dollars))
        {
            throw config_line_error(line_number, "bad entry", line);
        }
        if (dot != std::string_view::npos)
        {
            std::string_view decimals_text = price_text.substr(dot + 1);
            if (decimals_text.size() > 4 || !parse_digits(decimals_text, decimals))
            {
                throw config_line_error(line_number, "bad entry", line);
            }
            for (std::size_t i = decimals_text.size(); i < 4; i++)
            {
                decimals *= 10;
            }
        }
        if (dollars > UINT32_MAX / ITCH_UNITS_PER_DOLLAR ||
            dollars * ITCH_UNITS_PER_DOLLAR + decimals > UINT32_MAX)
        {
            throw config_line_error(line_number, "bad entry", line);
        }

        std::uint32_t prev_close =
            static_cast<std::uint32_t>(dollars * ITCH_UNITS_PER_DOLLAR + decimals);
        market.config[ticker] = SymbolConfig{prev_close, Market::NOT_TRADED};
    }

    if (market.config.size() >= Market::NOT_TRADED)
    {
        throw std::runtime_error("config has more symbols than locate_to_book can index");
    }
    market.books.reserve(market.config.size());
}

bool handle_startup_packet(Market& market, std::span<const std::byte> packet)
{
    std::size_t          offset = 0;
    itch::RMessageFields r_message{};
    while (itch::next_r_message(packet, offset, r_message))
    {
        bind_r_message(market, r_message);
    }

    if (offset == 0 && !market.seen_r_message)
    {
        return false;
    }
    market.seen_r_message = true;

    if (offset == packet.size())
    {
        return false;
    }
    market.leftover = packet.subspan(offset);
    return true;
}

void run_startup(Market& market, transport::UDP_Receiver& receiver)
{
    bool done = false;
    while (!done)
    {
        std::span<const std::byte> packet = receiver.receive();
        if (packet.empty())
        {
            continue;
        }
        done = handle_startup_packet(market, packet);
    }
    report_missing_tickers(market);
}
} // namespace mm::app
