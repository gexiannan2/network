#pragma once
#include "../net_core.h"
#include <cstdint>
#include <string>
#include <vector>
namespace MyNameSpace {
class MySocket {
public:
    explicit MySocket(net::Socket sock=net::invalid_socket);
    ~MySocket();
    MySocket(const MySocket&)=delete;
    MySocket& operator=(const MySocket&)=delete;
    net::Socket socket() const { return sock_; }
    int rcvBuffer();   // >0 bytes read, 0 idle/EOF, -1 fatal; closed() distinguishes EOF
    int readBuffer(std::vector<char>& payload); // >0 payload length, 0 incomplete, -1 invalid
    int sendDataWithBuffer(const char* payload, std::uint32_t length); // bounded queue
    int syncSendBuf(); // flushed byte count; -1 on fatal
    // IOCP sends keep a private stable copy until the completion arrives.
    bool appendReceived(const char* data, std::size_t size);
    void markPeerClosed() { closed_ = true; }
    std::vector<char> copyPendingOutput(std::size_t max_size) const;
    void consumePendingOutput(std::size_t size);
    void closeSocket();
    bool hasPendingWrite() const { return out_offset_<out_.size(); }
    std::size_t pendingBytes() const { return out_.size()-out_offset_; }
    bool closed() const { return closed_; }
    std::uint64_t lastActivity() const { return last_activity_ns_; }
    std::uint16_t getRemotePort() const;
    std::string getRemoteIp() const;
private:
    net::Socket sock_;
    std::vector<char> input_;
    std::vector<char> out_;
    std::size_t out_offset_=0;
    bool closed_=false;
    std::uint64_t last_activity_ns_;
};
}
