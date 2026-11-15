# MemoryDeal：C++20 跨平台 IOCP/epoll 与万连接测试（2026-11-15）

## 结论（当前阶段）

Windows 原生 IOCP 已实现并在 VS2026/MSVC C++20 Release 下编译、实际运行；Linux epoll 在 WSL2 Ubuntu 24.04/GCC 13.3 C++20 下编译、实际运行。两者通过协议、Echo、万连接及不同程度的慢读/断连测试。**这仍是网络层 Echo 测试，不能据此宣称 10000 MMO 玩家生产环境验收通过。**

连接到 WebCodex 的 Runner 只有 Windows x64 主机；Linux 测试均为 WSL2 虚拟化 Linux 内核，**没有接入独立原生 Linux 物理/虚拟主机进行压测**。客户端和服务端运行于同一宿主环境，本机回环存在资源竞争；没有独立压测机证据。

## 实现结构

- `base/IoBackend.h`：统一 worker 控制接口，`ready()/enqueue()/load()`，继承 `MyThread`；这是 worker 层抽象，不是所有平台事件 API 的完整独立抽象。
- `base/MySockTaskPool.cpp`：编译期选择 Linux `epoll`、Windows `CreateIoCompletionPort`/ `GetQueuedCompletionStatus`。Windows 使用带独立 `OVERLAPPED` 与固定存储期缓冲的 `WSARecv`/`WSASend`，读写完成回调仍调用相同 `MySockTask::getMsg` 和 `MySocket` 帧缓冲；没有对外业务协议修改。
- Windows/ Linux 均在启动 worker 前检查事件后端创建成功；Windows completion port 在 worker join 后关闭。
- `base/MySocket.h/.cpp`：支持 IOCP 接收追加、发送缓冲复制/确认消费和套接字提前关闭。发送队列单连接上限 2 MiB。
- `net_load_test.cpp`：支持 `hold`/`throughput`/`slow`，支持万连接、源端口选择和回环多源 IP 重试；后续增加随机保留采样的 P50/P95/P99，最多每个测试线程 4096 样本，避免 24h 样本内存无限增长。慢客户端模式不再默许 I/O 错误，并要求真实观测到发送背压。
- `CMakeLists.txt`：保持 C++20，构建 `net_server/net_client/net_protocol_test/net_churn_test/net_load_test`。

## Windows 原生（MSVC C++20，127.0.0.1 TCP）验证

| 场景 | 观测 | 状态 |
|---|---|---|
| 基础协议 | 6/6：粘包、拆包、64 KiB、半关闭、非法长度 | 通过 |
| 基础 churn | 800/800、0 连接/I/O/数据错误 | 通过 |
| 10000 长连接 + 10% 活跃，128B Echo，12s（含尾延迟采样最终复测） | 10000/10000 接入、323731 消息、26977.583 msg/s、平均 Echo 0.594ms；采样 P50 0.095ms / P95 2.118ms / P99 7.763ms（20227 样本）；0 I/O/数据错误 | 通过（不等于全员活跃 MMO） |
| 32 线程 churn（累计 10016 条，128B） | 10016/10016 成功、17.834s、561.631 conn/s、0 错误 | 通过 |
| 10000 在线 + 约 2% 慢读，4 KiB，10s | 10000/10000 接入、75040 次完整发送、7322 次发送 `would_block`；服务端 64512 请求，0 I/O / 协议错误 | 通过（验证背压触发，不代表内存上限安全） |
| 同上服务端资源 | 峰值 RSS ~407.527 MiB、对端断开计数 224、最终 `accepted=10000 closed=10000 active=0` | 资源可回收；背压风险未消除 |
| 压测器严格判定回归：3000 慢读、10% 活跃，4 KiB，10s | 3000/3000、107200 完整发送、23960 次 `would_block`、0 I/O/数据错误、退出码 0 | 通过：严格测试逻辑已触发并验证背压 |
| 10000 低频心跳，120s | 10000/10000 接入、50000 Echo、0 错误、平均 1.291ms；采样 P50 0.127ms / P95 6.603ms / P99 12.168ms（3120 样本） | 120s 通过，非 24h |
| 同上服务端退出 | `accepted=10000 closed=10000 active=0 messages=50000 io_errors=0 protocol_errors=0`，峰值 RSS 174.355 MiB | 通过 |

**采样 P99 是压测器端到端 Echo 延迟，包含客户端调度、上下文切换、本机网络与服务端耗时；并非服务端内部处理 P99。** `throughput` 模式默认仅 10% 连接参与循环 Echo 且各 worker 顺序阻塞等待响应；不能据此证明 10000 玩家全部并发上报或 AOI 扇出。

Windows 万连接短测曾在同样标称 12s、10% 活跃条件下报告 `210980` Echo / `17581.667` msg/s。后续加采样后的复测为 `323731` Echo / `26977.583` msg/s；短时同机负载变化明显，不能以单次最好成绩承诺生产 QPS。另有一次慢客户端测试与到期的服务端共用端口，发生 `WSAECONNREFUSED (10061)` 并超时；该轮**判为测试编排无效并已在独立端口重测**，不能算服务器回归失败或通过。

## WSL2 Linux（GCC 13.3 C++20，epoll）验证

| 场景 | 观测 | 状态 |
|---|---|---|
| 基础协议 | 6/6 PASS | 通过 |
| 基础 churn | 400/400、0 错误 | 通过 |
| 10000 连接 + 10% 活跃，128B Echo，12s | 10000/10000、335013 消息、27917.750 msg/s、平均 Echo 0.573ms、0 I/O/数据错误 | 通过（WSL2） |
| 连接建立期间的端口冲突 | 源端口绑定冲突 49 次，负载生成器切换源 IP 重试全部恢复 | 环境限制有规避，非服务端拒绝 |
| 随后累计 churn | 1280/1280、1.311s、0 错误，服务端总 `accepted=11280 closed=11280 active=0 io_errors=0` | 通过（WSL2） |
| 10000 心跳 120s | 10000/10000，50000 Echo，平均 0.288ms；采样 P50 0.258ms / P95 0.495ms / P99 0.748ms（3120 样本），0 I/O/数据错误 | 120s 通过（WSL2） |
| Linux ASan + UBSan 短时回归（最终变更后） | C++20 编译通过、协议 6/6、短连接 400/400、32 连接 Echo 203302 消息/2s、P99 1.142ms、0 I/O/数据错误，服务端 `accepted=438 closed=438 active=0` | 通过（不代表 10000 连接 sanitizer 或 24h） |

WSL2 的 Linux 内核版本为 `6.18.58.1-microsoft-standard-WSL2`；曾观察 `ip_local_port_range=50087..52134`，使用明确本地源端口和多 IP 重试绕过本机出站端口限制。请勿将 WSL2 测得的系统性能外推到生产物理 Linux。

最终 WSL2 120s 心跳负载：建连源端口冲突重试 558 次且全部恢复；客户端总耗时 136.710s（含建连），服务端 `SERVER_FINAL accepted=10000 closed=10000 active=0 peak_connections=10000 messages=50000 io_errors=0 protocol_errors=0 peer_disconnects=0 peak_rss_mib=7.38281`。注意此 RSS 不包含 Linux 内核 Socket 内存。

## 源码风险与生产验收缺口

| 位置 | 风险 | 触发条件 | 建议与验证 |
|---|---|---|---|
| `base/MySockTaskPool.cpp` `runIocp`/结束 drain（约 490–620 行） | **高：** 关闭后仍有完成通知未被领取时，5s 超时退出 drain 可能遗留操作/内存 | 慢驱动或异常取消完成 | 补充 IOCP shutdown 故障注入；Windows Application Verifier、WinDbg/ETW 和句柄泄漏检查；不要以测试绿灯替代代码证明 |
| `base/MySocket.cpp` `sendDataWithBuffer` | **高：** 仅单连接 2 MiB 输出队列上限，没有全实例统一预算 | 万连接大面积慢读 | 增加全局或 worker 级发送预算、高低水位/断开策略，慢客户端内存峰值、OOM 回归 |
| `base/MySocket.cpp` `readBuffer` | 中：`vector.erase(begin)` 对大量合并小包产生反复搬移 | 高频合包 | 改偏移解析/环形缓冲并建立 A/B Benchmark |
| `net_load_test.cpp` `throughput` | 中：每个测试线程串行收发，16 线程可能先于服务端成为瓶颈 | 大并发高 QPS | 多进程/事件驱动压测器，独立压测机，并报告服务端队列深度和 P99.9 |
| 跨业务网络架构 | **高：** 只有 TCP Echo，没有 AOI、30 Hz Tick、战斗、鉴权、超时恢复 | 真实 MMO 负载 | 模拟 10000 玩家，含移动/广播/NPC/技能；监控 30 Hz Tick 超时、P99 和内核 socket 内存 |
| 测试环境 | **高：** 缺少独立原生 Linux 机器与跨机器链路，且仅 120s 长稳 | 上线前容量认证 | 独立负载机 + 原生 Linux + 24h/72h 稳定性 + 慢读/重连风暴/故障注入；TSan/ASan/UBSan 分开验收 |

### 测试重现命令

Windows 原生 VS2026 环境构建（参考已验证的 `build-iocp-win3/build_native.cmd`，需初始化 VS2026 的 x64 Developer Command Prompt）：

```cmd
cd /d E:\MemorydDeal
cmake -S . -B build-iocp-win3 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-iocp-win3 --parallel 8
```

在两个 Windows 终端分别启动：

```cmd
build-iocp-win3\net_server.exe --host 127.0.0.1 --port 50384 --io-threads 8 --max-clients 10000
build-iocp-win3\net_load_test.exe --mode hold --port 50384 --connections 10000 --threads 16 --duration 120 --heartbeat-seconds 20
```

吞吐量：`net_load_test.exe --mode throughput --port 50384 --connections 10000 --threads 16 --duration 12 --ramp-ms 5000 --active-percent 10 --payload 128`。
短连接：`net_churn_test.exe --port 50384 --threads 32 --per-thread 313 --payload 128`。慢读：`net_load_test.exe --mode slow --port 50384 --connections 10000 --threads 16 --duration 10 --active-percent 2 --payload 4096`。应为不同场景选择新的端口、独立的服务端进程和清晰的日志。

在原生 Linux 机器上待执行的等价步骤（**未在独立原生 Linux 主机验证**）：

```bash
cmake -S . -B build-native-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-native-linux --parallel
ulimit -n 65535
./build-native-linux/net_server --host 127.0.0.1 --port 42044 --io-threads 8 --max-clients 10000
# 另一个终端：
./build-native-linux/net_load_test --mode hold --port 42044 --connections 10000 --threads 16 --duration 120 --heartbeat-seconds 20 --source-port-start 33000
```

以上所有结果均来自同机回环，不涉及外部网络带宽/丢包/RTT。生产上线门槛仍未满足；**已完成的是网络层功能和阶段性容量/稳定性测试，而非 MMO 生产认证。** 未执行 Git commit，也未修改共享内存旧业务或 third_party/vendor。
