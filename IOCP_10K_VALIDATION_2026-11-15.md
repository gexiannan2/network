# MemorydDeal — 跨平台 IOCP / epoll 网络库验证报告

日期：2026-11-15  
项目：E:\MemorydDeal（WSL 路径 /mnt/e/MemorydDeal）  
结论：**Windows 原生 IOCP、WSL2 Linux epoll 已完成第一阶段 10000 TCP 连接验证。尚不能宣布达到 MMO 生产验收标准。**

## 1. 实现与构建

| 文件 | 内容 |
| --- | --- |
| `base/IoBackend.h` | 跨平台 I/O worker 抽象：`enqueue()`、`load()` |
| `base/MySockTaskPool.cpp` | Linux `epoll` LT Reactor，Windows `IOCP` + OVERLAPPED 完成队列，其他 POSIX 平台保留 `poll` 后备 |
| `base/MySocket.*`, `base/MySockTask.h` | 统一收发缓冲辅助操作，IOCP 操作拥有稳定的独立内存，连接关闭辅助函数 |
| `net_core.*` | Winsock/POSIX 类型统一、`NOMINMAX`、断开错误分类 |
| `base/MySockTaskManager.*` | `peer_disconnects` 与 `io_errors` 分开计数 |
| `net_load_test.cpp` | `hold`、`throughput`、`slow` 三种负载；大连接数使用少量工作线程 |
| `net_churn_test.cpp` | 新增 `--timeout-ms` 和错误诊断 |
| `CMakeLists.txt`、`Makefile` | 统一 C++20，加入 `net_load_test` 构建 |

**真正的 I/O 模型**：Windows 为 `CreateIoCompletionPort`、`GetQueuedCompletionStatus`、异步 `WSARecv` / `WSASend`；Linux 为 `epoll_wait` + 非阻塞 `recv/send`。统一的是 worker 生命周期/任务分配接口，跨平台 readiness/completion 语义没有假装完全相同。

构建：
- Windows 11，Visual Studio 2026 Community，MSVC 19.52，Windows SDK 10.0.26100，x64 Release：**通过**，使用 `build-iocp-win3/build_native.cmd`。
- WSL2 Ubuntu，GCC 13.3，x64 Release，Linux 内核 `6.18.58.1-microsoft-standard-WSL2`：**通过**，目录 `build-iocp-wsl`。
- WSL2 Linux 的 `AddressSanitizer + UndefinedBehaviorSanitizer` 编译：**通过**，目录 `build-iocp-asan`。
- **未执行独立物理机/VM 内的原生 Linux 压测**，当前仅有 Windows 桌面 Runner 与 WSL2，不能将 WSL2 结果冒充原生 Linux。

## 2. 实测矩阵

| 场景 | Windows 原生 IOCP | WSL2 Linux epoll | 备注 |
| --- | --- | --- | --- |
| 协议碎包/粘包/非法包长/半关闭 | 6 / 6 通过 | 6 / 6 通过 | 非法包长产生预期 protocol_errors=2 |
| 基础连接抖动 | 800 / 800 | 800 / 800 | 连接、Echo、关闭 |
| 16 连接短吞吐，128B，pipeline=2，4s | 81,580 req/s，P99 1.183ms | 69,021 req/s，P99 0.774ms | 本地 Echo |
| 10000 静态长连接 | 10000 / 10000 | 10000 / 10000 | 服务端峰值连接均为10000 |
| 10000 连接，10% 活跃，128B，12s | 204,512 条；17,043 req/s；平均0.939ms | 497,534 条；41,461 req/s；平均0.386ms | 客户端同步往返回包、非 MMO 业务 |
| 3000 连接，10% 慢客户端，4096B，10s | 收到92,160消息；峰值 RSS391.8MiB；io_errors=0；peer_disconnects=320 | 收到90,689消息；峰值 RSS345.4MiB；io_errors=0（客户端关闭前） | 慢客户端发送但不读；Windows reset 与真正 I/O 错误分开 |
| 10000 连接保持120s，心跳间隔15s | 70,000回包；io_errors=0；peak RSS174.4MiB；连接全部回收 | 70,000回包；io_errors=0；peak RSS7.8MiB；连接全部回收 | WSL2 本轮客户端源地址重试1273次，全部恢复 |
| Windows 10016 次连接抖动，单次2.5秒超时 | 10004成功，12次客户端 I/O 失败 | — | 未通过；对端断开12，服务端 I/O 错误0 |
| Windows 10016 次连接抖动，单次10秒超时 | **10016 / 10016**；总耗时11.69s，约856次/s | — | 通过；服务端 io_errors/peer_disconnects均0 |
| ASan+UBSan 回归 | — | 6/6协议，400/400连接抖动，1000/1000长连接 | 未报告 ASan/UBSan/LSan 错误 |

### 指标解释与局限

1. Windows RSS 为 Windows Working Set 指标，WSL2 的 Linux RSS 基于 `getrusage`；进程 RSS **不包括全部内核 Socket 缓冲开销**，两平台不能简单按 174 / 7.8 比例比较。
2. 16 连接短吞吐与 10000 连接下 10% 活跃测试的并发、pipeline 和系统负载不同，不能直接比较吞吐数值。
3. Windows 慢客户端 320 个 `peer_disconnects` 是对端未读取响应后关闭/重置；分类修正后真正 `io_errors=0`。这不代表慢客户端内存开销可忽略，应增加发送队列配额和监控告警。
4. Linux 固定源端口大量建连存在 `EADDRINUSE=98` 冲突，备用回环地址重试恢复；不是服务端拒绝连接。
5. 120 秒只是**短时间 soak**，不是 24 小时持续稳定性证明；目前还未证明真实 MMO Tick/AOI/技能场景可以支持10000在线。

## 3. 复现实验

### Windows 原生（VS2026 开发环境）

先执行已验证的构建脚本：

```bat
cd /d E:\MemorydDeal
build-iocp-win3\build_native.cmd
```

打开独立终端启动服务端：

```bat
cd /d E:\MemorydDeal
build-iocp-win3\net_server.exe --host 127.0.0.1 --port 50430 --io-threads 8 --max-clients 10000
```

另一个终端执行：

```bat
build-iocp-win3\net_load_test.exe --mode hold --port 50430 --connections 10000 --threads 16 --ramp-ms 4000 --duration 86400 --heartbeat-seconds 15
build-iocp-win3\net_churn_test.exe --port 50430 --threads 32 --per-thread 313 --payload 128 --timeout-ms 10000
```

注意：24 小时保持测试期间不要运行同一端口上的连接抖动测试，分别启动服务端/安排独立时间窗口。测试脚本在 `build-iocp-win3` 中亦有 120s/吞吐/慢客户端等独立 cmd。

### 真正的原生 Linux（待在独立 Linux 主机执行）

```bash
cd /path/to/MemorydDeal
ulimit -n 65535
cmake -S . -B build/native-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-release -j16
./build/native-release/net_server --host 127.0.0.1 --port 19071 --max-clients 10000 --io-threads 8
```

第二终端：

```bash
cd /path/to/MemorydDeal
ulimit -n 65535
./build/native-release/net_load_test --mode hold --host 127.0.0.1 --port 19071 --connections 10000 --threads 16 --duration 86400 --ramp-ms 4000 --heartbeat-seconds 15 --source-port-start 55000
```

源端口区间、`ulimit`、主机回环地址与本地防火墙规则需按 Linux 机器实际情况调整；对于独立的压测发起机应使用其实际网卡地址/服务端监听地址，而非回环地址。

## 4. 生产前仍需完成的验收项

1. **原生 Linux**：在独立物理机/VM 进行同样构建及10000连接测试，留存实际 `uname -a`、CPU、RAM、`ulimit -n`、系统 TCP 配置。
2. **长稳 24h/72h**：连接持续在线，连接数曲线、服务端 RSS/Commit、Windows Handles、Linux FD、CPU、P99/P999、丢包和意外断线，设置报警阈值；当前仅通过120秒。
3. **MMO 业务负载**：每 Tick 的AOI进入/离开广播、100人密集战斗、1000人主城、移动/技能/AI、玩家落盘；当前仅纯 Echo。
4. **故障注入**：半关闭、RST风暴、网络抖动、异步IO取消、连接关闭与回调竞态、慢客户端读写背压；Windows 需额外用 Application Verifier/调试堆检查资源生命周期。
5. **事件循环架构**：当前统一 worker 抽象最小化；后续可将 `EpollBackend` / `IocpBackend` 拆为独立编译单元，通过更完整的事件/完成通知接口解耦业务层。
6. **业务级指标**：独立压力发生器避免单机压测瓶颈，至少测连接建立速率、长稳峰值在线、吞吐/延迟分布以及服务器 Tick 预算。

**结论**：Windows IOCP 功能已落地并在原生 Windows 路径下证实10000 TCP 长连接和120秒稳定；Linux epoll 已在 WSL2 验证相同连接数与时长。独立原生 Linux 与真实 MMO/24h 生产验收尚未完成。
