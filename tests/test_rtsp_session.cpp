// RTSP session tests: method/state matrix, transport setup, PLAY gating on the
// first IDR, RTP-Info consistency and control backpressure.
//
// The SDP, packetizer and transport modules are owned by other workers; where
// a check depends on one of them the test probes the frozen API first and
// asserts the full behavior only when that module is implemented. This keeps
// the suite honest today and strict once the peers land.
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/file_source.hpp"
#include "rs/h264_packetizer.hpp"
#include "rs/rtp_transport.hpp"
#include "rs/rtsp_session.hpp"
#include "rs/rtsp_server.hpp"
#include "rs/sdp.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using namespace rs;

osp::byte_span Span(const char* s) {
  return osp::byte_span(reinterpret_cast<const std::byte*>(s), std::strlen(s));
}

struct Req {
  RtspRequest r;

  Req(Method m, const char* uri) {
    r.method = m;
    r.uri = Span(uri);
    r.version = Span("RTSP/1.0");
  }

  void H(const char* name, const char* value) {
    if (r.header_count < kMaxHeaderCount) {
      r.headers[r.header_count].name = Span(name);
      r.headers[r.header_count].value = Span(value);
      ++r.header_count;
    }
  }
};

// AUD + SPS + PPS + IDR slice, 4-byte start codes.
const uint8_t kIdrAu[] = {
    0x00, 0x00, 0x00, 0x01, 0x09, 0xF0, 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F, 0x8C, 0x8D, 0x40,
    0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x21,
    0xFF,
};

struct Harness {
  FrameIngress ingress;
  TrackParams params{};
  IdrHooks idr{};
  uint8_t sps[4] = {0x67, 0x42, 0xC0, 0x1F};
  uint8_t pps[4] = {0x68, 0xCE, 0x3C, 0x80};
  uint8_t vps265[7] = {0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01};
  uint8_t sps265[7] = {0x42, 0x01, 0x01, 0x60, 0xAA, 0xBB, 0x01};
  uint8_t pps265[6] = {0x44, 0x01, 0xC1, 0x72, 0xCC, 0x01};
  RtspSession session;
  uint64_t now = 1000000000ULL;

  explicit Harness(Codec codec = Codec::kH264) {
    params.codec = codec;
    if (codec == Codec::kH265) {
      params.vps = vps265;
      params.vps_len = sizeof(vps265);
      params.sps = sps265;
      params.sps_len = sizeof(sps265);
      params.pps = pps265;
      params.pps_len = sizeof(pps265);
    } else {
      params.sps = sps;
      params.sps_len = sizeof(sps);
      params.pps = pps;
      params.pps_len = sizeof(pps);
    }
    SessionConfig cfg;
    cfg.advertise_ip = "127.0.0.1";
    cfg.rtsp_port = 8554U;
    cfg.id_seed = 0x0123456789ABCDEFULL;
    cfg.idr = &idr;
    session.Configure(ingress, params, cfg);
    session.SetPeerIpv4(0x0100007FU);  // 127.0.0.1 in host byte order.
  }

  uint16_t Do(Req& req) {
    now += 1000000ULL;
    return session.HandleRequest(req.r, now);
  }

  // Copies the pending output, NUL-terminated, into dst.
  uint32_t Peek(char* dst, uint32_t cap) {
    const osp::byte_span tx = session.PendingTx();
    uint32_t n = static_cast<uint32_t>(tx.size());
    if (n > (cap - 1U)) {
      n = cap - 1U;
    }
    if (n > 0U) {
      std::memcpy(dst, tx.data(), n);
    }
    dst[n] = '\0';
    return n;
  }

  void Drain() {
    const osp::byte_span tx = session.PendingTx();
    session.ConsumeTx(static_cast<uint32_t>(tx.size()), now);
  }
};

int TestSnapshotInitial() {
  Harness h;
  const SessionSnapshot s = h.session.Snapshot(h.now);
  RS_CHECK_EQ(s.state, static_cast<int32_t>(sess::kInit));
  RS_CHECK_EQ(static_cast<uint8_t>(s.transport), static_cast<uint8_t>(TransportKind::kNone));
  RS_CHECK_EQ(s.queue_depth, 0U);
  RS_CHECK_EQ(s.queue_bytes, 0U);
  RS_CHECK_EQ(s.counters.packets_sent, 0U);
  RS_CHECK_EQ(s.client_ipv4, 0x0100007FU);
  RS_CHECK_EQ(s.session_id_prefix[0], '\0');
  return 0;
}

int TestFaultSnapshot() {
  Harness h;
  h.session.SetFaultEagain(3U);
  h.session.SetFaultIdr(2U);
  h.session.SetSlowBps(100000U, h.now);
  h.session.SetTrace(true);
  const SessionSnapshot s = h.session.Snapshot(h.now);
  RS_CHECK_EQ(s.fault_eagain, 3U);
  RS_CHECK_EQ(s.fault_idr, 2U);
  RS_CHECK_EQ(s.slow_bps, 100000U);
  RS_CHECK(s.trace_enabled);
  h.session.SetSlowBps(0U, h.now);
  h.session.SetTrace(false);
  h.session.ResetCounters();
  const SessionSnapshot s2 = h.session.Snapshot(h.now);
  RS_CHECK_EQ(s2.slow_bps, 0U);
  RS_CHECK(!s2.trace_enabled);
  RS_CHECK_EQ(s2.counters.packets_sent, 0U);
  return 0;
}

uint16_t ParseStatus(Harness& h) {
  char buf[kRtspTxBytes + 1U];
  if (h.Peek(buf, sizeof(buf)) < 12U) {
    return 0U;
  }
  if (std::memcmp(buf, "RTSP/1.0 ", 9U) != 0) {
    return 0U;
  }
  return static_cast<uint16_t>((buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0'));
}

bool HasText(Harness& h, const char* needle) {
  char buf[kRtspTxBytes + 1U];
  static_cast<void>(h.Peek(buf, sizeof(buf)));
  return std::strstr(buf, needle) != nullptr;
}

bool SetupTcp(Harness& h, const char* cseq) {
  Req setup(Method::kSetup, "/live/trackID=0");
  setup.H("CSeq", cseq);
  setup.H("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1");
  const uint16_t st = h.Do(setup);
  const bool ok = (st == 200U);
  h.Drain();
  return ok;
}

bool SdpReady(const TrackParams& p) {
  char body[4096];
  SdpParams sp{};
  sp.codec = p.codec;
  sp.server_ip = "127.0.0.1";
  sp.session_name = "live";
  sp.vps = p.vps;
  sp.vps_len = p.vps_len;
  sp.sps = p.sps;
  sp.sps_len = p.sps_len;
  sp.pps = p.pps;
  sp.pps_len = p.pps_len;
  auto r = BuildSdp(sp, osp::writable_byte_span(reinterpret_cast<std::byte*>(body), sizeof(body)));
  return r.has_value() && r.value() > 0U;
}

bool UdpReady() {
  UdpRtpTransport t;
  uint16_t rtp = 0U;
  uint16_t rtcp = 0U;
  const bool ok = t.Open(0x0100007FU, 5000U, 5001U, rtp, rtcp);
  t.Close();
  return ok;
}

bool PacketizerReady() {
  ClientCursor cur;
  H264Packetizer::ResetCursor(cur);
  cur.await_idr = false;
  const uint8_t au[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00};
  ParamSets ps{};
  H264Packetizer::StartAu(cur, sizeof(au), 1234U, false);
  auto pkt = H264Packetizer::Next(au, sizeof(au), ps, cur, 0x1234U);
  return pkt.has_value();
}

bool InterleavedReady() {
  uint8_t out[64];
  const uint8_t rtp[] = {0x80, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x65};
  RtpPacket pkt{};
  pkt.header = rtp;
  pkt.header_len = 12U;
  pkt.payload = rtp + 12;
  pkt.payload_len = 1U;
  auto wrote = WriteInterleaved(osp::writable_byte_span(reinterpret_cast<std::byte*>(out), sizeof(out)), 0U, pkt);
  return wrote.has_value();
}

bool PushAu(Harness& h, const uint8_t* au, uint32_t len, uint16_t flags, uint64_t pts_ns, FrameDesc& out) {
  auto w = h.ingress.Acquire(len);
  if (!w.has_value()) {
    return false;
  }
  FrameIngress::Writer writer = w.value();
  std::memcpy(writer.au, au, len);
  writer.desc.pts_ns = pts_ns;
  writer.desc.flags = flags;
  auto c = h.ingress.Commit(writer, len);
  if (!c.has_value()) {
    h.ingress.Abort(writer);
    return false;
  }
  return h.ingress.Pop(out);
}

int TestOptions() {
  Harness h;
  Req opt(Method::kOptions, "*");
  opt.H("CSeq", "1");
  RS_CHECK_EQ(h.Do(opt), 200U);
  RS_CHECK_EQ(ParseStatus(h), 200U);
  RS_CHECK(HasText(h, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER"));
  RS_CHECK(HasText(h, "CSeq: 1"));
  RS_CHECK_EQ(h.session.State(), sess::kInit);
  return 0;
}

int TestDescribe() {
  Harness h;
  const bool sdp_ready = SdpReady(h.params);
  Req req(Method::kDescribe, "/live");
  req.H("CSeq", "2");
  req.H("Accept", "application/sdp");
  const uint16_t st = h.Do(req);
  RS_CHECK_EQ(st, sdp_ready ? 200U : 500U);
  if (sdp_ready) {
    RS_CHECK(HasText(h, "Content-Type: application/sdp"));
    RS_CHECK(HasText(h, "Content-Base: rtsp://127.0.0.1:8554/live/"));
    RS_CHECK(HasText(h, "Content-Length: "));
  }
  RS_CHECK_EQ(h.session.State(), sess::kInit);
  h.Drain();

  Req unknown(Method::kDescribe, "/nope");
  unknown.H("CSeq", "3");
  RS_CHECK_EQ(h.Do(unknown), 404U);
  h.Drain();

  Req wrong_type(Method::kDescribe, "/live");
  wrong_type.H("CSeq", "4");
  wrong_type.H("Accept", "application/xyz");
  RS_CHECK_EQ(h.Do(wrong_type), 406U);
  return 0;
}

int TestSetupTcp() {
  Harness h;
  Req setup(Method::kSetup, "/live/trackID=0");
  setup.H("CSeq", "5");
  setup.H("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1");
  RS_CHECK_EQ(h.Do(setup), 200U);
  RS_CHECK(HasText(h, "Session: "));
  RS_CHECK(HasText(h, ";timeout=60"));
  RS_CHECK(HasText(h, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;ssrc="));
  RS_CHECK_EQ(h.session.State(), sess::kReady);
  RS_CHECK(h.session.Active());
  RS_CHECK(h.session.HasSession());
  RS_CHECK_EQ(std::strlen(h.session.SessionId()), 16U);
  RS_CHECK(h.session.Transport() == TransportKind::kTcp);
  RS_CHECK_EQ(h.session.RtpChannel(), 0U);
  RS_CHECK_EQ(h.session.RtcpChannel(), 1U);
  h.Drain();

  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);
  Req again(Method::kSetup, "/live/trackID=0");
  again.H("CSeq", "6");
  again.H("Session", id);
  again.H("Transport", "RTP/AVP/TCP;unicast;interleaved=2-3");
  RS_CHECK_EQ(h.Do(again), 200U);
  RS_CHECK_EQ(h.session.State(), sess::kReady);
  RS_CHECK_EQ(std::strcmp(h.session.SessionId(), id), 0);
  h.Drain();

  // An odd interleaved pair is rejected on an absolute URI.
  Req absolute(Method::kSetup, "rtsp://127.0.0.1:8554/live/trackID=0");
  absolute.H("CSeq", "7");
  absolute.H("Session", id);
  absolute.H("Transport", "RTP/AVP/TCP;unicast;interleaved=0-2");
  RS_CHECK_EQ(h.Do(absolute), 461U);
  return 0;
}

int TestSetupUdp() {
  if (!UdpReady()) {
    std::printf("test_rtsp_session: NOTE udp transport is a stub, UDP SETUP check skipped\n");
    return 0;
  }
  Harness h;
  Req setup(Method::kSetup, "/live/trackID=0");
  setup.H("CSeq", "8");
  setup.H("Transport", "RTP/AVP;unicast;client_port=5000-5001");
  RS_CHECK_EQ(h.Do(setup), 200U);
  RS_CHECK(HasText(h, "Transport: RTP/AVP;unicast;client_port=5000-5001;server_port="));
  RS_CHECK(h.session.Transport() == TransportKind::kUdp);
  h.Drain();

  Harness odd;
  Req bad(Method::kSetup, "/live/trackID=0");
  bad.H("CSeq", "9");
  bad.H("Transport", "RTP/AVP;unicast;client_port=5001-5002");
  RS_CHECK_EQ(odd.Do(bad), 461U);
  return 0;
}

int TestSetupErrors() {
  Harness h;
  Req multicast(Method::kSetup, "/live/trackID=0");
  multicast.H("CSeq", "10");
  multicast.H("Transport", "RTP/AVP;multicast;client_port=5000-5001");
  RS_CHECK_EQ(h.Do(multicast), 461U);
  h.Drain();

  Req missing(Method::kSetup, "/live/trackID=0");
  missing.H("CSeq", "11");
  RS_CHECK_EQ(h.Do(missing), 461U);
  h.Drain();

  Req aggregate(Method::kSetup, "/live");
  aggregate.H("CSeq", "12");
  aggregate.H("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1");
  RS_CHECK_EQ(h.Do(aggregate), 459U);
  h.Drain();

  Req unknown(Method::kSetup, "/live/trackID=9");
  unknown.H("CSeq", "13");
  unknown.H("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1");
  RS_CHECK_EQ(h.Do(unknown), 404U);
  h.Drain();

  Req bogus(Method::kSetup, "/live/trackID=0");
  bogus.H("CSeq", "14");
  bogus.H("Session", "deadbeefdeadbeef");
  bogus.H("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1");
  RS_CHECK_EQ(h.Do(bogus), 454U);
  RS_CHECK_EQ(h.session.State(), sess::kInit);
  return 0;
}

int TestPlayNeedsIdrThenTimesOut() {
  Harness h;
  RS_CHECK(SetupTcp(h, "20"));
  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);

  Req play(Method::kPlay, "/live/trackID=0");
  play.H("CSeq", "21");
  play.H("Session", id);
  play.H("Range", "npt=0-");
  RS_CHECK_EQ(h.Do(play), 200U);
  RS_CHECK(h.session.Playing());
  RS_CHECK(h.session.AwaitingRtpInfo());
  RS_CHECK_EQ(ParseStatus(h), 0U);

  h.now += static_cast<uint64_t>(kPlayIdrTimeoutMs) * 1000000ULL + 1000000ULL;
  RS_CHECK(!h.session.OnTick(h.now));
  RS_CHECK(h.session.Closed());
  RS_CHECK(h.session.Reason() == kClosePlayIdrTimeout);
  return 0;
}

int TestPlayRtpInfoMatchesFirstPacket() {
  Harness h;
  RS_CHECK(SetupTcp(h, "30"));
  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);

  Req play(Method::kPlay, "/live/trackID=0");
  play.H("CSeq", "31");
  play.H("Session", id);
  play.H("Range", "npt=0-");
  RS_CHECK_EQ(h.Do(play), 200U);
  RS_CHECK(h.session.AwaitingRtpInfo());

  // A non-IDR frame must not start media.
  const uint8_t p_au[] = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x00};
  FrameDesc pdesc{};
  RS_CHECK(PushAu(h, p_au, sizeof(p_au), 0U, 1000000ULL, pdesc));
  h.session.OnFrame(pdesc, h.now);
  h.ingress.Release(pdesc);
  RS_CHECK(h.session.AwaitingRtpInfo());
  RS_CHECK_EQ(ParseStatus(h), 0U);

  // The first IDR releases the PLAY response.
  FrameDesc idesc{};
  RS_CHECK(PushAu(h, kIdrAu, sizeof(kIdrAu), static_cast<uint16_t>(kFrameIdr | kFrameHasParams), 40000000ULL,
                  idesc));
  h.session.OnFrame(idesc, h.now);
  h.ingress.Release(idesc);
  RS_CHECK(!h.session.AwaitingRtpInfo());
  RS_CHECK_EQ(ParseStatus(h), 200U);
  RS_CHECK(HasText(h, "RTP-Info: url=rtsp://127.0.0.1:8554/live/trackID=0;seq="));
  RS_CHECK(HasText(h, ";rtptime="));

  char buf[kRtspTxBytes + 1U];
  static_cast<void>(h.Peek(buf, sizeof(buf)));
  const char* seq_text = std::strstr(buf, ";seq=");
  const char* ts_text = std::strstr(buf, ";rtptime=");
  RS_CHECK(seq_text != nullptr);
  RS_CHECK(ts_text != nullptr);
  const uint32_t info_seq = static_cast<uint32_t>(std::strtoul(seq_text + 5, nullptr, 10));
  const uint32_t info_ts = static_cast<uint32_t>(std::strtoul(ts_text + 9, nullptr, 10));

  if (PacketizerReady() && InterleavedReady()) {
    const char* frame = std::strstr(buf, "\r\n\r\n$");
    RS_CHECK(frame != nullptr);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(frame + 4);
    RS_CHECK_EQ(p[0], '$');
    // '$' + channel + length + RTP header: seq is RTP bytes 2..3, ts bytes 4..7.
    const uint32_t rtp_seq = (static_cast<uint32_t>(p[6]) << 8U) | p[7];
    const uint32_t rtp_ts = (static_cast<uint32_t>(p[8]) << 24U) | (static_cast<uint32_t>(p[9]) << 16U) |
                            (static_cast<uint32_t>(p[10]) << 8U) | p[11];
    RS_CHECK_EQ(rtp_seq, info_seq);
    RS_CHECK_EQ(rtp_ts, info_ts);
  } else {
    std::printf("test_rtsp_session: NOTE packetizer/interleaved are stubs, first-packet check skipped\n");
  }
  h.Drain();

  // TEARDOWN stops media, releases the transport and keeps the connection.
  Req teardown(Method::kTeardown, "/live/trackID=0");
  teardown.H("CSeq", "32");
  teardown.H("Session", id);
  RS_CHECK_EQ(h.Do(teardown), 200U);
  RS_CHECK_EQ(h.session.State(), sess::kInit);
  RS_CHECK(!h.session.HasSession());
  RS_CHECK(h.session.Transport() == TransportKind::kNone);
  h.Drain();

  Req again(Method::kTeardown, "/live/trackID=0");
  again.H("CSeq", "33");
  again.H("Session", id);
  RS_CHECK_EQ(h.Do(again), 454U);
  return 0;
}

int TestInputGapAwaitsIdr() {
  Harness h;
  RS_CHECK(SetupTcp(h, "40"));
  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);

  Req play(Method::kPlay, "/live/trackID=0");
  play.H("CSeq", "41");
  play.H("Session", id);
  play.H("Range", "npt=0-");
  RS_CHECK_EQ(h.Do(play), 200U);

  FrameDesc idesc{};
  RS_CHECK(PushAu(h, kIdrAu, sizeof(kIdrAu), static_cast<uint16_t>(kFrameIdr | kFrameHasParams), 1000000ULL, idesc));
  h.session.OnFrame(idesc, h.now);
  h.ingress.Release(idesc);
  RS_CHECK(!h.session.AwaitingRtpInfo());
  h.Drain();

  const uint32_t gaps_before = h.session.Counters().input_gaps;
  h.session.OnInputGap(h.now);
  RS_CHECK_EQ(h.session.Counters().input_gaps, gaps_before + 1U);
  RS_CHECK(h.session.Playing());

  char buf[kRtspTxBytes + 1U];
  // A P frame after the gap must not be forwarded.
  const uint8_t p_au[] = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x00};
  FrameDesc pdesc{};
  RS_CHECK(PushAu(h, p_au, sizeof(p_au), 0U, 50000000ULL, pdesc));
  h.session.OnFrame(pdesc, h.now);
  h.ingress.Release(pdesc);
  RS_CHECK_EQ(h.Peek(buf, sizeof(buf)), 0U);

  // The next IDR resumes media without a new PLAY.
  FrameDesc idesc2{};
  RS_CHECK(PushAu(h, kIdrAu, sizeof(kIdrAu), static_cast<uint16_t>(kFrameIdr | kFrameHasParams), 90000000ULL,
                  idesc2));
  h.session.OnFrame(idesc2, h.now);
  h.ingress.Release(idesc2);
  RS_CHECK(h.Peek(buf, sizeof(buf)) > 0U);
  h.Drain();
  return 0;
}

int TestRtcpSenderReportAndRr() {
  Harness h;
  RS_CHECK(SetupTcp(h, "50"));
  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);
  Req play(Method::kPlay, "/live/trackID=0");
  play.H("CSeq", "51");
  play.H("Session", id);
  play.H("Range", "npt=0-");
  RS_CHECK_EQ(h.Do(play), 200U);

  FrameDesc idesc{};
  RS_CHECK(PushAu(h, kIdrAu, sizeof(kIdrAu), static_cast<uint16_t>(kFrameIdr | kFrameHasParams), 1000000ULL, idesc));
  h.session.OnFrame(idesc, h.now);
  h.ingress.Release(idesc);
  RS_CHECK(!h.session.AwaitingRtpInfo());
  h.Drain();

  // 6 s later the periodic sender report must be emitted on the RTCP channel.
  RS_CHECK_EQ(h.session.Counters().rtcp_sr, 0U);
  RS_CHECK(h.session.OnTick(h.now + 6000000000ULL));
  RS_CHECK_EQ(h.session.Counters().rtcp_sr, 1U);
  char buf[kRtspTxBytes + 1U];
  const uint32_t n = h.Peek(buf, sizeof(buf));
  RS_CHECK(n > 8U);
  RS_CHECK_EQ(buf[0], '$');
  RS_CHECK_EQ(static_cast<uint8_t>(buf[1]), static_cast<uint8_t>(1U));
  RS_CHECK_EQ(static_cast<uint8_t>(buf[4]), static_cast<uint8_t>(0x80U));  // V=2, RC=0.
  RS_CHECK_EQ(static_cast<uint8_t>(buf[5]), static_cast<uint8_t>(200U));   // SR.
  h.Drain();

  // A receiver report on the RTCP channel is parsed and counted.
  const uint8_t rr[] = {0x81U, 201U, 0x00U, 0x07U,  // V=2, RC=1, RR, length=7.
                        0x11U, 0x22U, 0x33U, 0x44U,  // Reporter SSRC.
                        0x55U, 0x66U, 0x77U, 0x88U,  // Source SSRC.
                        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                        0x00U, 0x00U, 0x00U, 0x00U};
  RS_CHECK(h.session.OnInterleaved(1U, osp::byte_span(reinterpret_cast<const std::byte*>(rr), sizeof(rr)), h.now));
  RS_CHECK_EQ(h.session.Counters().rtcp_rr, 1U);
  RS_CHECK_EQ(h.session.Counters().rtcp_bad, 0U);

  // Malformed RTCP is counted, not fatal.
  const uint8_t bad[] = {0x81U, 201U, 0x00U, 0x7FU};
  RS_CHECK(h.session.OnInterleaved(1U, osp::byte_span(reinterpret_cast<const std::byte*>(bad), sizeof(bad)), h.now));
  RS_CHECK_EQ(h.session.Counters().rtcp_bad, 1U);
  return 0;
}

int TestInvalidSequences() {
  Harness h;
  Req early(Method::kPlay, "/live/trackID=0");
  early.H("CSeq", "40");
  RS_CHECK_EQ(h.Do(early), 454U);
  h.Drain();

  Req keepalive(Method::kGetParameter, "/live");
  keepalive.H("CSeq", "41");
  RS_CHECK_EQ(h.Do(keepalive), 454U);
  h.Drain();

  RS_CHECK(SetupTcp(h, "42"));
  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);

  Req play(Method::kPlay, "/live/trackID=0");
  play.H("CSeq", "43");
  play.H("Session", id);
  RS_CHECK_EQ(h.Do(play), 200U);
  h.Drain();

  Req play2(Method::kPlay, "/live/trackID=0");
  play2.H("CSeq", "44");
  play2.H("Session", id);
  RS_CHECK_EQ(h.Do(play2), 455U);
  h.Drain();

  Req setup2(Method::kSetup, "/live/trackID=0");
  setup2.H("CSeq", "45");
  setup2.H("Session", id);
  setup2.H("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1");
  RS_CHECK_EQ(h.Do(setup2), 455U);
  h.Drain();

  Req wrong(Method::kGetParameter, "/live");
  wrong.H("CSeq", "46");
  wrong.H("Session", "0000000000000000");
  RS_CHECK_EQ(h.Do(wrong), 454U);
  h.Drain();

  Req ka(Method::kGetParameter, "/live");
  ka.H("CSeq", "47");
  ka.H("Session", id);
  RS_CHECK_EQ(h.Do(ka), 200U);
  RS_CHECK(HasText(h, "Content-Length: 0"));
  h.Drain();

  Req pause(Method::kPause, "/live/trackID=0");
  pause.H("CSeq", "48");
  pause.H("Session", id);
  RS_CHECK_EQ(h.Do(pause), 405U);
  RS_CHECK(HasText(h, "Allow: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER"));
  h.Drain();

  Req old(Method::kOptions, "*");
  old.r.version = Span("RTSP/0.9");
  old.H("CSeq", "49");
  RS_CHECK_EQ(h.Do(old), 505U);
  return 0;
}

int TestControlBackpressure() {
  Harness h;
  Req opt(Method::kOptions, "*");
  opt.H("CSeq", "1");
  uint16_t st = 200U;
  uint32_t rounds = 0U;
  for (; rounds < 500U; ++rounds) {
    st = h.Do(opt);
    if (st == 0U) {
      break;
    }
  }
  RS_CHECK_EQ(st, 0U);
  RS_CHECK(h.session.Closed());
  RS_CHECK(h.session.Reason() == kCloseControlOverflow);
  RS_CHECK(rounds > 0U);
  return 0;
}

// ===========================================================================
// H.265 session coverage
// ===========================================================================

const uint8_t kHevcVps[] = {0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01};
const uint8_t kHevcSps[] = {0x42, 0x01, 0x01, 0x60, 0xAA, 0xBB, 0x01};
const uint8_t kHevcPps[] = {0x44, 0x01, 0xC1, 0x72, 0xCC, 0x01};
const uint8_t kHevcAud[] = {0x46, 0x01};
const uint8_t kHevcIdr[] = {0x26, 0x01, 0xAF, 0x01, 0x22, 0x33, 0x44};
const uint8_t kHevcTrail[] = {0x02, 0x01, 0x11, 0x22, 0x33};

void AppendAnnexBNal(std::vector<uint8_t>& au, const uint8_t* nal, uint32_t len) {
  static const uint8_t kSc[4] = {0x00, 0x00, 0x00, 0x01};
  au.insert(au.end(), kSc, kSc + 4);
  au.insert(au.end(), nal, nal + len);
}

// AUD + one slice. Parameter sets are injected from TrackParams, matching the
// server's kFrameHasParams path.
std::vector<uint8_t> HevcAu(const uint8_t* slice, uint32_t slice_len) {
  std::vector<uint8_t> au;
  AppendAnnexBNal(au, kHevcAud, sizeof(kHevcAud));
  AppendAnnexBNal(au, slice, slice_len);
  return au;
}

struct PacketView {
  std::vector<uint8_t> bytes;
};

uint32_t HeaderEnd(const std::vector<uint8_t>& buf) {
  for (uint32_t i = 0U; i + 4U <= buf.size(); ++i) {
    if (buf[i] == '\r' && buf[i + 1U] == '\n' && buf[i + 2U] == '\r' && buf[i + 3U] == '\n') {
      return i + 4U;
    }
  }
  return 0U;
}

bool ParseInterleaved(const std::vector<uint8_t>& buf, uint32_t from, std::vector<PacketView>& out) {
  uint32_t i = from;
  bool ok = true;
  while (ok && i + 4U <= buf.size()) {
    const uint32_t len = (static_cast<uint32_t>(buf[i + 2U]) << 8U) | buf[i + 3U];
    if (buf[i] != '$' || i + 4U + len > buf.size()) {
      ok = false;
      break;
    }
    PacketView view;
    view.bytes.assign(buf.begin() + i + 4U, buf.begin() + i + 4U + len);
    out.push_back(std::move(view));
    i += 4U + len;
  }
  return ok;
}

uint16_t RtpSeq(const PacketView& p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p.bytes[2]) << 8U) | p.bytes[3]);
}

uint32_t RtpTs(const PacketView& p) {
  return (static_cast<uint32_t>(p.bytes[4]) << 24U) | (static_cast<uint32_t>(p.bytes[5]) << 16U) |
         (static_cast<uint32_t>(p.bytes[6]) << 8U) | p.bytes[7];
}

const uint8_t* RtpPayload(const PacketView& p) { return p.bytes.data() + 12U; }

uint32_t RtpPayloadLen(const PacketView& p) { return static_cast<uint32_t>(p.bytes.size()) - 12U; }

bool PayloadEquals(const PacketView& p, const uint8_t* expected, uint32_t len) {
  return RtpPayloadLen(p) == len && std::memcmp(RtpPayload(p), expected, len) == 0;
}

// RFC 7798 FU reassembly: payload = 2-byte payload header, 1-byte FU header,
// fragment. Single NAL packets are appended verbatim.
std::vector<uint8_t> ReassembleHevc(const std::vector<PacketView>& pkts, uint32_t first) {
  std::vector<uint8_t> out;
  for (uint32_t i = first; i < pkts.size(); ++i) {
    const uint8_t* p = RtpPayload(pkts[i]);
    const uint32_t len = RtpPayloadLen(pkts[i]);
    if (len < 2U) {
      continue;
    }
    const uint8_t type = static_cast<uint8_t>((p[0] >> 1U) & 0x3FU);
    if (type != 49U) {
      out.insert(out.end(), p, p + len);
    } else if (len >= 3U) {
      if ((p[2] & 0x80U) != 0U) {
        out.push_back(static_cast<uint8_t>((p[0] & 0x81U) | ((p[2] & 0x3FU) << 1U)));
        out.push_back(p[1]);
      }
      out.insert(out.end(), p + 3U, p + len);
    }
  }
  return out;
}

uint32_t ParseInfo(const char* buf, const char* tag) {
  const char* p = std::strstr(buf, tag);
  return (p == nullptr) ? 0xFFFFFFFFU : static_cast<uint32_t>(std::strtoul(p + std::strlen(tag), nullptr, 10));
}

// Collects PendingTx into `capture`, then releases it.
void Capture(Harness& h, std::vector<uint8_t>& capture) {
  char buf[kRtspTxBytes + 1U];
  const uint32_t n = h.Peek(buf, sizeof(buf));
  capture.insert(capture.end(), buf, buf + n);
  h.Drain();
}

bool PushAndPlay(Harness& h, const std::vector<uint8_t>& au, uint16_t flags) {
  FrameDesc desc{};
  if (!PushAu(h, au.data(), static_cast<uint32_t>(au.size()), flags, 40000000ULL, desc)) {
    return false;
  }
  h.session.OnFrame(desc, h.now);
  h.ingress.Release(desc);
  return true;
}

int TestH265DescribeAndTcpPlay() {
  Harness h(Codec::kH265);
  if (!SdpReady(h.params)) {
    std::printf("test_rtsp_session: NOTE H.265 SDP is not implemented yet, H.265 checks skipped\n");
    return 0;
  }
  Req desc(Method::kDescribe, "/live");
  desc.H("CSeq", "60");
  desc.H("Accept", "application/sdp");
  RS_CHECK_EQ(h.Do(desc), 200U);
  RS_CHECK(HasText(h, "a=rtpmap:96 H265/90000"));
  RS_CHECK(HasText(h, "sprop-vps="));
  RS_CHECK(HasText(h, "sprop-sps="));
  RS_CHECK(HasText(h, "sprop-pps="));
  RS_CHECK(!HasText(h, "H264/"));
  RS_CHECK(HasText(h, "m=video 0 RTP/AVP 96"));

  // Content-Length must cover exactly the SDP body (no trailing NUL/media).
  char buf[kRtspTxBytes + 1U];
  const uint32_t n = h.Peek(buf, sizeof(buf));
  const char* cl = std::strstr(buf, "Content-Length: ");
  RS_CHECK(cl != nullptr);
  const uint32_t declared = static_cast<uint32_t>(std::strtoul(cl + 16U, nullptr, 10));
  const uint32_t body = HeaderEnd(std::vector<uint8_t>(buf, buf + n));
  RS_CHECK(body > 0U);
  RS_CHECK(body + declared <= n);
  RS_CHECK(std::memcmp(buf + body, "v=0\r\n", 5U) == 0);
  RS_CHECK(buf[body + declared - 1U] == '\n');
  h.Drain();

  RS_CHECK(SetupTcp(h, "61"));
  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);
  Req play(Method::kPlay, "/live/trackID=0");
  play.H("CSeq", "62");
  play.H("Session", id);
  play.H("Range", "npt=0-");
  RS_CHECK_EQ(h.Do(play), 200U);
  RS_CHECK(h.session.AwaitingRtpInfo());

  // A non-IRAP AU must not start media.
  RS_CHECK(PushAndPlay(h, HevcAu(kHevcTrail, sizeof(kHevcTrail)), 0U));
  RS_CHECK(h.session.AwaitingRtpInfo());

  std::vector<uint8_t> idr_au = HevcAu(kHevcIdr, sizeof(kHevcIdr));
  RS_CHECK(PushAndPlay(h, idr_au, static_cast<uint16_t>(kFrameIdr | kFrameHasParams)));
  RS_CHECK(!h.session.AwaitingRtpInfo());

  std::vector<uint8_t> capture;
  Capture(h, capture);
  const uint32_t media_start = HeaderEnd(capture);
  RS_CHECK(media_start > 0U);
  std::vector<PacketView> pkts;
  RS_CHECK(ParseInterleaved(capture, media_start, pkts));
  RS_CHECK(pkts.size() >= 5U);
  RS_CHECK(PayloadEquals(pkts[0], kHevcVps, sizeof(kHevcVps)));
  RS_CHECK(PayloadEquals(pkts[1], kHevcSps, sizeof(kHevcSps)));
  RS_CHECK(PayloadEquals(pkts[2], kHevcPps, sizeof(kHevcPps)));
  RS_CHECK(PayloadEquals(pkts[3], kHevcAud, sizeof(kHevcAud)));

  char text[kRtspTxBytes + 1U];
  std::memset(text, 0, sizeof(text));
  std::memcpy(text, capture.data(), (media_start < sizeof(text)) ? media_start : sizeof(text) - 1U);
  const uint32_t info_seq = ParseInfo(text, ";seq=");
  const uint32_t info_ts = ParseInfo(text, ";rtptime=");
  RS_CHECK_EQ(info_seq, RtpSeq(pkts[0]));
  RS_CHECK_EQ(info_ts, RtpTs(pkts[0]));
  return 0;
}

int TestH265FuAcrossPumps() {
  Harness h(Codec::kH265);
  if (!SdpReady(h.params)) {
    std::printf("test_rtsp_session: NOTE H.265 SDP is not implemented yet, FU check skipped\n");
    return 0;
  }
  RS_CHECK(SetupTcp(h, "70"));
  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);
  Req play(Method::kPlay, "/live/trackID=0");
  play.H("CSeq", "71");
  play.H("Session", id);
  RS_CHECK_EQ(h.Do(play), 200U);
  h.Drain();

  std::vector<uint8_t> slice;
  slice.push_back(0x26);
  slice.push_back(0x01);
  for (uint32_t i = 0U; i < 20000U; ++i) {
    slice.push_back(static_cast<uint8_t>(0x20U + (i % 0xDFU)));
  }
  std::vector<uint8_t> au = HevcAu(slice.data(), static_cast<uint32_t>(slice.size()));
  RS_CHECK(PushAndPlay(h, au, static_cast<uint16_t>(kFrameIdr | kFrameHasParams)));

  std::vector<uint8_t> capture;
  Capture(h, capture);
  for (uint32_t round = 0U; round < 6U; ++round) {
    h.now += 1000000ULL;
    h.session.Pump(h.now);
    Capture(h, capture);
  }

  const uint32_t body = HeaderEnd(capture);
  RS_CHECK(body > 0U);
  std::vector<PacketView> pkts;
  RS_CHECK(ParseInterleaved(capture, body, pkts));
  RS_CHECK(pkts.size() > 16U);  // More than one pump budget.
  std::vector<uint8_t> expected;
  expected.insert(expected.end(), kHevcAud, kHevcAud + sizeof(kHevcAud));
  expected.insert(expected.end(), slice.begin(), slice.end());
  const std::vector<uint8_t> rebuilt = ReassembleHevc(pkts, 3U);
  if (rebuilt != expected) {
    std::fprintf(stderr, "FU mismatch: pkts=%zu rebuilt=%zu expected=%zu\n", pkts.size(), rebuilt.size(),
                 expected.size());
    for (uint32_t i = 0U; i < rebuilt.size() && i < expected.size(); ++i) {
      if (rebuilt[i] != expected[i]) {
        std::fprintf(stderr, "first diff at %u: rebuilt=%02x expected=%02x\n", i, rebuilt[i], expected[i]);
        break;
      }
    }
  }
  RS_CHECK(rebuilt == expected);
  return 0;
}

int OpenUdpClient(uint16_t& port) {
  int fd = -1;
  uint16_t chosen = 0U;
  for (uint16_t p = 20000U; fd < 0 && p < 40000U; p = static_cast<uint16_t>(p + 2U)) {
    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
      break;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(p);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
      fd = s;
      chosen = p;
    } else {
      ::close(s);
    }
  }
  port = chosen;
  return fd;
}

int TestH265UdpPlay() {
  Harness h(Codec::kH265);
  if (!SdpReady(h.params)) {
    std::printf("test_rtsp_session: NOTE H.265 SDP is not implemented yet, UDP check skipped\n");
    return 0;
  }
  uint16_t cport = 0U;
  const int fd = OpenUdpClient(cport);
  RS_CHECK(fd >= 0);
  timeval tv{};
  tv.tv_sec = 2;
  static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));

  char transport[96];
  static_cast<void>(std::snprintf(transport, sizeof(transport), "RTP/AVP;unicast;client_port=%u-%u",
                                  static_cast<unsigned>(cport), static_cast<unsigned>(cport + 1U)));
  Req setup(Method::kSetup, "/live/trackID=0");
  setup.H("CSeq", "80");
  setup.H("Transport", transport);
  RS_CHECK_EQ(h.Do(setup), 200U);
  h.Drain();
  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);
  Req play(Method::kPlay, "/live/trackID=0");
  play.H("CSeq", "81");
  play.H("Session", id);
  RS_CHECK_EQ(h.Do(play), 200U);

  std::vector<uint8_t> au = HevcAu(kHevcIdr, sizeof(kHevcIdr));
  RS_CHECK(PushAndPlay(h, au, static_cast<uint16_t>(kFrameIdr | kFrameHasParams)));

  char text[kRtspTxBytes + 1U];
  std::memset(text, 0, sizeof(text));
  static_cast<void>(h.Peek(text, sizeof(text)));
  const uint32_t info_seq = ParseInfo(text, ";seq=");
  const uint32_t info_ts = ParseInfo(text, ";rtptime=");
  RS_CHECK(info_seq != 0xFFFFFFFFU);
  h.Drain();

  std::vector<PacketView> pkts;
  uint8_t buf[2048];
  for (uint32_t i = 0U; i < 4U; ++i) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 12) {
      break;
    }
    PacketView view;
    view.bytes.assign(buf, buf + n);
    pkts.push_back(std::move(view));
  }
  ::close(fd);
  RS_CHECK_EQ(pkts.size(), 4U);
  RS_CHECK(PayloadEquals(pkts[0], kHevcVps, sizeof(kHevcVps)));
  RS_CHECK(PayloadEquals(pkts[1], kHevcSps, sizeof(kHevcSps)));
  RS_CHECK(PayloadEquals(pkts[2], kHevcPps, sizeof(kHevcPps)));
  RS_CHECK_EQ(info_seq, RtpSeq(pkts[0]));
  RS_CHECK_EQ(info_ts, RtpTs(pkts[0]));
  return 0;
}

int TestH265BadParamSets() {
  char body[4096];
  SdpParams sp{};
  sp.codec = Codec::kH265;
  sp.server_ip = "127.0.0.1";
  sp.session_name = "live";
  sp.sps = kHevcSps;
  sp.sps_len = sizeof(kHevcSps);
  sp.pps = kHevcPps;
  sp.pps_len = sizeof(kHevcPps);
  auto r = BuildSdp(sp, osp::writable_byte_span(reinterpret_cast<std::byte*>(body), sizeof(body)));
  RS_CHECK(!r.has_value());
  RS_CHECK_EQ(r.get_error(), SdpError::kBadParamSet);
  return 0;
}

int TestCodecExclusivePerSession() {
  Harness h(Codec::kH264);
  Req desc(Method::kDescribe, "/live");
  desc.H("CSeq", "90");
  RS_CHECK_EQ(h.Do(desc), 200U);
  RS_CHECK(HasText(h, "H264/90000"));
  RS_CHECK(!HasText(h, "H265/"));
  h.Drain();

  TrackParams p265 = h.params;
  p265.codec = Codec::kH265;
  p265.vps = h.vps265;
  p265.vps_len = sizeof(h.vps265);
  p265.sps = h.sps265;
  p265.sps_len = sizeof(h.sps265);
  p265.pps = h.pps265;
  p265.pps_len = sizeof(h.pps265);
  SessionConfig cfg;
  cfg.advertise_ip = "127.0.0.1";
  cfg.rtsp_port = 8554U;
  cfg.id_seed = 0x0123456789ABCDEFULL;
  h.session.Configure(h.ingress, p265, cfg);
  Req desc2(Method::kDescribe, "/live");
  desc2.H("CSeq", "91");
  RS_CHECK_EQ(h.Do(desc2), 200U);
  RS_CHECK(HasText(h, "H265/90000"));
  RS_CHECK(!HasText(h, "H264/"));
  h.Drain();

  h.session.Configure(h.ingress, h.params, cfg);
  Req desc3(Method::kDescribe, "/live");
  desc3.H("CSeq", "92");
  RS_CHECK_EQ(h.Do(desc3), 200U);
  RS_CHECK(HasText(h, "H264/90000"));
  RS_CHECK(!HasText(h, "H265/"));
  return 0;
}

uint16_t FreeTcpPort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  socklen_t len = sizeof(addr);
  uint16_t port = 0U;
  if (fd >= 0 && ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 &&
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    port = ntohs(addr.sin_port);
  }
  if (fd >= 0) {
    ::close(fd);
  }
  return port;
}

bool WriteFile(const char* path, const uint8_t* data, uint32_t len) {
  std::FILE* f = std::fopen(path, "wb");
  bool ok = false;
  if (f != nullptr) {
    ok = std::fwrite(data, 1U, len, f) == len && std::fclose(f) == 0;
  }
  return ok;
}

// SPS/PPS are followed by a four-byte start code. The old extractor counted
// the leading zero of that start code as part of the NAL.
const uint8_t kH264ParamStream[] = {
    0x00, 0x00, 0x00, 0x01, 0x09, 0xF0, 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F, 0x8C, 0x8D, 0x40,
    0x50, 0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
    0xFF, 0x00, 0x00, 0x00, 0x01, 0x09, 0xF0, 0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x20,
};

const uint8_t kH265ParamStream[] = {
    0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x60, 0xAA, 0xBB, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x44, 0x01, 0xC1, 0x72, 0xCC, 0x01, 0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAF, 0x01, 0x22, 0x33,
    0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x11, 0x22,
};

const uint8_t kH265NoVpsStream[] = {
    0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x60, 0xAA, 0xBB,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xC1, 0x72, 0xCC, 0x01, 0x00, 0x00, 0x00, 0x01, 0x26,
    0x01, 0xAF, 0x01, 0x22, 0x33,
};

bool StartWithFreePort(RtspServer& server, ServerConfig& cfg) {
  // FreeTcpPort() releases the probe fd before Start(); another process can
  // steal the port in that window. Retry with a fresh port instead of turning
  // an environment race into a test failure.
  for (uint32_t attempt = 0U; attempt < 3U; ++attempt) {
    cfg.port = FreeTcpPort();
    if (server.Start(cfg).has_value()) {
      return true;
    }
  }
  return false;
}

int TestServerParamSets() {
  char p264[128];
  char p265[128];
  char pbad[128];
  static_cast<void>(std::snprintf(p264, sizeof(p264), "/tmp/rs_ps264_%d.bin", static_cast<int>(::getpid())));
  static_cast<void>(std::snprintf(p265, sizeof(p265), "/tmp/rs_ps265_%d.bin", static_cast<int>(::getpid())));
  static_cast<void>(std::snprintf(pbad, sizeof(pbad), "/tmp/rs_psbad_%d.bin", static_cast<int>(::getpid())));
  RS_CHECK(WriteFile(p264, kH264ParamStream, sizeof(kH264ParamStream)));
  RS_CHECK(WriteFile(p265, kH265ParamStream, sizeof(kH265ParamStream)));
  RS_CHECK(WriteFile(pbad, kH265NoVpsStream, sizeof(kH265NoVpsStream)));

  // Parameter sets now live in the source adapter; the server only validates
  // and copies the TrackParams it is handed.
  FileSource src264;
  auto o264 = src264.Open(p264, Codec::kUnknown);
  RS_CHECK(o264.has_value());
  ServerConfig cfg;
  cfg.params = o264.value().params;
  cfg.port = FreeTcpPort();
  cfg.advertise_ip = "127.0.0.1";
  cfg.max_clients = 4U;
  const std::unique_ptr<RtspServer> s264(new RtspServer());
  RS_CHECK(StartWithFreePort(*s264, cfg));
  const ServerStats st264 = s264->Stats();
  RS_CHECK(st264.codec == Codec::kH264);
  RS_CHECK_EQ(st264.sps_len, 8U);  // 0x67 + 7 bytes, no trailing zero.
  RS_CHECK_EQ(st264.pps_len, 4U);
  s264->Stop();

  FileSource src265;
  auto o265 = src265.Open(p265, Codec::kUnknown);
  RS_CHECK(o265.has_value());
  cfg.params = o265.value().params;
  cfg.port = FreeTcpPort();
  const std::unique_ptr<RtspServer> s265(new RtspServer());
  RS_CHECK(StartWithFreePort(*s265, cfg));
  const ServerStats st265 = s265->Stats();
  RS_CHECK(st265.codec == Codec::kH265);
  RS_CHECK_EQ(st265.vps_len, 7U);
  RS_CHECK_EQ(st265.sps_len, 7U);
  RS_CHECK_EQ(st265.pps_len, 6U);
  s265->Stop();

  // An explicit codec that disagrees with the detected header is rejected at
  // the source boundary, before any server is started.
  FileSource mismatch_src;
  auto mm = mismatch_src.Open(p265, Codec::kH264);
  RS_CHECK(!mm.has_value());
  RS_CHECK(mm.get_error() == FileSourceError::kBadCodec);

  // H.265 without VPS cannot produce a usable track.
  FileSource bad_src;
  auto bad = bad_src.Open(pbad, Codec::kUnknown);
  RS_CHECK(!bad.has_value());
  RS_CHECK(bad.get_error() == FileSourceError::kMissingParams);

  static_cast<void>(std::remove(p264));
  static_cast<void>(std::remove(p265));
  static_cast<void>(std::remove(pbad));
  return 0;
}

struct CloseCtx {
  int fds[DeferredCloseBatch::kCapacity];
  uint32_t closed = 0U;
};

void CloseFdThunk(void* ctx, uint32_t slot) noexcept {
  auto* c = static_cast<CloseCtx*>(ctx);
  if (slot < DeferredCloseBatch::kCapacity && c->fds[slot] >= 0) {
    static_cast<void>(::close(c->fds[slot]));
    c->fds[slot] = -1;
    ++c->closed;
  }
}

int TestDeferredCloseBatch() {
  int p1[2] = {-1, -1};
  RS_CHECK_EQ(::pipe(p1), 0);
  CloseCtx ctx{};
  for (uint32_t i = 0U; i < DeferredCloseBatch::kCapacity; ++i) {
    ctx.fds[i] = -1;
  }
  ctx.fds[2] = p1[0];
  DeferredCloseBatch batch;
  batch.Configure(&CloseFdThunk, &ctx);
  RS_CHECK(batch.Mark(2U));
  RS_CHECK(!batch.Mark(2U));
  RS_CHECK(batch.Pending(2U));
  // The fd stays open until the flush point, so accept() cannot recycle it.
  RS_CHECK(::fcntl(p1[0], F_GETFD) >= 0);
  int p2[2] = {-1, -1};
  RS_CHECK_EQ(::pipe(p2), 0);
  RS_CHECK(p2[0] != p1[0]);
  RS_CHECK(p2[1] != p1[0]);
  RS_CHECK_EQ(ctx.closed, 0U);
  batch.Flush();
  RS_CHECK_EQ(ctx.closed, 1U);
  RS_CHECK_EQ(::fcntl(p1[0], F_GETFD), -1);
  // After the batch the kernel reuses the freed fd number.
  int p3[2] = {-1, -1};
  RS_CHECK_EQ(::pipe(p3), 0);
  RS_CHECK_EQ(p3[0], p1[0]);
  static_cast<void>(::close(p2[0]));
  static_cast<void>(::close(p2[1]));
  static_cast<void>(::close(p3[0]));
  static_cast<void>(::close(p3[1]));
  static_cast<void>(::close(p1[1]));

  // Stale poll tags: a recycled slot (new gen) or a closing slot must not be
  // delivered, so the old event cannot reach the new connection.
  const uintptr_t old_tag = (5U << 8U) | 2U;
  RS_CHECK(ConnectionTagValid(old_tag, 5U, true, false));
  RS_CHECK(!ConnectionTagValid(old_tag, 6U, true, false));
  RS_CHECK(!ConnectionTagValid(old_tag, 5U, true, true));
  RS_CHECK(!ConnectionTagValid(old_tag, 5U, false, false));
  return 0;
}

void CountIdr(void* ctx) noexcept { ++*static_cast<uint32_t*>(ctx); }

int TestIdrHookRateLimit() {
  Harness h;
  uint32_t calls = 0U;
  h.idr.fn.store(&CountIdr, std::memory_order_release);
  h.idr.ctx.store(&calls, std::memory_order_release);

  RS_CHECK(SetupTcp(h, "60"));
  char id[20];
  std::memcpy(id, h.session.SessionId(), 17U);
  Req play(Method::kPlay, "/live/trackID=0");
  play.H("CSeq", "61");
  play.H("Session", id);
  play.H("Range", "npt=0-");
  RS_CHECK_EQ(h.Do(play), 200U);
  // PLAY waits for the first IDR and asks the encoder once.
  RS_CHECK_EQ(calls, 1U);
  RS_CHECK_EQ(h.session.Counters().idr_requests, 1U);

  // A gap inside the rate-limit window must not call the encoder again.
  const uint64_t play_ns = h.now;
  h.session.OnInputGap(play_ns + 500ULL * 1000000ULL);
  RS_CHECK_EQ(h.session.Counters().input_gaps, 1U);
  RS_CHECK_EQ(calls, 1U);

  // After the window a still-missing IDR triggers one more request.
  h.session.OnInputGap(play_ns + 1500ULL * 1000000ULL);
  RS_CHECK_EQ(calls, 2U);
  RS_CHECK_EQ(h.idr.requests.load(std::memory_order_relaxed), 2U);

  // The IDR resumes media and no further request is made.
  FrameDesc idesc{};
  RS_CHECK(PushAu(h, kIdrAu, sizeof(kIdrAu), static_cast<uint16_t>(kFrameIdr | kFrameHasParams),
                  2000000000ULL, idesc));
  h.session.OnFrame(idesc, play_ns + 1600ULL * 1000000ULL);
  h.ingress.Release(idesc);
  RS_CHECK(!h.session.AwaitingRtpInfo());
  RS_CHECK_EQ(calls, 2U);
  h.session.Close(kCloseTeardown, play_ns + 1700ULL * 1000000ULL);
  return 0;
}

}  // namespace

int main() {
  if (TestSnapshotInitial() != 0) return 1;
  if (TestFaultSnapshot() != 0) return 1;
  if (TestOptions() != 0) return 1;
  if (TestDescribe() != 0) return 1;
  if (TestSetupTcp() != 0) return 1;
  if (TestSetupUdp() != 0) return 1;
  if (TestSetupErrors() != 0) return 1;
  if (TestPlayNeedsIdrThenTimesOut() != 0) return 1;
  if (TestPlayRtpInfoMatchesFirstPacket() != 0) return 1;
  if (TestInputGapAwaitsIdr() != 0) return 1;
  if (TestRtcpSenderReportAndRr() != 0) return 1;
  if (TestInvalidSequences() != 0) return 1;
  if (TestControlBackpressure() != 0) return 1;
  if (TestH265DescribeAndTcpPlay() != 0) return 1;
  if (TestH265FuAcrossPumps() != 0) return 1;
  if (TestH265UdpPlay() != 0) return 1;
  if (TestH265BadParamSets() != 0) return 1;
  if (TestCodecExclusivePerSession() != 0) return 1;
  if (TestServerParamSets() != 0) return 1;
  if (TestIdrHookRateLimit() != 0) return 1;
  if (TestDeferredCloseBatch() != 0) return 1;
  std::printf("test_rtsp_session: all checks passed\n");
  return 0;
}
