# 单 reactor、零堆、可预测：面向嵌入式量产的 RTSP 服务实现

> 对比对象：[PHZ76/RtspServer](https://github.com/PHZ76/RtspServer)（下称"参考实现"）。
> 本文只讨论视频、无音频、单编码器源、ARM-Linux、最多 16 路并发的量产场景；
> 数据来自仓库内 `docs/test-report.md` 的 x86_64 实测与两端源码级对照，不是同机 A/B 基准。

## 概要说明

[PHZ76/RtspServer](https://github.com/PHZ76/RtspServer) 是一套面向学习与原型开发的通用 RTSP 服务端/推流端实现：H.264、H.265、AAC、G711、VP8 都有，Windows 与 Linux 都能跑，示例完整。它的资源模型按桌面与服务器假设设计，这没有问题——它本来就不为某一块板子负责。

本实现只为一个场景负责：ARM-Linux、单路编码器、最多 16 路视频、无音频。场景一收窄，架构就能做减法，把所有上限前移到编译期。

观点很直接：嵌入式量产的瓶颈不是平均吞吐，而是最坏情况下还剩多少 CPU 和内存。参考实现在 16 路 4 Mbit/s 下大概率也能跑，但它的最坏情况不在设计里；我们把最坏情况写进了设计。

这个选择换来四项可验证的价值：

1. 每包零堆：一个 AU 只解析一次，客户端拿的是视图，不是拷贝。
2. 每帧一次扇出：UDP 用三 iovec 零拷贝直发，TCP 只往 32 KiB 流写一次。
3. 慢客户端隔离：24 KiB 媒体水位、2 s 无进展关闭、令牌桶限速，一个坏连接不拖累其余 15 路。
4. 时间轴可信：RTP 时间戳由编码器 PTS 映射到 90 kHz，而不是发送时刻。

代价同样明确：单 reactor 有单核上限，容量改动要重编译，功能面窄于参考实现。这些取舍在第 6 节逐条列出。

实测（x86_64，8 路 TCP + 8 路 UDP，4 Mbit/s/路，60 s）：RSS 5.6 MB、3 个线程、CPU 平均 20.4 %、loop P99 187 µs、没有 AU 丢弃与发送停滞。数据只用于说明量级，不是与参考实现的同机 A/B。

## 1. 场景先于架构

| 约束 | 取值 | 对架构的直接影响 |
|---|---|---|
| 平台 | ARM-Linux，1~4 核，512 MB~1 GB | 线程与拷贝都要省，锁竞争不可接受 |
| 并发客户端 | ≤ 16 | 连接表可以编译期定长，无需动态容器 |
| 媒体源 | 单路编码器，H.264/H.265，无音频 | 单生产者、单消费者，SPSC 足够 |
| 网络 | 局域网为主，存在弱网、拔线、慢读 | 必须有背压与失联检测，不能靠 TCP 重传兜底 |
| 资源预算 | 启动即定，运行期不增长 | 固定池 + 编译期上限，禁止运行期扩容 |

参考实现的默认假设不同：它要同时服务 RTSP 服务端与推流端，支持 AAC/G711/VP8、组播与鉴权，连接数与队列长度都按"够用就好"的经验值给。两者不是同一类产品，比较时要先接受这个前提。

## 2. 两条数据通路

### 2.1 参考实现

源线程构造 `AVFrame`（每次 `new uint8_t[size+1]`），`MediaSession` 按 NAL 切成 `RtpPacket`（每个包 `new uint8_t[1600]`，用 `shared_ptr` 共享），再为每个客户端投递一个 lambda 到任务队列；调度线程顺序执行，逐客户端改写 RTP 头序号与时间戳，最后走 `sendto`（UDP）或 `BufferWriter`（TCP，每包再 `new char[size+512]` 并 `memcpy`，队列上限 10000 包）。

```mermaid
%%{init: {'theme':'base','flowchart':{'nodeSpacing':24,'rankSpacing':40}}}%%
flowchart LR
  A0["参考实现<br/>源线程"]:::row --> A1["AVFrame<br/>每帧堆分配"]:::warn
  A1 --> A2["RtpPacket<br/>每包 1600B 堆"]:::warn
  A2 --> A3["每连接 lambda<br/>任务队列"]:::warn
  A3 --> A4["epoll 线程<br/>顺序执行"]:::user
  A4 --> A5["UDP sendto<br/>或 TCP 写队列"]:::user

  classDef row fill:#F3F4F6,stroke:#9AA4B2,color:#17324D
  classDef user fill:#E8F1FB,stroke:#3B82C4,color:#17324D
  classDef warn fill:#FDE8E8,stroke:#C0392B,color:#5A1A14
```

### 2.2 本实现

源线程把整帧写入 `FrameIngress` 的 8 个固定槽（每槽 256 KiB，`uint16_t` 引用计数），loop 线程每轮取一个 AU，校验后对每个播放会话推进自己的游标；包化只产生视图，RTP 头与 FU 头在游标里，负载指回共享 AU。UDP 用三 iovec `sendmsg` 直发，TCP 只往每连接的 32 KiB `TxStream` 写一次。

```mermaid
%%{init: {'theme':'base','flowchart':{'nodeSpacing':24,'rankSpacing':40}}}%%
flowchart LR
  B0["本实现<br/>编码器线程"]:::row --> B1["FrameIngress<br/>8×256KiB 槽"]:::user
  B1 --> B2["单 loop<br/>Drain 一帧"]:::user
  B2 --> B3["每会话游标<br/>variant 编码分支"]:::user
  B3 --> B4["包化视图<br/>零拷贝"]:::user
  B4 --> B5["UDP sendmsg<br/>TCP TxStream"]:::user

  classDef row fill:#F3F4F6,stroke:#9AA4B2,color:#17324D
  classDef user fill:#E8F1FB,stroke:#3B82C4,color:#17324D
```

## 3. 逐项对比

| 维度 | PHZ76/RtspServer | 本实现 |
|---|---|---|
| 并发模型 | Linux 上是 epoll + 任务队列（`SelectTaskScheduler` 只在 Windows 分支启用；select 本身受 `FD_SETSIZE` 限制） | 单 epoll reactor，固定 16 个连接槽，socket 只被 loop 线程访问 |
| 每包内存 | 每 NAL 一次 `new uint8_t[1600]`，`shared_ptr` 原子引用计数在客户端间传递 | 零堆；`RtpPacket` 是视图，负载指向共享 AU 或参数集 |
| 每帧拷贝 | UDP 路径共享缓冲；TCP 路径每客户端每包 `new char[size+512]` 再 `memcpy` | UDP 三 iovec 零拷贝；TCP 每包写入一次 32 KiB 流 |
| 时间戳 | 发送时刻 `steady_clock` 乘 90 kHz，与编码器 PTS 无关 | 编码器 `pts_ns` 映射 90 kHz，首帧建立会话随机基准，支持 32 位回绕 |
| 慢客户端 | 每连接出站队列到 10000 包才拒绝入队（约 16 MB），没有水位线 | 24 KiB 媒体水位 + 2 s 无进展关闭 + 令牌桶 8 Mbit/s（突发 32 KiB） |
| 失联检测 | 依赖 `send` 失败与 RTSP 保活请求 | RDHUP/EPOLLHUP + `TCP_USER_TIMEOUT` 10 s + keepalive 30/10/3 + 60 s 空闲关闭 |
| 状态管理 | 状态散落在连接对象的分支判断里 | 会话 HSM（Init / Active{Ready, Playing} / Closing）+ 服务 HSM（Init → Ready → Running → Stopping） |
| RTCP | 入站 RTCP 只当作保活读掉，不解析，也不发 SR | compound 解析（SR/RR/BYE），周期发送 SR + SDES（首包后 1 s，之后 5 s ±50%） |
| 编码支持 | H.264、H.265、AAC、G711、VP8 | H.264、H.265（本场景无音频） |
| 内存形态 | 峰值随包率与队列深度浮动 | 启动即固定：`RtspServer` 约 2.8 MiB（含 2 MiB 帧池），每会话约 34 KiB |

补充两点容易被忽略的差异：

1. 参考实现的 RTP 头由每个客户端写回同一块 `shared_ptr` 缓冲（`SetRtpHeader` 直接改 `pkt.data.get()+4`），因此依赖调度线程"顺序执行"这一隐含前提；一旦把调度线程数调大，同一包被多个线程改写。
2. 参考实现的 RTP 时间戳取发送时刻，帧在队列里排队多久，时间戳就差多久；本实现取编码器 PTS，排队只影响到达时间，不影响媒体时间轴。

## 4. 为什么这些差异在嵌入式上重要

- 抖动：本实现每轮只做"取一帧、扇出、按预算发包"，16 路 4 Mbit/s 下 loop P99 为 187 µs，相对 1 ms 维护 tick 约有 5 倍余量；参考实现每个包都要穿过任务队列、lambda 捕获与堆分配。
- 内存：固定池没有碎片，也没有"慢客户端把队列堆到上限"的峰值；参考实现的 10000 包上限意味着单连接最坏约 16 MB，在 512 MB 设备上不可忽视。
- CPU：零拷贝把用户态 memcpy 从"每路每包一次"降为"每 AU 一次（TCP）或零次（UDP）"。
- 可测性：所有上限都是编译期常量，CI 可以在目标架构上做符号级审计，而不是靠运行时观察。

## 5. 实测数据

条件：x86_64 共享虚机，8 路 TCP + 8 路 UDP，H.264 4 Mbit/s，60 s，客户端 `ffmpeg -c copy`。

| 指标 | 实测 |
|---|---:|
| 客户端存活 | 16 / 16 |
| RSS / FD / 线程 | 5600 KB / 40 / 3 |
| CPU 平均 / 峰值 | 20.42 % / 24.34 % |
| loop P50 / P95 / P99 | 7 / 105 / 187 µs |
| 输入 / 接受 / 丢弃 AU | 1578 / 1578 / 0 |
| gap / 队列丢弃 / 发送停滞 | 0 / 0 / 0 |
| 停机耗时 | 42 ms |

120 s 复跑：RSS 恒定 5600 KiB，CPU 平均 38.32 %、峰值 46.09 %，无内存增长。
短时复测（30 s，采样间隔 2 s）：RSS 3200 KiB，CPU 平均 14.76 %。

这些数字有三个边界：x86 不代表 ARM；测试机是共享虚机，绝对值有噪声；没有在同等条件下跑参考实现，所以不能据此推算"快多少倍"。

## 6. 代价与边界

| 取舍 | 代价 |
|---|---|
| 单 reactor | 单核上限；16 路 4 Mbit/s 有 5 倍余量，再高需要多 loop 分片 |
| 固定池 + 编译期上限 | 改容量要重编译并重跑回归 |
| 只做视频 | 无音频轨、无 A/V 同步 |
| 自研状态机与解析器 | 功能面窄于参考实现：无组播、无鉴权、无推流端 |
| 依赖编码器 PTS 单调 | B 帧重排输出需要上层先适配 |

## 7. 如何验证

- 主机侧：Debug 与 RelWithDebInfo 两套构建跑全量 `ctest`。
- 内存与并发：ASan/UBSan 与 TSan 两套 sanitizer 构建。
- 目标架构：aarch64 与 armhf 交叉编译后，用 qemu-user 执行全部测试。
- 零堆契约：对 `rs_core` 目标文件做符号审计，禁止 `operator new`、`malloc`、`__atomic_*`、`__cxa_throw` 与 `__dynamic_cast`。
- 规范：`-fno-exceptions -fno-rtti`，clang-format / cpplint / clang-tidy 三道门禁，工具版本固定。

## 8. 什么时候值得迁移

适合迁移：目标板内存紧张、客户端数量固定、需要可预测的 P99 与内存峰值、要接硬件编码器并处理参数集变化与 IDR 请求。

不适合迁移：需要音频、组播、鉴权、推流端，或者客户端规模远超 16 路、单核预算不够。

## 9. 小结

参考实现的强项是覆盖面与可读性，适合学习与原型；本实现的强项是资源确定性，适合量产设备。

真正决定嵌入式成败的不是"平均吞吐"，而是"最坏情况下还剩下多少 CPU 与内存"。单 reactor、固定池、共享 AU、单一时间基准这四件事，都是围绕这个目标做的减法。
