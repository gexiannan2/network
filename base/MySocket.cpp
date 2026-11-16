#include "MySocket.h"
#include <algorithm>
#include <cstring>

namespace MyNameSpace
{
    MySocket::MySocket(net::Socket sock) : sock_(sock), last_activity_ns_(net::monotonic_ns())
    {
    }

    MySocket::~MySocket()
    {
        net::close_socket(sock_);
    }

    void MySocket::closeSocket()
    {
        const net::Socket fd = sock_;
        sock_ = net::invalid_socket;
        net::close_socket(fd);
    }

    bool MySocket::appendReceived(const char *data, std::size_t size)
    {
        if (data == nullptr || input_.size() > net::max_pending_bytes || size > net::max_pending_bytes - input_.size())
        {
            return false;
        }

        input_.insert(input_.end(), data, data + size);
        last_activity_ns_ = net::monotonic_ns();
        return true;
    }

    std::vector<char> MySocket::copyPendingOutput(std::size_t max_size) const
    {
        const std::size_t bytes = std::min(pendingBytes(), max_size);
        return std::vector<char>(out_.begin() + static_cast<std::ptrdiff_t>(out_offset_),
                                 out_.begin() + static_cast<std::ptrdiff_t>(out_offset_ + bytes));
    }

    void MySocket::consumePendingOutput(std::size_t size)
    {
        if (size > pendingBytes())
        {
            size = pendingBytes();
        }

        out_offset_ += size;
        last_activity_ns_ = net::monotonic_ns();
        if (out_offset_ == out_.size())
        {
            out_.clear();
            out_offset_ = 0;
        }
    }

    int MySocket::rcvBuffer()
    {
        char chunk[16384];
        int total = 0;
        for (;;)
        {
            int n = ::recv(sock_, chunk, static_cast<int>(sizeof(chunk)), 0);
            if (n > 0)
            {
                if (input_.size() + static_cast<std::size_t>(n) > net::max_pending_bytes)
                {
                    return -1;
                }

                input_.insert(input_.end(), chunk, chunk + n);
                total += n;
                last_activity_ns_ = net::monotonic_ns();
                continue;
            }

            if (n == 0)
            {
                closed_ = true;
                return total;
            }

            int err = net::last_error();
            if (net::interrupted(err))
            {
                continue;
            }

            if (net::would_block(err))
            {
                return total;
            }

            return -1;
        }
    }

    int MySocket::readBuffer(std::vector<char> &payload)
    {
        if (input_.size() < 4)
        {
            return 0;
        }

        std::uint32_t network_size = 0;
        std::memcpy(&network_size, input_.data(), 4);
        std::uint32_t len = ntohl(network_size);
        if (len == 0 || len > net::max_payload)
        {
            return -1;
        }

        if (input_.size() < static_cast<std::size_t>(len) + 4)
        {
            return 0;
        }

        payload.assign(input_.begin() + 4, input_.begin() + 4 + len);
        input_.erase(input_.begin(), input_.begin() + 4 + len);
        return static_cast<int>(len);
    }

    int MySocket::sendDataWithBuffer(const char *data, std::uint32_t len)
    {
        if (len == 0 || len > net::max_payload || !data)
        {
            return -1;
        }

        const std::size_t frame = static_cast<std::size_t>(len) + 4;
        if (pendingBytes() + frame > net::max_pending_bytes)
        {
            return -1;
        }

        if (out_offset_ && out_offset_ * 2 >= out_.size())
        {
            out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(out_offset_));
            out_offset_ = 0;
        }

        std::uint32_t wire = htonl(len);
        const char *head = reinterpret_cast<const char *>(&wire);
        out_.insert(out_.end(), head, head + 4);
        out_.insert(out_.end(), data, data + len);
        return static_cast<int>(frame);
    }

    int MySocket::syncSendBuf()
    {
        int flushed = 0;
        while (out_offset_ < out_.size())
        {
            const int ask = static_cast<int>(std::min<std::size_t>(out_.size() - out_offset_, 1 << 20));
#ifdef MSG_NOSIGNAL
            int n = ::send(sock_, out_.data() + out_offset_, ask, MSG_NOSIGNAL);
#else
            int n = ::send(sock_, out_.data() + out_offset_, ask, 0);
#endif
            if (n > 0)
            {
                out_offset_ += n;
                flushed += n;
                last_activity_ns_ = net::monotonic_ns();
                continue;
            }

            if (n < 0 && net::interrupted(net::last_error()))
            {
                continue;
            }

            if (n < 0 && net::would_block(net::last_error()))
            {
                return flushed;
            }

            return -1;
        }

        out_.clear();
        out_offset_ = 0;
        return flushed;
    }

    std::uint16_t MySocket::getRemotePort() const
    {
        sockaddr_in addr{};
        net::SocketLength size = sizeof(addr);
        if (::getpeername(sock_, reinterpret_cast<sockaddr *>(&addr), &size) != 0)
        {
            return 0;
        }

        return ntohs(addr.sin_port);
    }

    std::string MySocket::getRemoteIp() const
    {
        sockaddr_in addr{};
        net::SocketLength size = sizeof(addr);
        char text[INET_ADDRSTRLEN] = {};
        if (::getpeername(sock_, reinterpret_cast<sockaddr *>(&addr), &size) != 0)
        {
            return {};
        }

        return inet_ntop(AF_INET, &addr.sin_addr, text, sizeof(text)) ? text : "";
    }
}
