**English** | [中文](README_zh.md)

# osp-rtspserver

[![CI](https://github.com/DeguiLiu/osp-rtspserver/actions/workflows/ci.yml/badge.svg)](https://github.com/DeguiLiu/osp-rtspserver/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## What it is

A fixed-capacity RTSP 1.0 server for H.264 and H.265 video, built on
[newosp](https://github.com/DeguiLiu/newosp). One reactor thread owns every
socket, session and timer. An encoder pushes complete access units into a
bounded `FrameIngress`, and the server fans them out to clients without
copying payloads. The media path allocates nothing at run time.

## What it provides

| Capability | Status |
|---|---|
| RTSP 1.0: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER | Implemented |
| H.264 (RFC 6184) and H.265 (RFC 7798) Annex B parsing and RTP packetization | Implemented |
| RTP/RTCP over UDP unicast, RTSP over TCP interleaved | Implemented |
| RTCP SR, SDES, RR, BYE | Implemented |
| IDR wait for new clients and for frame-loss recovery | Implemented |
| `SetIdrRequester()` hook, rate-limited across sessions | Implemented |
| `UpdateTrackParams()` for runtime SPS/PPS/VPS changes | Implemented |
| 16 fixed connection slots, per-client token bucket, slow-client isolation | Implemented |
| Optional telnet debug shell, test-only fault injection | Optional, off by default |

## What it does not provide

- No audio track of any kind (AAC, G.711A, Opus).
- No authentication. Deploy only on a trusted network.
- No TLS.
- No multicast, and no RTSP push: this is a pull server, not an ANNOUNCE/RECORD pusher.
- No `PAUSE`; the method is parsed and answered with `405`.
- No multi-track and no multi-session. The aggregate URI is fixed to `/live`.
- No SDP renegotiation on an existing session; a parameter-set change closes the old session.
- No PLI/FIR RTCP feedback generation.
- No playback service. The file source is a test input, not a recording or replay feature.
- No RT-Thread or MCU port. The target platforms are Linux x86-64 and ARM Linux.
- No board-level performance guarantee. Every measurement is an x86-64 host measurement.

## Positioning

- Built as an application on top of newosp. It uses newosp's event loop, HSM
  table, SPSC ring buffers and vocabulary types, and adds no dependency of its own.
- Aimed at embedded Linux edge devices that already have an H.264/H.265 encoder
  and need to expose it to standard RTSP clients (VLC, ffplay, NVR software).
- Fixed capacity by design: every buffer, queue and connection slot has a
  compile-time bound, so the memory footprint is known before deployment.
- The current stage is "runs and is tested on a host". It has not been
  measured on an ARM board.
- Not a replacement for a general-purpose media server. It covers single-track
  video pull only, and deliberately leaves audio, authentication and multicast
  out of scope.

## Usage

### Prerequisites

- CMake 3.16+, a C++17 compiler, Linux (x86-64 or ARM).
- A checkout of newosp next to this repository, or a path passed via `NEWOSP_DIR`.

### Build

```bash
git clone git@github.com:DeguiLiu/newosp.git ../newosp
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DRS_BUILD_E2E=ON -DRS_ENABLE_DEBUG_SHELL=ON -DRS_ENABLE_FAULT_INJECTION=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### Run with a file source

```bash
./build/make_test_stream /tmp/test.h264 --aus 250 --gop 25
./build/rtspserver_demo --port 8554 --file /tmp/test.h264 --ip 127.0.0.1
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/live
```

`--ip` is the address advertised in SDP and RTP-Info. Across machines, pass an
address the client can reach instead of `127.0.0.1`.

For H.265, use `--codec h265` with the generator, or produce a real stream with
`scripts/make_real_stream.sh out.h265 h265 4`.

### Run with an external encoder

```cpp
rs::ServerConfig cfg;
cfg.params = track_params;                          // codec + SPS/PPS/VPS
rs::RtspServer server;
server.SetIdrRequester(&OnIdrRequest, &encoder_state);  // loop thread, must not block
if (!server.Start(cfg).has_value()) {
  return 1;
}

// Encoder thread:
auto w = server.Ingress().Acquire(len);
// fill w.au, set w.desc.pts_ns and flags
server.Ingress().Commit(w, len);
server.Wake();

// Parameter sets changed: old sessions are closed, clients reconnect.
server.UpdateTrackParams(new_params);
```

`Ingress()` exposes only bounded `Acquire`/`Commit`; sockets, sessions, RTP,
RTCP and timers stay on the loop thread. A complete runnable program is in
`examples/encoder_source_demo.cpp`.

### Debug shell

Build with `-DRS_ENABLE_DEBUG_SHELL=ON`, then pass `--shell-port 5090` to the
demo and connect over telnet. Read-only commands include `stats`, `clients`,
`client <slot>`, `pool`, `fd`, `timers`, `stream`, `rtcp`, `latency`, `mem`,
`config`. Admin commands are `log`, `kick`, `reset`, `trace`, `idr` and
`shutdown`; `idr` requires a registered encoder hook. The command surface is
specified in [docs/2026-11-26-调试命令面设计.md](docs/2026-11-26-调试命令面设计.md).

## Documentation

- [Test report](docs/测试报告.md)
- [Current implementation review](docs/2026-11-26-当前实现评审与改进建议.md)
- [16-client stress and TCP tx evaluation](docs/2026-11-26-16路压力测试与TCP-tx评估.md)
- [Engineering comparison with coact](docs/2026-11-26-对标coact的工程配置与代码风格调整建议.md)

## License

MIT, see [LICENSE](LICENSE).
