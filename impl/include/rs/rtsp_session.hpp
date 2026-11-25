// Per-connection RTSP session: method handling, session state machine,
// transport setup, and the serial TCP/UDP media egress (design §4.5, §6, §8).
//
// One output byte stream per connection: RTSP responses and TCP interleaved
// frames are appended to the same bounded buffer, so a partially flushed
// interleaved frame can never be interleaved with a response in the middle of
// its bytes. The server flushes PendingTx() and reports progress through
// ConsumeTx().
//
// Loop thread only; no allocation, no blocking.
// SPDX-License-Identifier: MIT
#ifndef RS_RTSP_SESSION_HPP_
#define RS_RTSP_SESSION_HPP_

#include "rs/config.hpp"
#include "rs/codec.hpp"
#include "rs/frame_ingress.hpp"
#include "rs/h264_packetizer.hpp"
#include "rs/h265_packetizer.hpp"
#include "rs/media_sender.hpp"
#include "rs/rtcp_endpoint.hpp"
#include "rs/rtp_transport.hpp"
#include "rs/rtsp_parser.hpp"
#include "rs/tx_stream.hpp"
#include "rs/session_hsm.hpp"

#include "osp/span.hpp"
#include "osp/vocabulary.hpp"

#include <atomic>
#include <cstdint>
#include <variant>

namespace rs {

// RTSP status codes used by this implementation (design §6.4).
enum class RtspStatus : uint16_t {
  kOk = 200,
  kBadRequest = 400,
  kNotFound = 404,
  kMethodNotAllowed = 405,
  kNotAcceptable = 406,
  kRequestTooLarge = 413,
  kUriTooLong = 414,
  kUnsupportedMedia = 415,
  kNotEnoughBandwidth = 453,
  kSessionNotFound = 454,
  kInvalidState = 455,
  kInvalidRange = 457,
  kAggregateNotAllowed = 459,
  kUnsupportedTransport = 461,
  kInternalError = 500,
  kNotImplemented = 501,
  kServiceUnavailable = 503,
  kVersionNotSupported = 505
};

[[nodiscard]] const char* ReasonPhrase(uint16_t status) noexcept;

// Reason a connection/session was closed. Kept as a plain index so the server
// can keep one counter per entry without a switch in the hot path.
enum CloseReason : uint8_t {
  kCloseNone = 0,
  kCloseTeardown,
  kCloseSessionTimeout,
  kClosePlayIdrTimeout,
  kCloseSendStall,
  kCloseControlOverflow,
  kClosePeerClosed,
  kCloseTransportError,
  kCloseProtocolError,
  kCloseServerStop,
  kCloseAdminKick,
  kCloseParamsChanged,
  kCloseCount
};

[[nodiscard]] const char* CloseReasonName(CloseReason reason) noexcept;

// Request an IDR from the encoder. Called on the loop thread, so the hook must
// not block; typical implementations set a flag or push a command to the
// encoder control queue. `Request()` is rate limited globally across sessions.
using IdrRequestFn = void (*)(void* ctx) noexcept;

struct IdrHooks {
  std::atomic<IdrRequestFn> fn{nullptr};
  std::atomic<void*> ctx{nullptr};
  std::atomic<uint32_t> requests{0U};
  uint64_t last_request_ns = 0U;  // Loop thread only.

  // Returns true when the hook was invoked. The rate limit is shared by every
  // session; `force` bypasses it for an explicit admin request.
  bool Request(uint64_t now_ns, bool force = false) noexcept {
    IdrRequestFn hook = fn.load(std::memory_order_acquire);
    if (hook == nullptr) {
      return false;
    }
    constexpr uint64_t kMinIntervalNs = static_cast<uint64_t>(kIdrRequestIntervalMs) * kNsPerMs;
    if (!force && last_request_ns != 0U && (now_ns - last_request_ns) < kMinIntervalNs) {
      return false;
    }
    last_request_ns = now_ns;
    requests.fetch_add(1U, std::memory_order_relaxed);
    hook(ctx.load(std::memory_order_acquire));
    return true;
  }
};

struct SessionConfig {
  const char* advertise_ip = "0.0.0.0";  // SDP c= / Content-Base address.
  uint16_t rtsp_port = kDefaultRtspPort;
  uint64_t id_seed = 0U;                 // Secure seed from the server.
  const std::atomic<uint8_t>* log_level = nullptr;  // Server log level (debug trace).
  IdrHooks* idr = nullptr;               // Shared encoder hook; server owned.
};

struct SessionCounters {
  uint32_t queue_drops = 0U;  // F-02: per-session AU queue overrun.
  uint32_t udp_drops = 0U;    // F-11: UDP send failure/EAGAIN.
  uint32_t token_waits = 0U;  // Token bucket paused the cursor (no AU dropped).
  uint32_t interleaved_errors = 0U;
  uint32_t frames_completed = 0U;
  uint32_t packets_sent = 0U;
  uint32_t input_gaps = 0U;    // Source sequence gaps seen (review P0-5).
  uint32_t idr_requests = 0U;  // Encoder IDR requests emitted by this session.
  uint32_t rtcp_sr = 0U;       // Sender reports emitted.
  uint32_t rtcp_rr = 0U;       // Receiver reports parsed.
  uint32_t rtcp_bye = 0U;      // BYE packets parsed.
  uint32_t rtcp_bad = 0U;      // Malformed RTCP datagrams/packets.
};

// Read-only snapshot for the debug shell. Filled by the loop thread and copied
// by the shell thread; no pointers into session-owned storage.
struct SessionSnapshot {
  int32_t state = 0;
  uint8_t transport = 0U;  // TransportKind
  bool playing = false;
  bool active = false;
  bool awaiting_rtp_info = false;
  bool closed = false;
  bool trace_enabled = false;
  uint32_t queue_depth = 0U;
  uint32_t queue_bytes = 0U;
  uint32_t oldest_age_ms = 0U;
  uint32_t fault_eagain = 0U;
  uint32_t fault_idr = 0U;
  uint32_t slow_bps = 0U;
  uint16_t seq = 0U;
  uint32_t rtp_ts = 0U;
  uint32_t ssrc = 0U;
  uint32_t client_ipv4 = 0U;
  uint16_t client_rtp_port = 0U;
  uint16_t client_rtcp_port = 0U;
  uint16_t server_rtp_port = 0U;
  uint16_t server_rtcp_port = 0U;
  uint8_t rtp_channel = 0U;
  uint8_t rtcp_channel = 0U;
  SessionCounters counters;
  CloseReason close_reason = kCloseNone;
  char session_id_prefix[9] = {};
  uint64_t last_activity_ns = 0U;
};

static_assert(std::is_nothrow_move_constructible<std::variant<ClientCursor, HevcCursor>>::value,
              "codec cursor variant must be nothrow movable");
static_assert(std::is_trivially_copyable<FrameDesc>::value, "FrameDesc must stay trivially copyable");

class RtspSession {
 public:
  static constexpr uint32_t kMaxInterleavedBytes = 4U + 12U + kRtpPayloadMax;

  RtspSession() noexcept;
  ~RtspSession() noexcept;
  RtspSession(const RtspSession&) = delete;
  RtspSession& operator=(const RtspSession&) = delete;

  // Binds shared state and re-enters the Init state. Used when a connection
  // slot is reused by a new peer. `params.codec` selects the packetizer; the
  // parameter-set pointers must stay valid for the session's lifetime.
  void Configure(FrameIngress& ingress, const TrackParams& params, const SessionConfig& cfg) noexcept;
  // Peer address for UDP SETUP, host byte order (0 = unknown until accepted).
  void SetPeerIpv4(uint32_t host_ipv4) noexcept { client_ipv4_ = host_ipv4; }

  // Control plane. Returns the status written, or 0 when the session must be
  // closed. `now_ns` is a monotonic timestamp.
  [[nodiscard]] uint16_t HandleRequest(const RtspRequest& req, uint64_t now_ns) noexcept;
  // Returns false for an unknown interleaved channel (F-12: close).
  [[nodiscard]] bool OnInterleaved(uint8_t channel, osp::byte_span payload, uint64_t now_ns) noexcept;
  // UDP RTCP path: drain the RTCP socket; false when the session was closed.
  [[nodiscard]] bool ReadRtcp(uint64_t now_ns) noexcept;
  [[nodiscard]] int32_t RtcpFd() const noexcept { return udp_.RtcpFd(); }
  void OnPeerClosed(uint64_t now_ns) noexcept;

  // Media plane, loop thread only.
  void OnFrame(const FrameDesc& desc, uint64_t now_ns) noexcept;
  // A dropped source frame invalidates prediction: drop queued media and
  // wait for the next IDR (review P0-5).
  void OnInputGap(uint64_t now_ns) noexcept;
  void Pump(uint64_t now_ns) noexcept;

  // Maintenance: timeouts, stalled output, pending media. Returns false when
  // the connection has to be closed (reason in Reason()).
  [[nodiscard]] bool OnTick(uint64_t now_ns) noexcept;

  // Outbound byte stream shared by control responses and TCP interleaved media.
  [[nodiscard]] osp::byte_span PendingTx() const noexcept;
  [[nodiscard]] bool WantsWritable() const noexcept;
  void ConsumeTx(uint32_t bytes, uint64_t now_ns) noexcept;

  void Close(CloseReason reason, uint64_t now_ns) noexcept;
  void SetTrace(bool enable) noexcept;
  [[nodiscard]] bool TraceEnabled() const noexcept { return trace_enabled_; }
  void ResetCounters() noexcept { counters_ = SessionCounters{}; }
  void SetFaultEagain(uint32_t count) noexcept { fault_eagain_ = count; }
  void SetFaultIdr(uint32_t count) noexcept { fault_idr_ = count; }
  void SetSlowBps(uint32_t bps, uint64_t now_ns) noexcept;

  [[nodiscard]] bool Closed() const noexcept { return closed_; }
  [[nodiscard]] bool Playing() const noexcept { return hsm_.IsInState(sess::kPlaying); }
  [[nodiscard]] bool Active() const noexcept { return hsm_.IsInState(sess::kActive); }
  [[nodiscard]] int32_t State() const noexcept { return hsm_.State(); }
  [[nodiscard]] bool HasSession() const noexcept { return session_id_len_ != 0U; }
  [[nodiscard]] bool AwaitingRtpInfo() const noexcept { return awaiting_rtp_info_; }
  [[nodiscard]] TransportKind Transport() const noexcept { return transport_; }
  [[nodiscard]] uint8_t RtpChannel() const noexcept { return rtp_channel_; }
  [[nodiscard]] uint8_t RtcpChannel() const noexcept { return rtcp_channel_; }
  [[nodiscard]] CloseReason Reason() const noexcept { return close_reason_; }
  [[nodiscard]] uint16_t LastStatus() const noexcept { return last_status_; }
  [[nodiscard]] uint64_t LastActivityNs() const noexcept { return last_activity_ns_; }
  [[nodiscard]] const char* SessionId() const noexcept { return session_id_; }
  [[nodiscard]] SessionSnapshot Snapshot(uint64_t now_ns) const noexcept;
  [[nodiscard]] const SessionCounters& Counters() const noexcept { return counters_; }
  [[nodiscard]] const sess::Context& HsmContext() const noexcept { return hsm_ctx_; }

 private:
  struct OutBuf {
    char* dst = nullptr;
    uint32_t cap = 0U;
    uint32_t len = 0U;
    bool ok = false;
  };

  void BeginResponse(OutBuf& out, uint16_t status, uint32_t cseq) noexcept;
  [[nodiscard]] bool FinishResponse(OutBuf& out) noexcept;
  uint16_t WriteStatus(uint16_t status, uint32_t cseq) noexcept;
  [[nodiscard]] bool OpenResponse(OutBuf& out, uint16_t status, uint32_t cseq, uint32_t reserve) noexcept;
  uint16_t CloseResponse(OutBuf& out, uint16_t status) noexcept;
  static void OutStr(OutBuf& out, const char* s) noexcept;
  static void OutCh(OutBuf& out, char c) noexcept;
  static void OutU32(OutBuf& out, uint32_t v) noexcept;
  static void OutSpan(OutBuf& out, osp::byte_span s) noexcept;

  uint16_t HandleOptions(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept;
  uint16_t HandleDescribe(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept;
  uint16_t HandleSetup(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept;
  uint16_t HandlePlay(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept;
  uint16_t HandleTeardown(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept;
  uint16_t HandleGetParameter(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept;
  uint16_t HandleUnsupported(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept;

  [[nodiscard]] bool SessionMatches(const RtspRequest& req, bool& header_present) const noexcept;
  [[nodiscard]] bool ParseTransport(const RtspRequest& req) noexcept;
  void NewSessionIdentity(uint64_t now) noexcept;
  void ResetMedia() noexcept;
  void ResetCursorState() noexcept;
  void ReleaseFrames() noexcept;
  void StartCursor(uint32_t au_len, uint32_t ts, bool inject_params) noexcept;
  [[nodiscard]] uint16_t CursorSeq() const noexcept;
  [[nodiscard]] uint32_t CursorTs() const noexcept;
  [[nodiscard]] osp::expected<RtpPacket, PacketizeError> NextPacket(const uint8_t* au, uint32_t au_len) noexcept;

  [[nodiscard]] bool TakeNextFrame(uint64_t now) noexcept;
  [[nodiscard]] bool PumpMedia(uint64_t now) noexcept;
  [[nodiscard]] bool WritePlayResponse(uint64_t now) noexcept;
  [[nodiscard]] bool Admit(uint32_t bytes, uint64_t now) noexcept;
  [[nodiscard]] uint32_t RtpTs(uint64_t pts_ns) const noexcept;
  void RequestIdr(uint64_t now_ns) noexcept;
  void DropToAwaitIdr(uint64_t now_ns) noexcept;

  const TrackParams* params_ = nullptr;
  Codec codec_ = Codec::kUnknown;
  SessionConfig cfg_{};
  sess::Context hsm_ctx_;
  sess::Hsm hsm_{hsm_ctx_};
  MediaSender media_;

  // Session identity (regenerated per SETUP).
  char session_id_[20] = {};
  uint32_t session_id_len_ = 0U;
  uint32_t ssrc_ = 0U;
  uint16_t next_seq_ = 0U;
  uint32_t id_epoch_ = 0U;

  // Transport.
  TransportKind transport_ = TransportKind::kNone;
  uint8_t rtp_channel_ = 0U;
  uint8_t rtcp_channel_ = 1U;
  uint32_t client_ipv4_ = 0U;
  uint16_t client_rtp_port_ = 0U;
  uint16_t client_rtcp_port_ = 0U;
  uint16_t server_rtp_port_ = 0U;
  uint16_t server_rtcp_port_ = 0U;
  UdpRtpTransport udp_;

  // Serial outbound stream (control responses + TCP interleaved media).
  TxStream tx_;

  bool media_enabled_ = false;
  bool awaiting_rtp_info_ = false;

  uint64_t last_activity_ns_ = 0U;
  RtcpEndpoint rtcp_;
  uint64_t idr_deadline_ns_ = 0U;
  uint32_t cseq_ = 0U;
  uint16_t last_status_ = 0U;
  bool closed_ = false;
  CloseReason close_reason_ = kCloseNone;
  bool trace_enabled_ = false;
  uint64_t trace_last_ns_ = 0U;
  uint32_t fault_eagain_ = 0U;
  uint32_t fault_idr_ = 0U;
  SessionCounters counters_;
};

static_assert(sizeof(TrackParams) <= 64U, "TrackParams must stay small enough to pass by reference cheaply");

}  // namespace rs

#endif  // RS_RTSP_SESSION_HPP_
