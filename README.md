# MemoryDeal：原网络库纯净版

此工程最初是一个 Linux C++ 交易撮合原型，网络层与 System V 共享内存/订单/股票业务耦合。现在**只保留原有网络模块的类和层次结构**，并用纯 Echo 请求处理器验证收发、分包和压力承载。

## 当前保留的组件
- MyTcpServer / MyBaseServer / MyServer：TCP 监听、Accept、服务生命周期
- base/MySocket：TCP 帧缓冲、粘包/半包解析、部分发送、限量输出队列
- base/MySockTask / MyServerTask：连接任务与 Echo 回调（无订单撮合）
- base/MySockTaskPool：多个 I/O 线程、连接分配与线程内回收
- base/MySockTaskManager：连接注册、计数、性能指标，弱引用管理防悬空
- base/MyThread：std::thread 包装，取代旧 pthread 线程生命周期细节
- net_core.h/.cpp：跨平台 socket 基础封装；base/IoBackend.h 与 base/MySockTaskPool.cpp 统一 worker 接口，Windows 使用 IOCP（WSARecv/WSASend），Linux 使用 epoll
- net_client.cpp、net_protocol_test.cpp、net_churn_test.cpp、net_load_test.cpp：Echo Benchmark、协议、短连接和 10000 长连接/慢客户端负载测试

不再存在活动的 mem/、MatchingOrder、交易结构、股票订单、libmemmory.so、InitShareMemory、shmget、shmat 等构建与运行依赖。原始源码已在 **C:\Users\gexiannan\MemorydDeal_legacy_backup_20261115** 保存；上一轮独立 Echo 基线源码与构建产物在 **C:\Users\gexiannan\MemorydDeal_standalone_baseline_20261115** 保存。根目录的 .git 历史元数据仍保留。

**重要：**这是对原有网络类的实质性去业务化、线程安全和事件轮询改造，而不是原封不动的旧实现。当前 Windows 使用 IOCP、Linux 使用 epoll，均有编译和回环测试；Windows 原生 IOCP 已完成 10000 连接和 120 秒心跳/慢读/断连测试；Linux 已在 WSL2 环境验证，但尚无独立原生 Linux 机器的生产容量认证。

## 协议
TCP：4 字节大端 unsigned 长度 + 1～65536 B payload。服务端完整接收后原样 Echo。对超长/零长度包断连；线程内收发缓冲不超过 2 MiB（输出排队上限），无交易/共享内存操作。默认仅绑定 127.0.0.1:50114，不开放公网。

## 构建

Windows 原生 VS2026 / MSVC / C++20（本轮 IOCP 验证的主构建，先进入 x64 Developer Command Prompt）：
```cmd
cd /d E:\MemorydDeal
cmake -S . -B build-iocp-win3 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-iocp-win3 --parallel 8
```
输出位于 `build-iocp-win3/`，包括 `net_server.exe`、`net_load_test.exe` 和协议/短连接测试。已使用 Windows 原生程序完成万连接、慢读、churn 和 120 秒长稳。
备用 Windows MinGW GCC 14.2 + Ninja 构建（历史配置；本轮原生 IOCP 主测试使用上面的 MSVC 构建）：
```powershell
cd E:\MemorydDeal
cmake -S . -B build-original-network -G Ninja -DCMAKE_CXX_COMPILER=C:/mingw64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=C:/mingw64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Release
cmake --build build-original-network --parallel 4
```
也已验证在项目根目录执行 `C:\mingw64\bin\mingw32-make.exe -j4 all`。

Linux：
```bash
cmake -S . -B build-iocp-wsl -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release
cmake --build build-iocp-wsl -j4
```
Linux 已在 WSL2 Ubuntu 24.04 / GCC 13.3 完成 Release 与 ASan+UBSan 编译及协议/回环测试；该 WSL 未安装 Ninja，因此实测使用 CMake Unix Makefiles。

主要构建产物：Windows MSVC `build-iocp-win3/memorydeal_network.lib`、`build-iocp-win3/net_server.exe`、`build-iocp-win3/net_load_test.exe`；WSL2 Linux `build-iocp-wsl/libmemorydeal_network.a`、`build-iocp-wsl/net_server`、`build-iocp-wsl/net_load_test`。原 `build-original-network` 是历史构建目录，可能包含旧二进制；不要将它用于最新性能报告。

## 启动和 Benchmark
终端 1：
```powershell
cd E:\MemorydDeal
.\build-iocp-win3\net_server.exe --host 127.0.0.1 --port 50114 --io-threads 8 --max-clients 10000
```
终端 2：
```powershell
cd E:\MemorydDeal
.\build-iocp-win3\net_client.exe --port 50114 --connections 64 --duration 5 --payload 256 --pipeline 4
.\build-iocp-win3\net_protocol_test.exe --port 50114
.\build-iocp-win3\net_churn_test.exe --port 50114 --threads 16 --per-thread 500 --payload 128
.\build-iocp-win3\net_load_test.exe --mode hold --port 50114 --connections 10000 --threads 16 --duration 120 --heartbeat-seconds 20
```

完整测试数据：**BENCHMARK_RESULTS.md（改造后的原网络库）**。
历史对照：BENCHMARK_STANDALONE_BASELINE.md（前一轮独立 Echo 实现，不能混用）。
服务端默认限制 1024 活动连接，启动时可用 `--max-clients 10000` 指定 10000 上限。旧 `net_client` 最多 512 连接且每连接一个线程；新增 `bench/tenk_connections.py` 用于 Linux/WSL2 异步模拟 10000 TCP 连接。Linux 1 万连接轻载 Echo 已验证，但高负载尾延迟不达标；ASan+UBSan 短时烟测通过，TSan、24 小时长稳和跨机测试未完成。

## 生产化注意
当前 Windows 与 Linux 都已完成回环网络测试。Linux 已改为 epoll，每个连接由对应 I/O worker 持有并在其线程中回收，但 Echo 吞吐量不等于 MMO 生产并发能力。仍需在真实 Linux 部署环境下进行 24 小时以上长稳、TSan/故障注入、独立压测机、30Hz Tick 与 AOI 广播压测，解决尾延迟和慢客户端背压问题，才能讨论上线验收。

## MMO 万连接阶段性测试

**最新 IOCP/epoll 报告：BENCHMARK_IOCP_10K_20261115.md**（Windows 原生 IOCP 万连接、Linux WSL2 对照、慢客户端、短连接抖动、120 秒长稳及未通过的生产门槛）。旧 **BENCHMARK_MMO_10K.md** 是 IOCP 改造前的历史基线，不能当作当前后端结论。
