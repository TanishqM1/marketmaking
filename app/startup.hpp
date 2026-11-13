#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <map>
#include <span>
#include <vector>

#include "state/book.hpp"
#include "transport/udp_receiver.hpp"

namespace mm::app
{
// struct to map book : prev_close (center tick!)
struct SymbolConfig
{
    std::uint32_t prev_close;
    std::uint16_t book;
};

// the market consists of locates, when a locate has NO book, it defualts to the 'NOT_TRADED' value.
struct Market
{
    // default value for "no book" and LOCATES is max # of locate values.
    static constexpr std::uint16_t NOT_TRADED = 0xFFFF;
    static constexpr std::size_t   LOCATES    = 65536;

    Market() { locate_to_book.fill(NOT_TRADED); }

    // Ticker : {prev_close, book}
    std::map<std::array<char, 8>, SymbolConfig> config;

    std::vector<state::Book> books;
    state::OrderMap          orders;
    // this instantiates an array where each element is a 2byte integer. The array holds "LOCATES"
    // elements.
    std::array<std::uint16_t, LOCATES> locate_to_book;
    std::span<const std::byte>
        leftover; // the leftover messages from teh datagram with the lat of the RMessages. The rest
                  // of the datagram contains real market trades.

    // flag. If true, that means we have processed the RMessages (turns on when the first RMessage
    // is read. before that we consider anything else just noise.s)
    bool seen_r_message = false;
};

// 1) load symbols.csv file, that denotes ticket : prev_close price.
// 2) parse config file (trim lines, get ticke, get price->tick, map relations)
// 3) at the end when everything is parsed, created a space-reserved vector with everything.
void load_config(Market& market, std::istream& in);

// start RMesssage-reading loop! recieve packet, parse and check if done
void run_startup(Market& market, transport::UDP_Receiver& receiver);
// parse packet, add book to the market (using the pre-loaded config we read & setup!)
// return true/false if we are done w/ RMessage. Then continue to normal flow.
bool handle_startup_packet(Market& market, std::span<const std::byte> packet);

} // namespace mm::app
