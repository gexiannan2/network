#include "net_core.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    bool positive(const char *s, int min, int max, int &out)
    {
        char *end = nullptr;
        long x = std::strtol(s, &end, 10);
        if (!*s || !end || *end || x < min || x > max)
        {
            return false;
        }

        out = static_cast<int>(x);
        return true;
    }

    struct Args
    {
        public:
            int port = 50114, threads = 16, per_thread = 100, payload = 128, timeout_ms = 2500;
    };

    bool parse(int argc, char **argv, Args &a)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (i + 1 >= argc)
            {
                return false;
            }

            std::string key = argv[i];
            const char *x = argv[++i];
            if (key == "--port" && positive(x, 1, 65535, a.port))
            {
            }
            else if (key == "--threads" && positive(x, 1, 128, a.threads))
            {
            }
            else if (key == "--per-thread" && positive(x, 1, 5000, a.per_thread))
            {
            }
            else if (key == "--payload" && positive(x, 1, 65536, a.payload))
            {
            }
            else if (key == "--timeout-ms" && positive(x, 500, 60000, a.timeout_ms))
            {
            }
            else
            {
                return false;
            }
        }

        return true;
    }
}

int main(int argc, char **argv)
{
    Args a;
    if (!parse(argc, argv, a))
    {
        std::cerr
            << "usage: net_churn_test --port 50114 --threads 16 --per-thread 100 --payload 128 --timeout-ms 2500\n";
        return 2;
    }

    net::Runtime runtime;
    if (!runtime.ok())
    {
        return 2;
    }

    std::atomic<std::uint64_t> success{0}, connect_error{0}, io_error{0}, mismatch{0};
    std::atomic<int> logged_errors{0};
    const auto start = net::monotonic_ns();
    std::vector<std::thread> threads;
    for (int worker = 0; worker < a.threads; ++worker)
    {
        threads.emplace_back(
            [&, worker]()
            {
                const std::string payload(static_cast<std::size_t>(a.payload), static_cast<char>('a' + worker % 26));
                std::vector<char> data(payload.size() + 4);
                std::uint32_t header = htonl(static_cast<std::uint32_t>(a.payload));
                std::memcpy(data.data(), &header, 4);
                std::memcpy(data.data() + 4, payload.data(), payload.size());
                for (int i = 0; i < a.per_thread; i++)
                {
                    net::Socket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (fd == net::invalid_socket)
                    {
                        ++connect_error;
                        continue;
                    }

                    net::set_timeout(fd, a.timeout_ms);
                    sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(static_cast<std::uint16_t>(a.port));
                    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
                    {
                        ++connect_error;
                        net::close_socket(fd);
                        continue;
                    }

                    char wire[4];
                    bool ok = net::send_all(fd, data.data(), data.size()) && net::recv_all(fd, wire, sizeof(wire));
                    std::uint32_t actual_length = 0;
                    if (ok)
                    {
                        std::memcpy(&actual_length, wire, 4);
                        actual_length = ntohl(actual_length);
                    }

                    if (!ok || actual_length != static_cast<std::uint32_t>(a.payload))
                    {
                        if (logged_errors.fetch_add(1) < 12)
                        {
                            std::cerr << "CHURN_DIAGNOSTIC phase=header worker=" << worker << " iteration=" << i
                                      << " socket_error=" << net::last_error() << " valid_transfer=" << ok
                                      << " actual_length=" << actual_length << '\n';
                        }

                        ++io_error;
                    }
                    else
                    {
                        std::vector<char> response(payload.size());
                        if (!net::recv_all(fd, response.data(), response.size()))
                        {
                            if (logged_errors.fetch_add(1) < 12)
                            {
                                std::cerr << "CHURN_DIAGNOSTIC phase=body worker=" << worker << " iteration=" << i
                                          << " socket_error=" << net::last_error() << '\n';
                            }

                            ++io_error;
                        }
                        else if (std::memcmp(response.data(), payload.data(), payload.size()) != 0)
                        {
                            ++mismatch;
                        }
                        else
                        {
                            ++success;
                        }
                    }

                    net::close_socket(fd);
                }
            });
    }

    for (auto &t : threads)
    {
        t.join();
    }

    const double seconds = (net::monotonic_ns() - start) / 1e9;
    const std::uint64_t total = static_cast<std::uint64_t>(a.threads) * a.per_thread;
    std::cout << "CHURN total=" << total << " success=" << success << " connect_errors=" << connect_error
              << " io_errors=" << io_error << " mismatches=" << mismatch << " elapsed_s=" << seconds
              << " timeout_ms=" << a.timeout_ms << " connections_per_sec=" << (seconds > 0 ? success / seconds : 0)
              << net::process_metrics() << std::endl;
    return success == total && connect_error == 0 && io_error == 0 && mismatch == 0 ? 0 : 1;
}
