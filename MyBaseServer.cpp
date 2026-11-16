#include "MyBaseServer.h"
#include "base/MySockTaskManager.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <csignal>

namespace MyNameSpace
{
    namespace
    {
        volatile std::sig_atomic_t stop_requested = 0;

        void request_stop(int)
        {
            stop_requested = 1;
        }
    }

    bool MyBaseServer::init(int port, const std::string &host, int backlog)
    {
        if (!tcp_.bindPort(port, host, backlog))
        {
            return false;
        }

        stop_requested = 0;
        std::signal(SIGINT, request_stop);
#ifdef SIGTERM
        std::signal(SIGTERM, request_stop);
#endif
        return true;
    }

    void MyBaseServer::mainLoop(int duration_seconds)
    {
        const auto start = net::monotonic_ns();
        auto last = start;
        while (!isFini() && !stop_requested)
        {
            const auto now = net::monotonic_ns();
            if (duration_seconds > 0 && now - start >= static_cast<std::uint64_t>(duration_seconds) * 1000000000ULL)
            {
                break;
            }

            for (int i = 0; i < 128; ++i)
            {
                auto fd = tcp_.acceptCallBack();
                if (fd == net::invalid_socket)
                {
                    break;
                }

                if (!net::set_nonblocking(fd, true))
                {
                    net::close_socket(fd);
                    continue;
                }

                newTask(fd);
            }

            if (now - last >= 1000000000ULL)
            {
                auto s = MySockTaskManager::getInstance().snapshot();
                std::cout << "SERVER elapsed_s=" << (now - start) / 1e9 << " connections=" << s.active
                          << " peak_connections=" << s.peak << " accepted=" << s.accepted << " closed=" << s.closed
                          << " messages=" << s.messages << " requests_per_sec=" << (s.messages / ((now - start) / 1e9))
                          << " payload_mib=" << s.bytes / 1048576.0 << " rejected=" << s.rejected
                          << " io_errors=" << s.io_errors << " protocol_errors=" << s.protocol_errors
                          << " peer_disconnects=" << s.peer_disconnects << net::process_metrics() << std::endl;
                last = now;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        fini();
    }
}
