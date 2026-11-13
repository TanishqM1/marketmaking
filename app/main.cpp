#include <exception>
#include <fstream>
#include <iostream>

#include "app/startup.hpp"
#include "codec/itch.hpp"
#include "transport/udp_receiver.hpp"

// OS passed params.
// argc = 1 is a build failure (not found).
// argv is [0] = build command, [1] = symbols.csv, [2] = nullptr/end marker
int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: trading_system <config.csv>\n";
        return 1;
    }
    std::ifstream config_file{argv[1]};
    if (!config_file)
    {
        std::cerr << "cannot open config: " << argv[1] << '\n';
        return 1;
    }

    // create a new market that will hold the locate : book mapping!
    mm::app::Market market;

    // instantiate receiver socket
    mm::transport::UDP_Receiver receiver{8080};
    bool                        m_running = true;

    try
    {
        // using config load up market data, so we have locate :  book (with prev_close) setup.
        mm::app::load_config(market, config_file);
        // instantiate market structure using config + rmessages
        mm::app::run_startup(market, receiver);
    }
    catch (const std::exception& e)
    {
        std::cerr << "startup failed: " << e.what() << '\n';
        return 1;
    }
    mm::itch::decode_packet(market.leftover);

    // poll receieve
    while (m_running)
    {
        auto packet = receiver.receive();
        // error check if datagram is empty
        if (packet.empty())
        {
            continue;
        }
        // send to decode_packet
        mm::itch::decode_packet(packet);
    }
}
