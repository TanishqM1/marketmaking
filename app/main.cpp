#include "codec/itch.hpp"
#include "transport/udp_receiver.hpp"

int main()
{
    // instantiate receiver socket
    mm::transport::UDP_Receiver receiver{8080};
    bool                        m_running = true;
    // while loop to parse R messages
    while (true)
    {
        auto packet = receiver.receive();
        if (packet.empty())
        {
            continue;
        }
        // decode, if not R message, then change the address of first_packet.
    }
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