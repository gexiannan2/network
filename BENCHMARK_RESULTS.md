# MemoryDeal 原网络库改造 Benchmark（2026-11-15）

## 这次实际测的是什么？
被测服务端是改造后的**原工程网络类**，不是上一轮独立新写的单 Reactor Echo：
- 服务端源文件：server.cpp、MyTcpServer.cpp、MyBaseServer.cpp、MyServer.cpp、MyServerTask.cpp
- 网络库源文件：base/MySocket.cpp、base/MySockTask.cpp、base/MySockTaskPool.cpp、base/MySockTaskManager.cpp、base/MyThread.cpp、net_core.cpp
- 链接目标：build-original-network/libmemorydeal_network.a（MinGW）；Windows 服务端 build-original-network/net_server.exe
- 原交易撮合模块 / mem/ / libmemmory.so / shmget / shmat 均不在有效源码或构建目标中
- 4 个 I/O 工作线程；单线程管理各自连接，在线程内安全回收。Windows I/O=WSAPoll，Linux 代码路径=poll（Linux 尚未实机编译验证）
- 服务器对完整长度前缀 TCP 报文进行原样回显（Echo），不做任何业务计算

## 环境及方法
- Windows 11、本机 127.0.0.1:50114 TCP 回环
- MinGW-w64 GCC 14.2.0 / CMake 3.x / Ninja / Release C++17
- 网络帧：大端 uint32 payload 长度（4 B），有效 payload 1～65536 B
- 连接数与 Pipeline（每连接同时未完成请求数）可配置；客户端逐帧校验收到的数据内容
- 延迟百分位来自每个连接每第 16 次成功响应的抽样（每连接最多 4096 个样本），而非全量追踪
- 各场景单独执行；下表的请求数与速率是客户端实际程序输出，不是估算；客户端自身也消耗 CPU
- 结果仅能代表本机开发环境的压力参考，不是生产并发容量结论

## 吞吐量、延迟

| 并发 | Payload | Pipeline | 时长 | 成功请求 | req/s | Payload MiB/s | P50 ms | P95 ms | P99 ms | I/O错误 | 数据不一致 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 64 B | 1 | 2 s | 42,644 | 21,310.247 | 1.301 | 0.030 | 0.086 | 0.257 | 0 | 0 |
| 16 | 128 B | 1 | 5 s | 628,249 | 125,634.648 | 15.336 | 0.111 | 0.222 | 0.329 | 0 | 0 |
| 32 | 128 B | 2 | 3 s | 716,416 | 238,730.531 | 29.142 | 0.215 | 0.445 | 0.678 | 0 | 0 |
| 64 | 256 B | 4 | 5 s | 1,519,680 | 303,769.291 | 74.162 | 0.588 | 1.667 | 2.633 | 0 | 0 |
| 128 | 1024 B | 4 | 5 s | 1,371,872 | 274,163.733 | 267.738 | 1.395 | 3.525 | 4.914 | 0 | 0 |
| 256 | 128 B | 1 | 5 s | 612,196 | 122,247.003 | 14.923 | 1.842 | 3.215 | 3.892 | 0 | 0 |
| 512 | 128 B | 1 | 5 s | 594,494 | 118,566.104 | 14.473 | 4.027 | 5.849 | 6.987 | 0 | 0 |
| 8 | 65536 B | 2 | 4 s | 58,980 | 14,740.011 | 921.251 | 0.821 | 1.514 | 2.081 | 0 | 0 |
| 128 | 256 B | 8 | 30 s | 7,689,264 | 256,266.320 | 62.565 | 3.509 | 6.845 | 8.848 | 0 | 0 |

注意：Payload、连接数、Pipeline、运行时长在各行不同，不能从表中只凭吞吐量得出某个参数的单变量扩展规律。

30 秒、128 连接场景：客户端 CPU time=186.188 s，客户端峰值工作集=16.3125 MiB，成功请求 7,689,264，错误 0。

## 连接创建/关闭（Churn）
- 16 客户端线程 × 100 次 = 1,600 条新连接，1,600 成功，0 连接错误、0 I/O 错误、0 数据不一致；用时 2.89638 秒，552.414 conn/s
- 16 客户端线程 × 500 次 = 8,000 条新连接，8,000 成功，0 连接错误、0 I/O 错误、0 数据不一致；用时 11.8938 秒，672.618 conn/s
- Churn 测试后服务器累计快照：accepted=10,757，closed=10,757，active=0，peak_connections=512，io_errors=0，protocol_errors=4（对应对非法长度的预期拒绝）；服务器累计 peak_rss_mib=9.17969
- accepted/closed 包含本会话内不同阶段的客户端与协议测试，不是单次 Churn 独立指标；资源峰值也是该进程累计峰值。

## 协议边界用例
build-original-network/net_protocol_test.exe --port 50114：
1. PASS fragmentation（拆包）
2. PASS coalescing（粘包）
3. PASS 64KiB_max_payload
4. PASS half_close（客户端 SHUT_WR 后服务端仍完成 Echo）
5. PASS invalid_length_0
6. PASS invalid_length_65537

结果 **6/6 PASS**。

## 本机复现
命令行 1（工作目录 E:\MemorydDeal）：
```powershell
.\build-original-network\net_server.exe --host 127.0.0.1 --port 50114 --io-threads 4 --max-clients 1024
```
命令行 2：
```powershell
.\build-original-network\net_client.exe --host 127.0.0.1 --port 50114 --connections 128 --duration 30 --payload 256 --pipeline 8
.\build-original-network\net_protocol_test.exe --port 50114
.\build-original-network\net_churn_test.exe --port 50114 --threads 16 --per-thread 500 --payload 128
```

## 与上一轮独立 Echo 基线的区别
上一轮独立、单 Reactor 版本代码和报告另存到备份位置以及 BENCHMARK_STANDALONE_BASELINE.md，**不可将上一轮数据当作这次原网络库的结果**。这次的服务端实际调用原工程类结构，且从运行库中剥离了共享内存和订单撮合。

## 最终重建与服务运行复核
在给服务端增加 SIGINT/SIGTERM 退出请求处理后，重新执行 CMake/Ninja 全目标构建通过；`mingw32-make -j4 all` 也完整通过。最终启动服务端后验证：

- 运行时快照：net_server.exe，PID 2156，TCP 127.0.0.1:50114 处于 LISTENING（进程 PID 不保证长期不变）
- 64 连接、256 B、Pipeline 4、3 秒：1,020,416 成功，339,771.713 req/s，P99=2.613 ms，0 I/O 错误与 0 数据错误
- 协议测试重新执行 6/6 PASS
- 1,600 次短连接复测：成功 1,600，0 连接/I/O/数据错误，2.41255 s，663.197 conn/s

此轮复核仅验证最终构建和运行正常，历史 30 秒和 8,000 次 churn 数据属于本次改造前一版（新增信号退出处理前）的相同网络架构，二者不可被当作完全一致的二进制对照实验。

## 尚未通过的生产门槛
- Linux/epoll 原生编译及实测未完成；当前 Linux 支路是 poll，WSL2 本机无法启动
- 无多机网络延迟、真实 NIC 软中断和带宽瓶颈验证，也未做 24 小时稳定性/ASan/TSan/UBSan 或大规模模糊测试
- 缺少 TLS、认证、限速、反 DoS、监控告警、资源隔离等生产安全设施
- 单个客户端生成器为 one-thread-per-connection；512 以上高并发不宜直接用此生成器外推

Benchmark 性能在受测机器、进程调度和压力机配置变化下会变化；请保留原始命令、输出、CPU 和内存信息进行可重复实验。
