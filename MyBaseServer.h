#pragma once
#include "MyTcpServer.h"
#include <atomic>
#include <cstdint>
#include <string>
namespace MyNameSpace {
class MyBaseServer {
public:
    virtual ~MyBaseServer()=default;
    bool init(int port,const std::string& host="127.0.0.1",int backlog=1024);
    void mainLoop(int duration_seconds=0);
    virtual bool newTask(net::Socket fd)=0;
    virtual void fini() { complete_.store(true); }
    bool isFini() const { return complete_.load(); }
private:
    MyTcpServer tcp_;
    std::atomic<bool> complete_{false};
};
}
