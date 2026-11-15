#include "MyServer.h"
#include "MyServerTask.h"
#include "base/MySockTaskManager.h"
#include <memory>
namespace MyNameSpace {
bool MyServer::init(int port,const std::string& host) {
    if(!pool_.init()) return false;
    if(!MyBaseServer::init(port,host)) {pool_.fini();return false;}
    return true;
}
bool MyServer::newTask(net::Socket fd) {
    auto task=std::make_shared<MyServerTask>(fd,++next_id_);
    auto& manager=MySockTaskManager::getInstance();
    manager.addTask(task);
    if(!pool_.addTask(task)) {
        manager.removeTask(task->getId());
        manager.recordRejected();
        return false; // local task destructor closes rejected socket
    }
    return true;
}
void MyServer::fini() {
    if(finalized_.exchange(true)) return;
    MyBaseServer::fini();
    pool_.fini();
}
}
