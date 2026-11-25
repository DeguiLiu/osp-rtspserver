# osp-rtspserver

[![CI](https://github.com/DeguiLiu/osp-rtspserver/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/osp-rtspserver/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

基于 [newosp](https://github.com/DeguiLiu/newosp) 的固定容量 RTSP 1.0 服务端。单 Reactor 网络线程独占 listener、RTSP TCP、RTP/RTCP socket 和定时器，编码器通过有界 `FrameIngress`、SPSC 队列和 `Wake()` 接入；媒体路径运行期不分配堆内存，客户端之间只传 `FrameDesc`，不复制 AU payload。

## 能力

- 单视频轨 H.264 / H.265，H.265 使用 RFC 7798
- RTSP 1.0：OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN、GET_PARAMETER
- RTP/RTCP UDP 单播与 RTSP/TCP interleaved
- RTCP SR/RR/SDES/BYE；新客户端与丢帧恢复等待 IDR
- `SetIdrRequester()` 限频请求编码器产生 IDR
- `UpdateTrackParams()` 运行期更新 SPS/PPS/VPS，旧会话关闭后重连
- 16 连接固定槽位、每客户端令牌桶和慢客户端隔离
- 可选 telnet 调试 shell 与故障注入

当前只验证 Linux x86；ARM Linux 与 RS500 RISC-V RT-Thread 需要单独移植和板测。详细状态见 [impl/README.md](impl/README.md)。

## 目录

```
impl/                                     可构建实现（库、demo、测试、脚本、示例）
基于 newosp 的 RTSP Server 详细设计方案.md   详细设计方案
基于 newosp 的嵌入式 RTSP Server 改造方案.md 早期改造方案（背景与历史审查）
CSDN-RtspServer轻量级RTSP服务器和推流器.md   PHZ76/RtspServer 参考文章
现代 C++17 相比 C 的不可替代优势.md          设计取舍参考
```

## 构建与测试

`newosp` 默认查找与仓库同级的 `../newosp`；也可用 `NEWOSP_DIR` 指定。

```bash
git clone git@github.com:DeguiLiu/newosp.git
cmake -S impl -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRS_BUILD_E2E=ON -DRS_ENABLE_DEBUG_SHELL=ON -DRS_ENABLE_FAULT_INJECTION=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

最小构建（无 shell、无故障注入、无 E2E）：

```bash
cmake -S impl -B build-min -DCMAKE_BUILD_TYPE=Release \
  -DRS_BUILD_E2E=OFF -DRS_ENABLE_DEBUG_SHELL=OFF -DRS_ENABLE_FAULT_INJECTION=OFF
cmake --build build-min -j"$(nproc)"
ctest --test-dir build-min --output-on-failure
```

## 运行

```bash
./build/make_test_stream /tmp/test.h264 --aus 250
./build/rtspserver_demo --port 8554 --file /tmp/test.h264 --ip 127.0.0.1
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

外部编码器接入见 [impl/README.md](impl/README.md) 与 `impl/examples/encoder_source_demo.cpp`。

## 文档

- [详细设计方案](<基于 newosp 的 RTSP Server 详细设计方案.md>)
- [实现说明与测试记录](impl/README.md)
- [测试报告](impl/docs/测试报告.md)
- [调试命令面设计](impl/docs/2026-11-26-调试命令面设计.md)

## License

MIT，见 [LICENSE](LICENSE)。
