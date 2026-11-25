[English](README.md) | **中文**

# osp-rtspserver

[![CI](https://github.com/DeguiLiu/osp-rtspserver/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/osp-rtspserver/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

基于 [newosp](https://github.com/DeguiLiu/newosp) 的固定容量 RTSP 1.0 服务端。单 Reactor 网络线程独占 listener、RTSP TCP、RTP/RTCP socket 和定时器；编码器通过有界 `FrameIngress`、SPSC 队列和 `Wake()` 接入。媒体路径运行期不分配堆内存，客户端之间只传 `FrameDesc`，不复制 AU payload。

## 现状

范围：单视频轨道（H.264 或 H.265，按媒体源二选一、启动时自动检测）、RTSP 1.0、最多 16 个并发 TCP 控制连接、UDP 单播与 TCP interleaved 两种传输、RTCP SR/RR、`-fno-exceptions -fno-rtti`。

未实现：音频、TLS、鉴权、组播、文件回放、录制、PLI/FIR、PAUSE、多轨/多会话、向其他服务器推流、Windows 采集示例。参数集更新已实现，但按设计 §9.4 不做热切换：旧会话被关闭，客户端重新 DESCRIBE/SETUP。

### 能力状态

| 功能 | 当前状态 | 说明 |
|---|---|---|
| H.264 / H.265 | 已实现，部分验证 | 单视频轨道，启动时选择一种编码；已有 ffmpeg TCP/UDP 解码测试记录 |
| RTP/UDP 单播、RTSP/TCP interleaved | 已实现，部分验证 | 两种路径均有测试；VLC 互通尚未完成 |
| 新客户端等待关键帧 | 已实现，验证不完整 | 起播和丢帧恢复等待关键帧；`SetIdrRequester()` 限频请求编码器产生 IDR，实际强制编码需平台适配 |
| SPS/PPS 运行期更新 | 已实现 | `UpdateTrackParams()` 校验并复制参数集、递增 `param_gen`、关闭旧会话；新会话使用新 SDP |
| 会话保活与清理 | 已实现基础能力 | 支持 GET_PARAMETER、超时清理及 RTCP 处理；还需更完整的异常网络测试 |
| RTCP SR/RR、SDES、BYE | 已接线，待完整互通验证 | TCP/UDP RR/BYE 接收路径均已接线；VLC/商用播放器互通仍待验证 |
| 16 客户端 | x86 短时验证 | 8 TCP + 8 UDP、4 Mbps/路、60 秒；不是 600 秒长稳，也不是 ARM 板测 |
| AAC、G.711A 音频 | 未实现 | 当前只有单视频轨道 |
| RTP 组播 | 未实现 | 仅支持单播 |
| RTSP 推流器 | 未实现 | 当前是供客户端拉流的 Server，不是向其他服务器主动推流的 Pusher |
| Digest 鉴权 | 未实现 | 当前应仅部署于可信网络 |
| PAUSE | 未实现 | 当前明确返回 405 |
| 多媒体轨道、多路媒体会话 | 未实现 | 当前固定 `/live` 单视频轨道；文件源只是测试/示例输入，不等同于完整回放功能 |
| 桌面、麦克风采集示例及 Windows 支持 | 未实现 | 当前目标是 Linux x86 测试；ARM Linux 部署需工具链/板测；RS500 RISC-V RT-Thread 需单独 port |

## 目录

```
include/rs/        公共头（冻结接口）
src/               实现
tests/             单元测试 + 端到端测试
tools/             测试流生成器
examples/          外部编码器接入示例
scripts/           构建、压力测试脚本
docs/              测试报告、评审与调试设计
```

## 构建与测试

```bash
git clone git@github.com:DeguiLiu/newosp.git ../newosp
cmake -S . -B build -DRS_BUILD_E2E=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

`NEWOSP_DIR` 可覆盖 newosp 路径。基线 `newosp 0.9.6` 可直接构建；若 newosp 带有 `Accept4`、`IoEvent::kRdhup`、`EventLoop::HasFailed` 扩展，CMake 自动检测并启用，否则回退到 `accept` + `fcntl`、`read()==0` 半关闭和普通 loop 退出路径。

强制选项：`RS_NO_EXCEPTIONS=ON`（默认）给全部自编译目标加 `-fno-exceptions -fno-rtti`；`RS_SANITIZE=ON` 使用 ASan+UBSan（与 TSan 分开构建）。

## 运行

```bash
build/make_test_stream /tmp/test.h264 --aus 250 --gop 25
build/rtspserver_demo --port 8554 --file /tmp/test.h264 --ip 127.0.0.1

ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
# H.265：make_test_stream --codec h265 / scripts/make_real_stream.sh out.h265 h265 4
ffplay -rtsp_transport udp rtsp://127.0.0.1:8554/live
```

`--ip` 是 SDP `c=` 与 RTP-Info 中通告的地址；跨机测试必须填目标机可达的本机 IP，不能用 127.0.0.1。

## 外部编码器接入（可选）

`rtspserver_demo` 默认使用 `rs::FileSource`（文件读取 + Annex B AU 切分 + SPS/PPS 扫描）；嵌入式产品可绕过文件源，直接使用公开的 producer 边界：

```cpp
rs::ServerConfig cfg;
cfg.params = track_params;   // codec + SPS/PPS/VPS, copied by Start()
rs::RtspServer server;
server.SetIdrRequester(&OnIdrRequest, &encoder_state);  // loop thread, must not block
if (!server.Start(cfg).has_value()) {
  return 1;
}
// encoder thread:
auto w = server.Ingress().Acquire(len);
// write w.au[0..len), set w.desc.pts_ns/flags
server.Ingress().Commit(w, len);
server.Wake();               // or callbacks.wake

// SPS/PPS changed: old sessions are closed, clients reconnect.
server.UpdateTrackParams(new_params);
```

`Ingress()` 只做有界 Acquire/Commit，不暴露 socket/session 状态；loop 仍独占连接、RTP、RTCP 和定时器。
`SetIdrRequester()` 的回调在 loop 线程执行，且所有会话共享同一限频窗口（默认 1 s）；回调只应置标志或向编码器控制队列投递命令。
该接口不保证编码器接受请求或在指定时间内产出关键帧。完整可运行示例见 `examples/encoder_source_demo.cpp`
（`encoder_source_demo` 目标），覆盖 AU 提交、IDR 请求和运行期参数集更新。

## 调试 shell（可选）

```bash
cmake -S . -B build -DRS_ENABLE_DEBUG_SHELL=ON -DRS_ENABLE_FAULT_INJECTION=ON -DRS_BUILD_E2E=ON
cmake --build build -j"$(nproc)"
build/rtspserver_demo --port 8554 --file /tmp/test.h264 --ip 127.0.0.1 --shell-port 5090
telnet 127.0.0.1 5090
```

可用只读命令：`help`、`stats`、`clients`、`client <slot>`、`pool`、`fd`、`timers`、`stream`、`rtcp`、`latency`、`mem`、`config`、`time`、`version`。受控命令（admin）：`log <level>`、`kick <slot>`、`reset [stats|client <slot>]`、`trace <slot> on|off`、`idr`、`shutdown`；`idr` 需要先通过 `SetIdrRequester()` 注册编码器回调，未注册时返回错误。P2 故障注入（需 `RS_ENABLE_FAULT_INJECTION=ON`）：`fault drop <n>`、`fault stall <slot> <ms>`、`fault eagain <slot> <n>`、`fault idr <slot> <n>`、`simulate slow <slot> <bps>`。默认不启用；只读命令只读 `Snapshot()`，受控/故障命令经 SPSC 队列 + `Wake()` 回到 loop 执行。设计见 `docs/2026-11-26-调试命令面设计.md`。

## x86 测试场景假设

### 设计值 / 实现值

| 项 | 设计值 | 实现值 | 说明 |
|---|---|---|---|
| 帧槽 | 16 x 256 KiB | 8 x 256 KiB | 25 fps 下约 320 ms 覆盖；慢客户端按队列深度（4 AU）降级 |
| 每连接 TX | 8 KiB | 32 KiB | 控制响应 + TCP 媒体共用，保证 interleaved 不交错 |
| 出口整形 | 每客户端 8 Mbps | 每客户端 8 Mbps | 已删除全局 12 Mbps 聚合桶（评审 P0-1）；令牌不足时暂停游标而不丢 AU（4 Mbps 压测修复） |
| 单次发送预算 | 未规定 | 每会话 32 KiB / 32 包每轮 | 25 次/秒的帧唤醒可支撑约 6.5 Mbps，避免 16 包预算卡在 3.9 Mbps |
| UDP 端口 | RTP/RTCP 连续偶数对 | RTP 偶数、RTCP = RTP+1 | 失败重试 32 次，返回 503 |
| interleaved channel | 0..127 | 0..127 | 2-3 等非零基址可用 |
| RTCP | SR/RR/SDES/BYE | SR+SDES 发送；TCP/UDP RR/BYE 解析 | UDP RTCP socket 已注册 watcher |
| FU 分片 | 共享 AU，不复制 | payload 指向共享 AU + 2/3 字节 prefix | UDP 三 iovec；TCP 仍复制进 tx_ |
| 监听 backlog | 32 | 32 | `kListenBacklog` |
| TCP socket | 低延迟有界 | `SO_SNDBUF=64 KiB`、`SO_RCVBUF=32 KiB`、`TCP_NODELAY`、`TCP_NOTSENT_LOWAT=16 KiB`、`TCP_USER_TIMEOUT=10 s`、`CLOEXEC` | `strace -e setsockopt` 已验证 |
| UDP socket | connected、有界 | `SO_SNDBUF=128 KiB`、`SO_RCVBUF=32 KiB`、`IP_TOS=EF`、`CLOEXEC` | 仅 RTCP 注册 watcher |
| accept | 原子 nonblocking/cloexec | `accept4(SOCK_NONBLOCK \| SOCK_CLOEXEC)` | `strace` 已验证 |
| 半关闭 | `EPOLLRDHUP` | newosp `IoEvent::kRdhup` | `strace epoll_ctl` 已验证 |
| per-IP 限流 | 固定令牌桶 | accept 32 burst / 8 每秒；request 256 burst / 128 每秒 | 32 项固定表 |
| TCP 指标 | RTT/cwnd/重传/未发送/实际缓冲 | shell `net` | `TCP_INFO` + `getsockopt(SO_SNDBUF/RCVBUF)` |

| 项 | 假设 | 影响 |
|---|---|---|
| 内核 | Linux ≥ 5.x，epoll | 与 ARM Linux 同源，`EINTR` 已处理 |
| CPU | 单核 1 GHz 等效即可跑 16 路 | 不代表 ARM 余量，需板测复核 |
| 网络 | loopback 与同网段 veth | 不覆盖 Wi-Fi 抖动、跨网段路由 |
| 码率 | 测试流 ≤ 2 Mbps | 本轮已用 4 Mbps 720p25 × 16 路（8 TCP + 8 UDP）验证 |
| 播放器 | ffplay（ffmpeg RTSP 客户端） | 覆盖真实解封装，不代表全部商用播放器 |
| 时间 | 单调时钟，单机 | 跨机 RTP 时延只做相对判断 |

x86 结论不能替代 ARM 板测；文档状态在板测前保持“设计待验证”。

> 目标平台注意：本实现依赖 Linux 的 `pthread`、epoll 后端和 `getrandom`。`RS500_RTSP` BSP 实际是 RISC-V RT-Thread（`riscv64-unknown-elf`），不能直接运行本目录；若产品目标是 RS500，需要先做 RT-Thread/LwIP 后端与启动层适配。

## 资源预算（设计值）

| 项 | 单连接 | 16 连接 | 说明 |
|---|---:|---:|---|
| 接收缓冲 | 8 KiB | 128 KiB | `kRtspRxBytes` |
| 发送缓冲 | 32 KiB | 512 KiB | 控制响应 + TCP 媒体共用，保证不交错 |
| 会话/游标状态 | ~1 KiB | ~16 KiB | 游标仅 40 B；含 SDP 缓冲与队列 |
| 帧槽池 | - | 2 MiB | 8 × 256 KiB，全客户端共享 |
| 合计用户态 | - | ≈ 2.7 MiB | 不含代码段、日志、线程栈 |
| fd | 2–4 | ≤ 80 | 监听 1 + 控制 16 + UDP 32 + wake/poller |

峰值抖动预算见设计文档 §5.2、§8.5。

## 关键约束

1. 网络线程只有一条，所有 socket、会话状态、帧引用操作都在该线程；编码器线程只做 `Acquire/Commit/Abort`。
2. 媒体热路径不分配、不打印、不阻塞。
3. 控制响应与 TCP 媒体包共用一条字节流，任何情况下不得插入到未发完的 interleaved 帧中间。
4. 源丢帧会形成全局 `input_seq` 缺口，受影响客户端等待下一个 IDR。
5. 无鉴权，仅允许部署在可信网络。

## 测试矩阵

单元测试覆盖解析、SDP、分包、RTCP、帧池、HSM、会话状态机、传输封装；端到端测试覆盖 TCP/UDP 播放、RTP-Info 首包一致、FU-A 字节级重组、8 路并发、连接上限、慢客户端隔离和错误码。CSDN 社区问题（首帧花屏、H.265、`Invalid NAL unit 0`）的专项用例见设计文档 §13.5，执行记录见 `docs/测试报告.md`。
