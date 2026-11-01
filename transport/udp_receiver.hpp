#pragma once

#include <cstddef> // std::byte (raw bytes), std::size_t (BUFFER_SIZE)
#include <cstdint> // std::uint16_t (port number)
#include <span>    // std::span -- receive()'s return type, {ptr, size}

namespace mm::transport
{
class UDP_Receiver
{
  public:
    // constructor outline
    // we need 'explicit' so we can stop any implicit conversion. (for safety).
    explicit UDP_Receiver(std::uint16_t port);
    // destructor outline
    ~UDP_Receiver();
    // outline to prevent duplication!!!
    UDP_Receiver(const UDP_Receiver&)            = delete;
    UDP_Receiver& operator=(const UDP_Receiver&) = delete;

    // here, [[nodiscard]] makes sure we are using this function the correct way.
    // for example:
    // receiver.receive();              // warning: ignoring return value of 'receive'
    // auto pkt = receiver.receive();   // fine
    // it ensures we resolve the function call to an actual veraible, and we don't just discard it!.
    [[nodiscard]] ssize_t receive();

  private:
    //   outlining the constants in the file.
    // we need buffersize (to avoid truncation of data!)
    // we need to default to -1 for socket descriptor (error case)
    // we want to align the buffer correctly in memory.
    static constexpr std::size_t BUFFER_SIZE = 65536;
    int                          m_sockfd{-1};
    alignas(64) uint8_t m_buffer[BUFFER_SIZE];
};
} // namespace mm::transport