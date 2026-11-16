#include "MyTcpServer.h"
#include <algorithm>
#include <iostream>

namespace MyNameSpace
{
    MyTcpServer::~MyTcpServer()
    {
        net::close_socket(sock_);
    }

    bool MyTcpServer::bindPort(int port, const std::string &host, int backlog)
    {
        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == net::invalid_socket)
        {
            return false;
        }

        int reuse = 1;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
            ::bind(sock_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(sock_, std::max(16, backlog)) != 0 || !setNonBlock(true))
        {
            std::cerr << "MyTcpServer bind/listen failed error=" << net::last_error() << '\n';
            return false;
        }

        return true;
    }

    bool MyTcpServer::setNonBlock(bool enabled)
    {
        return sock_ != net::invalid_socket && net::set_nonblocking(sock_, enabled);
    }

    net::Socket MyTcpServer::acceptCallBack()
    {
        return ::accept(sock_, nullptr, nullptr);
    }
}
