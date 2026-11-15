#include "MySockTaskPool.h"
#include "MySockTaskManager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>
#elif defined(_WIN32)
#include <windows.h>
#include <unordered_map>
#else
#include <poll.h>
using PollFd = pollfd;
static int poll_sockets(PollFd* fds, std::size_t count, int timeout_ms)
{
    return ::poll(fds, static_cast<nfds_t>(count), timeout_ms);
}
#endif

namespace MyNameSpace
{
    class MyIoThread final : public IoBackend
    {
        public:
            explicit MyIoThread(std::size_t cap) : cap_(cap)
            {
#if defined(__linux__)
                epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
#elif defined(_WIN32)
                completion_port_ = ::CreateIoCompletionPort(
                    INVALID_HANDLE_VALUE, nullptr, 0, 0);
#endif
            }

            ~MyIoThread() override
            {
                terminate();
                join();
#if defined(__linux__)
                if (epoll_fd_ >= 0)
                {
                    ::close(epoll_fd_);
                }
#elif defined(_WIN32)
                if (completion_port_ != nullptr)
                {
                    ::CloseHandle(completion_port_);
                }
#endif
            }

            bool ready() const override
            {
#if defined(__linux__)
                return epoll_fd_ >= 0;
#elif defined(_WIN32)
                return completion_port_ != nullptr;
#else
                return true;
#endif
            }

            bool enqueue(const std::shared_ptr<MySockTask>& task) override
            {
                if (!task)
                {
                    return false;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                if (count_.load() >= cap_)
                {
                    return false;
                }

                pending_.push_back(task);
                ++count_;
                return true;
            }

            std::size_t load() const
            {
                return count_.load();
            }

            void run() override
            {
#if defined(__linux__)
                runEpoll();
#elif defined(_WIN32)
                runIocp();
#else
                runPoll();
#endif
            }

        private:
            void cleanupPending()
            {
                std::vector<std::shared_ptr<MySockTask>> pending;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pending.swap(pending_);
                }

                for (const auto& task : pending)
                {
                    MySockTaskManager::getInstance().removeTask(task->getId());
                }

                count_.fetch_sub(pending.size());
            }

#if defined(__linux__)
            void runEpoll()
            {
                const int epoll_fd = epoll_fd_;

                std::unordered_map<net::Socket, std::shared_ptr<MySockTask>> clients;
                std::array<epoll_event, 1024> events{};
                std::uint64_t last_sweep = net::monotonic_ns();

                while (!isFini())
                {
                    std::vector<std::shared_ptr<MySockTask>> newly_accepted;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        newly_accepted.swap(pending_);
                    }

                    for (auto& task : newly_accepted)
                    {
                        const net::Socket fd = task->socket();
                        epoll_event interest{};
                        interest.data.fd = fd;
                        interest.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
                        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &interest) != 0)
                        {
                            MySockTaskManager::getInstance().recordIoError();
                            MySockTaskManager::getInstance().removeTask(task->getId());
                            --count_;
                            continue;
                        }

                        clients.emplace(fd, std::move(task));
                    }

                    const int ready = ::epoll_wait(
                        epoll_fd, events.data(), static_cast<int>(events.size()), 10);
                    if (ready < 0)
                    {
                        if (net::interrupted(net::last_error()))
                        {
                            continue;
                        }

                        MySockTaskManager::getInstance().recordIoError();
                        break;
                    }

                    for (int index = 0; index < ready; ++index)
                    {
                        const epoll_event& event = events[static_cast<std::size_t>(index)];
                        auto iter = clients.find(event.data.fd);
                        if (iter == clients.end())
                        {
                            continue;
                        }

                        const auto& task = iter->second;
                        bool keep = true;

                        if ((event.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0)
                        {
                            if (task->rcvBuffer() < 0)
                            {
                                keep = false;
                                if (net::peer_disconnect_error(net::last_error()))
                                {
                                    MySockTaskManager::getInstance().recordPeerDisconnect();
                                }
                                else
                                {
                                    MySockTaskManager::getInstance().recordIoError();
                                }
                            }
                            else if (task->getMsg() < 0)
                            {
                                keep = false;
                                MySockTaskManager::getInstance().recordProtocolError();
                            }
                        }

                        // Echo responses are queued during getMsg(). Try sending
                        // immediately; subscribe to EPOLLOUT only after EWOULDBLOCK.
                        if (keep && task->pendingWrite())
                        {
                            if (task->syncSendBuf() < 0)
                            {
                                keep = false;
                                if (net::peer_disconnect_error(net::last_error()))
                                {
                                    MySockTaskManager::getInstance().recordPeerDisconnect();
                                }
                                else
                                {
                                    MySockTaskManager::getInstance().recordIoError();
                                }
                            }
                        }

                        if ((event.events & EPOLLERR) != 0)
                        {
                            keep = false;
                        }

                        if (keep && task->peerClosed() && !task->pendingWrite())
                        {
                            keep = false;
                        }

                        if (keep)
                        {
                            epoll_event interest{};
                            interest.data.fd = event.data.fd;
                            interest.events = EPOLLERR | EPOLLHUP;

                            if (!task->peerClosed() &&
                                task->pendingBytes() < net::max_pending_bytes / 2)
                            {
                                interest.events |= EPOLLIN | EPOLLRDHUP;
                            }

                            if (task->pendingWrite())
                            {
                                interest.events |= EPOLLOUT;
                            }

                            if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD,
                                            event.data.fd, &interest) != 0)
                            {
                                keep = false;
                                MySockTaskManager::getInstance().recordIoError();
                            }
                        }

                        if (!keep)
                        {
                            ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, event.data.fd, nullptr);
                            MySockTaskManager::getInstance().removeTask(task->getId());
                            clients.erase(iter);
                            --count_;
                        }
                    }

                    // Idle expiration is O(N) once per second, not on every event.
                    const std::uint64_t now = net::monotonic_ns();
                    if (now - last_sweep >= 1000000000ULL)
                    {
                        for (auto iter = clients.begin(); iter != clients.end();)
                        {
                            const auto& task = iter->second;
                            const std::uint64_t last_activity = task->lastActivity();
                            if (now > last_activity &&
                                now - last_activity > 120ULL * 1000000000ULL)
                            {
                                ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL,
                                            iter->first, nullptr);
                                MySockTaskManager::getInstance().removeTask(task->getId());
                                iter = clients.erase(iter);
                                --count_;
                            }
                            else
                            {

                                ++iter;
                            }
                        }

                        last_sweep = now;
                    }
                }

                for (const auto& entry : clients)
                {
                    MySockTaskManager::getInstance().removeTask(entry.second->getId());
                }

                count_.fetch_sub(clients.size());
                clients.clear();
                cleanupPending();
            }
#elif defined(_WIN32)
            struct IocpConnection
            {
                std::shared_ptr<MySockTask> task;
                net::Socket fd = net::invalid_socket;
                bool closing = false;
                bool receive_pending = false;
                bool send_pending = false;
                int last_post_error = 0;
            };

            struct IocpOperation
            {
                OVERLAPPED overlapped{};
                WSABUF buffer{};
                DWORD flags = 0;
                std::vector<char> storage;
                std::shared_ptr<IocpConnection> connection;
                bool is_send = false;
            };

            using ConnectionMap = std::unordered_map<net::Socket, std::shared_ptr<IocpConnection>>;

            void closeConnection(const std::shared_ptr<IocpConnection>& conn,
                                 ConnectionMap& connections)
            {
                if (conn->closing)
                {
                    return;
                }

                conn->closing = true;
                // closesocket cancels pending OVERLAPPED operations. Each operation
                // retains the connection until its completion has been dequeued.
                conn->task->closeSocket();
                MySockTaskManager::getInstance().removeTask(conn->task->getId());
                connections.erase(conn->fd);
                --count_;
            }

            bool postReceive(const std::shared_ptr<IocpConnection>& conn)
            {
                if (conn->closing || conn->receive_pending || conn->task->peerClosed() ||
                    conn->task->pendingBytes() >= net::max_pending_bytes / 2)
                {
                    return true;
                }

                auto operation = std::make_unique<IocpOperation>();
                operation->connection = conn;
                operation->storage.resize(16 * 1024);
                operation->buffer.buf = operation->storage.data();
                operation->buffer.len = static_cast<ULONG>(operation->storage.size());

                conn->receive_pending = true;
                ++outstanding_;
                DWORD received = 0;
                const int result = ::WSARecv(conn->fd, &operation->buffer, 1,
                                              &received, &operation->flags,
                                              &operation->overlapped, nullptr);
                if (result == SOCKET_ERROR)
                {
                    const int error = ::WSAGetLastError();
                    if (error != WSA_IO_PENDING)
                    {
                        conn->last_post_error = error;
                        conn->receive_pending = false;
                        --outstanding_;
                        return false;
                    }
                }

                operation.release();
                return true;
            }

            bool postSend(const std::shared_ptr<IocpConnection>& conn)
            {
                if (conn->closing || conn->send_pending || !conn->task->pendingWrite())
                {
                    return true;
                }

                auto operation = std::make_unique<IocpOperation>();
                operation->connection = conn;
                operation->is_send = true;
                operation->storage = conn->task->copyPendingOutput(64 * 1024);
                if (operation->storage.empty())
                {
                    return true;
                }

                operation->buffer.buf = operation->storage.data();
                operation->buffer.len = static_cast<ULONG>(operation->storage.size());

                conn->send_pending = true;
                ++outstanding_;
                DWORD transferred = 0;
                const int result = ::WSASend(conn->fd, &operation->buffer, 1,
                                              &transferred, 0,
                                              &operation->overlapped, nullptr);
                if (result == SOCKET_ERROR)
                {
                    const int error = ::WSAGetLastError();
                    if (error != WSA_IO_PENDING)
                    {
                        conn->last_post_error = error;
                        conn->send_pending = false;
                        --outstanding_;
                        return false;
                    }
                }

                operation.release();
                return true;
            }

            void handleCompletion(OVERLAPPED* overlapped, bool ok,
                                  DWORD transferred, DWORD completion_error,
                                  ConnectionMap& connections)
            {
                std::unique_ptr<IocpOperation> operation(
                    reinterpret_cast<IocpOperation*>(overlapped));
                const auto conn = operation->connection;
                --outstanding_;

                if (operation->is_send)
                {
                    conn->send_pending = false;
                }
                else
                {
                    conn->receive_pending = false;
                }

                if (conn->closing)
                {
                    return;
                }

                if (!ok)
                {
                    if (net::peer_disconnect_error(static_cast<int>(completion_error)))
                    {
                        MySockTaskManager::getInstance().recordPeerDisconnect();
                    }
                    else
                    {
                        MySockTaskManager::getInstance().recordIoError();
                    }
                    closeConnection(conn, connections);
                    return;
                }

                if (operation->is_send)
                {
                    if (transferred == 0)
                    {
                        closeConnection(conn, connections);
                        return;
                    }

                    conn->task->consumePendingOutput(transferred);
                }
                else if (transferred == 0)
                {
                    conn->task->markPeerClosed();
                }
                else
                {
                    if (!conn->task->appendReceived(operation->storage.data(), transferred) ||
                        conn->task->getMsg() < 0)
                    {
                        MySockTaskManager::getInstance().recordProtocolError();
                        closeConnection(conn, connections);
                        return;
                    }
                }

                if (!postSend(conn) || !postReceive(conn))
                {
                    if (net::peer_disconnect_error(conn->last_post_error))
                    {
                        MySockTaskManager::getInstance().recordPeerDisconnect();
                    }
                    else
                    {
                        MySockTaskManager::getInstance().recordIoError();
                    }
                    closeConnection(conn, connections);
                    return;
                }

                if (conn->task->peerClosed() && !conn->task->pendingWrite() &&
                    !conn->send_pending)
                {
                    closeConnection(conn, connections);
                }
            }

            void runIocp()
            {
                HANDLE completion_port = completion_port_;
                ConnectionMap connections;
                std::uint64_t last_sweep = net::monotonic_ns();

                while (!isFini())
                {
                    std::vector<std::shared_ptr<MySockTask>> newly_accepted;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        newly_accepted.swap(pending_);
                    }

                    for (auto& task : newly_accepted)
                    {
                        const net::Socket fd = task->socket();
                        HANDLE associated = ::CreateIoCompletionPort(
                            reinterpret_cast<HANDLE>(fd), completion_port, 0, 0);
                        if (associated != completion_port)
                        {
                            MySockTaskManager::getInstance().recordIoError();
                            MySockTaskManager::getInstance().removeTask(task->getId());
                            --count_;
                            continue;
                        }

                        auto conn = std::make_shared<IocpConnection>();
                        conn->fd = fd;
                        conn->task = std::move(task);
                        connections.emplace(fd, conn);

                        if (!postReceive(conn))
                        {
                            if (net::peer_disconnect_error(conn->last_post_error))
                            {
                                MySockTaskManager::getInstance().recordPeerDisconnect();
                            }
                            else
                            {
                                MySockTaskManager::getInstance().recordIoError();
                            }
                            closeConnection(conn, connections);
                        }
                    }

                    DWORD transferred = 0;
                    ULONG_PTR key = 0;
                    OVERLAPPED* overlapped = nullptr;
                    const BOOL success = ::GetQueuedCompletionStatus(
                        completion_port, &transferred, &key, &overlapped, 10);
                    const DWORD completion_error = success ? 0 : ::GetLastError();
                    if (overlapped != nullptr)
                    {
                        handleCompletion(overlapped, success != FALSE,
                                         transferred, completion_error, connections);
                    }
                    else if (!success && completion_error != WAIT_TIMEOUT)
                    {
                        MySockTaskManager::getInstance().recordIoError();
                    }

                    const std::uint64_t now = net::monotonic_ns();
                    if (now - last_sweep >= 1000000000ULL)
                    {
                        for (auto iter = connections.begin(); iter != connections.end();)
                        {
                            auto conn = iter->second;
                            ++iter;
                            const std::uint64_t activity = conn->task->lastActivity();
                            if (now > activity && now - activity > 120ULL * 1000000000ULL)
                            {
                                closeConnection(conn, connections);
                            }
                        }

                        last_sweep = now;
                    }
                }

                while (!connections.empty())
                {
                    closeConnection(connections.begin()->second, connections);
                }

                // Every posted operation owns its OVERLAPPED and buffer. Drain all
                // cancellation completions before closing the completion port.
                while (outstanding_ != 0)
                {
                    DWORD transferred = 0;
                    ULONG_PTR key = 0;
                    OVERLAPPED* overlapped = nullptr;
                    const BOOL success = ::GetQueuedCompletionStatus(
                        completion_port, &transferred, &key, &overlapped, 5000);
                    const DWORD completion_error = success ? 0 : ::GetLastError();
                    if (overlapped != nullptr)
                    {
                        handleCompletion(overlapped, success != FALSE,
                                         transferred, completion_error, connections);
                    }
                    else
                    {
                        // A timed-out drain can indicate a Windows or driver fault.
                        MySockTaskManager::getInstance().recordIoError();
                        break;
                    }
                }

                cleanupPending();
            }

            std::size_t outstanding_ = 0;
            HANDLE completion_port_ = nullptr;
#else
            void runPoll()
            {
                std::vector<std::shared_ptr<MySockTask>> clients;

                while (!isFini())
                {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        for (auto& task : pending_)
                        {
                            clients.push_back(std::move(task));
                        }

                        pending_.clear();
                    }

                    if (clients.empty())
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }

                    std::vector<PollFd> fds;
                    fds.reserve(clients.size());
                    for (const auto& task : clients)
                    {
                        PollFd descriptor{};
                        descriptor.fd = task->socket();

                        if (!task->peerClosed() &&
                            task->pendingBytes() < net::max_pending_bytes / 2)
                        {
                            descriptor.events |= POLLIN;
                        }

                        if (task->pendingWrite())
                        {
                            descriptor.events |= POLLOUT;
                        }

                        fds.push_back(descriptor);
                    }

                    const int ready = poll_sockets(fds.data(), fds.size(), 10);
                    if (ready < 0)
                    {
                        if (net::interrupted(net::last_error()))
                        {
                            continue;
                        }

                        MySockTaskManager::getInstance().recordIoError();
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }

                    const std::uint64_t now = net::monotonic_ns();
                    for (std::size_t i = clients.size(); i > 0; --i)
                    {
                        const std::size_t index = i - 1;
                        auto& task = clients[index];
                        const short revents = fds[index].revents;
                        bool keep = true;

                        if ((revents & (POLLIN | POLLHUP)) != 0)
                        {
                            if (task->rcvBuffer() < 0)
                            {
                                keep = false;
                                MySockTaskManager::getInstance().recordIoError();
                            }
                            else if (task->getMsg() < 0)
                            {
                                keep = false;
                                MySockTaskManager::getInstance().recordProtocolError();
                            }
                        }

                        if (keep && task->pendingWrite() &&
                            ((revents & (POLLOUT | POLLHUP)) != 0 || task->peerClosed()))
                        {
                            if (task->syncSendBuf() < 0)
                            {
                                keep = false;
                                MySockTaskManager::getInstance().recordIoError();
                            }
                        }

                        if ((revents & (POLLERR | POLLNVAL)) != 0)
                        {
                            keep = false;
                        }

                        if (keep && task->peerClosed() && !task->pendingWrite())
                        {
                            keep = false;
                        }

                        if (keep && now > task->lastActivity() &&
                            now - task->lastActivity() > 120ULL * 1000000000ULL)
                        {
                            keep = false;
                        }

                        if (!keep)
                        {
                            MySockTaskManager::getInstance().removeTask(task->getId());
                            clients[index] = std::move(clients.back());
                            clients.pop_back();
                            --count_;
                        }
                    }
                }

                for (const auto& task : clients)
                {
                    MySockTaskManager::getInstance().removeTask(task->getId());
                }

                count_.fetch_sub(clients.size());
                clients.clear();
                cleanupPending();
            }
#endif

            const std::size_t cap_;
            mutable std::mutex mutex_;
            std::vector<std::shared_ptr<MySockTask>> pending_;
            std::atomic<std::size_t> count_{0};
#if defined(__linux__)
            int epoll_fd_ = -1;
#endif
    };

    MySockTaskPool::MySockTaskPool(int ioCount, int maxConnPerIo)
        : io_count_(ioCount), max_conn_per_io_(maxConnPerIo)
    {
    }

    MySockTaskPool::~MySockTaskPool()
    {
        fini();
    }

    bool MySockTaskPool::init()
    {
        if (running_)
        {
            return true;
        }

        if (io_count_ < 1 || max_conn_per_io_ < 1)
        {
            return false;
        }

        for (int i = 0; i < io_count_; ++i)
        {
            auto worker = std::unique_ptr<IoBackend>(
                new MyIoThread(static_cast<std::size_t>(max_conn_per_io_)));
            if (!worker->ready() || !worker->start())
            {
                pool_.joinAll();
                pool_.delAllThread();
                return false;
            }
            pool_.putThread(std::move(worker));
        }

        running_ = true;
        return true;
    }

    bool MySockTaskPool::addTask(const std::shared_ptr<MySockTask>& task)
    {
        if (!running_)
        {
            return false;
        }

        IoBackend* chosen = nullptr;
        std::size_t min_load = std::numeric_limits<std::size_t>::max();
        for (std::size_t index = 0; index < pool_.getThreadCount(); ++index)
        {
            auto* worker = static_cast<IoBackend*>(pool_.getThreadByIndex(index));
            if (worker->load() < min_load)
            {
                chosen = worker;
                min_load = worker->load();
            }
        }

        return chosen != nullptr && chosen->enqueue(task);
    }

    void MySockTaskPool::fini()
    {
        if (!running_ && pool_.getThreadCount() == 0)
        {
            return;
        }

        pool_.joinAll();
        pool_.delAllThread();
        running_ = false;
    }
} // namespace MyNameSpace
