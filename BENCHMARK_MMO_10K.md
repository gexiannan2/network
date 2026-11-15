# MemoryDeal MMO 10000 连接阶段性 Benchmark（2026-11-15）

> 历史基线：本文记录 Linux epoll / Windows WSAPoll 阶段，不代表后续 Windows IOCP。最新结果见 `BENCHMARK_IOCP_10K_20261115.md`。

## 目标与范围

目标是 MMO 单进程支持 10000 名持续在线玩家，并在真实业务流量、AOI 广播、30 Hz Tick 下满足明确的延迟、吞吐和稳定性 SLO。**当前仅进行了 TCP Echo 网络层实验，不是 MMO 生产验收。**

依据 `AGENTS.md` 的 Allman / 4 空格 / 最小修改规范。该规则文件前部的 `C:\e996_proj-nav`、`3dgame` 等说明并非本目录的构建布局；以下实际以 `E:\MemorydDeal\CMakeLists.txt` 为准。

## 代码改动

- `base/MySockTaskPool.cpp`：Linux I/O worker 使用 `epoll` 事件通知，Windows 保留 `WSAPoll`。Linux 一秒一次过期扫描，连接由原 I/O worker 持有并回收；相关 C++ 实现整理为 Allman 格式。不改变对外类或线协议。
- `bench/tenk_connections.py`：新增 Python asyncio 多连接负载生成器、可配置持续发送频率和源端口重试，避免原 `net_client` 每连接一线程、最多 512 连接的问题。仅适用于 Unix/Python resource 模块可用的平台。
- `.clang-format`：记录 AGENTS.md 对 C++ 风格的约束；未对整个仓库进行大规模格式化。
- `bench/windows_smoke.cmd`：Windows 构建后进行协议/Echo 回归的脚本。

原工程有大量已有未提交变化；未做 git commit、清理、回滚或修改第三方依赖。

## 环境

- Windows 11、MinGW-w64 GCC 14.2、CMake/Ninja；Windows 重新构建成功，并已运行 Windows PE 服务端/客户端回归（见最终复核）。
- WSL2 Ubuntu 24.04、GCC 13.3、CMake/Unix Makefiles（本 WSL 未安装 Ninja）。
- Linux TCP 127.0.0.1 本机回环，服务端 8 个 I/O worker、`--max-clients 10000`；本机压测器与服务端竞争 CPU。
- TCP 帧协议为 4B 大端长度 + 128B payload。Echo 不执行游戏逻辑。
- WSL `ip_local_port_range=50087 52134`（仅 2048 个可用系统自动分配端口），因此负载生成器通过显式绑定多组源端口进行验证。未改动系统参数。

## 已完成验证

| 项目 | 测试结果 |
|---|---|
| Linux GCC Release 全目标编译 | 通过，`/tmp/memorydeal-10k-release-make` |
| Windows MinGW Release 重新构建 | 通过，`build-original-network` |
| Linux 6 项协议测试 | 6/6 PASS |
| Linux 800 次 Churn | 800/800，无连接/I/O/数据错误 |
| Linux Echo 64 连接，256B，Pipeline 4，3s | 266389.958 req/s，P99 2.024ms，0 错误 |
| 10000 连接，128B，1 msg/s，5s | 10000/10000 建立；18.051s 建连；50000 Echo 完成，0 I/O/数据错误；客户端 9870.7 req/s，P50 19.122ms，P95 306.205ms，P99 398.481ms |
| 10000 连接，4 个独立压测进程，每个 2500 连接，5 msg/s，配置 6s | 全部建立并全部回收；总 149876 Echo 完成、0 I/O/连接错误；实际测试耗时超出配置时长；四组 P99 约 1701–1834ms，未达到目标 50000 msg/s |

单进程 1msg/s 场景服务端退出统计：`accepted=10000 closed=10000 active=0 messages=50000 io_errors=0 peak_rss_mib=7.37109`。**RSS 是用户态进程工作集，未包含 TCP 内核 socket 内存和系统网络缓冲。**

4 进程 5msg/s 场景服务端退出统计：`accepted=10000 closed=10000 active=0 messages=149876 io_errors=0 peak_rss_mib=7.1875`。四客户端实测 elapsed 为 8.726–9.960 秒；这种情况下不能把总消息数除以配置的 6 秒来报告已达成的吞吐。

**P99 测量包含负载生成器的 Python asyncio 调度延迟，尚未做独立压测机或分段服务端耗时测量，不能仅凭尾延迟归因于网络库。** Echo 1msg/s 轻载万连接测试成功，不等于 MMO 在线用户负载通过。

## 最终改动后的复核与补充记录

`base/MySockTaskPool.cpp` 又补充了 epoll 创建失败时的启动前检查：先在 worker 启动前获取 epoll FD，无法创建则使初始化失败；worker 停止并 join 后再关闭该 FD。这是原有连接生命周期路径中的防静默失败补丁。以下数据均来自补丁后的重新构建：

- **Linux Release 10000 连接终验**（8 I/O worker、128B、1 msg/s、5s，附带先跑 6/6 协议测试）：`connected=10000/10000`、建连 `16.283s`、完成 `49880` 条、`9942.3 req/s`，P50 `92.729ms` / P95 `613.862ms` / P99 `774.914ms`，0 连接/I/O/数据错误。服务端最终 `accepted=10006 closed=10006 active=0 peak_connections=10000 messages=49885 io_errors=0 protocol_errors=2`（多出来的连接和消息是协议边界测试）。这一轮依然**没有达到 MMO 尾延迟目标**。
- **Windows MinGW 重新编译与运行**：6/6 协议通过、0 I/O/数据错误、连接全部回收。64 连接/256B/Pipeline4/3s，第一次复测 `187114.995 req/s`、P99 `3.705ms`；最终补丁后再次测得 `45220.284 req/s`、P99 `36.993ms`。两次差异巨大，需控制客户端/CPU/后台负载并做多轮重复，不能将单次数据作为容量保证。
- **Linux ASan + UBSan Debug**：最终补丁版本编译通过；6/6 协议通过；32 连接、128B、Pipeline2、2s 达到约 `92442 req/s`、P99 `1.194ms`；400 次短连接全部成功，服务端 `accepted=438 closed=438 active=0`。本次测试没有报告 ASan/UBSan/LSan 错误；仅覆盖短时轻负载，TSan 和 24 小时长稳仍未验证。

本次测试未对 `MySocket` 的输入缓冲头部擦除、连接慢读背压与全局发送缓冲预算做深度压力验证，这些仍为性能与生产稳定性风险。原工程默认最高连接数仍为 1024，需在启动时显式指定 `--max-clients 10000`。

## 复现

Linux 构建：

```bash
cmake -S /mnt/e/MemorydDeal -B /tmp/memorydeal-10k-release-make -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/memorydeal-10k-release-make -j4
```

Linux Shell 1（将 `ulimit -n` 提高到足以容纳 10000 个 socket）：

```bash
ulimit -n 65535
/tmp/memorydeal-10k-release-make/net_server --host 127.0.0.1 --port 42014 --io-threads 8 --max-clients 10000
```

Linux Shell 2：

```bash
ulimit -n 65535
python3 /mnt/e/MemorydDeal/bench/tenk_connections.py --port 42014 --connections 10000 --source-start-port 20000 --duration 5 --rate 1 --payload 128
```

确保源端口区间未被占用，且不同运行轮次避免重用尚在 TIME_WAIT 的源端口。用独立压测机时不应依赖本机回环与此端口分配技巧。

## 生产验收缺口（尚未达到生产）

1. **业务模型**：真实登录/鉴权、会话恢复、移动/AOI 1:N 广播、技能、NPC、背包和 30Hz 游戏逻辑 Tick；Echo 完全不覆盖这些。
2. **尾延迟与容量**：当前 10000 连接实验 P99 很高，5 msg/s 流量未达到负载目标。必须分离客户端压测器调度、服务器入队/执行/发送及网络传输耗时，使用独立 Linux 机器压测。
3. **长期可靠性**：至少连续 24 小时，加入断线重连风暴、慢读客户端、半关闭、包损坏、过载、限速和背压，再做 ASan/UBSan/TSan 等验证。
4. **安全与运维**：协议认证、TLS/可信内网边界、连接限速、防 DoS、队列/CPU/内核内存监控、告警、日志采样、平滑停服。
5. **Windows 10K 能力**：Windows 当前仍是 WSAPoll，不具备已验证的 IOCP 万连接生产容量。
6. **跨机实测与可复现性**：生产内核、物理 NIC、CPU 亲和和软中断、多核资源配置、p99/p99.9、网络带宽、故障注入以及测试报告原始日志。

**当前结论：Linux 10000 TCP 连接及轻载 Echo 通过；不能宣称已经达到 10000 MMO 玩家生产环境验收。**
