#pragma once
// Portable TCP primitives used by the network-only server and benchmark client.
// No System V shared memory, trading model, or external runtime dependency.
#include <cstddef>
#include <cstdint>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace net
{
#ifdef _WIN32
    using Socket = SOCKET;
    using SocketLength = int;
    constexpr Socket invalid_socket = INVALID_SOCKET;
#else
    using Socket = int;
    using SocketLength = socklen_t;
    constexpr Socket invalid_socket = -1;
#endif
    constexpr std::uint32_t max_payload = 64 * 1024;
    constexpr std::size_t max_pending_bytes = 2 * 1024 * 1024;

    struct Runtime
    {
        public:
            Runtime();
            ~Runtime();

            bool ok() const
            {
                return ready_;
            }

        private:
            bool ready_;
    };

    bool set_nonblocking(Socket fd, bool enabled);
    bool set_timeout(Socket fd, int milliseconds);
    void close_socket(Socket fd);
    int last_error();
    bool would_block(int error);
    bool interrupted(int error);
    bool peer_disconnect_error(int error);
    std::string error_message(int error);
    bool send_all(Socket fd, const char *data, std::size_t size);
    bool recv_all(Socket fd, char *data, std::size_t size);
    std::uint64_t monotonic_ns();
    std::string process_metrics(); // CPU seconds and RSS/peak RSS
}
