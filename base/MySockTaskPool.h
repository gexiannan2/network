#pragma once
#include "MyThread.h"
#include "IoBackend.h"
#include "MySockTask.h"
#include <cstdint>
#include <memory>
namespace MyNameSpace {
class MyIoThread;
class MySockTaskPool {
public:
    MySockTaskPool(int ioCount=4,int maxConnPerIo=256);
    ~MySockTaskPool();
    bool init();
    bool addTask(const std::shared_ptr<MySockTask>& task);
    void fini();
    int getMaxCoonPerIo() const { return max_conn_per_io_; }
private:
    int io_count_,max_conn_per_io_;
    MyThreadPool pool_;
    bool running_=false;
};
}
