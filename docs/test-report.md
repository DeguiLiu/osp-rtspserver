# RTSP Server 测试报告

> 构建: C++17，RelWithDebInfo，`-fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wpedantic`
> 结论: 当前测试全部通过；未执行项和限制见第 6 节

## 1. 结论

- `ctest` 全量 16/16 通过，约 41 s；其中 `test_e2e` 含 12 个用例，约 26 s。
- 4 槽与 16 槽配置各通过 16/16 项，覆盖满槽拒绝与释放后重新接入。
- H.264/H.265 的 TCP interleaved 与 UDP 单播均可拉流；客户端重组结果与源流逐 NAL 字节一致；ffmpeg 解码 25 帧，退出码 0。
- 16 路（8 TCP + 8 UDP、4 Mbps/路、60 s）通过，RSS 3200–4000 KB；包化缓存与 UDP 批量发送后 CPU 平均约 3.8%。
- H.264 回归用例与 H.265 新路径均通过。
- 未执行项见第 6 节。

## 2. 环境

| 项 | 值 |
|---|---|
| 主机/内核 | Linux x86_64，6.5.0-45-generic |
| CPU | 100 逻辑核（共享虚拟机，测试期间有并发负载） |
| 编译器 | GCC 11.4.0，C++17 |
| ffmpeg | N-125331（libx264、libx265） |
| 网络 | loopback（127.0.0.1），无丢包注入 |

## 3. 测试结果

| 套件 | 结果 |
|---|---|
| 单元测试 13 个 | 13/13 通过 |
| 负向编译 2 个（异常/RTTI） | 2/2 按预期失败 |
| `test_e2e`（12 用例） | 12/12 通过，约 26 s |
| 合计 | 16/16 通过，约 41 s |

E2E 用例：

| 用例 | 结果 |
|---|---|
| `tcp_interleaved_single_client` | 通过 |
| `tcp_interleaved_channel_2` | 通过 |
| `udp_single_client` | 通过 |
| `concurrent_tcp_clients` | 4 槽测 4 路，16 槽测 8 路，均通过 |
| `connection_limit` | 满槽拒绝及槽位复用通过 |
| `slow_client_isolation` | 通过 |
| `error_paths`（404/454/461、重复 SETUP） | 通过 |
| `h265_tcp_playback` | 通过 |
| `h265_udp_playback` | 通过 |
| `h265_ffmpeg_decode` | 通过 |
| `h264_regression_decode` | 通过 |
| `codec_mismatch` | 通过 |

## 4. 关键证据

| 项 | 实测 |
|---|---|
| `sizeof(RtspServer)` | x86-64：4 槽 2,291,264 B；16 槽 2,817,728 B；相差 526,464 B，不含线程栈及内核缓冲 |
| H.265 TCP/UDP 重组 | 3 个 AU 逐 NAL 字节一致；FU 5 包、单 NAL 11 包；最大 RTP 1212 B；seq gap 0 |
| H.265 SDP | `m=video 0 RTP/AVP 96`、`H265/90000`、`sprop-vps/sps/pps`、`a=control:trackID=0` |
| H.264 SDP | `H264/90000`、`packetization-mode=1`、`profile-level-id`、`sprop-parameter-sets` |
| `interleaved=2-3` | SETUP 回显一致；媒体只在 channel 2；14 包 / 3 AU |
| UDP 端口对 | 例：`server_port=54312-54313`、`46632-46633`，RTP 偶数、RTCP=RTP+1 |
| ffmpeg 解码 | H.265 TCP/UDP 各 25 帧 rc=0；H.264 TCP 25 帧 rc=0 |
| 参数集更新 | `param_gen` 递增；旧连接以 `params-changed` 关闭；重连 SDP 含新 SPS |
| IDR 回调 | PLAY 未收到 IDR 时调用一次；1 s 内重复缺口不重复调用 |

ASan/UBSan、TSan、aarch64+qemu 与静态分析由 CI 覆盖，本地未复跑。

## 5. 16 路并发

条件：x86_64，8 TCP + 8 UDP，H.264 4 Mbps/路，60 s，客户端 `ffmpeg -c copy`。

| 指标 | 实测 |
|---|---:|
| 客户端存活 | 16/16 |
| RSS / FD / 线程 | 4000 KB / 40 / 3 |
| CPU 平均 / 最大 | 3.85% / 6.41% |
| loop P50 / P95 / P99 | 5 / 162 / 255 us |
| AU 输入 / 接受 / 丢弃 | 1555 / 1555 / 0 |
| gap / queue drop / send stall | 0 / 0 / 0 |
| 停机耗时 | 89 ms |

| 配置 | CPU 平均 / 最大 | RSS | loop P99 |
|---|---:|---:|---:|
| 包化游标缓存 | 3.77% / 4.85% | 3200 KB | 255 us |
| 包化缓存 + UDP `sendmmsg`（8 包/次） | 3.85% / 6.41% | 4000 KB | 255 us |

包化微基准（单客户端单 AU）：

| IDR | 优化前 | 优化后 |
|---:|---:|---:|
| 10 KB | 64.1 us | 7.1 us |
| 100 KB | 5.52 ms | 62.4 us |
| 200 KB | 21.39 ms | 122.4 us |

优化前同场景 60 s：CPU 平均 20.42%、最大 24.34%、RSS 5600 KB。4 Mbps 下 UDP 批量发送的 CPU 变化在噪声内，主要收益是减少 `sendmsg` 次数；需在更高码率和 ARM 上验证。

1080P 补充测试：x86-64 loopback，H.264、1920 × 1080、25 fps、目标 4 Mbps、1 s GOP，生成的 500 帧中最大帧为 65,659 B。

| 场景 | 结果 |
|---|---|
| 8 TCP + 8 UDP，20 s，客户端流复制 | 16/16 完成；输入丢弃、gap、队列丢弃均为 0；服务端 CPU 平均 3.30%、最大 4.22% |
| 1 TCP + 1 UDP，8 s，客户端解码 | 两端各解码 201 帧并退出成功；均出现非单调 DTS 告警 |
| 帧池边界 | 256 KiB AU 接收、释放通过；超出 1 B 被拒绝，4 槽和 16 槽构建均通过 |

1080P 高码率、大 IDR、H.265、长稳和 ARM 实机仍待验证；帧大小受内容和编码参数影响。

## 6. 未执行与限制

| 项 | 说明 |
|---|---|
| VLC/NVR 实机 | 未执行；当前互通证据来自 ffmpeg 与测试客户端 |
| 600 s 长稳 | 未执行；现有 60 s/120 s 数据不能外推 |
| PCAP 与丢包注入 | 未执行；逐字节证据来自客户端重组 |
| ARM 板测 | 未执行；仅 CI 交叉编译 + qemu |
| 本地 TSan/完整 ASan | 未复跑；由 CI 覆盖 |
| 并发端口别名 | 同机并发运行两套件时可能碰撞；建议 `ctest -j1` |
| UDP 结论范围 | 仅 loopback 无丢包条件 |
| 合成流 | 切片为随机字节，只用于逐字节回归，不可解码 |

## 7. 复现

```bash
cmake -S . -B build -DNEWOSP_DIR=../newosp -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRS_BUILD_E2E=ON -DRS_ENABLE_DEBUG_SHELL=ON -DRS_ENABLE_FAULT_INJECTION=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'test_e2e$' --repeat until-fail:5
```

## 8. 栈使用静态分析

Linux x86_64、GCC 11.4、RelWithDebInfo，启用调试 shell 和故障注入，并使用 `-fstack-usage` 编译。数值为编译器报告的单函数栈帧，不是调用链总栈用量。

| 函数 | 改造前 | 改造后 |
|---|---:|---:|
| 示例程序 `main` | 3,068,288 B | 1,264 B |
| 编码器示例 `main` | 2,796,032 B | 368 B |
| `FileSource::LoadParamSets` | 16,496 B | 96 B |
| `RtspSession::HandleDescribe` | 4,320 B | 224 B |
| `RtspServer::PublishSnapshot` | 3,792 B | 272 B |
| `RtspParser::Drain` | 1,008 B | 144 B |
| 调试 shell `CmdStats` | 3,760 B | 48 B |

示例的大型对象使用静态存储和 placement new；解析器请求、快照暂存和每个调试 shell 线程的快照占用对应对象或线程的常驻内存，不代表总内存同步减少。

当前测量中，示例程序 `main` 仍为 1,264 B；newosp 的 `IoPoller::Wait` 和日志函数分别为 848 B、784 B，编码器示例的生产者函数为 688 B。以上数值超过 512 B，仍需审查。线程实际栈峰值、调用链叠加、ARM 编译结果和 ARM 板测尚未验证，不能据此确定目标线程栈大小。

改用 newosp 线程接口后的同条件测量：`RtspServer::StartThreads` 为 416 B，`FileSource::Start` 为 400 B，POSIX 线程入口为 176 B。两个业务线程目前均使用默认栈大小和默认调度策略；未宣称不同线程优先级已生效。

