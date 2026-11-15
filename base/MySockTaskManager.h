#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
namespace MyNameSpace {
class MySockTask;
class MySockTaskManager {
public:
    struct Snapshot {
        std::uint64_t accepted=0,closed=0,messages=0,bytes=0,rejected=0,io_errors=0,protocol_errors=0,peer_disconnects=0;
        std::size_t active=0,peak=0;
    };
    static MySockTaskManager& getInstance();
    void addTask(const std::shared_ptr<MySockTask>& task);
    void removeTask(std::uint64_t id);
    std::shared_ptr<MySockTask> getTaskByIdWithLock(std::uint64_t id);
    void recordMessage(std::size_t size);
    void recordRejected();
    void recordIoError();
    void recordPeerDisconnect();
    void recordProtocolError();
    Snapshot snapshot() const;
private:
    MySockTaskManager()=default;
    mutable std::mutex mutex_;
    std::map<std::uint64_t,std::weak_ptr<MySockTask>> tasks_;
    std::atomic<std::uint64_t> accepted_{0},closed_{0},messages_{0},bytes_{0};
    std::atomic<std::uint64_t> rejected_{0},io_errors_{0},protocol_errors_{0},peer_disconnects_{0};
    std::atomic<std::size_t> peak_{0};
};
}
