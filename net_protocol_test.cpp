#include "net_core.h"
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    int g_test_port = 50114;

    net::Socket connect_local()
    {
        net::Socket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == net::invalid_socket)
        {
            return fd;
        }

        net::set_timeout(fd, 2500);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(g_test_port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            net::close_socket(fd);
            return net::invalid_socket;
        }

        return fd;
    }

    std::vector<char> frame(const std::string &payload)
    {
        std::vector<char> data(payload.size() + 4);
        std::uint32_t bytes = htonl(static_cast<std::uint32_t>(payload.size()));
        std::memcpy(data.data(), &bytes, 4);
        std::memcpy(data.data() + 4, payload.data(), payload.size());
        return data;
    }

    bool echo_equals(net::Socket sock, const std::string &expected)
    {
        char wire[4];
        if (!net::recv_all(sock, wire, 4))
        {
            return false;
        }

        std::uint32_t len = 0;
        std::memcpy(&len, wire, 4);
        len = ntohl(len);
        if (len != expected.size())
        {
            return false;
        }

        std::vector<char> reply(len);
        return net::recv_all(sock, reply.data(), len) && std::memcmp(reply.data(), expected.data(), len) == 0;
    }

    bool fragmentation()
    {
        net::Socket fd = connect_local();
        if (fd == net::invalid_socket)
        {
            return false;
        }

        auto data = frame("fragmented-tcp-payload");
        bool ok = net::send_all(fd, data.data(), 1) && net::send_all(fd, data.data() + 1, 2) &&
                  net::send_all(fd, data.data() + 3, 4) && net::send_all(fd, data.data() + 7, data.size() - 7) &&
                  echo_equals(fd, "fragmented-tcp-payload");
        net::close_socket(fd);
        return ok;
    }

    bool coalescing()
    {
        net::Socket fd = connect_local();
        if (fd == net::invalid_socket)
        {
            return false;
        }

        auto a = frame(std::string(16, 'a')), b = frame(std::string(384, 'b'));
        a.insert(a.end(), b.begin(), b.end());
        bool ok = net::send_all(fd, a.data(), a.size()) && echo_equals(fd, std::string(16, 'a')) &&
                  echo_equals(fd, std::string(384, 'b'));
        net::close_socket(fd);
        return ok;
    }

    bool max_size()
    {
        net::Socket fd = connect_local();
        if (fd == net::invalid_socket)
        {
            return false;
        }

        std::string body(net::max_payload, 'x');
        auto data = frame(body);
        bool ok = net::send_all(fd, data.data(), data.size()) && echo_equals(fd, body);
        net::close_socket(fd);
        return ok;
    }

    bool bad_size(std::uint32_t len)
    {
        net::Socket fd = connect_local();
        if (fd == net::invalid_socket)
        {
            return false;
        }

        std::uint32_t wire = htonl(len);
        bool sent = net::send_all(fd, reinterpret_cast<const char *>(&wire), 4);
        char byte = 0;
        int count = ::recv(fd, &byte, 1, 0);
        bool ok = sent && count == 0; // invalid frames must cause peer close without echo
        net::close_socket(fd);
        return ok;
    }

    bool half_close()
    {
        net::Socket fd = connect_local();
        if (fd == net::invalid_socket)
        {
            return false;
        }

        auto data = frame("half-close");
        bool ok = net::send_all(fd, data.data(), data.size());
#ifdef _WIN32
        ok = ok && ::shutdown(fd, SD_SEND) == 0;
#else
        ok = ok && ::shutdown(fd, SHUT_WR) == 0;
#endif
        ok = ok && echo_equals(fd, "half-close");
        char byte = 0;
        if (ok)
        {
            ok = (::recv(fd, &byte, 1, 0) == 0);
        }

        net::close_socket(fd);
        return ok;
    }
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--port")
    {
        g_test_port = std::atoi(argv[2]);
    }
    else if (argc != 1)
    {
        std::cerr << "usage: net_protocol_test [--port 50114]\n";
        return 2;
    }

    if (g_test_port < 1 || g_test_port > 65535)
    {
        return 2;
    }

    net::Runtime rt;
    if (!rt.ok())
    {
        return 2;
    }

    struct Test
    {
        public:
            const char *name;
            bool (*run)();
    };

    const Test cases[] = {{"fragmentation", fragmentation},
                          {"coalescing", coalescing},
                          {"64KiB_max_payload", max_size},
                          {"half_close", half_close}};
    int passed = 0, total = 0;
    for (const auto &tc : cases)
    {
        ++total;
        bool ok = tc.run();
        std::cout << (ok ? "PASS " : "FAIL ") << tc.name << '\n';
        if (ok)
        {
            ++passed;
        }
    }

    for (std::uint32_t len : {0U, net::max_payload + 1U})
    {
        ++total;
        bool ok = bad_size(len);
        std::cout << (ok ? "PASS " : "FAIL ") << "invalid_length_" << len << '\n';
        if (ok)
        {
            ++passed;
        }
    }

    std::cout << "PROTOCOL_TESTS passed=" << passed << " total=" << total << '\n';
    return passed == total ? 0 : 1;
}
