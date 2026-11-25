// RTP egress: connected UDP socket pair, TCP interleaved framing, token bucket.
// Thread ownership: loop thread only; no session state is kept here.
// SPDX-License-Identifier: MIT
#include "rs/rtp_transport.hpp"

#include "osp/socket.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>

#include <sys/socket.h>
#include <sys/uio.h>

#include <array>
#include <utility>

namespace rs {
namespace {

// 8 bits/byte * 1e9 ns/s. Kept in 64 bits so 32-bit ARM never truncates.
constexpr uint64_t kNsPerByteAtOneBps = UINT64_C(8000000000);
constexpr uint32_t kSendRetryLimit = 4U;
constexpr uint32_t kUdpPortPairAttempts = 32U;

enum class IoResult : uint8_t { kSent = 0, kWouldBlock, kFatal };

// Saturating multiply so a large time delta cannot wrap on 32-bit targets.
uint64_t MulSaturate(uint64_t a, uint64_t b) noexcept {
  return (0U != a && b > UINT64_MAX / a) ? UINT64_MAX : a * b;
}

// Sends iovecs as one datagram. Returns kSent only when the whole datagram was
// handed to the kernel; a short UDP write is treated as fatal.
IoResult SendIovecs(osp::SocketHandle fd, struct iovec* iov, uint32_t iov_count, uint32_t total) noexcept {
  struct msghdr msg {};
  msg.msg_iov = iov;
  msg.msg_iovlen = static_cast<size_t>(iov_count);
  IoResult result = IoResult::kFatal;
  for (uint32_t attempt = 0U; attempt < kSendRetryLimit && IoResult::kFatal == result; ++attempt) {
    const ssize_t n = ::sendmsg(fd, &msg, osp::kSendNoSignal);
    if (n >= 0) {
      result = (static_cast<uint64_t>(n) == total) ? IoResult::kSent : IoResult::kFatal;
    } else if (EINTR == errno) {
      continue;
    } else {
      result = (EAGAIN == errno || EWOULDBLOCK == errno || ENOBUFS == errno || ENOMEM == errno)
                   ? IoResult::kWouldBlock
                   : IoResult::kFatal;
    }
  }
  return result;
}

// dest_ipv4 follows the project convention used by RtspSession::SetPeerIpv4:
// the in_addr value read in host byte order, so 127.0.0.1 is 0x0100007F on a
// little-endian target (octet 0 in the low 8 bits).
void SplitIpv4(uint32_t ipv4, std::array<uint8_t, 4>& octets) noexcept {
  octets[0] = static_cast<uint8_t>(ipv4 & kByteMask);
  octets[1] = static_cast<uint8_t>((ipv4 >> 8U) & kByteMask);
  octets[2] = static_cast<uint8_t>((ipv4 >> 16U) & kByteMask);
  octets[3] = static_cast<uint8_t>((ipv4 >> 24U) & kByteMask);
}

// Creates one UDP socket bound to 0.0.0.0:0 and connected to the peer, so ICMP
// errors surface on later sends and datagrams from other peers are filtered.
bool OpenOne(osp::UdpSocket& sock, uint32_t dest_ipv4, uint16_t dest_port, uint16_t bind_port,
             uint16_t& local_port) noexcept {
  auto created = osp::UdpSocket::Create();
  bool ok = static_cast<bool>(created);
  if (ok) {
    sock = std::move(created.value());
    const int32_t fd = static_cast<int32_t>(sock.Fd());
    const int fd_flags = ::fcntl(fd, F_GETFD);
    if (fd_flags >= 0) {
      static_cast<void>(::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC));
    }
#if defined(IP_TOS)
    if (kUdpIpTos != 0U) {
      int32_t tos = static_cast<int32_t>(kUdpIpTos);
      static_cast<void>(osp::socket_api::SetSockOpt(fd, IPPROTO_IP, IP_TOS, &tos,
                                                    static_cast<socklen_t>(sizeof(tos))));
    }
#endif
    const std::array<uint8_t, 4> any_ip{0U, 0U, 0U, 0U};
    auto local = osp::SocketAddress::FromIpv4Bytes(any_ip, bind_port);
    ok = static_cast<bool>(local) && static_cast<bool>(sock.Bind(local.value()));
  }
  if (ok) {
    sockaddr_in bound{};
    socklen_t bound_len = static_cast<socklen_t>(sizeof(bound));
    ok = osp::socket_api::GetSockName(sock.Fd(), reinterpret_cast<sockaddr*>(&bound), &bound_len) >= 0;
    if (ok) {
      local_port = osp::socket_api::Ntohs(bound.sin_port);
    }
  }
  if (ok) {
    std::array<uint8_t, 4> dest_ip{};
    SplitIpv4(dest_ipv4, dest_ip);
    auto peer = osp::SocketAddress::FromIpv4Bytes(dest_ip, dest_port);
    ok = static_cast<bool>(peer) &&
         osp::socket_api::Connect(sock.Fd(), peer.value().Raw(), peer.value().Size()) >= 0;
  }
  if (ok) {
    int32_t sndbuf = static_cast<int32_t>(kUdpSndBufBytes);
    static_cast<void>(osp::socket_api::SetSockOpt(sock.Fd(), SOL_SOCKET, SO_SNDBUF, &sndbuf,
                                                  static_cast<socklen_t>(sizeof(sndbuf))));
    int32_t rcvbuf = static_cast<int32_t>(kUdpRcvBufBytes);
    static_cast<void>(osp::socket_api::SetSockOpt(sock.Fd(), SOL_SOCKET, SO_RCVBUF, &rcvbuf,
                                                  static_cast<socklen_t>(sizeof(rcvbuf))));
    ok = osp::SetFdNonBlocking(sock.Fd(), true) >= 0;
  }
  if (!ok) {
    sock.Close();
    local_port = 0U;
  }
  return ok;
}

void PutHeader(osp::writable_byte_span out, uint8_t channel, uint32_t len) noexcept {
  out[0] = static_cast<std::byte>('$');
  out[1] = static_cast<std::byte>(channel);
  out[2] = static_cast<std::byte>((len >> 8U) & kByteMask);
  out[3] = static_cast<std::byte>(len & kByteMask);
}

}  // namespace

bool UdpRtpTransport::Open(uint32_t dest_ipv4, uint16_t dest_rtp_port, uint16_t dest_rtcp_port,
                           uint16_t& local_rtp_port, uint16_t& local_rtcp_port) noexcept {
  Close();
  local_rtp_port = 0U;
  local_rtcp_port = 0U;
  bool ok = false;
  // RFC 3550: RTP uses an even port and RTCP uses RTP+1. Bind the pair
  // explicitly and retry with a new ephemeral RTP port when RTP+1 is taken
  // (review P0-2); never advertise a non-contiguous or odd RTP port.
  for (uint32_t attempt = 0U; !ok && attempt < kUdpPortPairAttempts; ++attempt) {
    if (!OpenOne(rtp_, dest_ipv4, dest_rtp_port, 0U, local_rtp_port)) {
      break;
    }
    if (0U == (local_rtp_port & 1U)) {
      const uint16_t rtcp_bind = static_cast<uint16_t>(local_rtp_port + 1U);
      ok = OpenOne(rtcp_, dest_ipv4, dest_rtcp_port, rtcp_bind, local_rtcp_port) &&
           (local_rtcp_port == rtcp_bind);
    }
    if (!ok) {
      rtp_.Close();
      rtcp_.Close();
      local_rtp_port = 0U;
      local_rtcp_port = 0U;
    }
  }
  if (!ok) {
    Close();
  }
  return ok;
}

void UdpRtpTransport::Close() noexcept {
  rtp_.Close();
  rtcp_.Close();
}

osp::expected<SendStatus, TransportError> UdpRtpTransport::SendRtp(const RtpPacket& pkt) noexcept {
  if (!IsOpen()) {
    return osp::expected<SendStatus, TransportError>::error(TransportError::kNotConfigured);
  }
  const uint64_t total64 = static_cast<uint64_t>(pkt.header_len) + static_cast<uint64_t>(pkt.prefix_len) +
                           static_cast<uint64_t>(pkt.payload_len);
  if (0U == total64 || total64 > kMaxInterleavedFrameBytes) {
    return osp::expected<SendStatus, TransportError>::error(TransportError::kTooLarge);
  }
  const uint32_t total = static_cast<uint32_t>(total64);
  if ((0U != pkt.header_len && nullptr == pkt.header) || (0U != pkt.prefix_len && nullptr == pkt.prefix) ||
      (0U != pkt.payload_len && nullptr == pkt.payload)) {
    return osp::expected<SendStatus, TransportError>::error(TransportError::kFatal);
  }
  // header | FU prefix | shared payload; empty segments are skipped so a
  // single-NAL packet still costs one iovec.
  struct iovec iov[3] = {};
  uint32_t iov_count = 0U;
  if (0U != pkt.header_len) {
    iov[iov_count].iov_base = const_cast<uint8_t*>(pkt.header);
    iov[iov_count].iov_len = pkt.header_len;
    ++iov_count;
  }
  if (0U != pkt.prefix_len) {
    iov[iov_count].iov_base = const_cast<uint8_t*>(pkt.prefix);
    iov[iov_count].iov_len = pkt.prefix_len;
    ++iov_count;
  }
  if (0U != pkt.payload_len) {
    iov[iov_count].iov_base = const_cast<uint8_t*>(pkt.payload);
    iov[iov_count].iov_len = pkt.payload_len;
    ++iov_count;
  }
  switch (SendIovecs(rtp_.Fd(), iov, iov_count, total)) {
    case IoResult::kSent:
      return osp::expected<SendStatus, TransportError>::success(SendStatus::kSent);
    case IoResult::kWouldBlock:
      return osp::expected<SendStatus, TransportError>::success(SendStatus::kWouldBlock);
    default:
      return osp::expected<SendStatus, TransportError>::error(TransportError::kFatal);
  }
}

osp::expected<SendStatus, TransportError> UdpRtpTransport::SendRtcp(const uint8_t* data, uint32_t len) noexcept {
  if (!IsOpen()) {
    return osp::expected<SendStatus, TransportError>::error(TransportError::kNotConfigured);
  }
  if (nullptr == data || 0U == len || len > kMaxInterleavedFrameBytes) {
    return osp::expected<SendStatus, TransportError>::error(TransportError::kTooLarge);
  }
  struct iovec iov[1] = {};
  iov[0].iov_base = const_cast<uint8_t*>(data);
  iov[0].iov_len = len;
  switch (SendIovecs(rtcp_.Fd(), iov, 1U, len)) {
    case IoResult::kSent:
      return osp::expected<SendStatus, TransportError>::success(SendStatus::kSent);
    case IoResult::kWouldBlock:
      return osp::expected<SendStatus, TransportError>::success(SendStatus::kWouldBlock);
    default:
      return osp::expected<SendStatus, TransportError>::error(TransportError::kFatal);
  }
}

osp::expected<uint32_t, TransportError> UdpRtpTransport::RecvRtcp(uint8_t* data, uint32_t cap) noexcept {
  if (rtcp_.Fd() < 0 || data == nullptr || cap == 0U) {
    return osp::expected<uint32_t, TransportError>::error(TransportError::kNotConfigured);
  }
  for (;;) {
    const ssize_t n = ::recv(rtcp_.Fd(), data, cap, 0);
    if (n > 0) {
      return osp::expected<uint32_t, TransportError>::success(static_cast<uint32_t>(n));
    }
    if (n == 0) {
      return osp::expected<uint32_t, TransportError>::success(0U);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return osp::expected<uint32_t, TransportError>::error(TransportError::kWouldBlock);
    }
    return osp::expected<uint32_t, TransportError>::error(TransportError::kFatal);
  }
}

osp::expected<uint32_t, TransportError> WriteInterleavedRaw(osp::writable_byte_span out, uint8_t channel,
                                                           const uint8_t* data, uint32_t len) noexcept {
  if (channel > kMaxInterleavedChannel || nullptr == data) {
    return osp::expected<uint32_t, TransportError>::error(TransportError::kFatal);
  }
  if (0U == len || len > kMaxInterleavedFrameBytes ||
      out.size() < static_cast<size_t>(kInterleavedHeaderBytes) + len) {
    return osp::expected<uint32_t, TransportError>::error(TransportError::kTooLarge);
  }
  PutHeader(out, channel, len);
  std::memcpy(out.data() + kInterleavedHeaderBytes, data, len);
  return osp::expected<uint32_t, TransportError>::success(kInterleavedHeaderBytes + len);
}

osp::expected<uint32_t, TransportError> WriteInterleaved(osp::writable_byte_span out, uint8_t channel,
                                                         const RtpPacket& pkt) noexcept {
  if (channel > kMaxInterleavedChannel) {
    return osp::expected<uint32_t, TransportError>::error(TransportError::kFatal);
  }
  const uint64_t total64 = static_cast<uint64_t>(pkt.header_len) + static_cast<uint64_t>(pkt.prefix_len) +
                           static_cast<uint64_t>(pkt.payload_len);
  if (0U == total64 || total64 > kMaxInterleavedFrameBytes ||
      out.size() < static_cast<size_t>(kInterleavedHeaderBytes) + static_cast<size_t>(total64)) {
    return osp::expected<uint32_t, TransportError>::error(TransportError::kTooLarge);
  }
  const uint32_t total = static_cast<uint32_t>(total64);
  if ((0U != pkt.header_len && nullptr == pkt.header) || (0U != pkt.prefix_len && nullptr == pkt.prefix) ||
      (0U != pkt.payload_len && nullptr == pkt.payload)) {
    return osp::expected<uint32_t, TransportError>::error(TransportError::kFatal);
  }
  PutHeader(out, channel, total);
  uint32_t offset = kInterleavedHeaderBytes;
  if (0U != pkt.header_len) {
    std::memcpy(out.data() + offset, pkt.header, pkt.header_len);
    offset += pkt.header_len;
  }
  if (0U != pkt.prefix_len) {
    std::memcpy(out.data() + offset, pkt.prefix, pkt.prefix_len);
    offset += pkt.prefix_len;
  }
  if (0U != pkt.payload_len) {
    std::memcpy(out.data() + offset, pkt.payload, pkt.payload_len);
  }
  return osp::expected<uint32_t, TransportError>::success(kInterleavedHeaderBytes + total);
}

void TokenBucket::Configure(uint32_t rate_bps, uint32_t burst_bytes, uint64_t now_ns) noexcept {
  rate_bps_ = rate_bps;
  burst_bytes_ = burst_bytes;
  tokens_ = (0U != rate_bps) ? burst_bytes : 0U;
  last_ns_ = now_ns;
}

uint32_t TokenBucket::Available(uint64_t now_ns) noexcept {
  if (0U == rate_bps_) {
    return 0U;
  }
  if (now_ns > last_ns_) {
    const uint64_t delta_ns = now_ns - last_ns_;
    const uint64_t gap = (burst_bytes_ > tokens_) ? (burst_bytes_ - tokens_) : 0U;
    if (0U == gap) {
      last_ns_ = now_ns;
    } else {
      const uint64_t add = MulSaturate(delta_ns, rate_bps_) / kNsPerByteAtOneBps;
      tokens_ = (add >= gap) ? burst_bytes_ : (tokens_ + static_cast<uint32_t>(add));
      last_ns_ = now_ns;
    }
  }
  return tokens_;
}

bool TokenBucket::TryConsume(uint32_t bytes, uint64_t now_ns) noexcept {
  const uint32_t available = Available(now_ns);
  bool ok = (0U != rate_bps_) && (bytes <= available);
  if (ok) {
    tokens_ = available - bytes;
  }
  return ok;
}

}  // namespace rs
