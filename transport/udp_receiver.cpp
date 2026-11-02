#include <arpa/inet.h> // htons() -- host to network byte order (big-endian)
#include <cerrno>
#include <cstdint>      // std::uint8_t, std::uint16_t
#include <iostream>     // std::cerr -- startup diagnostics ONLY, never the hot path
#include <netinet/in.h> // sockaddr_in, INADDR_ANY (IPv4 addressing structs)
#include <stdexcept> // std::runtime_error -- thrown on bind failure (startup, so a throw is fine)
#include <sys/socket.h> // socket(), bind(), setsockopt(), recvfrom(), SOL_SOCKET, SO_RCVBUF
#include <unistd.h>     // close() -- POSIX, releases the fd in the destructor

#include "udp_receiver.hpp"

// perror() comes from <cstdio>, currently pulled in transitively by <iostream>
namespace mm::transport
{

//   constructor (using RAII)
// creating the endpoint, using IPv4, with DATAGRAM-based sockets (what we need for
// ITCH/UDP) socket() asks the kernel for a new endpoint object (NOT a network interface --
// that's the NIC). the kernel allocates its own bookkeeping plus a receive queue, and hands
// back a file descriptor referring to it.
UDP_Receiver::UDP_Receiver(std::uint16_t port)
// Step 1: Create an IPv4 (AF_INET), Datagram/UDP (SOCK_DGRAM) socket endpoint.
{
    m_sockfd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sockfd < 0)
    {
        perror("Socket instantiation failed!"); // print error
    }

    // Step 2: grow the KERNEL's socket receive queue (not the NIC ring, and not m_buffer).
    // this does NOT reduce latency -- it prevents DROPS. if a burst arrives while we're
    // busy decoding, packets queue here instead of being discarded.
    int receive_buffer_bytes = 2 * 1024 * 1024; // 2 MB
    if (::setsockopt(m_sockfd, SOL_SOCKET, SO_RCVBUF, &receive_buffer_bytes,
                     sizeof(receive_buffer_bytes)) < 0)
    {
        std::cerr << "failed to expand buffer size. using default size";
    }

    // Step 3: Initializing server-address (where the input data must arrive at)
    sockaddr_in serverAddr{};
    // sin_family tells bind() how to interpret this struct (which address family it is).
    serverAddr.sin_family = AF_INET;
    // htons -> network byte order, which is big-endian. same concept as the ITCH byteswap.
    serverAddr.sin_port = htons(port);
    // allow the OS to accept incoming data from ANY network card for this endpoint/socket.
    // (allow any NIC!)
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    // the cast is because bind() takes a generic sockaddr* -- one signature for every
    // address family (IPv4, IPv6, unix). bind copies what it needs; we don't keep this
    // struct.
    if (::bind(m_sockfd, reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0)
    {
        ::close(m_sockfd);
        throw std::runtime_error("binding exception, port might be already bound, or restricted.");
    }
}

// recvfrom params: recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr
// *src_addr, socklen_t *addrlen);
std::span<const std::byte> UDP_Receiver::receive()
{
    // call recvfrom, then return the span (packet address, size) pair.
    ssize_t return_code = recvfrom(m_sockfd, m_buffer, BUFFER_SIZE, 0, nullptr, nullptr);
    if (return_code < 0)
    {
        // TODO: handle errors! currently it flows through and returns an empty span.
    }
    if (return_code > 0)
    {
        return {m_buffer, static_cast<std::size_t>(return_code)};
    }
    // empty datagram!
    return {};
}

// destructor: RAII. releases the fd when the object goes out of scope, on every path
// including an exception.
UDP_Receiver::~UDP_Receiver()
{
    if (m_sockfd >= 0)
    {
        ::close(m_sockfd);
    }
}

// HOT-PATH EXECUTION LOOP. finally the part that actually matters.
// if we u

} // namespace mm::transport
