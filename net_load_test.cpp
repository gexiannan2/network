#include "net_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
    std::atomic<int> printed_connection_errors{0};

    void log_connection_failure(const char* operation, int index)
    {
        const int error = net::last_error();
        if (printed_connection_errors.fetch_add(1) < 12)
        {
            std::cerr << "CONNECT_DIAGNOSTIC stage=" << operation
                      << " client_index=" << index << " error=" << error << '\n';
        }
    }

    struct Options
    {
        std::string host = "127.0.0.1";
        std::string mode = "hold";
        int port = 50114;
        int connections = 10000;
        int threads = 16;
        int duration = 30;
        int payload = 128;
        int active_percent = 10;
        int ramp_ms = 6000;
        int source_ips = 1;
        int source_port_start = 0;
        int heartbeat_seconds = 20;
    };

    struct Connection
    {
        net::Socket fd = net::invalid_socket;
        std::size_t send_offset = 0;
    };

    struct Stats
    {
        std::uint64_t connected = 0;
        std::uint64_t connect_errors = 0;
        std::uint64_t retries = 0;
        std::uint64_t recovered = 0;
        std::uint64_t echoed = 0;
        std::uint64_t mismatches = 0;
        std::uint64_t io_errors = 0;
        std::uint64_t slow_packets = 0;
        std::uint64_t would_block = 0;
        std::uint64_t latency_ns = 0;
        std::uint64_t sampled_latency_count = 0;
        std::vector<double> latency_samples_ms;
        std::mt19937_64 sample_rng{0x4d4d4f2026ULL};
    };

    // Bounded reservoir sampling avoids unbounded growth during a 24-hour run.
    void record_latency(Stats& stats, std::uint64_t duration_ns)
    {
        stats.latency_ns += duration_ns;
        ++stats.echoed;
        if ((stats.echoed & 15ULL) != 0)
        {
            return;
        }

        constexpr std::size_t max_samples = 4096;
        ++stats.sampled_latency_count;
        const double latency_ms = duration_ns / 1e6;
        if (stats.latency_samples_ms.size() < max_samples)
        {
            stats.latency_samples_ms.push_back(latency_ms);
            return;
        }

        std::uniform_int_distribution<std::uint64_t> choose(0, stats.sampled_latency_count - 1);
        const std::uint64_t index = choose(stats.sample_rng);
        if (index < max_samples)
        {
            stats.latency_samples_ms[static_cast<std::size_t>(index)] = latency_ms;
        }
    }

    double sampled_percentile(const std::vector<double>& sorted, double quantile)
    {
        if (sorted.empty())
        {
            return 0.0;
        }

        const double position = (sorted.size() - 1) * quantile;
        const std::size_t lo = static_cast<std::size_t>(position);
        const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
        return sorted[lo] + (sorted[hi] - sorted[lo]) * (position - lo);
    }

    bool integer(const char* value, int minimum, int maximum, int& output)
    {
        char* end = nullptr;
        const long number = std::strtol(value, &end, 10);
        if (*value == '\0' || *end != '\0' || number < minimum || number > maximum)
        {
            return false;
        }

        output = static_cast<int>(number);
        return true;
    }

    bool parse(int argc, char** argv, Options& options)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string flag = argv[i];
            if (flag == "--help")
            {
                std::cout << "net_load_test --mode hold|throughput|slow"
                          << " --host 127.0.0.1 --port 50114 --connections 10000"
                          << " --threads 16 --duration 30 --payload 128"
                          << " --active-percent 10 --ramp-ms 6000"
                          << " --source-ips 1 --source-port-start 0 --heartbeat-seconds 20\n";
                std::exit(0);
            }

            if (i + 1 >= argc)
            {
                return false;
            }

            const char* value = argv[++i];
            if (flag == "--mode")
            {
                options.mode = value;
            }
            else if (flag == "--host")
            {
                options.host = value;
            }
            else if (flag == "--port" && integer(value, 1, 65535, options.port))
            {
            }
            else if (flag == "--connections" &&
                     integer(value, 1, 20000, options.connections))
            {
            }
            else if (flag == "--threads" && integer(value, 1, 64, options.threads))
            {
            }
            else if (flag == "--duration" && integer(value, 1, 86400, options.duration))
            {
            }
            else if (flag == "--payload" &&
                     integer(value, 16, static_cast<int>(net::max_payload), options.payload))
            {
            }
            else if (flag == "--active-percent" &&
                     integer(value, 1, 100, options.active_percent))
            {
            }
            else if (flag == "--ramp-ms" && integer(value, 0, 120000, options.ramp_ms))
            {
            }
            else if (flag == "--source-ips" && integer(value, 1, 128, options.source_ips))
            {
            }
            else if (flag == "--source-port-start" &&
                     integer(value, 1024, 65000, options.source_port_start))
            {
            }
            else if (flag == "--heartbeat-seconds" &&
                     integer(value, 1, 600, options.heartbeat_seconds))
            {
            }
            else
            {
                return false;
            }
        }

        return (options.mode == "hold" || options.mode == "throughput" ||
                options.mode == "slow") &&
               (options.source_port_start == 0 ||
                options.source_port_start + options.connections - 1 <= 65535);
    }

    net::Socket connect_one(const Options& options, int client_index)
    {
        net::Socket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == net::invalid_socket)
        {
            return fd;
        }

        net::set_timeout(fd, 3000);

        if (options.source_ips > 1 || options.source_port_start > 0)
        {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = options.source_port_start == 0 ? 0 :
                htons(static_cast<std::uint16_t>(options.source_port_start + client_index));
            const std::string source = options.source_ips == 1 ? "127.0.0.1" :
                "127.0.0." + std::to_string(2 + client_index % options.source_ips);
            if (inet_pton(AF_INET, source.c_str(), &local.sin_addr) != 1 ||
                ::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
            {
                log_connection_failure("bind", client_index);
                net::close_socket(fd);
                return net::invalid_socket;
            }
        }

        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(static_cast<std::uint16_t>(options.port));
        if (inet_pton(AF_INET, options.host.c_str(), &remote.sin_addr) != 1 ||
            ::connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0)
        {
            log_connection_failure("connect", client_index);
            net::close_socket(fd);
            return net::invalid_socket;
        }

        if (options.mode == "slow")
        {
            const int small_buffer = 4096;
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                         reinterpret_cast<const char*>(&small_buffer),
                         static_cast<int>(sizeof(small_buffer)));
            if (!net::set_nonblocking(fd, true))
            {
                net::close_socket(fd);
                return net::invalid_socket;
            }
        }

        return fd;
    }

    bool echo(net::Socket fd, const std::vector<char>& frame,
              std::vector<char>& response, Stats& stats)
    {
        const auto begin = net::monotonic_ns();
        char header[4] = {};
        if (!net::send_all(fd, frame.data(), frame.size()) ||
            !net::recv_all(fd, header, sizeof(header)))
        {
            ++stats.io_errors;
            return false;
        }

        std::uint32_t payload_size = 0;
        std::memcpy(&payload_size, header, sizeof(payload_size));
        if (ntohl(payload_size) != frame.size() - 4 ||
            !net::recv_all(fd, response.data(), response.size()))
        {
            ++stats.io_errors;
            return false;
        }

        if (std::memcmp(response.data(), frame.data() + 4, response.size()) != 0)
        {
            ++stats.mismatches;
            return false;
        }

        record_latency(stats, net::monotonic_ns() - begin);
        return true;
    }

    void run_worker(const Options& options, int worker_index,
                    std::atomic<int>& ready_workers, Stats& stats)
    {
        std::vector<Connection> connections;
        connections.reserve(static_cast<std::size_t>(
            (options.connections + options.threads - 1) / options.threads));

        std::vector<char> frame(static_cast<std::size_t>(options.payload) + 4,
                                static_cast<char>('A' + worker_index % 26));
        const std::uint32_t length = htonl(static_cast<std::uint32_t>(options.payload));
        std::memcpy(frame.data(), &length, sizeof(length));
        std::vector<char> response(static_cast<std::size_t>(options.payload));

        for (int index = worker_index; index < options.connections; index += options.threads)
        {
            net::Socket fd = connect_one(options, index);
            if (fd == net::invalid_socket &&
                options.source_port_start > 0 && options.source_ips == 1)
            {
                ++stats.retries;
                Options retry = options;
                retry.source_ips = 16;
                fd = connect_one(retry, index);
                if (fd != net::invalid_socket)
                {
                    ++stats.recovered;
                }
            }

            if (fd == net::invalid_socket)
            {
                ++stats.connect_errors;
            }
            else
            {
                connections.push_back(Connection{fd, 0});
                ++stats.connected;
            }

            if (options.ramp_ms > 0)
            {
                const int per_worker = (options.connections + options.threads - 1) /
                    options.threads;
                std::this_thread::sleep_for(std::chrono::microseconds(
                    static_cast<std::uint64_t>(options.ramp_ms) * 1000 / per_worker));
            }
        }

        ++ready_workers;
        while (ready_workers.load() < options.threads)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        const auto start = net::monotonic_ns();
        const auto stop = start + static_cast<std::uint64_t>(options.duration) *
            1000000000ULL;
        auto next_heartbeat = start + static_cast<std::uint64_t>(
            options.heartbeat_seconds) * 1000000000ULL;
        while (net::monotonic_ns() < stop)
        {
            if (options.mode == "hold")
            {
                const auto now = net::monotonic_ns();
                if (now >= next_heartbeat)
                {
                    for (auto& conn : connections)
                    {
                        if (conn.fd != net::invalid_socket &&
                            !echo(conn.fd, frame, response, stats))
                        {
                            net::close_socket(conn.fd);
                            conn.fd = net::invalid_socket;
                        }
                    }

                    next_heartbeat = net::monotonic_ns() +
                        static_cast<std::uint64_t>(options.heartbeat_seconds) *
                        1000000000ULL;
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
            else if (options.mode == "throughput")
            {
                for (std::size_t i = 0; i < connections.size(); ++i)
                {
                    Connection& conn = connections[i];
                    if (conn.fd == net::invalid_socket ||
                        (i % 100) >= static_cast<std::size_t>(options.active_percent))
                    {
                        continue;
                    }

                    if (!echo(conn.fd, frame, response, stats))
                    {
                        net::close_socket(conn.fd);
                        conn.fd = net::invalid_socket;
                    }

                    if (net::monotonic_ns() >= stop)
                    {
                        break;
                    }
                }
            }
            else
            {
                for (std::size_t i = 0; i < connections.size(); ++i)
                {
                    Connection& conn = connections[i];
                    if (conn.fd == net::invalid_socket ||
                        (i % 100) >= static_cast<std::size_t>(options.active_percent))
                    {
                        continue;
                    }

#ifdef MSG_NOSIGNAL
                    constexpr int flags = MSG_NOSIGNAL;
#else
                    constexpr int flags = 0;
#endif
                    const int sent = ::send(conn.fd, frame.data() + conn.send_offset,
                                            static_cast<int>(frame.size() - conn.send_offset),
                                            flags);
                    if (sent > 0)
                    {
                        conn.send_offset += static_cast<std::size_t>(sent);
                        if (conn.send_offset == frame.size())
                        {
                            conn.send_offset = 0;
                            ++stats.slow_packets;
                        }
                    }
                    else if (sent < 0 && net::would_block(net::last_error()))
                    {
                        ++stats.would_block;
                    }
                    else if (sent < 0 && net::interrupted(net::last_error()))
                    {
                        continue;
                    }
                    else
                    {
                        ++stats.io_errors;
                        net::close_socket(conn.fd);
                        conn.fd = net::invalid_socket;
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        for (auto& conn : connections)
        {
            net::close_socket(conn.fd);
        }
    }
}

int main(int argc, char** argv)
{
    Options options;
    if (!parse(argc, argv, options))
    {
        std::cerr << "Invalid load options; use --help\n";
        return 2;
    }

    net::Runtime runtime;
    if (!runtime.ok())
    {
        std::cerr << "Network initialization failed\n";
        return 2;
    }

    std::vector<Stats> stats(static_cast<std::size_t>(options.threads));
    std::vector<std::thread> threads;
    std::atomic<int> ready_workers{0};
    const auto begin = net::monotonic_ns();
    for (int index = 0; index < options.threads; ++index)
    {
        threads.emplace_back([&, index]()
        {
            run_worker(options, index, ready_workers,
                       stats[static_cast<std::size_t>(index)]);
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    std::vector<double> latencies;
    Stats total;
    for (const auto& item : stats)
    {
        total.connected += item.connected;
        total.connect_errors += item.connect_errors;
        total.retries += item.retries;
        total.recovered += item.recovered;
        total.echoed += item.echoed;
        total.mismatches += item.mismatches;
        total.io_errors += item.io_errors;
        total.slow_packets += item.slow_packets;
        total.would_block += item.would_block;
        total.latency_ns += item.latency_ns;
        latencies.insert(latencies.end(), item.latency_samples_ms.begin(),
                         item.latency_samples_ms.end());
    }

    std::sort(latencies.begin(), latencies.end());
    const double elapsed = (net::monotonic_ns() - begin) / 1e9;
    const double average_ms = total.echoed == 0 ? 0.0 :
        static_cast<double>(total.latency_ns) / total.echoed / 1e6;
    std::cout << std::fixed << std::setprecision(3)
              << "LOAD mode=" << options.mode
              << " requested=" << options.connections
              << " connected=" << total.connected
              << " connect_errors=" << total.connect_errors
              << " retries=" << total.retries
              << " recovered=" << total.recovered
              << " duration_s=" << options.duration
              << " ramp_ms=" << options.ramp_ms
              << " elapsed_s=" << elapsed
              << " echo_messages=" << total.echoed
              << " echo_per_sec=" << total.echoed /
                   std::max(0.001, static_cast<double>(options.duration))
              << " average_echo_ms=" << average_ms
              << " sampled_p50_ms=" << sampled_percentile(latencies, 0.50)
              << " sampled_p95_ms=" << sampled_percentile(latencies, 0.95)
              << " sampled_p99_ms=" << sampled_percentile(latencies, 0.99)
              << " latency_samples=" << latencies.size()
              << " slow_packets=" << total.slow_packets
              << " would_block=" << total.would_block
              << " io_errors=" << total.io_errors
              << " mismatches=" << total.mismatches
              << net::process_metrics() << '\n';

    if (total.connected != static_cast<std::uint64_t>(options.connections) ||
        total.connect_errors != 0 || total.mismatches != 0 ||
        total.io_errors != 0)
    {
        return 1;
    }

    // A slow-client test must actually encounter backpressure to prove it.
    if (options.mode == "slow" && total.would_block == 0)
    {
        std::cerr << "SLOW_TEST_INCONCLUSIVE: no send backpressure observed\n";
        return 1;
    }

    return 0;
}
