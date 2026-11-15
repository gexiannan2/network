#pragma once
#include "MySocket.h"
#include <cstdint>
#include <vector>
namespace MyNameSpace {
class MySockTask {
public:
    explicit MySockTask(net::Socket sock,std::uint64_t id):sock_(sock),id_(id){}
    virtual ~MySockTask()=default;
    MySockTask(const MySockTask&)=delete;
    MySockTask& operator=(const MySockTask&)=delete;
    std::uint64_t getId() const { return id_; }
    net::Socket socket() const { return sock_.socket(); }
    std::uint64_t lastActivity() const { return sock_.lastActivity(); }
    bool peerClosed() const { return sock_.closed(); }
    bool pendingWrite() const { return sock_.hasPendingWrite(); }
    std::size_t pendingBytes() const { return sock_.pendingBytes(); }
    int rcvBuffer() { return sock_.rcvBuffer(); }
    int syncSendBuf() { return sock_.syncSendBuf(); }
    bool appendReceived(const char* data, std::size_t size) { return sock_.appendReceived(data, size); }
    void markPeerClosed() { sock_.markPeerClosed(); }
    std::vector<char> copyPendingOutput(std::size_t max_size) const
    {
        return sock_.copyPendingOutput(max_size);
    }
    void consumePendingOutput(std::size_t size) { sock_.consumePendingOutput(size); }
    void closeSocket() { sock_.closeSocket(); }
    int sendDataWithBuffer(const char* data,std::uint32_t size) {return sock_.sendDataWithBuffer(data,size);}
    int getMsg(); // framing + virtual application handler, network-only
    virtual bool cmdParse(const char* payload,int length)=0;
private:
    MySocket sock_;
    std::uint64_t id_;
};
}
