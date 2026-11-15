# MemorydDeal 跨平台 I/O 改造与阶段性验收（2026-11-15）

> **结论**：Windows 原生 MSVC x64 / IOCP 的 10,000 TCP 长连接、10,000 连接下消息吞吐、8,000 次连接抖动、3,000 个慢读客户端均有实测记录；30～60 秒级场景不能替代 24 小时长稳。Linux 仅在 WSL2 上编译/测试，不能冒充独立原生 Linux 服务器的验收。

## 1. 代码与构建

| 组件 | 当前实现 | 验证 |
|---|---|---|
| 统一 I/O worker 合约 | `base/IoBackend.h`：`enqueue()`、`load()`，负责线程生命周期 | Linux / Windows 编译通过 |
| Linux I/O | `base/MySockTaskPool.cpp`：epoll LT（Reactor） | WSL2 Ubuntu GCC 13.3 C++20 Release |
| Windows I/O | `base/MySockTaskPool.cpp`：`CreateIoCompletionPort`、`GetQueuedCompletionStatus`、OVERLAPPED `WSARecv/WSASend` | **Windows 原生** VS2026 MSVC 14.52 / Ninja / C++20 Release |
| 异步 Buffer 生命周期 | I/O 操作持有独立 `std::vector<char>` 发送/接收存储、`shared_ptr` 连接引用；完成后释放 | 功能/负载冒烟通过；生产内存安全仍需全面验证 |
| 构建目标 | `net_server`、`net_client`、`net_protocol_test`、`net_churn_test`、`net_load_test` | 全目标构建通过 |

**限制**：这个 `IoBackend` 目前是统一 worker 管理接口，平台 I/O 循环依旧在同一文件的编译分支中，不是已经独立发布的通用传输层 API。

## 2. Windows 原生 IOCP 实测

测试机为 Windows 11，本机 TCP 回环；测试脚本由 WebCodex 在本机执行，二进制位于 `build-iocp-win3`。**这是 Echo 网络层测试，不执行 MMO 登录、AOI、30Hz Tick、战斗或数据库写入。**

| 测试项目 | 实测结果 | 判定 |
|---|---|---|
| 10,000 长连接（12 秒活跃） | 客户端 10,000/10,000；20,000 Echo；`SERVER_FINAL accepted=10000 closed=10000 active=0`，`io_errors=0`，峰值工作集 174.426 MiB | 通过 |
| 10,000 连接消息吞吐 | 10,000/10,000；204,512 Echo；12 秒配置活跃期客户端输出 17,042.667 msg/s；完整客户端耗时 23.786 秒，平均 Echo RTT 0.939ms，0 错误 | 网络功能通过；目标吞吐/SLO 尚未验收 |
| 10,000 长连接（60 秒活跃） | 10,000/10,000；30,000 Echo；客户端完整耗时 104.042 秒，平均 Echo RTT 5.121ms；最终 `accepted=10000 closed=10000 active=0`，`io_errors=0`，峰值 174.703 MiB | 网络层通过；原批处理因系统 PATH 缺少 `findstr` 误报失败，已由原始日志复核 |
| 8,000 次 Churn | 16 压测线程×500 次；成功 8,000/8,000；16.3278 秒、489.962 conn/s；0 连接/I/O/数据错误；服务端 `accepted=8000 closed=8000 active=0`；峰值同时在线 26 | 通过（不等于 10K 同时重连风暴） |
| 3,000 慢读客户端 | 3,000/3,000；客户端发送 107,200 帧、`would_block=57780`；服务端处理 92,160 帧；320 次对端断开单独归类；内部 `io_errors=0`；峰值工作集 391.785 MiB；最终连接 0 | 指定场景通过，需继续 10K 慢客户端与内存预算测试 |
| 新版 PowerShell soak runner 冒烟 | 1,000/1,000；3 秒活跃；2,000 Echo；0 错误；`SERVER_FINAL` 连接清零；PowerShell 退出码 0 | 通过 |

**吞吐统计注意**：`net_load_test` 的 `echo_per_sec` 用配置的活跃时长作分母，而 `elapsed_s` 含连接建立、预热等时间。不能将 17,043 msg/s 解释为全过程速率，也不能把服务器从启动到退出的整体 `requests_per_sec` 与活跃期速率直接比较。目前 10K 场景仅有平均 Echo RTT，缺少可靠的 P99/P99.9 尾延迟分布。

## 3. WSL2 Linux（epoll）实测，不等于原生 Linux

| 场景 | 结果 | 判定 |
|---|---|---|
| GCC 13.3 C++20 Release 构建 | 所有网络库和测试目标构建成功 | 通过 |
| 新版 Linux soak runner 1,000 连接 / 3 秒 | `connected=1000`，2,000 Echo；服务端 1,000/1,000 回收；0 错误 | 通过 |
| WSL2 固定源端口 10K | 9,000 成功，1,000 个本地 `bind` 失败（EADDRINUSE=98）；9000 全部回收 | 未通过，压测源端口复用问题 |
| WSL2 自动源端口 10K | `peak_connections=10000`；服务端 `accepted=10000 closed=10000 active=0 io_errors=0`；客户端心跳期间有部分连接提前断开，整轮执行达到 150 秒超时 | **未通过完整端到端验收** |
| Linux ASan+UBSan 短时回归 | Debug/GCC13.3 构建通过；协议 6/6；Churn 400/400；32 连接、128B、Pipeline 2、3 秒 124,778 Echo（约 41,396 req/s），P99 17.827ms；服务器最终 438/438 回收、0 I/O 错误；无 ASan/UBSan/LSan 报错 | 短时通过，非 10K/24h 内存安全验收 |

Linux 侧源端口冲突和本机环回压测器调度负担需与服务端缺陷区别分析。即使 WSL2 达到连接峰值也不能代表独立 Linux 生产机在真实网卡上的容量。

## 4. 可重复执行的长稳脚本

**原生 Windows PowerShell（安装 VS2026 + 构建完成）：**

```powershell
cd E:\MemorydDeal
.\build-iocp-win3\build_native.cmd
powershell.exe -ExecutionPolicy Bypass -File .\bench\windows_native_soak.ps1 -DurationSeconds 86400 -Connections 10000 -Port 50525
```

**真正原生 Linux 机器上（Linux 主机本地 checkout）：**

```bash
cd /path/to/MemorydDeal
bash bench/linux_native_soak.sh 86400 10000 50524
```

脚本会构建 Release（Linux）或使用 MSVC Release 二进制（Windows），持有连接、按周期心跳、核对 Echo、保存服务端/客户端日志与结束指标。结果位于 `bench/results`。Linux 脚本默认拒绝 WSL2；仅在开发冒烟时使用 `ALLOW_WSL=1`，并明确输出“非原生 Linux 验收”。

**24 小时长稳尚未运行完成，不能标记通过。** 运行前应确认磁盘容量、TCP 端口/文件描述符限制、独立压测机和不会影响其他服务的端口，长期运行中监测 CPU、RSS、系统网络缓冲、TCP Retrans、FD、上下线曲线。

## 5. 生产上线前仍未完成

1. 真实原生 Linux 主机部署并在独立压测机上复现 10K 与 24 小时长稳；目前 WebCodex 只看到 Windows 主机上的 WSL2 Linux。
2. 10,000 **同时**断线重连风暴与 10,000 慢读客户端，不只是连续 8,000 次 Churn 或 3,000 慢读。
3. MMO 业务层压测：鉴权、地图 AOI 1:N、移动/战斗、30Hz Tick、日志和持久化，以及明确的 CPU、内存、网络、P99/P99.9 SLO。
4. 网络库内存与并发安全：ASan/UBSan/LSan 仅做了小规模短时回归；还需 10K/长稳下诊断、Windows 运行时检查，以及 IOCP 关闭时未决完成项回收边界验证。
5. 生产级限流/背压、异常连接处理、监控告警、滚动升级、运营环境网络安全及故障注入。

**总评：Windows IOCP 网络层原型已通过多种规模的功能/压力测试；统一 I/O 抽象已初步落地。当前不能宣称“单点 10,000 MMO 玩家生产达标”或“跨原生 Linux/Windows 24 小时验收完成”。**
