// Per-session RTCP endpoint: periodic SR+SDES and incoming compound parsing.
// Owns only RTCP timing/octet state; transport and counters stay in the session.
// SPDX-License-Identifier: MIT
#ifndef RS_RTCP_ENDPOINT_HPP_
#define RS_RTCP_ENDPOINT_HPP_

#include "osp/span.hpp"
#include "rs/config.hpp"
#include "rs/rtcp.hpp"
#include "rs/rtp_transport.hpp"
#include "rs/tx_stream.hpp"

#include <cstdint>
#include <cstdio>
#include <ctime>

namespace rs {

class RtcpEndpoint final {
 public:
  void Reset() noexcept {
    next_sr_ns_ = 0U;
    octet_count_ = 0U;
    last_rtp_ns_ = 0U;
    last_rtp_ts_ = 0U;
  }

  void OnRtpSent(uint32_t payload_bytes, uint64_t now_ns, uint32_t rtp_ts) noexcept {
    octet_count_ += payload_bytes;
    last_rtp_ns_ = now_ns;
    last_rtp_ts_ = rtp_ts;
    if (next_sr_ns_ == 0U) {
      // RFC 3550 6.1: the first sender report should follow the first data
      // packet quickly so receivers get the NTP/RTP mapping early. Later
      // reports keep the 5 s +/- 50% interval.
      next_sr_ns_ = now_ns + static_cast<uint64_t>(kMsPerSec) * kNsPerMs;
    }
  }

  // Returns true when the peer sent BYE.
  [[nodiscard]] bool OnDatagram(osp::byte_span payload, uint32_t& rr_count, uint32_t& bye_count,
                                uint32_t& bad_count) noexcept {
    auto summary =
        ParseCompound(reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()));
    if (summary.has_value() == false) {
      ++bad_count;
      return false;
    }
    rr_count += summary.value().receiver_reports;
    bye_count += summary.value().byes;
    return summary.value().byes > 0U;
  }

  void MaybeSend(uint64_t now_ns, uint32_t ssrc, const char* advertise_ip, uint32_t packet_count,
                 TransportKind transport, uint8_t rtcp_channel, TxStream& tx, UdpRtpTransport& udp, uint32_t& sr_count,
                 uint32_t& bad_count) noexcept {
    if (next_sr_ns_ == 0U || now_ns < next_sr_ns_ ||
        (transport != TransportKind::kUdp && transport != TransportKind::kTcp)) {
      return;
    }
    struct timespec wall_time{};
    if ((0 != ::clock_gettime(CLOCK_REALTIME, &wall_time)) || (wall_time.tv_sec < 0)) {
      next_sr_ns_ = now_ns + kNsPerMs * kMsPerSec;
      return;
    }
    const uint64_t wall_ns =
        static_cast<uint64_t>(wall_time.tv_sec) * kNsPerSec + static_cast<uint64_t>(wall_time.tv_nsec);
    const uint64_t elapsed_ns = (now_ns >= last_rtp_ns_) ? (now_ns - last_rtp_ns_) : 0U;
    const uint64_t elapsed_ticks =
        (elapsed_ns / kNsPerSec) * kClockRate + ((elapsed_ns % kNsPerSec) * kClockRate) / kNsPerSec;
    const uint32_t rtp_ts = last_rtp_ts_ + static_cast<uint32_t>(elapsed_ticks);
    uint8_t buf[kMaxRtcpBytes] = {};
    char cname[32] = {};
    std::snprintf(cname, sizeof(cname), "rs500@%s", (advertise_ip != nullptr) ? advertise_ip : "0.0.0.0");
    auto built = BuildSenderReportWithSdes(osp::writable_byte_span(reinterpret_cast<std::byte*>(buf), sizeof(buf)),
                                           ssrc, cname, wall_ns, rtp_ts, packet_count, octet_count_);
    if (built.has_value() == false) {
      ++bad_count;
      next_sr_ns_ = now_ns + kNsPerMs * kMsPerSec;
      return;
    }
    const uint32_t len = built.value();
    bool sent = false;
    if (transport == TransportKind::kUdp) {
      const auto rc = udp.SendRtcp(buf, len);
      sent = rc.has_value() && rc.value() == SendStatus::kSent;
    } else if ((tx.Unflushed() <= (kMediaTxLimitBytes - (kInterleavedHeaderBytes + len))) &&
               tx.Reserve(kInterleavedHeaderBytes + len)) {
      const auto written = WriteInterleavedRaw(tx.Tail(), rtcp_channel, buf, len);
      if (written.has_value()) {
        tx.MarkProgressIfEmpty(now_ns);
        sent = tx.Commit(written.value());
      }
    }
    if (sent) {
      ++sr_count;
      const uint64_t spread = 2U * kRtcpSrJitterMs + 1U;
      const uint64_t jitter = SplitMix64(now_ns ^ static_cast<uint64_t>(ssrc)) % spread;
      const uint64_t period_ms = kRtcpSrPeriodMs - kRtcpSrJitterMs + jitter;
      next_sr_ns_ = now_ns + period_ms * kNsPerMs;
    } else {
      next_sr_ns_ = now_ns + kNsPerMs * kMsPerSec;
    }
  }

  [[nodiscard]] uint64_t NextSrNs() const noexcept { return next_sr_ns_; }
  [[nodiscard]] uint32_t OctetCount() const noexcept { return octet_count_; }

 private:
  static uint64_t SplitMix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    uint64_t z = x;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  uint64_t next_sr_ns_ = 0U;
  uint64_t last_rtp_ns_ = 0U;
  uint32_t last_rtp_ts_ = 0U;
  uint32_t octet_count_ = 0U;
};

}  // namespace rs

#endif  // RS_RTCP_ENDPOINT_HPP_
