#include "net_core.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <cerrno>
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace net
{
    Runtime::Runtime() : ready_(true)
    {
#ifdef _WIN32
        WSADATA data{};
        ready_ = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    }

    Runtime::~Runtime()
    {
#ifdef _WIN32
        if (ready_)
        {
            WSACleanup();
        }
#endif
    }

    int last_error()
    {
#ifdef _WIN32
        return WSAGetLastError();
#else
        return errno;
#endif
    }

    bool would_block(int e)
    {
#ifdef _WIN32
        return e == WSAEWOULDBLOCK;
#else
        return e == EAGAIN || e == EWOULDBLOCK;
#endif
    }

    bool interrupted(int e)
    {
#ifdef _WIN32
        return e == WSAEINTR;
#else
        return e == EINTR;
#endif
    }

    std::string error_message(int error)
    {
        return std::to_string(error);
    }

    bool peer_disconnect_error(int error)
    {
#ifdef _WIN32
        return error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAESHUTDOWN ||
               error == ERROR_NETNAME_DELETED || error == ERROR_CONNECTION_ABORTED || error == ERROR_OPERATION_ABORTED;
#else
        return error == ECONNRESET || error == ECONNABORTED || error == EPIPE;
#endif
    }

    void close_socket(Socket fd)
    {
        if (fd == invalid_socket)
        {
            return;
        }
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }

    bool set_nonblocking(Socket fd, bool enabled)
    {
#ifdef _WIN32
        u_long state = enabled ? 1UL : 0UL;
        return ioctlsocket(fd, FIONBIO, &state) == 0;
#else
        int flags = fcntl(fd, F_GETFL, 0);
        return flags >= 0 && fcntl(fd, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
    }

    bool set_timeout(Socket fd, int milliseconds)
    {
#ifdef _WIN32
        DWORD timeout = static_cast<DWORD>(milliseconds);
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout)) ==
                   0 &&
               setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout)) == 0;
#else
        timeval tv{milliseconds / 1000, (milliseconds % 1000) * 1000};
        return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
               setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
    }

    bool send_all(Socket fd, const char *data, std::size_t size)
    {
        std::size_t written = 0;
        while (written < size)
        {
            int chunk = static_cast<int>(std::min<std::size_t>(size - written, 1 << 20));
#ifdef MSG_NOSIGNAL
            int count = ::send(fd, data + written, chunk, MSG_NOSIGNAL);
#else
            int count = ::send(fd, data + written, chunk, 0);
#endif
            if (count > 0)
            {
                written += static_cast<std::size_t>(count);
                continue;
            }

            if (count < 0 && interrupted(last_error()))
            {
                continue;
            }

            return false;
        }

        return true;
    }

    bool recv_all(Socket fd, char *data, std::size_t size)
    {
        std::size_t read = 0;
        while (read < size)
        {
            int chunk = static_cast<int>(std::min<std::size_t>(size - read, 1 << 20));
            int count = ::recv(fd, data + read, chunk, 0);
            if (count > 0)
            {
                read += static_cast<std::size_t>(count);
                continue;
            }

            if (count < 0 && interrupted(last_error()))
            {
                continue;
            }

            return false;
        }

        return true;
    }

    std::uint64_t monotonic_ns()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    std::string process_metrics()
    {
        std::ostringstream out;
#ifdef _WIN32
        FILETIME create{}, exit{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user))
        {
            ULARGE_INTEGER k{}, u{};
            k.LowPart = kernel.dwLowDateTime;
            k.HighPart = kernel.dwHighDateTime;
            u.LowPart = user.dwLowDateTime;
            u.HighPart = user.dwHighDateTime;
            out << " process_cpu_seconds=" << (k.QuadPart + u.QuadPart) / 10000000.0;
        }

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc)))
        {
            out << " rss_mib=" << (pmc.WorkingSetSize / 1048576.0);
            out << " peak_rss_mib=" << (pmc.PeakWorkingSetSize / 1048576.0);
        }
#else
        rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) == 0)
        {
            out << " process_cpu_seconds="
                << (ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6);
            out << " peak_rss_mib=" << ru.ru_maxrss / 1024.0;
        }
#endif
        return out.str();
    }
}
