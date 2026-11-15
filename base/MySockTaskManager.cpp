#include "MySockTaskManager.h"
#include "MySockTask.h"
namespace MyNameSpace {
MySockTaskManager& MySockTaskManager::getInstance() {
    static MySockTaskManager instance;
    return instance;
}
void MySockTaskManager::addTask(const std::shared_ptr<MySockTask>& task) {
    std::lock_guard<std::mutex> guard(mutex_);
    tasks_[task->getId()]=task;
    ++accepted_;
    const auto count=tasks_.size();
    std::size_t peak=peak_.load();
    while(peak<count && !peak_.compare_exchange_weak(peak,count)) {}
}
void MySockTaskManager::removeTask(std::uint64_t id) {
    std::lock_guard<std::mutex> guard(mutex_);
    if(tasks_.erase(id)) ++closed_;
}
std::shared_ptr<MySockTask> MySockTaskManager::getTaskByIdWithLock(std::uint64_t id) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it=tasks_.find(id);
    return it==tasks_.end()?nullptr:it->second.lock();
}
void MySockTaskManager::recordMessage(std::size_t n) { ++messages_; bytes_.fetch_add(n); }
void MySockTaskManager::recordRejected() { ++rejected_; }
void MySockTaskManager::recordIoError() { ++io_errors_; }
void MySockTaskManager::recordPeerDisconnect() { ++peer_disconnects_; }
void MySockTaskManager::recordProtocolError() { ++protocol_errors_; }
MySockTaskManager::Snapshot MySockTaskManager::snapshot() const {
    Snapshot s;
    s.accepted=accepted_; s.closed=closed_; s.messages=messages_; s.bytes=bytes_;
    s.rejected=rejected_; s.io_errors=io_errors_; s.protocol_errors=protocol_errors_;
    s.peer_disconnects=peer_disconnects_;
    s.peak=peak_.load();
    { std::lock_guard<std::mutex> guard(mutex_); s.active=tasks_.size(); }
    return s;
}
}
