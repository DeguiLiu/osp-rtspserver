**English** | [中文](README_zh.md)

# osp-rtspserver

[![CI](https://github.com/DeguiLiu/osp-rtspserver/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/osp-rtspserver/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A fixed-capacity RTSP 1.0 server built on [newosp](https://github.com/DeguiLiu/newosp).
One reactor thread owns the listener, RTSP TCP sockets, RTP/RTCP sockets and
timers. An encoder feeds complete access units through a bounded `FrameIngress`
plus SPSC queues and `Wake()`; the media path allocates nothing at run time and
clients share `FrameDesc` references instead of copying AU payloads.

## Features

- Single video track, H.264 or H.265 (RFC 6184 / RFC 7798), selected at startup.
- RTSP 1.0: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER.
- RTP/RTCP unicast over UDP and RTSP/TCP interleaved.
- RTCP SR/RR/SDES/BYE; new clients and loss recovery wait for an IDR.
- `SetIdrRequester()` with a global rate limit to ask the encoder for a keyframe.
- `UpdateTrackParams()` for runtime SPS/PPS/VPS changes; old sessions are closed.
- 16 fixed connection slots, per-client token bucket and slow-client isolation.
- Optional telnet debug shell and test-only fault injection.

Scope: Linux x86 is the current verification target. ARM Linux and the RS500
RISC-V RT-Thread BSP need a separate port and board test. See
[README_zh.md](README_zh.md) for the full capability matrix.

## Capability status

| Area | Status | Notes |
|---|---|---|
| H.264 / H.265 | Implemented, partially verified | One video track selected at startup; ffmpeg TCP/UDP decode tests exist |
| RTP/UDP unicast, RTSP/TCP interleaved | Implemented, partially verified | Both paths tested; VLC interop still open |
| New-client IDR wait | Implemented, partial coverage | PLAY and input gaps wait for an IDR; `SetIdrRequester()` asks the encoder |
| Runtime SPS/PPS update | Implemented | `UpdateTrackParams()` validates and copies, bumps `param_gen`, closes old sessions |
| RTCP SR/RR/SDES/BYE | Wired, interop pending | SR+SDES sent; RR/BYE parsed over TCP and UDP |
| 16 clients | Short x86 run | 8 TCP + 8 UDP, 4 Mbit/s each, 60 s; not a 600 s soak or board test |
| Audio, multicast, RTSP pusher, Digest, PAUSE, multi-track | Not implemented | Deploy only on trusted networks until authentication is added |

## Design constraints

1. Exactly one network thread owns every socket, session, frame reference and timer.
2. The encoder thread only performs `Acquire`/`Commit`/`Abort` and `Wake()`.
3. The media hot path does not allocate, log or block.
4. Control responses and TCP interleaved media share one byte stream; a response can never split an interleaved frame.
5. A source sequence gap moves affected clients to `AwaitIDR`; the next complete IDR resumes playback.

## Layout

```
include/rs/   public headers
src/          implementation
tests/        unit and end-to-end tests
tools/        deterministic test-stream generator
examples/     encoder-source integration demo
scripts/      build, E2E and stress helpers
docs/         test report, reviews and debug-shell design
```

## Build

`newosp` is looked up at `../newosp` by default; override with `NEWOSP_DIR`.

```bash
git clone git@github.com:DeguiLiu/newosp.git ../newosp
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRS_BUILD_E2E=ON -DRS_ENABLE_DEBUG_SHELL=ON -DRS_ENABLE_FAULT_INJECTION=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The upstream `newosp 0.9.6` baseline builds as-is. If the checkout provides
`TcpListener::Accept4`, `IoEvent::kRdhup` or `EventLoop::HasFailed`, CMake
enables them automatically; otherwise the server falls back to
`accept` + `fcntl`, `read()==0` half-close handling and a plain loop exit.

Minimal build without shell, fault injection or E2E:

```bash
cmake -S . -B build-min -DCMAKE_BUILD_TYPE=Release \
  -DRS_BUILD_E2E=OFF -DRS_ENABLE_DEBUG_SHELL=OFF -DRS_ENABLE_FAULT_INJECTION=OFF
cmake --build build-min -j"$(nproc)"
ctest --test-dir build-min --output-on-failure
```

## Run

```bash
./build/make_test_stream /tmp/test.h264 --aus 250 --gop 25
./build/rtspserver_demo --port 8554 --file /tmp/test.h264 --ip 127.0.0.1
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

`--ip` is the address advertised in SDP and RTP-Info. Use an address reachable
by the client, not `127.0.0.1`, when testing across machines.

## Encoder integration

```cpp
rs::ServerConfig cfg;
cfg.params = track_params;                 // copied by Start()
rs::RtspServer server;
server.SetIdrRequester(&OnIdrRequest, &encoder_state);  // loop thread, non-blocking
if (!server.Start(cfg).has_value()) {
  return 1;
}
// Encoder thread:
auto w = server.Ingress().Acquire(len);
// fill w.au, set w.desc.pts_ns and flags
server.Ingress().Commit(w, len);
server.Wake();

// SPS/PPS changed: existing sessions are closed and clients reconnect.
server.UpdateTrackParams(new_params);
```

`Ingress()` exposes only bounded Acquire/Commit; sockets, sessions, RTP/RTCP
and timers stay on the loop thread. A complete runnable example is
`examples/encoder_source_demo.cpp`.

## Resource profile

| Item | Per connection | 16 connections | Notes |
|---|---:|---:|---|
| RX buffer | 8 KiB | 128 KiB | `kRtspRxBytes` |
| TX buffer | 32 KiB | 512 KiB | control + TCP media, serialized |
| Session and cursor state | ~1 KiB | ~16 KiB | includes SDP scratch and queue |
| Frame pool | - | 2 MiB | 8 x 256 KiB shared by all clients |
| User-space total | - | ~2.7 MiB | excludes code, thread stacks and allocator |
| File descriptors | 2-4 | <= 80 | listener + control + UDP pairs + wake/poller |

These are design values measured on x86; they are not a board-level guarantee.

## Verification

- Unit tests cover parser, SDP, H.264/H.265 packetizers, RTCP, frame pool, HSM, session state machine and transport framing.
- End-to-end tests cover TCP/UDP playback, RTP-Info first-packet alignment, FU-A byte-exact reassembly, 8 concurrent clients, connection limits, slow-client isolation and error paths.
- CI runs Debug/RelWithDebInfo with E2E, a minimal build, ASan+UBSan and TSan.
- `docs/测试报告.md` records the environment, commands, results and residual risks.

## Debug shell

Build with `-DRS_ENABLE_DEBUG_SHELL=ON`, then start the demo with
`--shell-port 5090` and connect over telnet. Read-only commands include
`stats`, `clients`, `client <slot>`, `pool`, `fd`, `timers`, `stream`, `net`,
`rtcp`, `latency`, `mem`, `config`. Admin commands are `log`, `kick`, `reset`,
`trace`, `idr` and `shutdown`; `idr` requires a registered encoder hook.
See [docs/2026-11-26-调试命令面设计.md](docs/2026-11-26-调试命令面设计.md).

## Documentation

- [中文 README](README_zh.md)
- [Test report](docs/测试报告.md)
- [Current implementation review](docs/2026-11-26-当前实现评审与改进建议.md)
- [16-client stress and TCP tx evaluation](docs/2026-11-26-16路压力测试与TCP-tx评估.md)

## License

MIT, see [LICENSE](LICENSE).
