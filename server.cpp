#include "MyServer.h"
#include "base/MySockTaskManager.h"
#include "net_core.h"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
namespace {
struct Options {
    std::string host="127.0.0.1";
    int port=50114;
    int io_threads=4;
    int max_clients=1024;
    int duration=0;
};
bool parse_int(const char* text,int min,int max,int& out) {
    char* end=nullptr;
    long v=std::strtol(text,&end,10);
    if(!*text||!end||*end||v<min||v>max) return false;
    out=static_cast<int>(v);return true;
}
bool parse(int argc,char** argv,Options& o) {
    for(int i=1;i<argc;++i) {
        std::string flag=argv[i];
        if(flag=="--help") {
            std::cout<<"net_server --host 127.0.0.1 --port 50114"
                <<" --io-threads 4 --max-clients 1024 --duration 0\n";
            std::exit(0);
        }
        if(i+1>=argc) return false;
        const char* v=argv[++i];
        if(flag=="--host") o.host=v;
        else if(flag=="--port"&&parse_int(v,1,65535,o.port)){}
        else if(flag=="--io-threads"&&parse_int(v,1,32,o.io_threads)){}
        else if(flag=="--max-clients"&&parse_int(v,1,10000,o.max_clients)){}
        else if(flag=="--duration"&&parse_int(v,0,172800,o.duration)){}
        else return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    Options opt;
    if(!parse(argc,argv,opt)) { std::cerr<<"Bad options; --help\n"; return 2; }
    net::Runtime runtime;
    if(!runtime.ok()) {std::cerr<<"Network initialization failed\n";return 2;}
#ifdef SIGPIPE
    std::signal(SIGPIPE,SIG_IGN);
#endif
    const int per_thread=(opt.max_clients+opt.io_threads-1)/opt.io_threads;
    MyNameSpace::MyServer server(opt.io_threads,per_thread);
    if(!server.init(opt.port,opt.host)) {
        std::cerr<<"Server init failed\n"; return 2;
    }
    std::cout<<"READY host="<<opt.host<<" port="<<opt.port
             <<" io_threads="<<opt.io_threads<<" max_clients="<<opt.max_clients
             <<" mode=original_network_classes_no_shared_memory"<<std::endl;
    server.mainLoop(opt.duration);
    auto s=MyNameSpace::MySockTaskManager::getInstance().snapshot();
    std::cout<<"SERVER_FINAL accepted="<<s.accepted<<" closed="<<s.closed
             <<" active="<<s.active<<" peak_connections="<<s.peak
             <<" messages="<<s.messages<<" payload_mib="<<s.bytes/1048576.0
             <<" rejected="<<s.rejected<<" io_errors="<<s.io_errors
              << " protocol_errors="<<s.protocol_errors<<" peer_disconnects="<<s.peer_disconnects
              <<net::process_metrics()<<std::endl;
    return s.io_errors?1:0;
}

