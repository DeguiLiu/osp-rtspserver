[English](README.md) | **中文**

# osp-rtspserver

[![CI](https://github.com/DeguiLiu/osp-rtspserver/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/osp-rtspserver/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

> 主要面向嵌入式 Linux：把已有 H.264/H.265 编码器的码流转发给标准 RTSP 客户端。
> Linux x86-64 同样默认支持，也是开发和 CI 平台。Windows 尚未验证；
> newosp 提供 Winsock 后端，移植预计可行，需适配线程、定时与信号层。

## 这是什么

基于 [newosp](https://github.com/DeguiLiu/newosp) 的定容 RTSP 1.0 服务端，支持 H.264 与
H.265 视频。单 Reactor 网络线程独占全部 socket、会话与定时器；编码器把完整访问单元推入
有界 `FrameIngress`，服务端再分发给各客户端，客户端之间只传引用，不复制 AU 负载。媒体路径
运行期不做堆分配。

## 提供了什么

| 能力 | 状态 |
|---|---|
| RTSP 1.0：OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN、GET_PARAMETER | 已实现 |
| H.264（RFC 6184）与 H.265（RFC 7798）Annex B 解析与 RTP 分包 | 已实现 |
| RTP/RTCP over UDP 单播、RTSP over TCP interleaved | 已实现 |
| RTCP SR、SDES、RR、BYE | 已实现 |
| 新客户端起播与丢帧恢复时等待 IDR | 已实现 |
| `SetIdrRequester()` 编码器回调，跨会话共享限频 | 已实现 |
| `UpdateTrackParams()` 运行期更新 SPS/PPS/VPS | 已实现 |
| 16 个固定连接槽、每客户端令牌桶、慢客户端隔离 | 已实现 |
| telnet 调试 shell、测试专用故障注入 | 可选，默认关闭 |

## 不提供什么

- 不提供任何音频轨道（AAC、G.711A、Opus 均无）。
- 不提供鉴权。只能部署在可信网络。
- 不提供 TLS。
- 不提供组播，也不提供 RTSP 推流：这是供客户端拉流的 Server，不是 ANNOUNCE/RECORD 推流器。
- 不支持 `PAUSE`：该方法会被解析并返回 `405`。
- 不支持多轨道与多会话。聚合 URI 固定为 `/live`。
- 不支持 IPv6：监听、SDP 与 UDP 传输均为 IPv4。
- 不支持同一对象重启：调用 `Stop()` 后需重新构造 `RtspServer`（生命周期 HSM 与输入队列已进入终态）。
- 不支持在已有会话上重新协商 SDP；参数集变更会关闭旧会话。
- 不产生 PLI/FIR 类型的 RTCP 反馈。
- 不提供回放服务。文件源只是测试输入，不是录制或回放功能。
- 不提供 RT-Thread 与 MCU 移植。支持 Linux x86-64 与 ARM Linux；Windows 尚未验证。
- 不提供板级性能保证。所有实测数据均来自 x86-64 主机。

## 定位

- 作为 newosp 之上的应用层实现。复用 newosp 的事件循环、HSM 表、SPSC 环形缓冲与词汇类型，
  自身不引入额外依赖。
- 定容是设计前提：所有缓冲、队列与连接槽位都有编译期上界，部署前即可确定内存占用。
- 当前阶段是「能在主机上跑通并通过测试」，尚未在 ARM 板子上实测。
- 不替代通用媒体服务器。只覆盖单轨道视频拉流，音频、鉴权、组播均有意留在范围之外。

## 怎么用

### 前置条件

- CMake 3.16+、C++17 编译器、Linux（x86-64 或 ARM）。
- 与本仓库同级放置的 newosp 检出，或通过 `NEWOSP_DIR` 指定路径。
- Linux 构建要求 newosp 提供 `OSP_THREAD_POSIX_CHECKED_START=1`；仅使用旧版 0.9.6 基线不满足该要求，CMake 会检查此能力。

### 构建

```bash
git clone git@github.com:DeguiLiu/newosp.git ../newosp
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRS_BUILD_E2E=ON -DRS_ENABLE_DEBUG_SHELL=ON -DRS_ENABLE_FAULT_INJECTION=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### 用文件源运行

```bash
./build/make_test_stream /tmp/test.h264 --aus 250 --gop 25
./build/rtspserver_demo --port 8554 --file /tmp/test.h264 --ip 127.0.0.1
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

`--ip` 是 SDP `c=` 与 RTP-Info 中通告的地址。跨机测试必须填客户端可达的地址，不能用
`127.0.0.1`。

H.265 用生成器的 `--codec h265`，或用 `scripts/make_real_stream.sh out.h265 h265 4` 产生
真实码流。

### 用外部编码器运行

```cpp
rs::ServerConfig cfg;
cfg.params = track_params;                          // codec + SPS/PPS/VPS
rs::RtspServer server;
server.SetIdrRequester(&OnIdrRequest, &encoder_state);  // loop thread, must not block
if (!server.Start(cfg).has_value()) {
  return 1;
}

// 编码器线程：
auto w = server.Ingress().Acquire(len);
// 填写 w.au，设置 w.desc.pts_ns 与 flags
server.Ingress().Commit(w, len);
server.Wake();

// 参数集变更：旧会话关闭，客户端重新连接。
server.UpdateTrackParams(new_params);
```

`Ingress()` 只暴露有界的 `Acquire`/`Commit`；socket、会话、RTP、RTCP 与定时器仍在 loop
线程内。完整可运行程序见 `examples/encoder_source_demo.cpp`。

### 线程配置

`ServerConfig::loop_thread` 配置网络线程；`FileSource::Start` 的可选第三个参数配置文件源线程，
两者均使用 `osp::ThreadOptions`。默认使用系统线程栈和默认调度策略。Linux 下正优先级请求
`SCHED_FIFO`，需要相应权限；负优先级请求 `SCHED_IDLE`。配置失败时启动返回错误，不会自动降级。
如设置 CPU 亲和性，CPU 集合指针必须在对应的 `Start` 返回前保持有效。线程名由服务端分别设置为
`rtsp-loop` 和 `rtsp-source`。

自定义栈大小前应测量实际调用链及第三方库的峰值用量；x86 的编译器栈帧结果不能作为 ARM 线程栈配置依据。

### 调试 shell

以 `-DRS_ENABLE_DEBUG_SHELL=ON` 构建，给 demo 传 `--shell-port 5090`，然后用 telnet 连接。
只读命令包括 `stats`、`clients`、`client <slot>`、`pool`、`fd`、`timers`、`stream`、
`rtcp`、`latency`、`mem`、`config`；受控命令为 `log`、`kick`、`reset`、`trace`、`idr`、
`shutdown`，其中 `idr` 需要先注册编码器回调。

## 文档

- [概要架构设计](docs/architecture.md)
- [测试报告](docs/test-report.md)
- [上游 issue 对照核查](docs/upstream-issue-audit.md)

## 许可

MIT，见 [LICENSE](LICENSE)。
