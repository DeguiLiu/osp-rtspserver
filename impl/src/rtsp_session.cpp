// RTSP session: method dispatch, HSM-driven state, transport setup and the
// serial media path (design §6, §8).
//
// Response/media ordering: everything written to the peer goes through one
// linear buffer (tx_). Units are appended at the tail and flushed from the
// head, so a short write can only hold back the head unit; a control response
// may land after a partially flushed interleaved frame but never inside it.
//
// SPDX-License-Identifier: MIT
#include "rs/rtsp_session.hpp"

#include "rs/rtcp.hpp"

#include <cstdio>

#include "rs/sdp.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace rs {
namespace {

constexpr uint32_t kClientRateBps = kClientEgressRateBps;  // Per-client shaper.
constexpr uint32_t kClientBurstBytes = kClientEgressBurstBytes;
constexpr uint32_t kMaxTransportHeader = 256U;
constexpr uint32_t kMaxSdpBody = 4096U;
constexpr uint64_t kSessionTimeoutNs = static_cast<uint64_t>(kSessionTimeoutMs) * 1000000ULL;
constexpr uint64_t kPlayIdrTimeoutNs = static_cast<uint64_t>(kPlayIdrTimeoutMs) * 1000000ULL;
constexpr uint64_t kSendStallNs = static_cast<uint64_t>(kSendStallTimeoutMs) * 1000000ULL;

constexpr const char* kPublicMethods = "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER";
constexpr const char* kAllowMethods = "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER";
constexpr const char* kTrackPath = "/live/trackID=0";
constexpr const char* kAggregatePath = "/live";

bool IsSpace(char c) noexcept { return c == ' ' || c == '\t'; }

const uint8_t* Bytes(osp::byte_span s) noexcept { return reinterpret_cast<const uint8_t*>(s.data()); }

char ToLower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsLit(osp::byte_span s, const char* lit) noexcept {
  uint32_t n = 0U;
  while (lit[n] != '\0') {
    ++n;
  }
  if (s.size() != n) {
    return false;
  }
  const uint8_t* p = Bytes(s);
  for (uint32_t i = 0U; i < n; ++i) {
    if (p[i] != static_cast<uint8_t>(lit[i])) {
      return false;
    }
  }
  return true;
}

bool EqualsLitCI(osp::byte_span s, const char* lit) noexcept {
  uint32_t n = 0U;
  while (lit[n] != '\0') {
    ++n;
  }
  if (s.size() != n) {
    return false;
  }
  const uint8_t* p = Bytes(s);
  for (uint32_t i = 0U; i < n; ++i) {
    if (ToLower(static_cast<char>(p[i])) != ToLower(lit[i])) {
      return false;
    }
  }
  return true;
}

bool ContainsLitCI(osp::byte_span s, const char* lit) noexcept {
  uint32_t ln = 0U;
  while (lit[ln] != '\0') {
    ++ln;
  }
  if (ln == 0U || s.size() < ln) {
    return false;
  }
  const uint8_t* p = Bytes(s);
  for (uint32_t i = 0U; i + ln <= s.size(); ++i) {
    uint32_t j = 0U;
    while (j < ln && ToLower(static_cast<char>(p[i + j])) == ToLower(lit[j])) {
      ++j;
    }
    if (j == ln) {
      return true;
    }
  }
  return false;
}

bool StartsWithLitCI(const char* p, uint32_t n, const char* lit) noexcept {
  uint32_t i = 0U;
  while (lit[i] != '\0') {
    if (i >= n || ToLower(p[i]) != ToLower(lit[i])) {
      return false;
    }
    ++i;
  }
  return true;
}

bool FindHeader(const RtspRequest& req, const char* name, osp::byte_span& out) noexcept {
  for (uint32_t i = 0U; i < req.header_count; ++i) {
    if (EqualsLitCI(req.headers[i].name, name)) {
      out = req.headers[i].value;
      return true;
    }
  }
  return false;
}

bool ParseU32(osp::byte_span s, uint32_t& out) noexcept {
  if (s.size() == 0U) {
    return false;
  }
  uint64_t v = 0U;
  const uint8_t* p = Bytes(s);
  for (uint32_t i = 0U; i < s.size(); ++i) {
    const uint8_t c = p[i];
    if (c < static_cast<uint8_t>('0') || c > static_cast<uint8_t>('9')) {
      return false;
    }
    v = v * 10U + static_cast<uint64_t>(c - static_cast<uint8_t>('0'));
    if (v > 0xFFFFFFFFULL) {
      return false;
    }
  }
  out = static_cast<uint32_t>(v);
  return true;
}

bool FindHeaderU32(const RtspRequest& req, const char* name, uint32_t& out) noexcept {
  osp::byte_span v;
  if (!FindHeader(req, name, v)) {
    return false;
  }
  return ParseU32(v, out);
}

enum class UriKind : uint8_t { kStar = 0, kAggregate, kTrack, kUnknown, kMalformed };

UriKind ClassifyUri(osp::byte_span uri) noexcept {
  if (uri.size() == 0U || uri.size() > kMaxUriBytes) {
    return UriKind::kMalformed;
  }
  const char* p = reinterpret_cast<const char*>(uri.data());
  const uint32_t n = uri.size();
  if (n == 1U && p[0] == '*') {
    return UriKind::kStar;
  }
  for (uint32_t i = 0U; i < n; ++i) {
    const uint8_t c = static_cast<uint8_t>(p[i]);
    if (c < 0x20U || c == 0x7FU) {
      return UriKind::kMalformed;
    }
    if ((c == '.') && (i + 1U < n) && p[i + 1] == '.') {
      return UriKind::kMalformed;
    }
  }
  uint32_t pos = 0U;
  if (StartsWithLitCI(p, n, "rtsp://")) {
    pos = 7U;
    while (pos < n && p[pos] != '/') {
      ++pos;
    }
  }
  if (pos >= n || p[pos] != '/') {
    return UriKind::kMalformed;
  }
  uint32_t len = n - pos;
  if (len > 1U && p[n - 1U] == '/') {
    --len;
  }
  const osp::byte_span path(reinterpret_cast<const std::byte*>(p + pos), len);
  if (EqualsLit(path, kAggregatePath)) {
    return UriKind::kAggregate;
  }
  if (EqualsLit(path, kTrackPath)) {
    return UriKind::kTrack;
  }
  return UriKind::kUnknown;
}

// Splits on ';' and trims spaces. Returns false when the input is exhausted.
bool NextToken(const char*& p, const char* end, const char*& tok, uint32_t& tok_len) noexcept {
  while (p < end && *p == ';') {
    ++p;
  }
  if (p >= end) {
    return false;
  }
  const char* start = p;
  while (p < end && *p != ';') {
    ++p;
  }
  const char* stop = p;
  while (start < stop && IsSpace(*start)) {
    ++start;
  }
  while (stop > start && IsSpace(stop[-1])) {
    --stop;
  }
  tok = start;
  tok_len = static_cast<uint32_t>(stop - start);
  return tok_len > 0U;
}

bool ParsePortPair(const char* p, uint32_t n, uint32_t& a, uint32_t& b, bool pair_required) noexcept {
  uint32_t i = 0U;
  uint64_t va = 0U;
  while (i < n && p[i] >= '0' && p[i] <= '9') {
    va = va * 10U + static_cast<uint64_t>(p[i] - '0');
    if (va > 65535U) {
      return false;
    }
    ++i;
  }
  if (i == 0U) {
    return false;
  }
  a = static_cast<uint32_t>(va);
  if (i == n) {
    b = a + 1U;
    return !pair_required;
  }
  if (p[i] != '-') {
    return false;
  }
  ++i;
  uint32_t start = i;
  uint64_t vb = 0U;
  while (i < n && p[i] >= '0' && p[i] <= '9') {
    vb = vb * 10U + static_cast<uint64_t>(p[i] - '0');
    if (vb > 65535U) {
      return false;
    }
    ++i;
  }
  if (i == start || i != n) {
    return false;
  }
  b = static_cast<uint32_t>(vb);
  return true;
}

constexpr uint16_t StatusCode(RtspStatus status) noexcept { return static_cast<uint16_t>(status); }

uint64_t SplitMix64(uint64_t x) noexcept {
  x += 0x9E3779B97F4A7C15ULL;
  uint64_t z = x;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

void Hex16(char* dst, uint64_t v) noexcept {
  static constexpr char kDigits[] = "0123456789abcdef";
  for (uint32_t i = 0U; i < 16U; ++i) {
    dst[15U - i] = kDigits[(v >> (i * 4U)) & 0x0FU];
  }
}


}  // namespace

const char* ReasonPhrase(uint16_t status) noexcept {
  switch (status) {
    case StatusCode(RtspStatus::kOk):
      return "OK";
    case StatusCode(RtspStatus::kBadRequest):
      return "Bad Request";
    case StatusCode(RtspStatus::kNotFound):
      return "Not Found";
    case StatusCode(RtspStatus::kMethodNotAllowed):
      return "Method Not Allowed";
    case StatusCode(RtspStatus::kNotAcceptable):
      return "Not Acceptable";
    case StatusCode(RtspStatus::kRequestTooLarge):
      return "Request Entity Too Large";
    case StatusCode(RtspStatus::kUriTooLong):
      return "Request-URI Too Long";
    case StatusCode(RtspStatus::kUnsupportedMedia):
      return "Unsupported Media Type";
    case StatusCode(RtspStatus::kNotEnoughBandwidth):
      return "Not Enough Bandwidth";
    case StatusCode(RtspStatus::kSessionNotFound):
      return "Session Not Found";
    case StatusCode(RtspStatus::kInvalidState):
      return "Method Not Valid in This State";
    case StatusCode(RtspStatus::kInvalidRange):
      return "Invalid Range";
    case StatusCode(RtspStatus::kAggregateNotAllowed):
      return "Aggregate Operation Not Allowed";
    case StatusCode(RtspStatus::kUnsupportedTransport):
      return "Unsupported Transport";
    case StatusCode(RtspStatus::kInternalError):
      return "Internal Server Error";
    case StatusCode(RtspStatus::kNotImplemented):
      return "Not Implemented";
    case StatusCode(RtspStatus::kServiceUnavailable):
      return "Service Unavailable";
    case StatusCode(RtspStatus::kVersionNotSupported):
      return "RTSP Version Not Supported";
    default:
      return "Error";
  }
}

const char* CloseReasonName(CloseReason reason) noexcept {
  switch (reason) {
    case kCloseNone:
      return "none";
    case kCloseTeardown:
      return "teardown";
    case kCloseSessionTimeout:
      return "session-timeout";
    case kClosePlayIdrTimeout:
      return "play-idr-timeout";
    case kCloseSendStall:
      return "send-stall";
    case kCloseControlOverflow:
      return "control-overflow";
    case kClosePeerClosed:
      return "peer-closed";
    case kCloseTransportError:
      return "transport-error";
    case kCloseProtocolError:
      return "protocol-error";
    case kCloseServerStop:
      return "server-stop";
    case kCloseAdminKick:
      return "admin-kick";
    case kCloseParamsChanged:
      return "params-changed";
    default:
      return "unknown";
  }
}

// ===========================================================================
// Construction and lifecycle
// ===========================================================================

RtspSession::RtspSession() noexcept = default;

RtspSession::~RtspSession() noexcept { udp_.Close(); }

void RtspSession::Configure(FrameIngress& ingress, const TrackParams& params, const SessionConfig& cfg) noexcept {
  udp_.Close();
  params_ = &params;
  codec_ = params.codec;
  media_.Configure(ingress, params);
  cfg_ = cfg;
  hsm_.Restart();
  transport_ = TransportKind::kNone;
  tx_.Reset();
  session_id_[0] = '\0';
  session_id_len_ = 0U;
  ssrc_ = 0U;
  next_seq_ = 0U;
  id_epoch_ = 0U;
  rtp_channel_ = 0U;
  rtcp_channel_ = 1U;
  server_rtp_port_ = 0U;
  server_rtcp_port_ = 0U;
  media_enabled_ = false;
  awaiting_rtp_info_ = false;
  rtcp_.Reset();
  last_activity_ns_ = osp::SteadyNowNs();
  idr_deadline_ns_ = 0U;
  cseq_ = 0U;
  last_status_ = 0U;
  closed_ = false;
  close_reason_ = kCloseNone;
  fault_eagain_ = 0U;
  fault_idr_ = 0U;
  counters_ = SessionCounters{};
}

void RtspSession::ResetMedia() noexcept {
  media_.Reset();
  media_enabled_ = false;
  awaiting_rtp_info_ = false;
}

void RtspSession::Close(CloseReason reason, uint64_t now_ns) noexcept {
  if (closed_) {
    return;
  }
  closed_ = true;
  close_reason_ = reason;
  ResetMedia();
  udp_.Close();
  transport_ = TransportKind::kNone;
  if (hsm_.IsInState(sess::kActive) || hsm_.IsInState(sess::kInit)) {
    static_cast<void>(hsm_.Dispatch(sess::kPeerClosed));
  }
  if (hsm_.IsInState(sess::kClosing)) {
    static_cast<void>(hsm_.Dispatch(sess::kCloseDone));
  }
  last_activity_ns_ = now_ns;
}

void RtspSession::OnPeerClosed(uint64_t now_ns) noexcept { Close(kClosePeerClosed, now_ns); }

bool RtspSession::ReadRtcp(uint64_t now_ns) noexcept {
  if (closed_ || transport_ != TransportKind::kUdp) {
    return !closed_;
  }
  uint8_t buf[kMaxRtcpBytes] = {};
  for (uint32_t i = 0U; i < 4U; ++i) {
    auto got = udp_.RecvRtcp(buf, sizeof(buf));
    if (!got.has_value()) {
      if (got.get_error() == TransportError::kWouldBlock) {
        return true;
      }
      ++counters_.rtcp_bad;
      return true;
    }
    if (got.value() == 0U) {
      return true;
    }
    last_activity_ns_ = now_ns;
    if (rtcp_.OnDatagram(osp::byte_span(reinterpret_cast<const std::byte*>(buf), got.value()),
                         counters_.rtcp_rr, counters_.rtcp_bye, counters_.rtcp_bad)) {
      Close(kCloseTeardown, now_ns);
      return false;
    }
  }
  return true;
}

void RtspSession::SetTrace(bool enable) noexcept {
  trace_enabled_ = enable;
  trace_last_ns_ = 0U;
}

void RtspSession::SetSlowBps(uint32_t bps, uint64_t now_ns) noexcept {
  media_.SetSlowBps(bps, now_ns);
}

SessionSnapshot RtspSession::Snapshot(uint64_t now_ns) const noexcept {
  SessionSnapshot out;
  out.state = hsm_.State();
  out.transport = static_cast<uint8_t>(transport_);
  out.playing = hsm_.IsInState(sess::kPlaying);
  out.active = hsm_.IsInState(sess::kActive);
  out.awaiting_rtp_info = awaiting_rtp_info_;
  out.closed = closed_;
  out.trace_enabled = trace_enabled_;
  const MediaSender::Snapshot media = media_.GetSnapshot(now_ns);
  out.queue_depth = media.queue_depth;
  out.queue_bytes = media.queue_bytes;
  out.oldest_age_ms = media.oldest_age_ms;
  out.fault_eagain = fault_eagain_;
  out.fault_idr = fault_idr_;
  out.slow_bps = media.slow_bps;
  out.seq = media.seq;
  out.rtp_ts = media.rtp_ts;
  out.ssrc = ssrc_;
  out.client_ipv4 = client_ipv4_;
  out.client_rtp_port = client_rtp_port_;
  out.client_rtcp_port = client_rtcp_port_;
  out.server_rtp_port = server_rtp_port_;
  out.server_rtcp_port = server_rtcp_port_;
  out.rtp_channel = rtp_channel_;
  out.rtcp_channel = rtcp_channel_;
  out.counters = counters_;
  out.close_reason = close_reason_;
  out.last_activity_ns = last_activity_ns_;
  const uint32_t prefix = (session_id_len_ < 8U) ? session_id_len_ : 8U;
  if (prefix != 0U) {
    std::memcpy(out.session_id_prefix, session_id_, prefix);
  }
  out.session_id_prefix[prefix] = '\0';
  return out;
}

// ===========================================================================
// Outbound buffer
// ===========================================================================

void RtspSession::OutStr(OutBuf& out, const char* s) noexcept {
  if (!out.ok) {
    return;
  }
  uint32_t n = 0U;
  while (s[n] != '\0') {
    ++n;
  }
  if ((out.cap - out.len) < n) {
    out.ok = false;
    return;
  }
  std::memcpy(out.dst + out.len, s, n);
  out.len += n;
}

void RtspSession::OutCh(OutBuf& out, char c) noexcept {
  if (!out.ok || out.len >= out.cap) {
    out.ok = false;
    return;
  }
  out.dst[out.len] = c;
  ++out.len;
}

void RtspSession::OutU32(OutBuf& out, uint32_t v) noexcept {
  char buf[10];
  uint32_t n = 0U;
  do {
    buf[n] = static_cast<char>('0' + (v % 10U));
    ++n;
    v /= 10U;
  } while (v != 0U);
  while (n > 0U) {
    --n;
    OutCh(out, buf[n]);
  }
}

void RtspSession::OutSpan(OutBuf& out, osp::byte_span s) noexcept {
  if (!out.ok) {
    return;
  }
  if ((out.cap - out.len) < s.size()) {
    out.ok = false;
    return;
  }
  std::memcpy(out.dst + out.len, s.data(), s.size());
  out.len += static_cast<uint32_t>(s.size());
}

void RtspSession::BeginResponse(OutBuf& out, uint16_t status, uint32_t cseq) noexcept {
  OutStr(out, "RTSP/1.0 ");
  OutU32(out, status);
  OutCh(out, ' ');
  OutStr(out, ReasonPhrase(status));
  OutStr(out, "\r\n");
  if (cseq != 0U) {
    OutStr(out, "CSeq: ");
    OutU32(out, cseq);
    OutStr(out, "\r\n");
  }
}

bool RtspSession::FinishResponse(OutBuf& out) noexcept {
  if (!out.ok) {
    return false;
  }
  tx_.MarkProgressIfEmpty(last_activity_ns_);
  const bool committed = tx_.Commit(out.len);
  return committed && tx_.Unflushed() <= kControlTxLimit;
}

bool RtspSession::OpenResponse(OutBuf& out, uint16_t status, uint32_t cseq, uint32_t reserve) noexcept {
  out = OutBuf{};
  if (!tx_.Reserve(reserve)) {
    return false;
  }
  const osp::writable_byte_span tail = tx_.Tail();
  out.dst = reinterpret_cast<char*>(tail.data());
  out.cap = static_cast<uint32_t>(tail.size());
  out.ok = true;
  BeginResponse(out, status, cseq);
  return out.ok;
}

uint16_t RtspSession::CloseResponse(OutBuf& out, uint16_t status) noexcept {
  if (!FinishResponse(out)) {
    Close(kCloseControlOverflow, last_activity_ns_);
    return 0U;
  }
  return status;
}

uint16_t RtspSession::WriteStatus(uint16_t status, uint32_t cseq) noexcept {
  OutBuf out;
  if (!OpenResponse(out, status, cseq, 192U)) {
    Close(kCloseControlOverflow, last_activity_ns_);
    return 0U;
  }
  OutStr(out, "\r\n");
  return CloseResponse(out, status);
}

osp::byte_span RtspSession::PendingTx() const noexcept { return tx_.Pending(); }

bool RtspSession::WantsWritable() const noexcept { return tx_.WantsWritable(); }

void RtspSession::ConsumeTx(uint32_t bytes, uint64_t now_ns) noexcept {
  tx_.Consume(bytes, now_ns);
  last_activity_ns_ = now_ns;
}

// ===========================================================================
// Request handling
// ===========================================================================

bool RtspSession::SessionMatches(const RtspRequest& req, bool& header_present) const noexcept {
  osp::byte_span v;
  header_present = FindHeader(req, "Session", v);
  if (!header_present) {
    return session_id_len_ == 0U;
  }
  uint32_t n = 0U;
  while (n < v.size() && v.data()[n] != static_cast<std::byte>(';')) {
    ++n;
  }
  if (n == 0U || n != session_id_len_) {
    return false;
  }
  return std::memcmp(v.data(), session_id_, n) == 0;
}

void RtspSession::NewSessionIdentity(uint64_t now) noexcept {
  ++id_epoch_;
  const uint64_t mixed = SplitMix64(cfg_.id_seed ^ (static_cast<uint64_t>(id_epoch_) << 32U) ^ now);
  Hex16(session_id_, mixed);
  session_id_len_ = 16U;
  ssrc_ = static_cast<uint32_t>(SplitMix64(mixed ^ 0x5DEECE66DULL)) | 1U;
  next_seq_ = static_cast<uint16_t>(SplitMix64(mixed ^ 0xA5A5A5A5ULL) & 0xFFFFU);
}

bool RtspSession::ParseTransport(const RtspRequest& req) noexcept {
  osp::byte_span hv;
  if (!FindHeader(req, "Transport", hv) || hv.size() == 0U || hv.size() > kMaxTransportHeader) {
    return false;
  }
  char buf[kMaxTransportHeader];
  std::memcpy(buf, hv.data(), hv.size());
  const char* p = buf;
  const char* end = buf + hv.size();
  const char* tok = nullptr;
  uint32_t tok_len = 0U;
  if (!NextToken(p, end, tok, tok_len)) {
    return false;
  }
  TransportKind kind = TransportKind::kNone;
  if (StartsWithLitCI(tok, tok_len, "RTP/AVP/TCP")) {
    kind = TransportKind::kTcp;
  } else if (StartsWithLitCI(tok, tok_len, "RTP/AVP")) {
    kind = TransportKind::kUdp;
  } else {
    return false;
  }

  bool have_interleaved = false;
  bool have_client_port = false;
  bool unicast = false;
  uint32_t rtp_ch = 0U;
  uint32_t rtcp_ch = 1U;
  uint32_t crtp = 0U;
  uint32_t crtcp = 0U;
  while (NextToken(p, end, tok, tok_len)) {
    if (StartsWithLitCI(tok, tok_len, "unicast")) {
      unicast = true;
    } else if (StartsWithLitCI(tok, tok_len, "multicast") || StartsWithLitCI(tok, tok_len, "ttl=") ||
               StartsWithLitCI(tok, tok_len, "destination=")) {
      return false;
    } else if (StartsWithLitCI(tok, tok_len, "client_port=")) {
      const uint32_t off = static_cast<uint32_t>(sizeof("client_port=") - 1U);
      if (!ParsePortPair(tok + off, tok_len - off, crtp, crtcp, true)) {
        return false;
      }
      have_client_port = true;
    } else if (StartsWithLitCI(tok, tok_len, "interleaved=")) {
      const uint32_t off = static_cast<uint32_t>(sizeof("interleaved=") - 1U);
      if (!ParsePortPair(tok + off, tok_len - off, rtp_ch, rtcp_ch, true)) {
        return false;
      }
      if (rtp_ch > kMaxInterleavedChannel || rtcp_ch > kMaxInterleavedChannel || rtcp_ch != rtp_ch + 1U) {
        return false;
      }
      have_interleaved = true;
    } else if (StartsWithLitCI(tok, tok_len, "mode=")) {
      if (!StartsWithLitCI(tok, tok_len, "mode=play")) {
        return false;
      }
    }
  }
  if (!unicast) {
    return false;
  }
  if (kind == TransportKind::kTcp) {
    if (!have_interleaved) {
      return false;
    }
    rtp_channel_ = static_cast<uint8_t>(rtp_ch);
    rtcp_channel_ = static_cast<uint8_t>(rtcp_ch);
    transport_ = TransportKind::kTcp;
    return true;
  }
  // RFC 3550 pair: even RTP port, next odd port for RTCP.
  if (!have_client_port || crtp < 1024U || (crtp % 2U) != 0U || crtcp != crtp + 1U) {
    return false;
  }
  client_rtp_port_ = static_cast<uint16_t>(crtp);
  client_rtcp_port_ = static_cast<uint16_t>(crtcp);
  transport_ = TransportKind::kUdp;
  return true;
}

uint16_t RtspSession::HandleRequest(const RtspRequest& req, uint64_t now_ns) noexcept {
  if (closed_) {
    return 0U;
  }
  last_activity_ns_ = now_ns;
  uint32_t cseq = 0U;
  const bool have_cseq = FindHeaderU32(req, "CSeq", cseq);
  if (!EqualsLit(req.version, "RTSP/1.0")) {
    last_status_ = WriteStatus(StatusCode(RtspStatus::kVersionNotSupported), cseq);
    return last_status_;
  }
  if (!have_cseq) {
    last_status_ = WriteStatus(StatusCode(RtspStatus::kBadRequest), 0U);
    return last_status_;
  }
  cseq_ = cseq;
  uint16_t status = 0U;
  switch (req.method) {
    case Method::kOptions:
      status = HandleOptions(req, cseq, now_ns);
      break;
    case Method::kDescribe:
      status = HandleDescribe(req, cseq, now_ns);
      break;
    case Method::kSetup:
      status = HandleSetup(req, cseq, now_ns);
      break;
    case Method::kPlay:
      status = HandlePlay(req, cseq, now_ns);
      break;
    case Method::kTeardown:
      status = HandleTeardown(req, cseq, now_ns);
      break;
    case Method::kGetParameter:
      status = HandleGetParameter(req, cseq, now_ns);
      break;
    default:
      status = HandleUnsupported(req, cseq, now_ns);
      break;
  }
  last_status_ = status;
  return status;
}

uint16_t RtspSession::HandleOptions(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept {
  static_cast<void>(req);
  static_cast<void>(now);
  OutBuf out;
  if (!OpenResponse(out, StatusCode(RtspStatus::kOk), cseq, 192U)) {
    Close(kCloseControlOverflow, last_activity_ns_);
    return 0U;
  }
  OutStr(out, "Public: ");
  OutStr(out, kPublicMethods);
  OutStr(out, "\r\n\r\n");
  return CloseResponse(out, StatusCode(RtspStatus::kOk));
}

uint16_t RtspSession::HandleDescribe(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept {
  static_cast<void>(now);
  const UriKind kind = ClassifyUri(req.uri);
  if (kind == UriKind::kMalformed) {
    return WriteStatus(StatusCode(RtspStatus::kBadRequest), cseq);
  }
  if (kind == UriKind::kUnknown || kind == UriKind::kStar) {
    return WriteStatus(StatusCode(RtspStatus::kNotFound), cseq);
  }
  osp::byte_span accept;
  if (FindHeader(req, "Accept", accept) && !ContainsLitCI(accept, "application/sdp")) {
    return WriteStatus(StatusCode(RtspStatus::kNotAcceptable), cseq);
  }
  if (params_ == nullptr || codec_ == Codec::kUnknown) {
    return WriteStatus(StatusCode(RtspStatus::kInternalError), cseq);
  }
  char body[kMaxSdpBody];
  SdpParams sp{};
  sp.codec = codec_;
  sp.server_ip = cfg_.advertise_ip;
  sp.session_name = "live";
  sp.port = 0U;
  sp.ssrc = ssrc_;
  sp.vps = params_->vps;
  sp.vps_len = params_->vps_len;
  sp.sps = params_->sps;
  sp.sps_len = params_->sps_len;
  sp.pps = params_->pps;
  sp.pps_len = params_->pps_len;
  sp.clock_rate = kClockRate;
  sp.payload_type = kRtpPayloadType;
  auto sdp = BuildSdp(sp, osp::writable_byte_span(reinterpret_cast<std::byte*>(body), sizeof(body)));
  if (!sdp.has_value() || sdp.value() == 0U || sdp.value() > sizeof(body)) {
    return WriteStatus(StatusCode(RtspStatus::kInternalError), cseq);
  }

  OutBuf out;
  if (!OpenResponse(out, StatusCode(RtspStatus::kOk), cseq, sdp.value() + 320U)) {
    Close(kCloseControlOverflow, last_activity_ns_);
    return 0U;
  }
  OutStr(out, "Content-Base: rtsp://");
  OutStr(out, cfg_.advertise_ip);
  OutCh(out, ':');
  OutU32(out, cfg_.rtsp_port);
  OutStr(out, "/live/\r\n");
  OutStr(out, "Content-Type: application/sdp\r\n");
  OutStr(out, "Content-Length: ");
  OutU32(out, sdp.value());
  OutStr(out, "\r\n\r\n");
  OutSpan(out, osp::byte_span(reinterpret_cast<const std::byte*>(body), sdp.value()));
  return CloseResponse(out, StatusCode(RtspStatus::kOk));
}

uint16_t RtspSession::HandleSetup(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept {
  const UriKind kind = ClassifyUri(req.uri);
  if (kind == UriKind::kMalformed) {
    return WriteStatus(StatusCode(RtspStatus::kBadRequest), cseq);
  }
  if (kind == UriKind::kAggregate) {
    return WriteStatus(StatusCode(RtspStatus::kAggregateNotAllowed), cseq);
  }
  if (kind != UriKind::kTrack) {
    return WriteStatus(StatusCode(RtspStatus::kNotFound), cseq);
  }
  if (hsm_.IsInState(sess::kPlaying) || hsm_.IsInState(sess::kClosing)) {
    return WriteStatus(StatusCode(RtspStatus::kInvalidState), cseq);
  }
  bool session_header = false;
  if (!SessionMatches(req, session_header)) {
    return WriteStatus(StatusCode(RtspStatus::kSessionNotFound), cseq);
  }
  if (!ParseTransport(req)) {
    return WriteStatus(StatusCode(RtspStatus::kUnsupportedTransport), cseq);
  }

  if (transport_ == TransportKind::kUdp) {
    if (client_ipv4_ == 0U) {
      return WriteStatus(StatusCode(RtspStatus::kServiceUnavailable), cseq);
    }
    // Transactional repeated SETUP: open the replacement pair first, then
    // swap. A failure keeps the previous transport intact (review P1-3).
    UdpRtpTransport fresh;
    uint16_t new_rtp_port = 0U;
    uint16_t new_rtcp_port = 0U;
    if (!fresh.Open(client_ipv4_, client_rtp_port_, client_rtcp_port_, new_rtp_port, new_rtcp_port)) {
      hsm_ctx_.setup_ok = false;
      static_cast<void>(hsm_.Dispatch(sess::kSetupFail));
      transport_ = TransportKind::kNone;
      return WriteStatus(StatusCode(RtspStatus::kServiceUnavailable), cseq);
    }
    udp_ = std::move(fresh);
    server_rtp_port_ = new_rtp_port;
    server_rtcp_port_ = new_rtcp_port;
  } else {
    udp_.Close();
    server_rtp_port_ = 0U;
    server_rtcp_port_ = 0U;
  }

  if (!HasSession()) {
    NewSessionIdentity(now);
  }
  ResetMedia();
  hsm_ctx_.setup_ok = true;
  if (!hsm_.Dispatch(sess::kSetupOk)) {
    udp_.Close();
    transport_ = TransportKind::kNone;
    return WriteStatus(StatusCode(RtspStatus::kInternalError), cseq);
  }

  OutBuf out;
  if (!OpenResponse(out, StatusCode(RtspStatus::kOk), cseq, 320U)) {
    Close(kCloseControlOverflow, now);
    return 0U;
  }
  OutStr(out, "Session: ");
  OutStr(out, session_id_);
  OutStr(out, ";timeout=");
  OutU32(out, kSessionTimeoutMs / kMsPerSec);
  OutStr(out, "\r\nTransport: ");
  if (transport_ == TransportKind::kUdp) {
    OutStr(out, "RTP/AVP;unicast;client_port=");
    OutU32(out, client_rtp_port_);
    OutCh(out, '-');
    OutU32(out, client_rtcp_port_);
    OutStr(out, ";server_port=");
    OutU32(out, server_rtp_port_);
    OutCh(out, '-');
    OutU32(out, server_rtcp_port_);
  } else {
    OutStr(out, "RTP/AVP/TCP;unicast;interleaved=");
    OutU32(out, rtp_channel_);
    OutCh(out, '-');
    OutU32(out, rtcp_channel_);
  }
  OutStr(out, ";ssrc=");
  OutU32(out, ssrc_);
  OutStr(out, "\r\n\r\n");
  return CloseResponse(out, StatusCode(RtspStatus::kOk));
}

uint16_t RtspSession::HandlePlay(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept {
  const UriKind kind = ClassifyUri(req.uri);
  if (kind == UriKind::kMalformed) {
    return WriteStatus(StatusCode(RtspStatus::kBadRequest), cseq);
  }
  if (kind == UriKind::kUnknown) {
    return WriteStatus(StatusCode(RtspStatus::kNotFound), cseq);
  }
  bool session_header = false;
  if (!SessionMatches(req, session_header) || !session_header) {
    return WriteStatus(StatusCode(RtspStatus::kSessionNotFound), cseq);
  }
  if (!hsm_.IsInState(sess::kReady)) {
    return WriteStatus(StatusCode(RtspStatus::kInvalidState), cseq);
  }
  osp::byte_span range;
  if (FindHeader(req, "Range", range) && !ContainsLitCI(range, "npt=0") ) {
    return WriteStatus(StatusCode(RtspStatus::kInvalidRange), cseq);
  }
  if (transport_ == TransportKind::kNone) {
    return WriteStatus(StatusCode(RtspStatus::kInvalidState), cseq);
  }
  hsm_ctx_.setup_ok = true;
  if (!hsm_.Dispatch(sess::kPlayOk)) {
    return WriteStatus(StatusCode(RtspStatus::kInvalidState), cseq);
  }

  media_enabled_ = true;
  awaiting_rtp_info_ = true;
  media_.RequireIdr();
  RequestIdr(now);
  idr_deadline_ns_ = now + kPlayIdrTimeoutNs;
  media_.SetSlowBps(0U, now);
  // A frame may already be queued: try to write RTP-Info and start media now.
  Pump(now);
  if (closed_) {
    return 0U;
  }
  return StatusCode(RtspStatus::kOk);
}

uint16_t RtspSession::HandleTeardown(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept {
  const UriKind kind = ClassifyUri(req.uri);
  if (kind == UriKind::kMalformed) {
    return WriteStatus(StatusCode(RtspStatus::kBadRequest), cseq);
  }
  bool session_header = false;
  if (!SessionMatches(req, session_header) || !session_header) {
    return WriteStatus(StatusCode(RtspStatus::kSessionNotFound), cseq);
  }
  if (!hsm_.IsInState(sess::kActive)) {
    return WriteStatus(StatusCode(RtspStatus::kInvalidState), cseq);
  }
  OutBuf out;
  if (!OpenResponse(out, StatusCode(RtspStatus::kOk), cseq, 192U)) {
    Close(kCloseControlOverflow, now);
    return 0U;
  }
  OutStr(out, "Session: ");
  OutStr(out, session_id_);
  OutStr(out, "\r\n\r\n");
  const uint16_t status = CloseResponse(out, StatusCode(RtspStatus::kOk));
  if (status == 0U) {
    return 0U;
  }

  ResetMedia();
  udp_.Close();
  transport_ = TransportKind::kNone;
  static_cast<void>(hsm_.Dispatch(sess::kTeardown));
  static_cast<void>(hsm_.Dispatch(sess::kCloseDone));
  session_id_[0] = '\0';
  session_id_len_ = 0U;
  return StatusCode(RtspStatus::kOk);
}

uint16_t RtspSession::HandleGetParameter(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept {
  static_cast<void>(now);
  bool session_header = false;
  if (!SessionMatches(req, session_header) || !session_header) {
    return WriteStatus(StatusCode(RtspStatus::kSessionNotFound), cseq);
  }
  if (!hsm_.IsInState(sess::kActive)) {
    return WriteStatus(StatusCode(RtspStatus::kInvalidState), cseq);
  }
  OutBuf out;
  if (!OpenResponse(out, StatusCode(RtspStatus::kOk), cseq, 128U)) {
    Close(kCloseControlOverflow, last_activity_ns_);
    return 0U;
  }
  OutStr(out, "Content-Length: 0\r\n\r\n");
  return CloseResponse(out, StatusCode(RtspStatus::kOk));
}

uint16_t RtspSession::HandleUnsupported(const RtspRequest& req, uint32_t cseq, uint64_t now) noexcept {
  static_cast<void>(req);
  static_cast<void>(now);
  OutBuf out;
  if (!OpenResponse(out, StatusCode(RtspStatus::kMethodNotAllowed), cseq, 256U)) {
    Close(kCloseControlOverflow, last_activity_ns_);
    return 0U;
  }
  OutStr(out, "Allow: ");
  OutStr(out, kAllowMethods);
  OutStr(out, "\r\n\r\n");
  return CloseResponse(out, StatusCode(RtspStatus::kMethodNotAllowed));
}

bool RtspSession::OnInterleaved(uint8_t channel, osp::byte_span payload, uint64_t now_ns) noexcept {
  if (closed_) {
    return false;
  }
  if (transport_ != TransportKind::kTcp || channel != rtcp_channel_) {
    ++counters_.interleaved_errors;
    return false;
  }
  last_activity_ns_ = now_ns;
  if (rtcp_.OnDatagram(payload, counters_.rtcp_rr, counters_.rtcp_bye, counters_.rtcp_bad)) {
    // The peer ended the session; stop media instead of waiting for timeout.
    Close(kCloseTeardown, last_activity_ns_);
  }
  return true;
}

// ===========================================================================
// Media path
// ===========================================================================

void RtspSession::OnInputGap(uint64_t now_ns) noexcept {
  ++counters_.input_gaps;
  media_.OnInputGap();
  RequestIdr(now_ns);
}

void RtspSession::OnFrame(const FrameDesc& desc, uint64_t now_ns) noexcept {
  if (closed_ || !media_enabled_ || !hsm_.IsInState(sess::kPlaying)) {
    return;
  }
  if ((desc.flags & kFrameIdr) == 0U && media_.NeedIdr()) {
    return;  // No media before the first IDR of this PLAY.
  }
#if defined(RS_ENABLE_FAULT_INJECTION)
  if ((desc.flags & kFrameIdr) != 0U && fault_idr_ > 0U) {
    --fault_idr_;
    return;  // Simulate a missing IDR; PLAY eventually times out.
  }
#endif
  const MediaSender::EnqueueResult result = media_.OnFrame(desc, now_ns);
  if (result == MediaSender::EnqueueResult::kQueueFull) {
    ++counters_.queue_drops;
    RequestIdr(now_ns);
  }
  if (result == MediaSender::EnqueueResult::kEnqueued) {
    Pump(now_ns);
  }
}

void RtspSession::RequestIdr(uint64_t now_ns) noexcept {
  if (cfg_.idr == nullptr) {
    return;
  }
  if (cfg_.idr->Request(now_ns)) {
    ++counters_.idr_requests;
  }
}

void RtspSession::DropToAwaitIdr(uint64_t now_ns) noexcept {
  media_.DropToAwaitIdr();
  RequestIdr(now_ns);
}

void RtspSession::Pump(uint64_t now_ns) noexcept {
  if (closed_ || !media_enabled_) {
    return;
  }
  if (transport_ == TransportKind::kUdp || transport_ == TransportKind::kTcp) {
    static_cast<void>(PumpMedia(now_ns));
  }
}

bool RtspSession::WritePlayResponse(uint64_t now) noexcept {
  OutBuf out;
  if (!OpenResponse(out, StatusCode(RtspStatus::kOk), cseq_, 288U)) {
    return false;  // Retry after the server drains the buffer.
  }
  OutStr(out, "Session: ");
  OutStr(out, session_id_);
  OutStr(out, "\r\nRange: npt=0-\r\nRTP-Info: url=rtsp://");
  OutStr(out, cfg_.advertise_ip);
  OutCh(out, ':');
  OutU32(out, cfg_.rtsp_port);
  OutStr(out, "/live/trackID=0;seq=");
  OutU32(out, CursorSeq());
  OutStr(out, ";rtptime=");
  OutU32(out, CursorTs());
  OutStr(out, "\r\n\r\n");
  if (!out.ok || !FinishResponse(out)) {
    Close(kCloseControlOverflow, now);
    return false;
  }
  awaiting_rtp_info_ = false;
  return true;
}

// Single media pump for both transports: packet production is shared, only
// the egress call differs. Keeping one loop guarantees the H.264 and H.265
// paths, and TCP and UDP, observe identical ordering and drop semantics.
uint16_t RtspSession::CursorSeq() const noexcept { return media_.CursorSeq(); }

uint32_t RtspSession::CursorTs() const noexcept { return media_.CursorTs(); }

bool RtspSession::PumpMedia(uint64_t now) noexcept {
  if (params_ == nullptr) {
    return false;
  }
  const bool tcp = transport_ == TransportKind::kTcp;
  uint32_t sent_bytes = 0U;
  for (uint32_t packets = 0U; packets < kMaxPacketsPerPump && sent_bytes < kPumpBudgetBytes; ++packets) {
    if (!media_.HasCurrent() && !media_.TakeNext(now, next_seq_, ssrc_)) {
      return true;
    }
    if (awaiting_rtp_info_ && !WritePlayResponse(now)) {
      return false;  // Buffer full: retry after a flush.
    }
    if (media_.Data() == nullptr) {
      media_.ReleaseCurrent();
      DropToAwaitIdr(now);
      return false;
    }
    auto pkt = media_.NextPacket(ssrc_);
    if (!pkt.has_value()) {
      const PacketizeError err = pkt.get_error();
      if (err == PacketizeError::kEndOfAu) {
        media_.ReleaseCurrent();
        ++counters_.frames_completed;
        continue;
      }
      DropToAwaitIdr(now);
      return false;
    }
    if (tcp && !tx_.Reserve(kMaxInterleavedBytes)) {
      return false;  // Wait for the server to drain the buffer.
    }
    const RtpPacket view = pkt.value();
    const uint32_t wire_bytes = view.header_len + view.prefix_len + view.payload_len;
    if (!media_.Admit(wire_bytes, now)) {
      // Shaping pauses the cursor and never discards the access unit: dropping
      // here forced AwaitIDR whenever an IDR exceeded the 32 KiB burst, which
      // throttled UDP to roughly one AU per second at 4 Mbit/s.
      ++counters_.token_waits;
      return false;
    }
    if (tcp) {
      auto written = WriteInterleaved(tx_.Tail(), rtp_channel_, view);
      if (!written.has_value()) {
        ++counters_.interleaved_errors;
        DropToAwaitIdr(now);
        return false;
      }
      tx_.MarkProgressIfEmpty(now);
      if (!tx_.Commit(written.value())) {
        ++counters_.interleaved_errors;
        DropToAwaitIdr(now);
        return false;
      }
    } else {
#if defined(RS_ENABLE_FAULT_INJECTION)
      if (fault_eagain_ > 0U) {
        --fault_eagain_;
        ++counters_.udp_drops;
        DropToAwaitIdr(now);
        return false;
      }
#endif
      auto sent = udp_.SendRtp(view);
      if (!sent.has_value() || sent.value() == SendStatus::kWouldBlock) {
        ++counters_.udp_drops;
        DropToAwaitIdr(now);
        return false;
      }
    }
    next_seq_ = media_.CursorSeq();
    ++counters_.packets_sent;
    rtcp_.OnRtpSent(view.payload_len, now);
    sent_bytes += wire_bytes;
    if (trace_enabled_ && cfg_.log_level != nullptr &&
        cfg_.log_level->load(std::memory_order_relaxed) >= static_cast<uint8_t>(LogLevel::kDebug) &&
        (trace_last_ns_ == 0U || (now - trace_last_ns_) >= 100000000ULL)) {
      std::fprintf(stderr, "[trace] sess=%.8s seq=%u ts=%u len=%u marker=%d transport=%s\n", session_id_,
                   static_cast<unsigned>(CursorSeq()), static_cast<unsigned>(CursorTs()),
                   static_cast<unsigned>(view.header_len + view.prefix_len + view.payload_len),
                   view.marker ? 1 : 0,
                   (transport_ == TransportKind::kTcp) ? "tcp" : "udp");
      trace_last_ns_ = now;
    }
    last_activity_ns_ = now;  // Successful media output counts as activity.
  }
  return true;
}

bool RtspSession::OnTick(uint64_t now_ns) noexcept {
  if (closed_) {
    return false;
  }
  if ((hsm_.IsInState(sess::kReady) || hsm_.IsInState(sess::kPlaying)) &&
      (now_ns - last_activity_ns_) > kSessionTimeoutNs) {
    Close(kCloseSessionTimeout, now_ns);
    return false;
  }
  // Retry the IDR request once per rate-limit window while PLAY is still
  // waiting; the encoder may have missed the first advisory request.
  if (hsm_.IsInState(sess::kPlaying) && media_.NeedIdr()) {
    RequestIdr(now_ns);
  }
  if (hsm_.IsInState(sess::kPlaying)) {
    rtcp_.MaybeSend(now_ns, ssrc_, cfg_.advertise_ip, CursorTs(), counters_.packets_sent, transport_,
                    rtcp_channel_, tx_, udp_, counters_.rtcp_sr, counters_.rtcp_bad);
  }
  if (awaiting_rtp_info_ && now_ns > idr_deadline_ns_) {
    // Design §9.3: a PLAY waiting for its first IDR must not hang; close.
    Close(kClosePlayIdrTimeout, now_ns);
    return false;
  }
  if (tx_.Unflushed() > 0U && (now_ns - tx_.ProgressNs()) > kSendStallNs) {
    Close(kCloseSendStall, now_ns);
    return false;
  }
  Pump(now_ns);
  return !closed_;
}

}  // namespace rs
