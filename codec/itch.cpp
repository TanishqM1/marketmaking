#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "itch.hpp"

namespace itch
{

// helper functions to read 2, 4 and 8 bytes from our packet stream
// we need to memcpy (to read) and byteswap (for little endianness).
uint16_t read_be16(const std::byte* packet, std::size_t offset)
{
    uint16_t temp;
    std::memcpy(&temp, &packet[offset], sizeof(temp));
    return std::byteswap(temp);
}

uint32_t read_be32(const std::byte* packet, std::size_t offset)
{
    uint32_t temp;
    std::memcpy(&temp, &packet[offset], sizeof(temp));
    return std::byteswap(temp);
}

uint64_t read_be64(const std::byte* packet, std::size_t offset)
{
    uint64_t temp;
    std::memcpy(&temp, &packet[offset], sizeof(temp));
    return std::byteswap(temp);
}

// "span" is a view over a contigous seuqence of objects it holds a pointer to the start of some
// data, and a size. in our case, this is the start of the buffer, and the len of the packet.

// we know that for our incoming data, the message header will have:

// 0   msg type        1 (char)
// 1   stock locate    2 byte integer
// 3   tracking num    2 byte integer
// 5   timestamp       6 byte nanoseconds since midnight.

void decode_packet(std::span<const std::byte> packet)
{

    // general flow:
    // 1) parse message header. bail if it's > the end
    // 2) get message type. based on that, decode the message!
    // 3) use jump table to jump to the correct parsing logic
    // 4) parse it into variables using memcpy + bswap
    // now it's in the variable, as little endianness
    // 5) call our market state function with the information
    // 6) use std::span's subspan feature to shrink the size of
    // the area we are considering (subspan(start_packet, range))
    // i.e packet = packet.subspan(offset of next starting message, new size)
    // subspan also reduces the size for us!

    std::size_t offset = 0;

    while (offset < packet.size())
    {
        // PARSE HEADER

        // read message type (dont need memcpy, can directly read!)
        auto type =
            static_cast<itch::MsgType>(packet.data()[offset + itch::MessageHeader::msg_type]);
        std::uint16_t stock_locate =
            read_be16(packet.data(), offset + itch::MessageHeader::stock_locate);
    }
}
}; // namespace itch