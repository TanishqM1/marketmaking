#include <cstddef>      // Standard definitions (size_t)
#include <cstdint>      // Fixed-width integer types (uint8_t, uint16_t)
#include <cstring>      // For strerror() error text conversion
#include <iostream>     // For std::cout and std::cerr diagnostics
#include <netinet/in.h> // Internet domain structures (sockaddr_in, INADDR_ANY, htons)
#include <stdexcept>    // Standard exception classes (std::runtime_error)
#include <sys/socket.h> // Core POSIX socket mechanics (socket, bind, recvfrom, setsockopt)
#include <unistd.h>     // Standard symbolic constants (close() file descriptor removal)

class UDP_Receiver
{
    // member fields (private) representing internal state of "UDP_Receiver".
  private:
    int  m_sockfd{-1}; // -1 = no socket. a valid fd is >= 0 (0 is legal), so check < 0 for error.
    bool m_running                      = false; // running flag.
    static constexpr size_t BUFFER_SIZE = 2048;
    // cache-line align the buffer. the compiler would NOT do this on its own -- default
    // alignment for uint8_t[] is 1. keeps the first message off a split line.
    alignas(64) uint8_t m_buffer[BUFFER_SIZE];

  public:
    //   constructor (using RAII)
    // creating the endpoint, using IPv4, with DATAGRAM-based sockets (what we need for ITCH/UDP)
    // socket() asks the kernel for a new endpoint object (NOT a network interface -- that's the
    // NIC). the kernel allocates its own bookkeeping plus a receive queue, and hands back a file
    // descriptor referring to it.
    UDP_Receiver(uint16_t port)
    // Step 1: Create an IPv4 (AF_INET), Datagram/UDP (SOCK_DGRAM) socket endpoint.
    {
        m_sockfd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sockfd < 0)
        {
            perror("Socket instantiation failed!"); // print error
        }

        // Step 2: grow the KERNEL's socket receive queue (not the NIC ring, and not m_buffer).
        // this does NOT reduce latency -- it prevents DROPS. if a burst arrives while we're busy
        // decoding, packets queue here instead of being discarded.
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

        // the cast is because bind() takes a generic sockaddr* -- one signature for every address
        // family (IPv4, IPv6, unix). bind copies what it needs; we don't keep this struct.
        if (::bind(m_sockfd, reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)) <
            0)
        {
            ::close(m_sockfd);
            throw std::runtime_error(
                "binding exception, port might be already bound, or restricted.");
        }
    }

    // destructor: RAII. releases the fd when the object goes out of scope, on every path
    // including an exception.
    // TODO: currently commented out, so the fd LEAKS.

    ~UDP_Receiver()
    {
        stop();
        if (m_sockfd >= 0)
        {
            ::close(m_sockfd);
        }
    }

    // we cannot allow copy semantics on network sockets. this is because when we derefence a
    // socket, we delete. however if there are *two* references to it, and we try to delete it the
    // second time, we run into a crash.

    // so let's explicitly tell the compiler, to error if a duplicate of the object is attempted to
    // be created.
    UDP_Receiver(const UDP_Receiver&)            = delete;
    UDP_Receiver& operator=(const UDP_Receiver&) = delete;

    // explicit flag to stop the hot-path execution completely.
    void stop() { m_running = false; };

    // HOT-PATH EXECUTION LOOP. finally the part that actually matters.
    // if we u
};