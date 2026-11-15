#pragma once
#include "net_core.h"
#include <string>
namespace MyNameSpace {
class MyTcpServer {
public:
    MyTcpServer()=default;
    ~MyTcpServer();
    bool bindPort(int port,const std::string& host="127.0.0.1",int backlog=1024);
    net::Socket acceptCallBack();
    bool setNonBlock(bool enabled);
private:
    net::Socket sock_=net::invalid_socket;
};
}
