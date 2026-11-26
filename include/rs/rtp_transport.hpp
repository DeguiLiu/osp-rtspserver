// RTP egress: UDP send socket, TCP interleaved framing, shared token bucket.
// SPDX-License-Identifier: MIT
#ifndef RS_RTP_TRANSPORT_HPP_
#define RS_RTP_TRANSPORT_HPP_

#include "osp/socket.hpp"
#include "osp/span.hpp"
#include "osp/vocabulary.hpp"
#include "rs/h264_packetizer.hpp"

#include <cstdint>

namespace rs {

enum class TransportError : uint8_t { kNotConfigured = 0, kTooLarge, kFatal, kWouldBlock };

enum class SendStatus : uint8_t { kSent = 0, kWouldBlock };

// One UDP socket pair per playing session (RTP + RTCP), bound to ephemeral
// local ports and connected to the client's advertised ports.
class UdpRtpTransport {
 public:
  UdpRtpTransport() noexcept = default;
  ~UdpRtpTransport() = default;
  UdpRtpTransport(const UdpRtpTransport&) = delete;
  UdpRtpTransport& operator=(const UdpRtpTransport&) = delete;
  // Movable so a repeated SETUP can build the new pair first and swap it in
  // only after Open() succeeds (review P1-3).
  UdpRtpTransport(UdpRtpTransport&&) noexcept = default;
  UdpRtpTransport& operator=(UdpRtpTransport&&) noexcept = default;

  // dest_ipv4 is host byte order. Local ports are written back on success.
  [[nodiscard]] bool Open(uint32_t dest_ipv4, uint16_t dest_rtp_port, uint16_t dest_rtcp_port, uint16_t& local_rtp_port,
                          uint16_t& local_rtcp_port) noexcept;
  void Close() noexcept;
  bool IsOpen() const noexcept { return rtp_.Fd() >= 0; }

  [[nodiscard]] osp::expected<SendStatus, TransportError> SendRtp(const RtpPacket& pkt) noexcept;
  [[nodiscard]] osp::expected<SendStatus, TransportError> SendRtcp(const uint8_t* data, uint32_t len) noexcept;
  [[nodiscard]] int32_t RtcpFd() const noexcept { return static_cast<int32_t>(rtcp_.Fd()); }
  [[nodiscard]] osp::expected<uint32_t, TransportError> RecvRtcp(uint8_t* data, uint32_t cap) noexcept;

 private:
  osp::UdpSocket rtp_;
  osp::UdpSocket rtcp_;
};

// Writes '$' | channel | 16-bit length | payload into out.
// Returns bytes written; kTooLarge when the packet exceeds out or 65535.
[[nodiscard]] osp::expected<uint32_t, TransportError> WriteInterleaved(osp::writable_byte_span out, uint8_t channel,
                                                                       const RtpPacket& pkt) noexcept;
[[nodiscard]] osp::expected<uint32_t, TransportError> WriteInterleavedRaw(osp::writable_byte_span out, uint8_t channel,
                                                                          const uint8_t* data, uint32_t len) noexcept;

// Shared egress shaper. Bytes are admitted in whole packets; a rejected
// packet is dropped (never split) and the caller counts it.
class TokenBucket {
 public:
  void Configure(uint32_t rate_bps, uint32_t burst_bytes, uint64_t now_ns) noexcept;
  bool TryConsume(uint32_t bytes, uint64_t now_ns) noexcept;
  uint32_t Available(uint64_t now_ns) noexcept;
  bool IsConfigured() const noexcept { return rate_bps_ != 0U; }

 private:
  uint32_t rate_bps_ = 0U;
  uint32_t burst_bytes_ = 0U;
  uint32_t tokens_ = 0U;
  uint64_t last_ns_ = 0U;
};

}  // namespace rs

#endif  // RS_RTP_TRANSPORT_HPP_
