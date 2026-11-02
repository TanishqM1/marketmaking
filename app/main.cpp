#include "codec/itch.hpp"
#include "transport/udp_receiver.hpp"

int main()
{
    // instantiate receiver socket
    mm::transport::UDP_Receiver receiver{8080};
    bool                        m_running = true;
    // poll receieve
    while (m_running)
    {
        // TODO: change from sleep-wake threading to busy-polling
        // currently receive sleeps until it's woken up. this is slow because of the wake overhead
        // instead we want to keep polling constantly.
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