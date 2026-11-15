#pragma once
#include "MyBaseServer.h"
#include "base/MySockTaskPool.h"
#include <atomic>
#include <cstdint>
namespace MyNameSpace {
class MyServer final : public MyBaseServer {
public:
    MyServer(int io_threads,int max_per_thread):pool_(io_threads,max_per_thread){}
    ~MyServer() override {fini();}
    bool init(int port,const std::string& host);
    bool newTask(net::Socket fd) override;
    void fini() override;
private:
    MySockTaskPool pool_;
    std::atomic<std::uint64_t> next_id_{0};
    std::atomic<bool> finalized_{false};
};
}
