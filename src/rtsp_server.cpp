// Single-thread reactor server (design §3.1, §9.1, §9.5).
//
// fd -> connection mapping: AddFd carries `slot | (generation << 8)`, so an
// event captured for an fd that was closed and re-accepted inside the same
// poll batch is dropped on the generation check instead of being delivered to
// the new occupant of that fd.
//
// SPDX-License-Identifier: MIT
// newosp's EventLoop::Wake() ignores the pipe write result with a static_cast<void>
// cast, which GCC still reports under -Werror=unused-result for this TU.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#include "rs/rtsp_server.hpp"
#pragma GCC diagnostic pop

#include <cerrno>
#include <cstring>

#include <chrono>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/random.h>
#endif

namespace rs {
namespace {

void SetCloexec(int32_t fd) noexcept {
#if defined(FD_CLOEXEC)
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags >= 0) {
    static_cast<void>(::fcntl(fd, F_SETFD, flags | FD_CLOEXEC));
  }
#else
  static_cast<void>(fd);
#endif
}

void ConfigureTcpSocket(osp::TcpSocket& sock) noexcept {
  const int32_t fd = static_cast<int32_t>(sock.Fd());
  if (fd < 0) {
    return;
  }
  SetCloexec(fd);
  int value = static_cast<int>(kTcpSndBufBytes);
  static_cast<void>(osp::socket_api::SetSockOpt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)));
  value = static_cast<int>(kTcpRcvBufBytes);
  static_cast<void>(osp::socket_api::SetSockOpt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)));
#if defined(TCP_NOTSENT_LOWAT)
  unsigned int lowat = kTcpNotSentLowatBytes;
  static_cast<void>(osp::socket_api::SetSockOpt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat)));
#endif
#if defined(TCP_USER_TIMEOUT)
  unsigned int user_timeout = kTcpUserTimeoutMs;
  static_cast<void>(
      osp::socket_api::SetSockOpt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof(user_timeout)));
#endif
  value = 1;
  static_cast<void>(osp::socket_api::SetSockOpt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value)));
#if defined(TCP_KEEPIDLE)
  value = static_cast<int>(kTcpKeepIdleSec);
  static_cast<void>(osp::socket_api::SetSockOpt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value)));
#endif
#if defined(TCP_KEEPINTVL)
  value = static_cast<int>(kTcpKeepIntvlSec);
  static_cast<void>(osp::socket_api::SetSockOpt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value)));
#endif
#if defined(TCP_KEEPCNT)
  value = static_cast<int>(kTcpKeepCnt);
  static_cast<void>(osp::socket_api::SetSockOpt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof(value)));
#endif
}

constexpr uint8_t kReadable = static_cast<uint8_t>(osp::IoEvent::kReadable);
constexpr uint8_t kWritable = static_cast<uint8_t>(osp::IoEvent::kWritable);
constexpr uint8_t kError = static_cast<uint8_t>(osp::IoEvent::kError);
constexpr uint8_t kHangup = static_cast<uint8_t>(osp::IoEvent::kHangup);
#if defined(RS_HAVE_NEWOSP_RDHUP)
constexpr uint8_t kRdhup = static_cast<uint8_t>(osp::IoEvent::kRdhup);
#else
// Upstream newosp reports FIN as kReadable; read()==0 closes the connection.
constexpr uint8_t kRdhup = 0U;
#endif
constexpr uint8_t kReadInterest = static_cast<uint8_t>(kReadable | kError | kHangup | kRdhup);

static_assert(std::atomic<uint32_t>::is_always_lock_free, "source counters must stay lock-free on ARMv7");
constexpr uint64_t kAcceptBackoffNs = 100ULL * kNsPerMs;
constexpr uint32_t kMaxGen = 1U << 24U;
constexpr uint32_t kSlotBits = 8U;
constexpr uint32_t kSlotMask = (1U << kSlotBits) - 1U;

uint64_t RandomSeed() noexcept {
#if defined(__linux__)
  uint64_t v = 0U;
  if (::getrandom(&v, sizeof(v), 0) == static_cast<ssize_t>(sizeof(v))) {
    return v;
  }
#endif
  std::FILE* f = std::fopen("/dev/urandom", "rb");
  if (f != nullptr) {
    uint64_t v = 0U;
    const size_t n = std::fread(&v, 1, sizeof(v), f);
    std::fclose(f);
    if (n == sizeof(v)) {
      return v;
    }
  }
  return osp::SteadyNowNs();
}

uint32_t HostIpv4(const osp::SocketAddress& addr) noexcept {
  const auto octets = addr.Ipv4Address();
  return static_cast<uint32_t>(octets[0]) | (static_cast<uint32_t>(octets[1]) << kSlotBits) |
         (static_cast<uint32_t>(octets[2]) << (2U * kSlotBits)) |
         (static_cast<uint32_t>(octets[3]) << (3U * kSlotBits));
}

// Validates and copies the parameter sets into caller-owned storage. `dst`
// then points at `vps`/`sps`/`pps` instead of the caller's buffers.
bool CopyParamSets(const TrackParams& src, uint8_t* vps, uint8_t* sps, uint8_t* pps, TrackParams& dst) noexcept {
  if (src.codec != Codec::kH264 && src.codec != Codec::kH265) {
    return false;
  }
  if (src.vps_len > kMaxParamSetBytes || src.sps_len > kMaxParamSetBytes || src.pps_len > kMaxParamSetBytes) {
    return false;
  }
  if (ValidateParamSets(src) == false) {
    return false;
  }
  if (src.vps_len > 0U) {
    std::memcpy(vps, src.vps, src.vps_len);
  }
  std::memcpy(sps, src.sps, src.sps_len);
  std::memcpy(pps, src.pps, src.pps_len);
  dst = src;
  dst.vps = (src.vps_len > 0U) ? vps : nullptr;
  dst.sps = sps;
  dst.pps = pps;
  return true;
}

}  // namespace

void RtspServer::FillTcpInfo(Connection& c) noexcept {
  const int32_t fd = static_cast<int32_t>(c.sock.Fd());
  if (fd < 0) {
    return;
  }
  int32_t value = 0;
  socklen_t len = static_cast<socklen_t>(sizeof(value));
  if (::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, &len) == 0) {
    c.sndbuf = static_cast<uint32_t>(value);
  }
  len = static_cast<socklen_t>(sizeof(value));
  if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, &len) == 0) {
    c.rcvbuf = static_cast<uint32_t>(value);
  }
#if defined(TCP_INFO)
  struct tcp_info info{};
  len = static_cast<socklen_t>(sizeof(info));
  if (::getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &len) == 0) {
    c.tcp_rtt_us = info.tcpi_rtt;
    c.tcp_cwnd = info.tcpi_snd_cwnd;
    c.tcp_retrans = info.tcpi_total_retrans;
    c.tcp_unacked = info.tcpi_unacked;
#if defined(SIOCOUTQ)
    int outq = 0;
    if (::ioctl(fd, SIOCOUTQ, &outq) == 0 && outq > 0) {
      c.tcp_notsent = static_cast<uint32_t>(outq);
    }
#endif
  }
#endif
}

bool DeferredCloseBatch::Mark(uint32_t slot) noexcept {
  const bool ok = (slot < kCapacity) && pending_[slot] == false;
  if (ok) {
    pending_[slot] = true;
  }
  return ok;
}

bool DeferredCloseBatch::Pending(uint32_t slot) const noexcept {
  return (slot < kCapacity) && pending_[slot];
}

void DeferredCloseBatch::Flush() noexcept {
  for (uint32_t i = 0U; i < kCapacity; ++i) {
    if (pending_[i]) {
      pending_[i] = false;
      if (close_fn_ != nullptr) {
        close_fn_(ctx_, i);
      }
    }
  }
}

bool ConnectionTagValid(uintptr_t tag, uint32_t gen, bool active, bool closing) noexcept {
  const uint32_t slot = static_cast<uint32_t>(tag & kSlotMask);
  const uint32_t tag_gen = static_cast<uint32_t>(tag >> kSlotBits);
  return active && closing == false && slot < kMaxConnections && tag_gen == gen && tag_gen < kMaxGen;
}

RtspServer::RtspServer() noexcept {
  pending_close_.Configure(&RtspServer::CloseThunk, this);
  // The lifecycle HSM enters Init here; Start() drives the remaining arcs.
  svc_hsm_.Restart();
}

RtspServer::~RtspServer() noexcept {
  Stop();
}

// ===========================================================================
// Startup and shutdown
// ===========================================================================

bool RtspServer::OpenListener() noexcept {
  auto created = osp::TcpListener::Create();
  if (created.has_value() == false) {
    return false;
  }
  listener_ = std::move(created.value());
  const int32_t fd = static_cast<int32_t>(listener_.Fd());
  SetCloexec(fd);
  int32_t one = 1;
  static_cast<void>(
      osp::socket_api::SetSockOpt(fd, SOL_SOCKET, SO_REUSEADDR, &one, static_cast<socklen_t>(sizeof(one))));
  auto addr = osp::SocketAddress::FromIpv4("0.0.0.0", cfg_.port);
  if (addr.has_value() == false) {
    return false;
  }
  if (listener_.Bind(addr.value()).has_value() == false) {
    return false;
  }
  if (listener_.Listen(static_cast<int32_t>(kListenBacklog)).has_value() == false) {
    return false;
  }
  if (osp::SetFdNonBlocking(fd, true) < 0) {
    return false;
  }
  listener_fd_ = fd;
  return true;
}

void RtspServer::CloseListener() noexcept {
  if (listener_fd_ >= 0) {
    static_cast<void>(RemoveFd(listener_fd_));
    listener_fd_ = -1;
  }
  listener_armed_ = false;
  listener_.Close();
}

osp::expected<void, ServerError> RtspServer::Start(const ServerConfig& cfg) noexcept {
  if (running_.load(std::memory_order_acquire)) {
    return osp::expected<void, ServerError>::error(ServerError::kAlreadyRunning);
  }
  if (cfg.advertise_ip == nullptr || cfg.port == 0U || cfg.max_clients == 0U || cfg.max_clients > kMaxConnections) {
    return osp::expected<void, ServerError>::error(ServerError::kInvalidConfig);
  }
  if (svc::Hsm::TablesValid() == false || sess::Hsm::TablesValid() == false) {
    return osp::expected<void, ServerError>::error(ServerError::kBackendFailed);
  }
  cfg_ = cfg;
  last_input_seq_ = 0U;
  {
    std::lock_guard<std::mutex> lock(params_mutex_);
    params_pending_ = false;
  }
  idr_hooks_.requests.store(0U, std::memory_order_relaxed);
  idr_hooks_.last_request_ns = 0U;
  params_ = TrackParams{};
  codec_ = Codec::kUnknown;
  if (cfg_.id_seed == 0U) {
    cfg_.id_seed = RandomSeed();
  }
  // RAII: every failure path below releases the listener exactly once; only
  // the fully started server dismisses the guard.
  osp::ScopeGuard start_guard{osp::FixedFunction<void()>{[this]() noexcept { ReleaseStartHandles(); }}};
  if (CopyParamSets(cfg_.params, vps_, sps_, pps_, params_) == false) {
    return osp::expected<void, ServerError>::error(ServerError::kInvalidConfig);
  }
  codec_ = params_.codec;
  param_gen_ = 1U;
  stats_.codec = codec_;
  stats_.param_gen = param_gen_;
  stats_.vps_len = params_.vps_len;
  stats_.sps_len = params_.sps_len;
  stats_.pps_len = params_.pps_len;
  PublishStats();
  if (IsValid() == false) {
    return osp::expected<void, ServerError>::error(ServerError::kBackendFailed);
  }
  if (OpenListener() == false) {
    return osp::expected<void, ServerError>::error(ServerError::kBackendFailed);
  }
  if (AddFd(listener_fd_, kReadInterest, kListenerTag).has_value() == false) {
    return osp::expected<void, ServerError>::error(ServerError::kBackendFailed);
  }
  listener_armed_ = true;
  auto timer = Schedule(kMaintenancePeriodMs);
  if (timer.has_value() == false) {
    return osp::expected<void, ServerError>::error(ServerError::kBackendFailed);
  }
  maint_timer_ = timer.value();

  svc_ctx_.listener_ready = true;
  if (svc_hsm_.Dispatch(svc::kStartReady) == false || svc_ctx_.start_ok == false) {
    return osp::expected<void, ServerError>::error(ServerError::kBackendFailed);
  }

  ClearStop();
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  if (StartThreads() == false) {
    running_.store(false, std::memory_order_release);
    static_cast<void>(svc_hsm_.Dispatch(svc::kStartFail));
    return osp::expected<void, ServerError>::error(ServerError::kBackendFailed);
  }
  static_cast<void>(svc_hsm_.Dispatch(svc::kThreadStarted));
  start_guard.release();
  return osp::expected<void, ServerError>::success();
}

void RtspServer::ReleaseStartHandles() noexcept {
  if (maint_timer_ != 0U) {
    static_cast<void>(Cancel(maint_timer_));
    maint_timer_ = 0U;
  }
  CloseListener();
}

bool RtspServer::StartThreads() noexcept {
  osp::ThreadOptions options = cfg_.loop_thread;
  options.name = "rtsp-loop";
  return loop_thread_.Start(options, [this]() noexcept { LoopThreadMain(); });
}

void RtspServer::JoinThreads() noexcept {
  loop_thread_.join();
}

void RtspServer::LoopThreadMain() noexcept {
  EventLoop::Run();
#if defined(RS_HAVE_NEWOSP_HASFAILED)
  if (HasFailed()) {
    stats_.loop_failed = true;
  }
#endif
  PublishStats();
}

void RtspServer::Stop() noexcept {
  if (running_.load(std::memory_order_acquire) == false) {
    return;
  }
  stop_requested_.store(true, std::memory_order_release);
  // §9.5: refuse new frames, then stop and join the loop before touching the
  // poller or listener. RemoveFd/Close from this thread would otherwise race
  // with Wait() inside the loop thread.
  ingress_.Stop();
  EventLoop::Stop();
  Wake();
  JoinThreads();
  CloseListener();
  // The loop has returned: nobody can push empty slots any more, so the
  // producer's local slot can be handed back without breaking the SPSC rule.
  ingress_.ReleaseProducerSlot();

  const uint64_t now = osp::SteadyNowNs();
  for (uint32_t i = 0U; i < kMaxConnections; ++i) {
    if (conn_[i].active) {
      CloseConnection(i, kCloseServerStop, now);
    }
  }
  pending_close_.Flush();
  if (maint_timer_ != 0U) {
    static_cast<void>(Cancel(maint_timer_));
    maint_timer_ = 0U;
  }
  PublishStats();
  static_cast<void>(svc_hsm_.Dispatch(svc::kStopRequested));
  static_cast<void>(svc_hsm_.Dispatch(svc::kDrainDone));
  running_.store(false, std::memory_order_release);
}

osp::expected<void, ServerError> RtspServer::UpdateTrackParams(const TrackParams& params) noexcept {
  if (running_.load(std::memory_order_acquire) == false || stop_requested_.load(std::memory_order_acquire)) {
    return osp::expected<void, ServerError>::error(ServerError::kNotRunning);
  }
  {
    std::lock_guard<std::mutex> lock(params_mutex_);
    if (CopyParamSets(params, pending_vps_, pending_sps_, pending_pps_, pending_params_) == false) {
      return osp::expected<void, ServerError>::error(ServerError::kInvalidParams);
    }
    params_pending_ = true;
  }
  Wake();
  return osp::expected<void, ServerError>::success();
}

void RtspServer::ApplyPendingParams(uint64_t now) noexcept {
  {
    std::lock_guard<std::mutex> lock(params_mutex_);
    if (params_pending_ == false) {
      return;
    }
    std::memcpy(vps_, pending_vps_, pending_params_.vps_len);
    std::memcpy(sps_, pending_sps_, pending_params_.sps_len);
    std::memcpy(pps_, pending_pps_, pending_params_.pps_len);
    params_ = pending_params_;
    params_.vps = (params_.vps_len > 0U) ? vps_ : nullptr;
    params_.sps = sps_;
    params_.pps = pps_;
    params_pending_ = false;
  }
  codec_ = params_.codec;
  ++param_gen_;
  stats_.codec = codec_;
  stats_.param_gen = param_gen_;
  stats_.vps_len = params_.vps_len;
  stats_.sps_len = params_.sps_len;
  stats_.pps_len = params_.pps_len;
  // Design §9.4: no hot switch. Close every connection that still holds the
  // previous parameter snapshot; clients reconnect and DESCRIBE again.
  for (uint32_t i = 0U; i < kMaxConnections; ++i) {
    if (conn_[i].active) {
      CloseConnection(i, kCloseParamsChanged, now);
    }
  }
  pending_close_.Flush();
}

ServerStats RtspServer::Stats() const noexcept {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_;
}

ServerSnapshot RtspServer::Snapshot() const noexcept {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_full_;
}

void RtspServer::CopySnapshot(ServerSnapshot& out) const noexcept {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  out = snapshot_full_;
}

void RtspServer::RequestStop() noexcept {
  stop_requested_.store(true, std::memory_order_release);
  Wake();
}

bool RtspServer::PostShellCommand(const ShellCommand& command) noexcept {
  if (stop_requested_.load(std::memory_order_acquire)) {
    return false;
  }
  if (shell_cmds_.Push(command) == false) {
    return false;
  }
  Wake();
  return true;
}

SourceCallbacks RtspServer::Callbacks() noexcept {
  SourceCallbacks callbacks;
  callbacks.ctx = this;
  callbacks.wake = &RtspServer::WakeThunk;
  callbacks.drop = &RtspServer::DropThunk;
  callbacks.error = &RtspServer::ErrorThunk;
  return callbacks;
}

void RtspServer::WakeThunk(void* ctx) noexcept {
  static_cast<RtspServer*>(ctx)->Wake();
}

void RtspServer::DropThunk(void* ctx) noexcept {
  ++static_cast<RtspServer*>(ctx)->source_dropped_;
}

void RtspServer::ErrorThunk(void* ctx, uint32_t error) noexcept {
  static_cast<RtspServer*>(ctx)->source_error_.store(error, std::memory_order_relaxed);
}

// ===========================================================================
// Connections
// ===========================================================================

int32_t RtspServer::FindFreeSlot() const noexcept {
  for (uint32_t i = 0U; i < kMaxConnections; ++i) {
    if (conn_[i].active == false) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

void RtspServer::OnFd(int32_t fd, uint8_t events, uintptr_t user_data) noexcept {
  const uint64_t t0 = osp::SteadyNowNs();
  if (user_data == kListenerTag) {
    static_cast<void>(fd);
    OnListenerReadable(t0);
  } else if (user_data < kMaxConnections) {
    OnRtcpReadable(static_cast<uint32_t>(user_data), fd, t0);
  } else {
    const uint32_t slot = static_cast<uint32_t>(user_data & kSlotMask);
    // A slot marked closing keeps its fd open but must never receive events.
    if (slot < kMaxConnections &&
        ConnectionTagValid(user_data, conn_[slot].gen, conn_[slot].active, conn_[slot].closing)) {
      OnConnectionEvent(slot, events, t0);
    }
  }
  SampleLatency(t0);
}

void RtspServer::OnListenerReadable(uint64_t now) noexcept {
  // Accept is the only place a freed fd number can be recycled, so drain the
  // deferred closes of the previous dispatch batch before looking for peers.
  pending_close_.Flush();
  if (listener_armed_ == false || now < accept_retry_ns_) {
    return;
  }
  for (uint32_t round = 0U; round < kMaxConnections; ++round) {
    osp::SocketAddress peer;
#if defined(RS_HAVE_NEWOSP_ACCEPT4)
    auto accepted = listener_.Accept4(peer, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    auto accepted = listener_.Accept(peer);
#endif
    if (accepted.has_value() == false) {
      if (errno == EMFILE || errno == ENFILE) {
        // Remove the level-triggered listener instead of re-entering accept()
        // on every poll: the maintenance tick re-arms it after the backoff.
        static_cast<void>(RemoveFd(listener_fd_));
        listener_armed_ = false;
        accept_retry_ns_ = now + kAcceptBackoffNs;
      }
      break;  // EAGAIN or a transient accept error: retry next round.
    }
    osp::TcpSocket sock = std::move(accepted.value());
    const uint32_t peer_ipv4 = HostIpv4(peer);
    if (ip_limiter_.AllowAccept(peer_ipv4, now) == false) {
      ++stats_.accept_rate_limited;
      sock.Close();
      continue;
    }
    ++stats_.accepts;
    if (stats_.connections_active >= cfg_.max_clients) {
      ++stats_.accept_full;
      sock.Close();
      continue;
    }
    const int32_t slot = FindFreeSlot();
    if (slot < 0) {
      ++stats_.accept_full;
      sock.Close();
      continue;
    }
    ConfigureTcpSocket(sock);
    static_cast<void>(sock.SetNonBlocking(true));
    static_cast<void>(sock.SetNoDelay(true));

    Connection& c = conn_[slot];
    c.sock = std::move(sock);
    c.parser.Reset();
    c.gen = next_gen_;
    c.peer_ipv4 = peer_ipv4;
    next_gen_ = (next_gen_ + 1U) & (kMaxGen - 1U);
    if (next_gen_ == 0U) {
      next_gen_ = 1U;
    }
    c.connected_ns = now;
    c.interest = kReadInterest;
    c.active = true;
    c.closing = false;
    SessionConfig sc;
    sc.advertise_ip = cfg_.advertise_ip;
    sc.rtsp_port = cfg_.port;
    sc.id_seed = cfg_.id_seed ^ (static_cast<uint64_t>(c.gen) << 32U) ^ static_cast<uint64_t>(slot);
    sc.idr = &idr_hooks_;
    c.session.Configure(ingress_, params_, sc);
    c.session.SetPeerIpv4(HostIpv4(peer));

    const uintptr_t tag =
        static_cast<uintptr_t>(static_cast<uint32_t>(slot)) | (static_cast<uintptr_t>(c.gen) << kSlotBits);
    const int32_t fd = static_cast<int32_t>(c.sock.Fd());
    if (AddFd(fd, c.interest, tag).has_value() == false) {
      ++stats_.accept_failed;
      c.sock.Close();
      c.active = false;
      continue;
    }
    ++stats_.connections_active;
    if (stats_.connections_active > stats_.connections_peak) {
      stats_.connections_peak = stats_.connections_active;
    }
  }
}

void RtspServer::OnConnectionEvent(uint32_t slot, uint8_t events, uint64_t now) noexcept {
  Connection& c = conn_[slot];
  if ((events & kWritable) != 0U) {
    FlushConnection(slot, now);
    if (c.active == false || c.closing) {
      return;
    }
  }
  if ((events & kReadable) != 0U) {
    ReadConnection(slot, now);
    if (c.active == false || c.closing) {
      return;
    }
  } else if ((events & (kError | kHangup | kRdhup)) != 0U) {
    CloseConnection(slot, kClosePeerClosed, now);
    return;
  }
  if (now < c.stall_until_ns) {
    UpdateInterest(slot);
    return;
  }
  c.session.Pump(now);
  FlushConnection(slot, now);
  if (c.active && c.closing == false) {
    UpdateInterest(slot);
  }
}

void RtspServer::ReadConnection(uint32_t slot, uint64_t now) noexcept {
  Connection& c = conn_[slot];
  for (uint32_t round = 0U; round < 4U; ++round) {
    auto got = c.sock.Recv(rx_, sizeof(rx_));
    if (got.has_value() == false) {
      if (got.get_error() == osp::SocketError::kWouldBlock) {
        return;
      }
      CloseConnection(slot, kCloseTransportError, now);
      return;
    }
    if (got.value() == 0) {
      CloseConnection(slot, kClosePeerClosed, now);
      return;
    }
    FeedCtx ctx;
    ctx.self = this;
    ctx.slot = slot;
    ctx.now_ns = now;
    const ParserCallbacks cb{&ctx, &RtspServer::RequestThunk, &RtspServer::InterleavedThunk};
    auto fed = c.parser.Feed(rx_, static_cast<uint32_t>(got.value()), cb);
    if (fed.has_value() == false || ctx.close) {
      // F-06/F-12: malformed, oversized or unknown interleaved channel.
      CloseConnection(slot, kCloseProtocolError, now);
      return;
    }
    FlushConnection(slot, now);
    if (c.active == false || c.closing) {
      return;
    }
    if (got.value() < static_cast<int32_t>(sizeof(rx_))) {
      return;  // Socket drained.
    }
  }
}

void RtspServer::FlushConnection(uint32_t slot, uint64_t now) noexcept {
  Connection& c = conn_[slot];
  if (now < c.stall_until_ns) {
    return;
  }
  while (c.active && c.closing == false) {
    const osp::byte_span pending = c.session.PendingTx();
    if (pending.size() == 0U) {
      break;
    }
    auto sent = c.sock.Send(pending);
    if (sent.has_value() == false) {
      if (sent.get_error() == osp::SocketError::kWouldBlock) {
        ++stats_.would_block;
        break;
      }
      CloseConnection(slot, kClosePeerClosed, now);
      return;
    }
    const uint32_t n = static_cast<uint32_t>(sent.value());
    if (n == 0U) {
      break;
    }
    stats_.tx_bytes += n;
    c.session.ConsumeTx(n, now);
    if (n < pending.size()) {
      ++stats_.short_writes;
    }
  }
}

void RtspServer::UpdateInterest(uint32_t slot) noexcept {
  Connection& c = conn_[slot];
  if (c.closing) {
    return;
  }
  uint8_t want = kReadInterest;
  if (c.session.WantsWritable() && osp::SteadyNowNs() >= c.stall_until_ns) {
    want = static_cast<uint8_t>(want | kWritable);
  }
  if (want != c.interest) {
    if (ModifyFd(static_cast<int32_t>(c.sock.Fd()), want).has_value()) {
      c.interest = want;
    }
  }
}

void RtspServer::CloseConnection(uint32_t slot, CloseReason reason, uint64_t now) noexcept {
  Connection& c = conn_[slot];
  if (c.active == false || c.closing) {
    return;
  }
  ClearUdpWatch(slot);
  CloseReason actual = reason;
  if (c.session.Closed() && c.session.Reason() != kCloseNone) {
    actual = c.session.Reason();
  }
  c.session.Close(actual, now);
  stats_.queue_drops += c.session.Counters().queue_drops;
  rtcp_closed_sr_ += c.session.Counters().rtcp_sr;
  rtcp_closed_rr_ += c.session.Counters().rtcp_rr;
  rtcp_closed_bye_ += c.session.Counters().rtcp_bye;
  rtcp_closed_bad_ += c.session.Counters().rtcp_bad;
  if (actual == kClosePlayIdrTimeout) {
    ++stats_.idr_timeouts;
  }
  if (actual < kCloseCount) {
    ++stats_.close_reasons[actual];
  }
  // Remove the fd from the poller now, but keep it open until the batch ends:
  // closing it here would let accept() recycle the fd number while stale poll
  // results from this batch are still queued, and the old event would then be
  // delivered to the new connection.
  if (c.sock.IsValid()) {
    static_cast<void>(RemoveFd(static_cast<int32_t>(c.sock.Fd())));
  }
  c.closing = true;
  c.interest = 0U;
  static_cast<void>(pending_close_.Mark(slot));
}

void RtspServer::ClearUdpWatch(uint32_t slot) noexcept {
  if (slot >= kUdpWatchCount) {
    return;
  }
  UdpWatch& w = udp_watch_[slot];
  if (w.active && w.fd >= 0) {
    static_cast<void>(RemoveFd(w.fd));
  }
  w = UdpWatch{};
}

void RtspServer::SyncUdpWatch(uint32_t slot) noexcept {
  if (slot >= kUdpWatchCount) {
    return;
  }
  Connection& c = conn_[slot];
  UdpWatch& w = udp_watch_[slot];
  const int32_t fd = c.active && c.closing == false ? c.session.RtcpFd() : -1;
  if (fd < 0) {
    ClearUdpWatch(slot);
    return;
  }
  if (w.active && w.fd == fd && w.gen == c.gen) {
    return;
  }
  if (w.active) {
    static_cast<void>(RemoveFd(w.fd));
    w.active = false;
  }
  w.fd = fd;
  w.slot = slot;
  w.gen = c.gen;
  if (AddFd(fd, kReadInterest, static_cast<uintptr_t>(slot)).has_value()) {
    w.active = true;
  } else {
    w = UdpWatch{};
  }
}

void RtspServer::OnRtcpReadable(uint32_t slot, int32_t fd, uint64_t now) noexcept {
  if (slot >= kMaxConnections) {
    return;
  }
  UdpWatch& w = udp_watch_[slot];
  Connection& c = conn_[slot];
  if (w.active == false || w.fd != fd || c.active == false || c.closing || c.gen != w.gen) {
    return;
  }
  if (c.session.ReadRtcp(now) == false) {
    CloseConnection(slot, c.session.Reason(), now);
  }
}

void RtspServer::FinishClose(uint32_t slot) noexcept {
  Connection& c = conn_[slot];
  if (c.active == false) {
    return;
  }
  c.sock.Close();
  c.closing = false;
  c.active = false;
  if (stats_.connections_active > 0U) {
    --stats_.connections_active;
  }
}

void RtspServer::CloseThunk(void* ctx, uint32_t slot) noexcept {
  static_cast<RtspServer*>(ctx)->FinishClose(slot);
}

void RtspServer::DispatchRequest(FeedCtx& ctx, const RtspRequest& req) noexcept {
  Connection& c = conn_[ctx.slot];
  if (ip_limiter_.AllowRequest(c.peer_ipv4, ctx.now_ns) == false) {
    ++stats_.request_rate_limited;
    ctx.close = true;
    return;
  }
  ++stats_.requests;
  const uint16_t status = c.session.HandleRequest(req, ctx.now_ns);
  SyncUdpWatch(ctx.slot);
  if (status == 0U) {
    // Close after Feed() returns: the parser is still on the stack here.
    ctx.close = true;
  }
}

void RtspServer::DispatchInterleaved(FeedCtx& ctx, const InterleavedFrame& frame) noexcept {
  Connection& c = conn_[ctx.slot];
  if (c.session.OnInterleaved(frame.channel, frame.payload, ctx.now_ns) == false) {
    ctx.close = true;
  }
}

void RtspServer::RequestThunk(void* p, const RtspRequest& req) noexcept {
  auto* ctx = static_cast<FeedCtx*>(p);
  ctx->self->DispatchRequest(*ctx, req);
}

void RtspServer::InterleavedThunk(void* p, const InterleavedFrame& frame) noexcept {
  auto* ctx = static_cast<FeedCtx*>(p);
  ctx->self->DispatchInterleaved(*ctx, frame);
}

// ===========================================================================
// Media fan-out and maintenance
// ===========================================================================

void RtspServer::NotifyInputGap() noexcept {
  const uint64_t now = osp::SteadyNowNs();
  for (uint32_t i = 0U; i < kMaxConnections; ++i) {
    Connection& c = conn_[i];
    if (c.active && c.closing == false && c.session.Playing()) {
      c.session.OnInputGap(now);
    }
  }
}

void RtspServer::DrainIngress(uint64_t now) noexcept {
  FrameDesc desc;
  for (uint32_t n = 0U; n < kInputQueueDepth * 2U; ++n) {
    if (ingress_.Pop(desc) == false) {
      break;
    }
    ++stats_.au_in;
    if (ingress_.Validate(desc) == false) {
      ++stats_.frame_pool_drop;
      ingress_.Release(desc);
      continue;
    }
    // A sequence gap means the source dropped at least one AU: predictive
    // frames after the loss are undecodable, so every PLAY session waits for
    // the next IDR (review P0-5).
    if (0U != last_input_seq_ && desc.input_seq != last_input_seq_ + 1U) {
      stats_.input_gaps += desc.input_seq - last_input_seq_ - 1U;
      NotifyInputGap();
    }
    last_input_seq_ = desc.input_seq;
    for (uint32_t i = 0U; i < kMaxConnections; ++i) {
      Connection& c = conn_[i];
      if (c.active == false || c.closing || c.session.Playing() == false) {
        continue;
      }
      const bool waiting = c.session.AwaitingRtpInfo();
      c.session.OnFrame(desc, now);
      if (waiting && c.session.AwaitingRtpInfo() == false) {
        ++stats_.idr_waits;
      }
    }
    ++stats_.au_accepted;
    ingress_.Release(desc);  // Drop the loop's own reference.
  }
}

void RtspServer::PumpAll(uint64_t now) noexcept {
  for (uint32_t i = 0U; i < kMaxConnections; ++i) {
    Connection& c = conn_[i];
    if (c.active == false || c.closing || now < c.stall_until_ns) {
      continue;
    }
    c.session.Pump(now);
    FlushConnection(i, now);
    if (c.active && c.closing == false) {
      UpdateInterest(i);
    }
  }
}

void RtspServer::OnWake() noexcept {
  if (stop_requested_.load(std::memory_order_acquire)) {
    return;
  }
  const uint64_t now = osp::SteadyNowNs();
  // Wake runs before the next poll batch, so the previous batch is complete.
  pending_close_.Flush();
  DrainShellCommands(now);
  pending_close_.Flush();
  ApplyPendingParams(now);
  DrainIngress(now);
  PumpAll(now);
}

void RtspServer::RunMaintenance(uint64_t now) noexcept {
  if (listener_armed_ == false && listener_fd_ >= 0 && now >= accept_retry_ns_) {
    if (AddFd(listener_fd_, kReadInterest, kListenerTag).has_value()) {
      listener_armed_ = true;
    }
  }
  stats_.sessions_playing = 0U;
  for (uint32_t i = 0U; i < kMaxConnections; ++i) {
    Connection& c = conn_[i];
    if (c.active == false || c.closing || now < c.stall_until_ns) {
      continue;
    }
    // L-09: a connection that made no progress for 10 s (no request, no
    // response, no media) is closed; this is not "10 s since accept".
    if (c.session.Active() == false && (now - c.session.LastActivityNs()) > kIdleConnectNs) {
      CloseConnection(i, kCloseSessionTimeout, now);
      continue;
    }
    if (c.session.OnTick(now) == false) {
      CloseConnection(i, c.session.Reason(), now);
      continue;
    }
    FlushConnection(i, now);
    if (c.active && c.closing == false) {
      UpdateInterest(i);
      if (c.session.Playing()) {
        ++stats_.sessions_playing;
      }
    }
  }
}

void RtspServer::OnTimer(uint32_t timer_id) noexcept {
  if (timer_id != maint_timer_) {
    return;
  }
  // Timers fire after the fd dispatch loop of the current iteration.
  pending_close_.Flush();
  const uint64_t start = osp::SteadyNowNs();
  DrainShellCommands(start);
  pending_close_.Flush();
  ApplyPendingParams(start);
  DrainIngress(start);
  RunMaintenance(osp::SteadyNowNs());
  stats_.au_dropped = source_dropped_.load(std::memory_order_relaxed);
  stats_.source_stopped = source_error_.load(std::memory_order_relaxed) != 0U;
  PublishStats();
  SampleLatency(start);
}

void RtspServer::SampleLatency(uint64_t start_ns) noexcept {
  const uint64_t us = (osp::SteadyNowNs() - start_ns) / 1000ULL;
  const uint32_t bucket = (us < kLatencyBuckets) ? static_cast<uint32_t>(us) : (kLatencyBuckets - 1U);
  ++lat_hist_[bucket];
  ++lat_total_;
}

void RtspServer::ComputePercentiles() noexcept {
  if (lat_total_ == 0U) {
    return;
  }
  const uint64_t p50 = (lat_total_ * 50U) / 100U;
  const uint64_t p95 = (lat_total_ * 95U) / 100U;
  const uint64_t p99 = (lat_total_ * 99U) / 100U;
  uint64_t acc = 0U;
  bool got50 = false;
  bool got95 = false;
  bool got99 = false;
  for (uint32_t i = 0U; i < kLatencyBuckets; ++i) {
    acc += lat_hist_[i];
    if (got50 == false && acc >= p50) {
      stats_.loop_p50_us = i;
      got50 = true;
    }
    if (got95 == false && acc >= p95) {
      stats_.loop_p95_us = i;
      got95 = true;
    }
    if (got99 == false && acc >= p99) {
      stats_.loop_p99_us = i;
      got99 = true;
    }
    if (got50 && got95 && got99) {
      break;
    }
  }
  stats_.loop_samples = lat_total_;
}

void RtspServer::DrainShellCommands(uint64_t now) noexcept {
  ShellCommand cmd;
  for (uint32_t i = 0U; i < 8U; ++i) {
    if (shell_cmds_.Pop(cmd) == false) {
      break;
    }
    switch (cmd.id) {
      case ShellCommandId::kKick:
        if (cmd.arg0 < kMaxConnections && conn_[cmd.arg0].active && conn_[cmd.arg0].closing == false) {
          CloseConnection(cmd.arg0, kCloseAdminKick, now);
        } else {
          ++stats_.shell_rejects;
        }
        break;
      case ShellCommandId::kResetStats:
        ResetServerStats();
        break;
      case ShellCommandId::kResetClient:
        if (cmd.arg0 < kMaxConnections && conn_[cmd.arg0].active) {
          conn_[cmd.arg0].session.ResetCounters();
        } else {
          ++stats_.shell_rejects;
        }
        break;
      case ShellCommandId::kSetTrace:
        if (cmd.arg0 < kMaxConnections && conn_[cmd.arg0].active) {
          conn_[cmd.arg0].session.SetTrace(cmd.arg1 != 0U);
        } else {
          ++stats_.shell_rejects;
        }
        break;
      case ShellCommandId::kRequestIdr:
        if (idr_hooks_.Request(now, true) == false) {
          ++stats_.shell_rejects;
        }
        break;
#if defined(RS_ENABLE_FAULT_INJECTION)
      case ShellCommandId::kFaultDrop:
        ingress_.SetFaultDrop(cmd.arg0);
        break;
      case ShellCommandId::kFaultStall:
        if (cmd.arg0 < kMaxConnections && conn_[cmd.arg0].active) {
          conn_[cmd.arg0].stall_until_ns = (cmd.arg1 == 0U) ? 0U : (now + static_cast<uint64_t>(cmd.arg1) * kNsPerMs);
          UpdateInterest(cmd.arg0);
        } else {
          ++stats_.shell_rejects;
        }
        break;
      case ShellCommandId::kFaultEagain:
        if (cmd.arg0 < kMaxConnections && conn_[cmd.arg0].active) {
          conn_[cmd.arg0].session.SetFaultEagain(cmd.arg1);
        } else {
          ++stats_.shell_rejects;
        }
        break;
      case ShellCommandId::kFaultIdr:
        if (cmd.arg0 < kMaxConnections && conn_[cmd.arg0].active) {
          conn_[cmd.arg0].session.SetFaultIdr(cmd.arg1);
        } else {
          ++stats_.shell_rejects;
        }
        break;
      case ShellCommandId::kSlowClient:
        if (cmd.arg0 < kMaxConnections && conn_[cmd.arg0].active) {
          conn_[cmd.arg0].session.SetSlowBps(cmd.arg1, now);
        } else {
          ++stats_.shell_rejects;
        }
        break;
#endif
      default:
        ++stats_.shell_rejects;
        break;
    }
    ++stats_.shell_commands;
  }
}

void RtspServer::ResetServerStats() noexcept {
  const Codec codec = stats_.codec;
  const uint32_t param_gen = stats_.param_gen;
  const uint32_t vps_len = stats_.vps_len;
  const uint32_t sps_len = stats_.sps_len;
  const uint32_t pps_len = stats_.pps_len;
  const uint32_t active = stats_.connections_active;
  const uint32_t peak = stats_.connections_peak;
  const uint32_t playing = stats_.sessions_playing;
  const bool loop_failed = stats_.loop_failed;
  const bool source_stopped = stats_.source_stopped;
  stats_ = ServerStats{};
  stats_.codec = codec;
  stats_.param_gen = param_gen;
  stats_.vps_len = vps_len;
  stats_.sps_len = sps_len;
  stats_.pps_len = pps_len;
  stats_.connections_active = active;
  stats_.connections_peak = peak;
  stats_.sessions_playing = playing;
  stats_.loop_failed = loop_failed;
  stats_.source_stopped = source_stopped;
  std::memset(lat_hist_, 0, sizeof(lat_hist_));
  lat_total_ = 0U;
  last_input_seq_ = 0U;
  ingress_.SetFaultDrop(0U);
  source_dropped_.store(0U, std::memory_order_relaxed);
  source_error_.store(0U, std::memory_order_relaxed);
  idr_hooks_.requests.store(0U, std::memory_order_relaxed);
}

void RtspServer::PublishStats() noexcept {
  PublishSnapshot();
}

void RtspServer::PublishSnapshot() noexcept {
  ComputePercentiles();
  ServerSnapshot& out = snapshot_staging_;
  out.client_count = 0U;
  out.advertise_ip[0] = '\0';
  out.stats = stats_;
  out.pool = ingress_.Stats();
  out.listener_fd = listener_fd_;
  out.port = cfg_.port;
  out.max_clients = cfg_.max_clients;
  out.log_level = log_level_.load(std::memory_order_acquire);
  out.fault_drop_remaining = ingress_.FaultDropRemaining();
  out.idr_hook_set = idr_hooks_.fn.load(std::memory_order_acquire) != nullptr;
  out.stats.idr_requests = idr_hooks_.requests.load(std::memory_order_relaxed);
  if (cfg_.advertise_ip != nullptr) {
    std::strncpy(out.advertise_ip, cfg_.advertise_ip, sizeof(out.advertise_ip) - 1U);
    out.advertise_ip[sizeof(out.advertise_ip) - 1U] = '\0';
  }
  out.timers_used = (maint_timer_ != 0U) ? 1U : 0U;
  // Aggregate RTCP counters: closed sessions are folded into rtcp_closed_*.
  uint64_t sr = rtcp_closed_sr_;
  uint64_t rr = rtcp_closed_rr_;
  uint64_t bye = rtcp_closed_bye_;
  uint64_t bad = rtcp_closed_bad_;
  for (uint32_t i = 0U; i < kMaxConnections; ++i) {
    if (conn_[i].active) {
      sr += conn_[i].session.Counters().rtcp_sr;
      rr += conn_[i].session.Counters().rtcp_rr;
      bye += conn_[i].session.Counters().rtcp_bye;
      bad += conn_[i].session.Counters().rtcp_bad;
    }
  }
  out.stats.rtcp_sr = sr;
  out.stats.rtcp_rr = rr;
  out.stats.rtcp_bye = bye;
  out.stats.rtcp_bad = bad;
  const uint64_t now = osp::SteadyNowNs();
  const bool sample_tcp = (next_tcp_metrics_ns_ == 0U || now >= next_tcp_metrics_ns_);
  if (sample_tcp) {
    next_tcp_metrics_ns_ = now + kNsPerSec;
  }
  uint32_t active_nonclosing = 0U;
  for (uint32_t i = 0U; i < kMaxConnections; ++i) {
    Connection& c = conn_[i];
    if (c.active == false) {
      continue;
    }
    if (c.closing == false) {
      ++active_nonclosing;
    }
    ClientSnapshot& row = out.clients[out.client_count];
    row.slot = i;
    row.gen = c.gen;
    row.fd = static_cast<int32_t>(c.sock.Fd());
    row.closing = c.closing;
    row.stall_until_ns = c.stall_until_ns;
    row.connected_ms = (now > c.connected_ns) ? ((now - c.connected_ns) / 1000000ULL) : 0U;
    row.session = c.session.Snapshot(now);
    if (sample_tcp) {
      FillTcpInfo(c);
    }
    row.tcp_rtt_us = c.tcp_rtt_us;
    row.tcp_cwnd = c.tcp_cwnd;
    row.tcp_retrans = c.tcp_retrans;
    row.tcp_unacked = c.tcp_unacked;
    row.tcp_notsent = c.tcp_notsent;
    row.sndbuf = c.sndbuf;
    row.rcvbuf = c.rcvbuf;
    ++out.client_count;
  }
  uint32_t udp_watchers = 0U;
  for (uint32_t i = 0U; i < kUdpWatchCount; ++i) {
    if (udp_watch_[i].active) {
      ++udp_watchers;
    }
  }
  out.watchers_used = 1U + active_nonclosing + udp_watchers;
  out.published_ns = now;
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  snapshot_ = out.stats;
  snapshot_full_ = out;
}

// ===========================================================================
// Annex B source thread
// ===========================================================================

}  // namespace rs
