# MemoryDeal Network-only Benchmark — 2026-11-15

## Execution environment
- Host: Windows 11, MinGW-w64 GCC 14.2.0, CMake + Ninja, Release C++17
- Test transport: 127.0.0.1:50113 TCP loopback, 4-byte big-endian length framing, Echo payload validation
- Server: one nonblocking WSAPoll reactor, 1024 max connections, 64 KiB max payload
- Client: one thread per connection; pipeline controls in-flight frames; all returned payloads bytewise checked
- Percentile samples: each 16th successful request per client (up to 4096 samples per client)
- These measurements are a development-machine baseline, **not production capacity estimates**.

## Measured scenarios

| Connections | Payload | Pipeline | Duration | Successful messages | Requests/s | Payload MiB/s | P50 ms | P95 ms | P99 ms | I/O errors | Mismatches |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 64 B | 1 | 2 s | 53,253 | 26,625.022 | 1.625 | 0.027 | 0.070 | 0.182 | 0 | 0 |
| 16 | 128 B | 1 | 5 s | 202,796 | 40,553.255 | 4.950 | 0.319 | 0.685 | 0.867 | 0 | 0 |
| 64 | 256 B | 4 | 5 s | 655,168 | 130,975.866 | 31.977 | 1.695 | 3.421 | 4.769 | 0 | 0 |
| 128 | 1,024 B | 4 | 5 s | 514,296 | 102,718.455 | 100.311 | 4.724 | 6.932 | 8.405 | 0 | 0 |
| 256 | 128 B | 1 | 5 s | 181,911 | 36,256.918 | 4.426 | 7.031 | 8.160 | 9.866 | 0 | 0 |
| 512 | 128 B | 1 | 5 s | 161,201 | 32,113.627 | 3.920 | 14.329 | 24.365 | 25.175 | 0 | 0 |
| 8 | 65,536 B | 2 | 4 s | 19,982 | 4,991.646 | 311.978 | 2.853 | 4.975 | 6.466 | 0 | 0 |
| 64 | 16 B | 16 | 4 s | 1,458,176 | 364,238.404 | 5.558 | 2.164 | 3.957 | 5.040 | 0 | 0 |
| 64 | 256 B | 4 | 10 s | 1,154,032 | 115,374.633 | 28.168 | 1.962 | 4.251 | 5.151 | 0 | 0 |
| 128 | 256 B | 8 | 30 s | 5,579,968 | 185,959.745 | 45.400 | 5.249 | 7.148 | 8.704 | 0 | 0 |

The 30-second client process reported 108.906 CPU-seconds (about 3.63 logical cores averaged over its 30.006-second measured wall time) and peak working set of 13.78 MiB. Server cumulative metrics were emitted to its process log; one observation after the tests showed peak working set about 7.35 MiB, zero I/O errors, and expected protocol-error counts from malformed-frame tests. These are not isolated measurements for the 30-second run.

Throughput differs substantially with pipeline depth and payload size. Comparing row throughput without controlling these variables is not valid scaling analysis.

## Protocol / fault checks
Executed `build-network/net_protocol_test.exe` with the local server running:

- PASS fragmented TCP header/payload
- PASS coalesced frames
- PASS max 64 KiB payload
- PASS TCP half-close after request
- PASS reject payload length zero
- PASS reject payload length 65,537

Result: **6/6 passed**.

During initial smoke testing a client could connect but the server would close it before Echo: unsigned subtraction of an older poll timestamp and a newer per-connection activity timestamp caused idle-timeout underflow. Guarded this subtraction, rebuilt and re-ran tests successfully.

## Reproduction
Start locally (one terminal):
```powershell
.\build-network\net_server.exe --host 127.0.0.1 --port 50113 --max-clients 1024
```
Benchmark and protocol tests (another terminal):
```powershell
.\build-network\net_client.exe --connections 128 --duration 30 --payload 256 --pipeline 8
.\build-network\net_protocol_test.exe
```

## Limitations / next production gates
- Windows WSAPoll and Linux poll are single-reactor baselines, not a proven multi-core epoll implementation; Linux build/runtime not verified because WSL2 was unavailable on this machine.
- No TLS, authentication, ACL/rate limits, independent load generator, CI integration, packet-loss/latency injection, 24-hour soak, fault-injected network partition, packet fuzzing at scale, or cross-machine measurement.
- Benchmark client uses one OS thread per connection. It is useful for a 1–512 connection local reference but not a 5,000–50,000 socket scalable load generator.
- Service only binds loopback by default. Never expose the Echo service to public interfaces without a threat model and hardening.
- Source tree contains only network code and build artifacts; old code is archived outside the workspace under `C:\Users\gexiannan\MemorydDeal_legacy_backup_20261115`.
