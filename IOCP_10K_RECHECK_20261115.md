# IOCP / epoll — 10000 连接复核记录（2026-11-15）

工程：`E:\MemorydDeal`；编译标准：C++20。本文是**新一轮复测**，与已有的 `IOCP_10K_VALIDATION_2026-11-15.md` 历史报告分开记录。

## 范围与平台

- **Windows 原生**：Windows 11；`build-iocp-win3` MSVC Release；真实 Windows `CreateIoCompletionPort` + `GetQueuedCompletionStatus` + `WSARecv/WSASend`。
- **Linux 兼容性测试**：WSL2 Ubuntu 24.04.3，内核 `6.18.58.1-microsoft-standard-WSL2`，GCC 13.3；`build-iocp-wsl`；`epoll` + 非阻塞 `recv/send`。
- **独立原生 Linux 物理机或 VM**：未连接/未执行。WSL2 不等于独立原生 Linux。
- 环境为同机器 TCP 回环；负载生成器会消耗客户端 CPU。除明确注明外，均为 Echo，没有登录、移动、AOI、战斗或 30 Hz Tick。

## 当前源码架构复核

- `base/IoBackend.h`：统一 `ready()`、`enqueue()`、`load()` 与继承的 worker 生命周期；是**最小 worker 接口**，非完全统一的异步 I/O completion API。
- `base/MySockTaskPool.cpp`：Windows 的 `runIocp()`；Linux 的 `runEpoll()`；其他 POSIX 平台的 `runPoll()`。仍在同一源文件内，尚未拆分独立平台编译单元。
- `base/MySocket.cpp`：输入及输出缓冲、长度帧协议、IOCP 输出复制与发送完成消费。
- `server.cpp`：`--duration` 允许 0..172800 秒，支持 24 小时 + 服务端退出余量。
- `net_load_test.cpp`：`hold`、`throughput`、`slow`。注意 `echo_per_sec` 使用配置的负载阶段 `duration_s` 计算；`elapsed_s` 包含建连、ramp 与部分退出/处理时间，不能把两者当作相同区间。

## 本轮实测（重新编译、重新运行）

| 测试 | Windows 原生 IOCP | WSL2 Linux epoll |
| --- | --- | --- |
| CMake Release 全目标构建 | 通过 | 通过 |
| 协议回归 | 6/6 PASS | 6/6 PASS |
| 800 次短连接 | 800/800，0 错误 | 800/800，0 错误 |
| 16 连接，128 B，Pipeline 2，短时 Echo | 99,856.146 req/s；P99 1.071ms，4s | 66,873.090 req/s；P99 0.875ms，3s |
| 10000 保持连接、心跳（配置 12s） | 10000/10000，Echo 10000，0 I/O/数据错误；客户端 elapsed 25.526s，采样 P99 38.296ms | 10000/10000，Echo 20000，0 I/O/数据错误；客户端 elapsed 35.114s，采样 P99 3.294ms |
| 10000 连接、10% 活跃、128 B、配置 12s | 127,476 Echo；工具报告 10,623 req/s；P99 17.968ms；0 错误 | 576,552 Echo；工具报告 48,046 req/s；P99 2.331ms；0 错误 |
| 3000 连接、10% 慢客户端、4096 B、10s | 3000/3000；`would_block=14040`；0 客户端 I/O 错误；服务端峰值 RSS 390.91 MiB | 3000/3000；`would_block=203462`；0 客户端 I/O 错误；服务端峰值 RSS 341.566 MiB；`peer_disconnects=316` |
| 10016 次连接抖动 | 10016/10016、0 错误；12.0644s，约 830.21 次/秒 | 本轮未执行 10016 次抖动，不能列为通过 |
| 长稳执行脚本短测 | `windows_native_soak.ps1`：128 连接，8s，通过 | `linux_native_soak.sh`：WSL2 显式允许模式，128 连接，8s，通过 |

注意 Windows/WSL 同机且测试时长、环境竞争可能不同；不能用此表推断平台间性能倍数。慢客户端测试不读取 Echo，因而没有有效的 Echo P99。WSL 源端口冲突发生重试，本轮万连接 hold `retries=4501 recovered=4501`，throughput `retries=447 recovered=447`，均无最终连接失败。

### 先前 120 秒记录（已核对日志，非本轮重新跑 120 秒）

- `build-iocp-win3/soak10k-server.log`：`SERVER_FINAL accepted=10000 closed=10000 active=0 peak_connections=10000 messages=70000 io_errors=0`。
- `build-iocp-wsl/soak10k-19071-server.log`：同样 `accepted=10000 closed=10000 active=0 peak_connections=10000 messages=70000 io_errors=0`。
- **目前最长有日志证据的 10000 连接 soak 是 120 秒，尚无 24h/72h 完成证据。**

## 可重复执行的长稳脚本

Windows 原生 PowerShell（已用 128 连接、8 秒验证执行机制）：

```powershell
cd E:\MemorydDeal
.\bench\windows_native_soak.ps1 -DurationSeconds 86400 -Connections 10000 -Port 50525
```

独立原生 Linux（尚无目标主机连接与实际结果；脚本默认拒绝 WSL）：

```bash
cd /path/to/MemorydDeal
bash bench/linux_native_soak.sh 86400 10000 50524
```

这些是**前台执行命令**，不是已经开始的后台计划或已完成的 24 小时验收。不要在未监控的生产服务器直接运行。建议独立压力机、空闲测试端口和专用测试环境；保存日志、资源曲线及负载生成器硬件规格。默认回环地址只用于同机开发测试，跨机须更新监听和发压目标。

## 生产级剩余问题及优先级

1. **P0 — IOCP 关闭与取消竞态**：`base/MySockTaskPool.cpp::runIocp()/handleCompletion()/closeConnection()`，`outstanding_` 若 5 秒内无法 drain 则记录 I/O 错误并退出循环；仍需在故障注入下验证未完成 OVERLAPPED 的内存与句柄生命周期。建议 Windows Application Verifier、异常断网/RST、强制取消、线程退出测试。
2. **P0 — 慢读与全局背压**：`MySocket` 每连接输出缓冲 2 MiB 上限，并不等于每进程全局内存配额。3000 慢客户端已测到 340–391 MiB 用户态峰值 RSS。需为每客户端/全局出站字节数、水位线、踢出和降级定义容量预算。
3. **P0 — 原生 Linux 缺失**：在独立 Linux VM/裸金属执行构建、10000 长连接、吞吐、churn、慢读、24h/72h soak，记录 `uname -a`、CPU、RAM、FD 上限、TCP 配置及错误统计。
4. **P0 — 真实 MMO 负载缺失**：Echo 不包含 30 Hz Tick、AOI 1:N、多人密集战斗、NPC AI、玩家上下线、落盘、流控与业务队列。
5. **P1 — I/O 抽象仍较薄**：目前 `IoBackend` 抽象生命周期/连接分派；如需向业务层完全屏蔽 readiness/completion 差异，需要定义完整的事件回调/任务所有权及发送状态机，并分别拆分实现及测试。
6. **P1 — 负载统计与误差预算**：采样 P99 依赖客户端调度；需要独立压力机、服务端分段计时、P99.9、稳定连接数时序和超过阈值的失败判定。
7. **P1 — 内存与线程验证**：Windows Application Verifier；Linux ASan + UBSan、独立 TSan；保留 core、异常栈和 24h 进程/内核内存曲线。

## 验收结论

**已证实**：原生 Windows IOCP 与 WSL2 Linux epoll 的基本功能、10000 TCP 连接、短时 Echo 吞吐、连接抖动（Windows）、慢客户端背压、长稳脚本短测；有双方 120 秒万连接历史日志。

**未证实**：独立原生 Linux 运行结果、24 小时万连接稳定、真实 MMO 业务消息负载、AOI 广播容量与生产 SLO。不能宣布生产级 MMO 10000 玩家已验收。
