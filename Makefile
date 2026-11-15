# MemoryDeal original network classes only; shared memory and matching excluded.
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall -Wextra -Wpedantic -pthread
ifeq ($(OS),Windows_NT)
EXE = .exe
LIBS = -lws2_32 -lpsapi
else
EXE =
LIBS = -pthread
endif
NETWORK_SOURCES = net_core.cpp MyTcpServer.cpp MyBaseServer.cpp MyServer.cpp MyServerTask.cpp \
                   base/MySocket.cpp base/MySockTask.cpp base/MySockTaskManager.cpp \
                   base/MySockTaskPool.cpp base/MyThread.cpp

.PHONY: all server client benchmark clean
all: net_server$(EXE) net_client$(EXE) net_protocol_test$(EXE) net_churn_test$(EXE) net_load_test$(EXE)
server: net_server$(EXE)
client: net_client$(EXE)
net_server$(EXE): server.cpp $(NETWORK_SOURCES) net_core.h
	$(CXX) $(CXXFLAGS) -I. server.cpp $(NETWORK_SOURCES) -o $@ $(LIBS)
net_client$(EXE): net_client.cpp $(NETWORK_SOURCES) net_core.h
	$(CXX) $(CXXFLAGS) -I. net_client.cpp $(NETWORK_SOURCES) -o $@ $(LIBS)
net_protocol_test$(EXE): net_protocol_test.cpp $(NETWORK_SOURCES) net_core.h
	$(CXX) $(CXXFLAGS) -I. net_protocol_test.cpp $(NETWORK_SOURCES) -o $@ $(LIBS)
net_churn_test$(EXE): net_churn_test.cpp $(NETWORK_SOURCES) net_core.h
	$(CXX) $(CXXFLAGS) -I. net_churn_test.cpp $(NETWORK_SOURCES) -o $@ $(LIBS)
net_load_test$(EXE): net_load_test.cpp $(NETWORK_SOURCES) net_core.h
	$(CXX) $(CXXFLAGS) -I. net_load_test.cpp $(NETWORK_SOURCES) -o $@ $(LIBS)
benchmark: net_client$(EXE)
	./net_client$(EXE) --host 127.0.0.1 --port 50114 --connections 16 --duration 5 --payload 128 --pipeline 1
clean:
	$(RM) net_server$(EXE) net_client$(EXE) net_protocol_test$(EXE) net_churn_test$(EXE) net_load_test$(EXE)
