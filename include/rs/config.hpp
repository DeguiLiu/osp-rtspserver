// Compile-time limits for the newosp-based RTSP server (v1).
// All capacities are fixed; no dynamic growth on the media path.
//
// The log level lives in newosp (osp::log::Level) and is deliberately NOT
// redeclared here: two enumerations with opposite ordering (this file used a
// "verbosity rank", newosp uses a "severity rank") invite silent misreads at
// every comparison site. The header is not included here either - a public
// header must not drag the logging backend into every translation unit.
// SPDX-License-Identifier: MIT
#ifndef RS_CONFIG_HPP_
#define RS_CONFIG_HPP_

#include <cstdint>

namespace rs {

// Connections
constexpr uint32_t kMaxConnections = 16U;  // RTSP control connections, incl. not-yet-SETUP.
constexpr uint32_t kMaxUriBytes = 256U;
constexpr uint32_t kMaxHeaderCount = 24U;
constexpr uint32_t kMaxHeaderBytes = 2048U;
constexpr uint32_t kRtspRxBytes = 8192U;     // Per-connection inbound linear buffer.
constexpr uint32_t kRtspTxBytes = 32768U;    // Per-connection outbound arena (control + TCP media).
constexpr uint32_t kControlTxLimit = 8192U;  // Control-only backpressure threshold.

// Media
constexpr uint32_t kFrameSlots = 8U;            // Power of two.
constexpr uint32_t kEmptySlotQueueDepth = 16U;  // Power of two, >= kFrameSlots.
constexpr uint32_t kInputQueueDepth = 8U;       // Power of two, <= kFrameSlots.
constexpr uint32_t kMaxAuBytes = 256U * 1024U;  // One access unit.
constexpr uint32_t kRtpPayloadMax = 1200U;
// One session may emit at most this many media bytes (and packets) per loop
// pass. 32 KiB per 40 ms pass covers a 4 Mbit/s stream with headroom while
// bounding how long one client can hold the single reactor thread.
constexpr uint32_t kPumpBudgetBytes = 32U * 1024U;
constexpr uint32_t kMaxPacketsPerPump = 32U;
constexpr uint32_t kMaxNalUnitsPerAu = 256U;
constexpr uint32_t kMaxParamSetBytes = 256U;
constexpr uint32_t kClockRate = 90000U;
constexpr uint32_t kFps = 25U;
constexpr uint32_t kRtpTsStep = kClockRate / kFps;

// Timers (milliseconds)
constexpr uint32_t kSessionTimeoutMs = 60000U;
constexpr uint32_t kPlayIdrTimeoutMs = 2000U;
// Encoder IDR requests are advisory; one per second covers a 2 s GOP.
constexpr uint32_t kIdrRequestIntervalMs = 1000U;
constexpr uint32_t kSendStallTimeoutMs = 2000U;
constexpr uint32_t kMaintenancePeriodMs = 100U;
constexpr uint32_t kSourceLossTimeoutMs = 5000U;
constexpr uint32_t kStatsLogPeriodMs = 5000U;
constexpr uint32_t kRtcpSrPeriodMs = 5000U;  // RFC 3550 average.
constexpr uint32_t kRtcpSrJitterMs = 2500U;  // +/- 50% to avoid synchronised SR.
constexpr uint32_t kMaxRtcpBytes = 128U;     // SR + SDES(CNAME) budget.

// Network
constexpr uint16_t kDefaultRtspPort = 8554U;  // Unprivileged default for tests.
constexpr uint32_t kMaxPortNumber = 0xFFFFU;
constexpr uint16_t kDefaultRtcpPortBase = 0U;  // 0 = ephemeral bind.
constexpr uint32_t kListenBacklog = 32U;
constexpr uint32_t kMaxPollEvents = 64U;

// TCP socket memory/latency bounds (Linux doubles SO_SNDBUF/SO_RCVBUF).
constexpr uint32_t kTcpSndBufBytes = 64U * 1024U;
constexpr uint32_t kTcpRcvBufBytes = 32U * 1024U;
constexpr uint32_t kTcpNotSentLowatBytes = 16U * 1024U;
constexpr uint32_t kTcpUserTimeoutMs = 10000U;
constexpr uint32_t kUdpSndBufBytes = 128U * 1024U;
constexpr uint32_t kUdpRcvBufBytes = 32U * 1024U;

// Per-IP admission. Bursts keep the 16-client test and reconnect pattern
// working while bounding a single peer's accept/request rate.
constexpr uint32_t kIpRateTableSize = 32U;
constexpr uint32_t kIpAcceptBurst = 32U;
constexpr uint32_t kIpAcceptRatePerSec = 8U;
constexpr uint32_t kIpRequestBurst = 256U;
constexpr uint32_t kIpRequestRatePerSec = 128U;

// TCP keepalive and UDP DSCP (EF = 0xB8). Set kUdpIpTos=0 to leave default.
constexpr uint32_t kTcpKeepIdleSec = 30U;
constexpr uint32_t kTcpKeepIntvlSec = 10U;
constexpr uint32_t kTcpKeepCnt = 3U;
constexpr uint32_t kUdpIpTos = 0xB8U;

// RTP wire constants (RFC 3550 / RFC 6184 / RFC 7798).
constexpr uint8_t kRtpPayloadType = 96U;
constexpr uint8_t kRtpVersionByte = 0x80U;  // V=2, P=0, X=0, CC=0.
constexpr uint8_t kRtpMarkerMask = 0x80U;
constexpr uint32_t kRtpHeaderBytes = 12U;
constexpr uint32_t kInterleavedHeaderBytes = 4U;
constexpr uint32_t kMaxInterleavedFrameBytes = 0xFFFFU;
constexpr uint32_t kMaxInterleavedChannel = 127U;
constexpr uint32_t kByteMask = 0xFFU;

// Unit conversions used by timers and RTP timestamps.
constexpr uint64_t kNsPerMs = 1000000ULL;
constexpr uint64_t kNsPerSec = 1000000000ULL;
constexpr uint32_t kMsPerSec = 1000U;

// Per-client egress shaping and build identity.
constexpr uint32_t kClientEgressRateBps = 8U * kMsPerSec * kMsPerSec;
constexpr uint32_t kClientEgressBurstBytes = 32U * 1024U;
// RS_VERSION comes from CMake project(VERSION); the fallback keeps this
// header self-contained for a compiler that does not define it.
#if defined(RS_VERSION)
constexpr const char* kVersion = RS_VERSION;
#else
constexpr const char* kVersion = "0.0.0-dev";
#endif

enum class TransportKind : uint8_t { kNone = 0, kUdp, kTcp };

// Target cache-line assumption. 64 bytes is used by current ARM/RISC-V cores;
// alignas() remains safe if a target uses a smaller line.
constexpr uint32_t kCacheLineBytes = 64U;
static_assert((kCacheLineBytes & (kCacheLineBytes - 1U)) == 0U, "cache line must be a power of two");

static_assert((kFrameSlots & (kFrameSlots - 1U)) == 0U, "kFrameSlots must be a power of two");
static_assert((kEmptySlotQueueDepth & (kEmptySlotQueueDepth - 1U)) == 0U, "empty slot queue must be a power of two");
static_assert((kInputQueueDepth & (kInputQueueDepth - 1U)) == 0U, "input queue must be a power of two");
static_assert(kEmptySlotQueueDepth >= kFrameSlots, "empty slot queue must cover all slots");
static_assert(kInputQueueDepth <= kFrameSlots, "input queue deeper than the pool cannot drain");
static_assert(kMaxConnections <= 256U, "connection slot must fit advertised tag limits");
static_assert(kRtpPayloadMax <= 1200U, "keep RTP payload inside a 1280-byte MTU budget");
static_assert(kMaxPollEvents <= 64U, "newosp IoPoller tag table holds 64 fds");

}  // namespace rs

#endif  // RS_CONFIG_HPP_
