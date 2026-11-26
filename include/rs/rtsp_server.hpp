// Single-thread reactor server: listener, per-connection sessions, one fixed
// frame pool, one Annex B source thread (design §3, §9.1, §10.1).
//
// Threads: the loop thread owns every socket, parser and session; the source
// thread only writes frame slots and calls Wake(). No allocation, no logging
// and no blocking calls on the loop thread.
// SPDX-License-Identifier: MIT
#ifndef RS_RTSP_SERVER_HPP_
#define RS_RTSP_SERVER_HPP_

#include "osp/event_loop.hpp"
#include "osp/log.hpp"
#include "osp/socket.hpp"
#include "osp/spsc_ringbuffer.hpp"
#include "osp/thread.hpp"
#include "osp/vocabulary.hpp"
#include "rs/codec.hpp"
#include "rs/config.hpp"
#include "rs/frame_ingress.hpp"
#include "rs/ip_rate_limiter.hpp"
#include "rs/rtp_transport.hpp"
#include "rs/rtsp_parser.hpp"
#include "rs/rtsp_session.hpp"

#include <cstdint>
#include <cstdio>

#include <atomic>
#include <mutex>

namespace rs {

enum class ServerError : uint8_t { kInvalidConfig = 0, kAlreadyRunning, kNotRunning, kBackendFailed, kInvalidParams };

struct ServerConfig {
  uint16_t port = kDefaultRtspPort;
  TrackParams params{};  // Codec + parameter sets; copied by Start().
  const char* advertise_ip = "127.0.0.1";
  uint32_t max_clients = kMaxConnections;
  uint64_t id_seed = 0U;  // 0 = fill from the system RNG.
  osp::ThreadOptions loop_thread{};
  // kUnknown = auto-detect from the file header; kH264/kH265 must match it.
  Codec codec = Codec::kUnknown;
};

// POD snapshot; the loop writes through Publish(), readers get a copy.
struct ServerStats {
  Codec codec = Codec::kUnknown;
  uint32_t param_gen = 0U;  // Bumped by UpdateTrackParams().
  uint32_t vps_len = 0U;
  uint32_t sps_len = 0U;
  uint32_t pps_len = 0U;
  uint32_t connections_active = 0U;
  uint32_t connections_peak = 0U;
  uint32_t sessions_playing = 0U;
  uint64_t accepts = 0U;
  uint64_t accept_full = 0U;
  uint64_t accept_failed = 0U;
  uint64_t accept_rate_limited = 0U;
  uint64_t request_rate_limited = 0U;
  uint64_t requests = 0U;
  uint64_t au_in = 0U;
  uint64_t au_accepted = 0U;
  uint64_t au_dropped = 0U;       // Source-side Acquire/Commit failures.
  uint64_t frame_pool_drop = 0U;  // Descriptor failed validation at the loop.
  uint64_t input_queue_full = 0U;
  uint64_t idr_waits = 0U;
  uint64_t idr_requests = 0U;  // Encoder hooks invoked (rate limited).
  uint64_t input_gaps = 0U;    // Source sequence gaps -> AwaitIDR (P0-5).
  uint64_t rtcp_sr = 0U;       // Sender reports sent.
  uint64_t rtcp_rr = 0U;       // Receiver reports parsed.
  uint64_t rtcp_bye = 0U;      // BYE packets parsed.
  uint64_t rtcp_bad = 0U;      // Malformed RTCP packets.
  uint64_t idr_timeouts = 0U;
  uint64_t queue_drops = 0U;
  uint64_t tx_bytes = 0U;
  uint64_t short_writes = 0U;
  uint64_t would_block = 0U;
  uint64_t shell_commands = 0U;
  uint64_t shell_rejects = 0U;
  uint64_t close_reasons[kCloseCount] = {};
  uint32_t loop_p50_us = 0U;
  uint32_t loop_p95_us = 0U;
  uint32_t loop_p99_us = 0U;
  uint64_t loop_samples = 0U;
  bool loop_failed = false;
  bool source_stopped = false;
  uint32_t source_error = 0U;
};

// Read-only client row for the debug shell. Filled on the loop thread and
// copied by Snapshot(); pointers never escape the session.
struct ClientSnapshot {
  uint32_t slot = 0U;
  uint32_t gen = 0U;
  int32_t fd = -1;
  uint64_t connected_ms = 0U;
  uint64_t stall_until_ns = 0U;
  uint32_t tcp_rtt_us = 0U;
  uint32_t tcp_cwnd = 0U;
  uint32_t tcp_retrans = 0U;
  uint32_t tcp_unacked = 0U;
  uint32_t tcp_notsent = 0U;
  uint32_t sndbuf = 0U;
  uint32_t rcvbuf = 0U;
  bool closing = false;
  SessionSnapshot session;
};

struct ServerSnapshot {
  ServerStats stats;
  ClientSnapshot clients[kMaxConnections];
  uint32_t client_count = 0U;
  FrameIngressStats pool;
  int32_t listener_fd = -1;
  uint32_t watchers_used = 0U;  // app watchers: listener + active TCP
  uint32_t timers_used = 0U;
  uint16_t port = 0U;
  uint32_t max_clients = 0U;
  uint8_t log_level = 0U;  // osp::log::Level value, for shell display only.
  uint32_t fault_drop_remaining = 0U;
  bool idr_hook_set = false;
  char advertise_ip[64] = {};
  uint64_t published_ns = 0U;
};

enum class ShellCommandId : uint8_t {
  kKick = 0,
  kResetStats,
  kResetClient,
  kSetTrace,
  kFaultDrop,
  kFaultStall,
  kFaultEagain,
  kFaultIdr,
  kSlowClient,
  kRequestIdr
};

struct ShellCommand {
  ShellCommandId id = ShellCommandId::kKick;
  uint32_t arg0 = 0U;  // slot
  uint32_t arg1 = 0U;  // enable/disable
};

// Deferred close bookkeeping. A slot marked here stays active and keeps its fd
// open until the server reaches a flush point that is known to lie outside the
// current fd dispatch (wake, maintenance tick, or just before accept), so the
// kernel cannot hand the same fd number to accept() while stale poll results
// from the current batch are still queued.
class DeferredCloseBatch {
 public:
  using CloseFn = void (*)(void* ctx, uint32_t slot) noexcept;
  static constexpr uint32_t kCapacity = kMaxConnections;

  void Configure(CloseFn fn, void* ctx) noexcept {
    close_fn_ = fn;
    ctx_ = ctx;
  }
  // Returns false when `slot` is out of range or already pending.
  [[nodiscard]] bool Mark(uint32_t slot) noexcept;
  [[nodiscard]] bool Pending(uint32_t slot) const noexcept;
  // Closes every pending slot; call only from a point outside fd dispatch.
  void Flush() noexcept;

 private:
  CloseFn close_fn_ = nullptr;
  void* ctx_ = nullptr;
  bool pending_[kCapacity] = {};
};

static_assert(sizeof(DeferredCloseBatch) <= 64U, "deferred close set must stay small and fixed");

// Poll tag predicate: a slot only accepts an event when the generation still
// matches and the slot is neither closing nor free. Shared with unit tests so
// the stale-tag invariant is checked directly.
[[nodiscard]] bool ConnectionTagValid(uintptr_t tag, uint32_t gen, bool active, bool closing) noexcept;

// CRTP: EventLoop invokes OnFd/OnTimer/OnWake on this derived type with no
// vtable or std::function indirection on the loop path.
class RtspServer final : public osp::EventLoop<RtspServer, kMaxPollEvents, 8U> {
 public:
  RtspServer() noexcept;
  ~RtspServer() noexcept;
  RtspServer(const RtspServer&) = delete;
  RtspServer& operator=(const RtspServer&) = delete;

  [[nodiscard]] osp::expected<void, ServerError> Start(const ServerConfig& cfg) noexcept;
  void Stop() noexcept;
  [[nodiscard]] ServerStats Stats() const noexcept;
  [[nodiscard]] ServerSnapshot Snapshot() const noexcept;
  void CopySnapshot(ServerSnapshot& out) const noexcept;
  [[nodiscard]] bool Running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] bool StopRequested() const noexcept { return stop_requested_.load(std::memory_order_acquire); }
  // Public producer boundary for an external encoder. Acquire/Commit on this
  // ingress, then call Wake(); the loop owns all sessions and sockets.
  [[nodiscard]] FrameIngress& Ingress() noexcept { return ingress_; }
  void RequestStop() noexcept;
  // Test hook: make the next Start() fail after the listener is open.
  void SetFaultStartThreadsFail(bool fail) noexcept { fault_start_threads_ = fail; }
  // Set the log level for this server AND for the newosp logging backend, so
  // that library-internal messages and server messages share one threshold.
  void SetLogLevel(osp::log::Level level) noexcept {
    log_level_.store(static_cast<uint8_t>(level), std::memory_order_release);
    osp::log::SetLevel(level);
  }
  [[nodiscard]] osp::log::Level GetLogLevel() const noexcept {
    return static_cast<osp::log::Level>(log_level_.load(std::memory_order_acquire));
  }
  // Shell thread -> loop. Single producer (one shell session), fixed queue.
  [[nodiscard]] bool PostShellCommand(const ShellCommand& command) noexcept;
  // Producer callbacks for FileSource / an encoder adapter.
  [[nodiscard]] SourceCallbacks Callbacks() noexcept;
  // Register the encoder IDR hook. Safe to call at any time; sessions pick it
  // up through a shared atomic pointer. The hook runs on the loop thread.
  void SetIdrRequester(IdrRequestFn fn, void* ctx) noexcept {
    idr_hooks_.fn.store(nullptr, std::memory_order_release);
    idr_hooks_.ctx.store(ctx, std::memory_order_release);
    idr_hooks_.fn.store(fn, std::memory_order_release);
  }
  // Replace SPS/PPS/VPS at runtime (design §9.4). Copies the parameter sets
  // into server storage, bumps param_gen and closes existing sessions so new
  // clients do a fresh DESCRIBE/SETUP. Only valid while the server runs.
  [[nodiscard]] osp::expected<void, ServerError> UpdateTrackParams(const TrackParams& params) noexcept;

  // EventLoop hooks (loop thread).
  void OnFd(int32_t fd, uint8_t events, uintptr_t user_data) noexcept;
  void OnTimer(uint32_t timer_id) noexcept;
  void OnWake() noexcept;

 private:
  static constexpr uintptr_t kListenerTag = ~static_cast<uintptr_t>(0U);
  static constexpr uint32_t kLatencyBuckets = 256U;
  static constexpr uint32_t kUdpWatchCount = kMaxConnections;
  static constexpr uint64_t kIdleConnectNs = 10000000000ULL;

  struct Connection {
    osp::TcpSocket sock;
    RtspParser parser;
    RtspSession session;
    uint32_t gen = 0U;
    uint32_t peer_ipv4 = 0U;
    uint64_t connected_ns = 0U;
    // Sampled at a slower cadence than the 100 ms snapshot to keep loop
    // latency low; Snapshot() copies these cached values.
    uint32_t tcp_rtt_us = 0U;
    uint32_t tcp_cwnd = 0U;
    uint32_t tcp_retrans = 0U;
    uint32_t tcp_unacked = 0U;
    uint32_t tcp_notsent = 0U;
    uint32_t sndbuf = 0U;
    uint32_t rcvbuf = 0U;
    uint64_t stall_until_ns = 0U;  // Fault injection: pause media/flush.
    uint8_t interest = 0U;
    bool active = false;
    bool closing = false;  // Marked for deferred close: fd stays open.
  };

  struct FeedCtx {
    RtspServer* self = nullptr;
    uint32_t slot = 0U;
    uint64_t now_ns = 0U;
    bool close = false;
  };

  [[nodiscard]] bool OpenListener() noexcept;
  void CloseListener() noexcept;
  void ReleaseStartHandles() noexcept;
  void OnListenerReadable(uint64_t now) noexcept;
  void OnConnectionEvent(uint32_t slot, uint8_t events, uint64_t now) noexcept;
  void ReadConnection(uint32_t slot, uint64_t now) noexcept;
  void FlushConnection(uint32_t slot, uint64_t now) noexcept;
  void UpdateInterest(uint32_t slot) noexcept;
  void CloseConnection(uint32_t slot, CloseReason reason, uint64_t now) noexcept;
  void FinishClose(uint32_t slot) noexcept;
  void SyncUdpWatch(uint32_t slot) noexcept;
  void ClearUdpWatch(uint32_t slot) noexcept;
  void FillTcpInfo(Connection& c) noexcept;
  void OnRtcpReadable(uint32_t slot, int32_t fd, uint64_t now) noexcept;
  static void CloseThunk(void* ctx, uint32_t slot) noexcept;
  [[nodiscard]] int32_t FindFreeSlot() const noexcept;

  void DispatchRequest(FeedCtx& ctx, const RtspRequest& req) noexcept;
  void DispatchInterleaved(FeedCtx& ctx, const InterleavedFrame& frame) noexcept;
  static void RequestThunk(void* p, const RtspRequest& req) noexcept;
  static void InterleavedThunk(void* p, const InterleavedFrame& frame) noexcept;

  void DrainIngress(uint64_t now) noexcept;
  void NotifyInputGap() noexcept;
  void PumpAll(uint64_t now) noexcept;
  void RunMaintenance(uint64_t now) noexcept;
  void PublishStats() noexcept;
  void PublishSnapshot() noexcept;
  void ApplyPendingParams(uint64_t now) noexcept;
  void DrainShellCommands(uint64_t now) noexcept;
  void ResetServerStats() noexcept;
  void SampleLatency(uint64_t start_ns) noexcept;
  void ComputePercentiles() noexcept;

  static void WakeThunk(void* ctx) noexcept;
  static void DropThunk(void* ctx) noexcept;
  static void ErrorThunk(void* ctx, uint32_t error) noexcept;
  [[nodiscard]] bool StartThreads() noexcept;
  void JoinThreads() noexcept;
  void LoopThreadMain() noexcept;

  uint64_t last_input_seq_ = 0U;
  uint64_t rtcp_closed_sr_ = 0U;
  uint64_t rtcp_closed_rr_ = 0U;
  uint64_t rtcp_closed_bye_ = 0U;
  uint64_t rtcp_closed_bad_ = 0U;
  struct UdpWatch {
    int32_t fd = -1;
    uint32_t slot = 0U;
    uint32_t gen = 0U;
    bool active = false;
  };

  Connection conn_[kMaxConnections];
  UdpWatch udp_watch_[kUdpWatchCount];
  IpRateLimiter ip_limiter_;
  FrameIngress ingress_;
  TrackParams params_{};
  Codec codec_ = Codec::kUnknown;
  uint8_t vps_[kMaxParamSetBytes] = {};
  uint8_t sps_[kMaxParamSetBytes] = {};
  uint8_t pps_[kMaxParamSetBytes] = {};
  DeferredCloseBatch pending_close_{};
  ServerConfig cfg_{};
  IdrHooks idr_hooks_{};

  // Staged parameter-set update (control thread -> loop thread). The mutex is
  // taken only on the rare update path, never on the media path.
  mutable std::mutex params_mutex_;
  uint8_t pending_vps_[kMaxParamSetBytes] = {};
  uint8_t pending_sps_[kMaxParamSetBytes] = {};
  uint8_t pending_pps_[kMaxParamSetBytes] = {};
  TrackParams pending_params_{};
  bool params_pending_ = false;
  uint32_t param_gen_ = 0U;

  osp::TcpListener listener_{};
  int32_t listener_fd_ = -1;
  uint32_t next_gen_ = 1U;
  uint64_t next_tcp_metrics_ns_ = 0U;
  uint32_t maint_timer_ = 0U;
  uint64_t accept_retry_ns_ = 0U;
  bool listener_armed_ = false;
  bool fault_start_threads_ = false;
  uint8_t rx_[kRtspRxBytes] = {};

  // 32-bit lock-free producer status counters (callbacks from FileSource).
  std::atomic<uint32_t> source_dropped_{0U};
  std::atomic<uint32_t> source_error_{0U};

  osp::Thread loop_thread_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};

  svc::Context svc_ctx_{};
  svc::Hsm svc_hsm_{svc_ctx_};

  ServerStats stats_{};
  osp::SpscRingbuffer<ShellCommand, 8U> shell_cmds_;
  std::atomic<uint8_t> log_level_{static_cast<uint8_t>(osp::log::Level::kInfo)};
  uint32_t lat_hist_[kLatencyBuckets] = {};
  uint64_t lat_total_ = 0U;
  mutable std::mutex snapshot_mutex_;
  ServerStats snapshot_{};
  ServerSnapshot snapshot_full_{};
  ServerSnapshot snapshot_staging_{};
};

}  // namespace rs

#endif  // RS_RTSP_SERVER_HPP_
