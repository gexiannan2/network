#include "net_core.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct Options
    {
        public:
            std::string host = "127.0.0.1";
            int port = 50113;
            int connections = 16;
            int duration = 5;
            int payload = 128;
            int pipeline = 1;
    };

    struct Stats
    {
        public:
            std::uint64_t messages = 0, payload_bytes = 0, io_errors = 0, corrupt = 0;
            int connected = 0;
            std::vector<double> latency_ms;
    };

    bool valid_int(const char *value, int minimum, int maximum, int &dest)
    {
        char *end = nullptr;
        long v = std::strtol(value, &end, 10);
        if (!value[0] || !end || *end || v < minimum || v > maximum)
        {
            return false;
        }

        dest = static_cast<int>(v);
        return true;
    }

    bool parse_options(int argc, char **argv, Options &o)
    {
        for (int i = 1; i < argc; i++)
        {
            std::string flag = argv[i];
            if (flag == "--help")
            {
                std::cout << "net_client --host 127.0.0.1 --port 50113 --connections 16"
                          << " --duration 5 --payload 128 --pipeline 1\n";
                std::exit(0);
            }

            if (i + 1 >= argc)
            {
                return false;
            }

            const char *v = argv[++i];
            if (flag == "--host")
            {
                o.host = v;
            }
            else if (flag == "--port" && valid_int(v, 1, 65535, o.port))
            {
            }
            else if (flag == "--connections" && valid_int(v, 1, 512, o.connections))
            {
            }
            else if (flag == "--duration" && valid_int(v, 1, 3600, o.duration))
            {
            }
            else if (flag == "--payload" && valid_int(v, 16, static_cast<int>(net::max_payload), o.payload))
            {
            }
            else if (flag == "--pipeline" && valid_int(v, 1, 64, o.pipeline))
            {
            }
            else
            {
                return false;
            }
        }

        return true;
    }

    struct Pending
    {
        public:
            std::vector<char> frame;
            std::uint64_t sent_ns = 0;
    };

    void run_connection(const Options &o, std::uint64_t start_ns, Stats &stats, std::uint32_t worker_id)
    {
        net::Socket sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == net::invalid_socket)
        {
            ++stats.io_errors;
            return;
        }

        net::set_timeout(sock, 5000);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(o.port));
        if (inet_pton(AF_INET, o.host.c_str(), &address.sin_addr) != 1 ||
            ::connect(sock, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        {
            ++stats.io_errors;
            net::close_socket(sock);
            return;
        }

        stats.connected = 1;
        while (net::monotonic_ns() < start_ns)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const std::uint64_t stop_ns = start_ns + static_cast<std::uint64_t>(o.duration) * 1000000000ULL;
        std::uint64_t sequence = 0;
        std::vector<Pending> pending;
        pending.reserve(static_cast<std::size_t>(o.pipeline));
        std::vector<char> actual(static_cast<std::size_t>(o.payload));
        bool healthy = true;
        while (healthy && net::monotonic_ns() < stop_ns)
        {
            pending.clear();
            for (int k = 0; k < o.pipeline && healthy; k++)
            {
                Pending pkt;
                pkt.frame.resize(static_cast<std::size_t>(o.payload) + 4);
                std::uint32_t len = htonl(static_cast<std::uint32_t>(o.payload));
                std::memcpy(pkt.frame.data(), &len, 4);
                std::uint64_t unique = (static_cast<std::uint64_t>(worker_id) << 48) | (++sequence);
                for (int b = 4; b < o.payload + 4; b++)
                {
                    pkt.frame[static_cast<std::size_t>(b)] = static_cast<char>((unique + b * 37U) & 255);
                }

                pkt.sent_ns = net::monotonic_ns();
                std::memcpy(pkt.frame.data() + 4, &pkt.sent_ns, 8);
                std::memcpy(pkt.frame.data() + 12, &unique, 8);
                if (!net::send_all(sock, pkt.frame.data(), pkt.frame.size()))
                {
                    ++stats.io_errors;
                    healthy = false;
                    break;
                }

                pending.push_back(std::move(pkt));
            }

            for (const Pending &pkt : pending)
            {
                char header[4];
                if (!net::recv_all(sock, header, sizeof(header)))
                {
                    std::cerr << "client worker " << worker_id
                              << " recv header failed, socket_error=" << net::last_error() << '\n';
                    ++stats.io_errors;
                    healthy = false;
                    break;
                }

                std::uint32_t len;
                std::memcpy(&len, header, 4);
                len = ntohl(len);
                if (len != static_cast<std::uint32_t>(o.payload))
                {
                    std::cerr << "client worker " << worker_id << " wrong frame length=" << len
                              << " expected=" << o.payload << '\n';
                    ++stats.io_errors;
                    healthy = false;
                    break;
                }

                if (!net::recv_all(sock, actual.data(), actual.size()))
                {
                    std::cerr << "client worker " << worker_id
                              << " recv body failed, socket_error=" << net::last_error() << '\n';
                    ++stats.io_errors;
                    healthy = false;
                    break;
                }

                if (std::memcmp(actual.data(), pkt.frame.data() + 4, actual.size()) != 0)
                {
                    ++stats.corrupt;
                    healthy = false;
                    break;
                }

                ++stats.messages;
                stats.payload_bytes += static_cast<std::uint64_t>(o.payload);
                if ((stats.messages & 15) == 0 && stats.latency_ms.size() < 4096)
                {
                    double latency = (net::monotonic_ns() - pkt.sent_ns) / 1e6;
                    stats.latency_ms.push_back(latency);
                }
            }
        }

        net::close_socket(sock);
    }

    double percentile(const std::vector<double> &samples, double p)
    {
        if (samples.empty())
        {
            return 0.0;
        }

        double index = (samples.size() - 1) * p;
        std::size_t lo = static_cast<std::size_t>(index);
        std::size_t hi = std::min(lo + 1, samples.size() - 1);
        return samples[lo] + (samples[hi] - samples[lo]) * (index - lo);
    }
}

int main(int argc, char **argv)
{
    Options o;
    if (!parse_options(argc, argv, o))
    {
        std::cerr << "Invalid options; use --help\n";
        return 2;
    }

    net::Runtime runtime;
    if (!runtime.ok())
    {
        std::cerr << "Network runtime failed\n";
        return 2;
    }

    std::vector<Stats> stats(static_cast<std::size_t>(o.connections));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(o.connections));
    const std::uint64_t begin_ns = net::monotonic_ns() + 500000000ULL;
    try
    {
        for (int i = 0; i < o.connections; i++)
        {
            workers.emplace_back(
                [&, i]()
                {
                    run_connection(o, begin_ns, stats[static_cast<std::size_t>(i)], static_cast<std::uint32_t>(i));
                });
        }
    }
    catch (const std::exception &ex)
    {
        for (auto &worker : workers)
        {
            worker.join();
        }

        std::cerr << "Thread creation failed: " << ex.what() << '\n';
        return 2;
    }

    for (auto &worker : workers)
    {
        worker.join();
    }

    const double wall_s = std::max(0.001, (net::monotonic_ns() - begin_ns) / 1e9);
    std::uint64_t success = 0, bytes = 0, errors = 0, bad = 0;
    int connected = 0;
    std::vector<double> latency;
    for (const auto &st : stats)
    {
        connected += st.connected;
        success += st.messages;
        bytes += st.payload_bytes;
        errors += st.io_errors;
        bad += st.corrupt;
        latency.insert(latency.end(), st.latency_ms.begin(), st.latency_ms.end());
    }

    std::sort(latency.begin(), latency.end());
    std::cout << std::fixed << std::setprecision(3) << "BENCHMARK connected=" << connected
              << " requested_connections=" << o.connections << " duration_s=" << o.duration
              << " wall_elapsed_s=" << wall_s << " payload_bytes=" << o.payload << " pipeline=" << o.pipeline
              << " successful_messages=" << success << " requests_per_sec=" << success / wall_s
              << " payload_mib_per_sec=" << (bytes / 1048576.0) / wall_s << " p50_ms=" << percentile(latency, 0.50)
              << " p95_ms=" << percentile(latency, 0.95) << " p99_ms=" << percentile(latency, 0.99)
              << " sampled_latencies=" << latency.size() << " io_errors=" << errors << " data_mismatches=" << bad
              << net::process_metrics() << std::endl;
    return (connected == o.connections && success > 0 && errors == 0 && bad == 0) ? 0 : 1;
}
