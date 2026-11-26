// Unit tests for interleaved framing, UDP loopback send and the token bucket.
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/rtp_transport.hpp"
#include "rs/tx_stream.hpp"

#include <cstdint>
#include <cstring>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace {

using rs::RtpPacket;
using rs::TokenBucket;
using rs::TransportError;
using rs::UdpRtpTransport;
using rs::WriteInterleaved;
using rs::WriteInterleavedRaw;

const uint8_t* AsBytes(const std::byte* data) noexcept {
  return reinterpret_cast<const uint8_t*>(data);
}

int TestInterleavedLayout() {
  uint8_t header[12];
  for (uint32_t i = 0U; i < sizeof(header); ++i) {
    header[i] = static_cast<uint8_t>(0x80U + i);
  }
  const uint8_t payload[5] = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U};
  const RtpPacket pkt{header, sizeof(header), nullptr, 0U, payload, sizeof(payload), true};

  std::byte out[64] = {};
  auto result = WriteInterleaved(osp::writable_byte_span{out, sizeof(out)}, 0U, pkt);
  RS_CHECK(result.has_value());
  RS_CHECK_EQ(result.value(), 4U + 12U + 5U);
  const uint8_t* bytes = AsBytes(out);
  RS_CHECK_EQ(bytes[0], static_cast<uint8_t>('$'));
  RS_CHECK_EQ(bytes[1], 0U);
  RS_CHECK_EQ(bytes[2], 0x00U);
  RS_CHECK_EQ(bytes[3], 17U);
  RS_CHECK_EQ(std::memcmp(bytes + 4, header, sizeof(header)), 0);
  RS_CHECK_EQ(std::memcmp(bytes + 16, payload, sizeof(payload)), 0);

  // FU packet: the 2-byte prefix sits between the RTP header and the fragment
  // bytes, and the 16-bit length covers all three segments.
  const uint8_t prefix[2] = {0x7CU, 0x85U};
  const RtpPacket fu_pkt{header, sizeof(header), prefix, sizeof(prefix), payload, sizeof(payload), true};
  std::byte fu_out[64] = {};
  auto fu = WriteInterleaved(osp::writable_byte_span{fu_out, sizeof(fu_out)}, 0U, fu_pkt);
  RS_CHECK(fu.has_value());
  RS_CHECK_EQ(fu.value(), 4U + 12U + 2U + 5U);
  const uint8_t* fu_bytes = AsBytes(fu_out);
  RS_CHECK_EQ(fu_bytes[2], 0x00U);
  RS_CHECK_EQ(fu_bytes[3], 19U);
  RS_CHECK_EQ(std::memcmp(fu_bytes + 4, header, sizeof(header)), 0);
  RS_CHECK_EQ(std::memcmp(fu_bytes + 16, prefix, sizeof(prefix)), 0);
  RS_CHECK_EQ(std::memcmp(fu_bytes + 18, payload, sizeof(payload)), 0);

  // Empty segments are skipped but still allowed: header + prefix, no payload.
  const RtpPacket prefix_only{header, sizeof(header), prefix, sizeof(prefix), nullptr, 0U, false};
  std::byte prefix_out[64] = {};
  auto prefix_only_written = WriteInterleaved(osp::writable_byte_span{prefix_out, sizeof(prefix_out)}, 0U, prefix_only);
  RS_CHECK(prefix_only_written.has_value());
  RS_CHECK_EQ(prefix_only_written.value(), 4U + 12U + 2U);
  RS_CHECK_EQ(AsBytes(prefix_out)[3], 14U);
  RS_CHECK_EQ(std::memcmp(AsBytes(prefix_out) + 16, prefix, sizeof(prefix)), 0);

  // Channel 1 uses the RTCP slot; the length field stays big endian.
  std::byte out_ch1[64] = {};
  auto ch1 = WriteInterleaved(osp::writable_byte_span{out_ch1, sizeof(out_ch1)}, 1U, pkt);
  RS_CHECK(ch1.has_value());
  RS_CHECK_EQ(AsBytes(out_ch1)[1], 1U);

  // Raw form has identical framing.
  std::byte raw_out[32] = {};
  auto raw = WriteInterleavedRaw(osp::writable_byte_span{raw_out, sizeof(raw_out)}, 1U, payload, sizeof(payload));
  RS_CHECK(raw.has_value());
  RS_CHECK_EQ(raw.value(), 9U);
  RS_CHECK_EQ(AsBytes(raw_out)[0], static_cast<uint8_t>('$'));
  RS_CHECK_EQ(AsBytes(raw_out)[1], 1U);
  RS_CHECK_EQ(AsBytes(raw_out)[2], 0x00U);
  RS_CHECK_EQ(AsBytes(raw_out)[3], 5U);
  RS_CHECK_EQ(std::memcmp(AsBytes(raw_out) + 4, payload, sizeof(payload)), 0);
  return 0;
}

int TestInterleavedBoundaries() {
  uint8_t header[12] = {};
  std::vector<uint8_t> payload(65536U, 0xA5U);
  std::vector<std::byte> out(4U + 65536U);

  // Exact 65535-byte payload is the largest legal interleaved packet.
  RtpPacket max_pkt{header, sizeof(header), nullptr, 0U, payload.data(), 65535U - sizeof(header), true};
  auto max_ok = WriteInterleaved(osp::writable_byte_span{out.data(), out.size()}, 0U, max_pkt);
  RS_CHECK(max_ok.has_value());
  RS_CHECK_EQ(max_ok.value(), 4U + 65535U);
  RS_CHECK_EQ(AsBytes(out.data())[2], 0xFFU);
  RS_CHECK_EQ(AsBytes(out.data())[3], 0xFFU);

  // One byte more must be rejected before touching the buffer.
  RtpPacket over_pkt{header, sizeof(header), nullptr, 0U, payload.data(), 65536U - sizeof(header), true};
  auto over = WriteInterleaved(osp::writable_byte_span{out.data(), out.size()}, 0U, over_pkt);
  RS_CHECK(over.has_value() == false);
  RS_CHECK(over.get_error() == TransportError::kTooLarge);

  // Insufficient output space.
  std::byte small[16] = {};
  RtpPacket small_pkt{header, sizeof(header), nullptr, 0U, payload.data(), 32U, true};
  auto no_space = WriteInterleaved(osp::writable_byte_span{small, sizeof(small)}, 0U, small_pkt);
  RS_CHECK(no_space.has_value() == false);
  RS_CHECK(no_space.get_error() == TransportError::kTooLarge);

  // Interleaved channels are 8-bit: 2-3 is valid, 128+ is not (review P0-3).
  auto channel_two = WriteInterleaved(osp::writable_byte_span{out.data(), out.size()}, 2U, max_pkt);
  RS_CHECK(channel_two.has_value());
  RS_CHECK_EQ(static_cast<uint8_t>(out[1]), static_cast<uint8_t>(2U));
  auto bad_channel = WriteInterleaved(osp::writable_byte_span{out.data(), out.size()}, 128U, max_pkt);
  RS_CHECK(bad_channel.has_value() == false);
  RtpPacket empty_pkt{header, 0U, nullptr, 0U, nullptr, 0U, false};
  auto empty = WriteInterleaved(osp::writable_byte_span{out.data(), out.size()}, 0U, empty_pkt);
  RS_CHECK(empty.has_value() == false);
  RS_CHECK(empty.get_error() == TransportError::kTooLarge);

  // A prefix that pushes the frame one byte past 65535 must be rejected.
  const uint8_t prefix[2] = {0x7CU, 0x85U};
  RtpPacket fu_over{header, sizeof(header), prefix, sizeof(prefix), payload.data(), 65534U - sizeof(header), true};
  auto fu_over_result = WriteInterleaved(osp::writable_byte_span{out.data(), out.size()}, 0U, fu_over);
  RS_CHECK(fu_over_result.has_value() == false);
  RS_CHECK(fu_over_result.get_error() == TransportError::kTooLarge);

  // A wrapped 32-bit sum must never pass the capacity check.
  const RtpPacket wrapped{header, UINT32_MAX, prefix, 2U, payload.data(), 1U, false};
  auto wrapped_result = WriteInterleaved(osp::writable_byte_span{small, sizeof(small)}, 0U, wrapped);
  RS_CHECK(wrapped_result.has_value() == false);
  RS_CHECK(wrapped_result.get_error() == TransportError::kTooLarge);

  // Raw form boundaries.
  auto raw_max = WriteInterleavedRaw(osp::writable_byte_span{out.data(), out.size()}, 0U, payload.data(), 65535U);
  RS_CHECK(raw_max.has_value());
  RS_CHECK_EQ(raw_max.value(), 65539U);
  auto raw_over = WriteInterleavedRaw(osp::writable_byte_span{out.data(), out.size()}, 0U, payload.data(), 65536U);
  RS_CHECK(raw_over.has_value() == false);
  RS_CHECK(raw_over.get_error() == TransportError::kTooLarge);
  auto raw_zero = WriteInterleavedRaw(osp::writable_byte_span{out.data(), out.size()}, 0U, payload.data(), 0U);
  RS_CHECK(raw_zero.has_value() == false);
  auto raw_channel = WriteInterleavedRaw(osp::writable_byte_span{out.data(), out.size()}, 7U, payload.data(), 8U);
  RS_CHECK(raw_channel.has_value());
  auto raw_bad_channel = WriteInterleavedRaw(osp::writable_byte_span{out.data(), out.size()}, 200U, payload.data(), 8U);
  RS_CHECK(raw_bad_channel.has_value() == false);
  return 0;
}

int TestTxCommitBounds() {
  rs::TxStream tx;
  RS_CHECK(tx.Commit(rs::TxStream::kCapacity));
  RS_CHECK_EQ(tx.Unflushed(), rs::TxStream::kCapacity);
  RS_CHECK(tx.Commit(1U) == false);
  RS_CHECK_EQ(tx.Unflushed(), rs::TxStream::kCapacity);
  tx.Consume(rs::TxStream::kCapacity, 1U);
  RS_CHECK(tx.Empty());
  RS_CHECK(tx.Commit(UINT32_MAX) == false);
  RS_CHECK(tx.Empty());
  return 0;
}

int TestTokenBucket() {
  TokenBucket bucket;
  RS_CHECK(bucket.IsConfigured() == false);
  RS_CHECK_EQ(bucket.Available(0U), 0U);
  RS_CHECK(bucket.TryConsume(1U, 0U) == false);

  // 8 Mbit/s = 1 MB/s, burst 4096 bytes, starts full.
  bucket.Configure(8000000U, 4096U, 1000U);
  RS_CHECK(bucket.IsConfigured());
  RS_CHECK_EQ(bucket.Available(1000U), 4096U);
  RS_CHECK(bucket.TryConsume(4096U, 1000U));
  RS_CHECK_EQ(bucket.Available(1000U), 0U);
  RS_CHECK(bucket.TryConsume(1U, 1000U) == false);
  RS_CHECK_EQ(bucket.Available(1000U), 0U);

  // 1 ms of credit is 1000 bytes.
  const uint32_t after_1ms = bucket.Available(1001000U);
  RS_CHECK(after_1ms >= 999U);
  RS_CHECK(after_1ms <= 1000U);
  RS_CHECK(bucket.TryConsume(after_1ms + 1U, 1001000U) == false);
  RS_CHECK_EQ(bucket.Available(1001000U), after_1ms);

  // Long idle time saturates at burst instead of overflowing.
  bucket.Configure(8000U, 2000U, 0U);
  RS_CHECK_EQ(bucket.Available(10000000000ULL), 2000U);
  RS_CHECK(bucket.TryConsume(2000U, 10000000000ULL));
  RS_CHECK_EQ(bucket.Available(10000000000ULL), 0U);

  // Backwards time must not mint tokens.
  bucket.Configure(8000000U, 4096U, 5000U);
  RS_CHECK_EQ(bucket.Available(4000U), 4096U);

  // Whole-second credit at 12 Mbit/s is capped by the 64 KiB burst.
  bucket.Configure(12000000U, 65536U, 0U);
  RS_CHECK_EQ(bucket.Available(1000000000ULL), 65536U);
  return 0;
}

int TestTokenBucketNoOverIssue() {
  TokenBucket bucket;
  bucket.Configure(8000000U, 1000U, 0U);
  RS_CHECK(bucket.TryConsume(600U, 0U));
  uint32_t consumed = 600U;
  while (bucket.TryConsume(1U, 0U)) {
    ++consumed;
  }
  RS_CHECK_EQ(consumed, 1000U);
  return 0;
}

int TestUdpLoopback() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  RS_CHECK(fd >= 0);
  struct sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  local.sin_port = 0;
  RS_CHECK_EQ(::bind(fd, reinterpret_cast<const struct sockaddr*>(&local), sizeof(local)), 0);
  socklen_t local_len = static_cast<socklen_t>(sizeof(local));
  RS_CHECK_EQ(::getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &local_len), 0);
  struct timeval timeout{};
  timeout.tv_sec = 2;
  timeout.tv_usec = 0;
  RS_CHECK_EQ(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
  const uint16_t rx_port = ntohs(local.sin_port);

  UdpRtpTransport transport;
  RS_CHECK(transport.IsOpen() == false);
  RtpPacket unopened{};
  auto not_configured = transport.SendRtp(unopened);
  RS_CHECK(not_configured.has_value() == false);
  RS_CHECK(not_configured.get_error() == TransportError::kNotConfigured);

  uint16_t local_rtp = 0U;
  uint16_t local_rtcp = 0U;
  // 127.0.0.1 in the project's host byte order convention (octet 0 low).
  RS_CHECK(transport.Open(0x0100007FU, rx_port, rx_port, local_rtp, local_rtcp));
  RS_CHECK(transport.IsOpen());
  const uint8_t overflow_byte = 0U;
  const RtpPacket wrapped{&overflow_byte, UINT32_MAX, &overflow_byte, 2U, &overflow_byte, 1U, false};
  const auto wrapped_result = transport.SendRtp(wrapped);
  RS_CHECK(wrapped_result.has_value() == false);
  RS_CHECK(wrapped_result.get_error() == TransportError::kTooLarge);
  // RFC 3550 pair: RTP even, RTCP = RTP + 1 (review P0-2).
  RS_CHECK_EQ(local_rtcp, static_cast<uint16_t>(local_rtp + 1U));
  RS_CHECK_EQ(static_cast<uint16_t>(local_rtp & 1U), static_cast<uint16_t>(0U));
  RS_CHECK(local_rtp != 0U);
  RS_CHECK(local_rtcp != 0U);
  RS_CHECK(local_rtp != local_rtcp);

  uint8_t header[12];
  for (uint32_t i = 0U; i < sizeof(header); ++i) {
    header[i] = static_cast<uint8_t>(0x80U + i);
  }
  const uint8_t payload[16] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U};
  const RtpPacket pkt{header, sizeof(header), nullptr, 0U, payload, sizeof(payload), true};
  auto sent = transport.SendRtp(pkt);
  RS_CHECK(sent.has_value());
  RS_CHECK(sent.value() == rs::SendStatus::kSent);

  uint8_t rx_buf[2048] = {};
  const ssize_t rx_len = ::recv(fd, rx_buf, sizeof(rx_buf), 0);
  RS_CHECK_EQ(rx_len, static_cast<ssize_t>(sizeof(header) + sizeof(payload)));
  RS_CHECK_EQ(std::memcmp(rx_buf, header, sizeof(header)), 0);
  RS_CHECK_EQ(std::memcmp(rx_buf + sizeof(header), payload, sizeof(payload)), 0);

  // FU packet: three iovecs must arrive as one datagram in wire order.
  const uint8_t prefix[2] = {0x7CU, 0x85U};
  const RtpPacket fu_pkt{header, sizeof(header), prefix, sizeof(prefix), payload, sizeof(payload), true};
  auto fu_sent = transport.SendRtp(fu_pkt);
  RS_CHECK(fu_sent.has_value());
  RS_CHECK(fu_sent.value() == rs::SendStatus::kSent);
  const ssize_t fu_len = ::recv(fd, rx_buf, sizeof(rx_buf), 0);
  RS_CHECK_EQ(fu_len, static_cast<ssize_t>(sizeof(header) + sizeof(prefix) + sizeof(payload)));
  RS_CHECK_EQ(std::memcmp(rx_buf, header, sizeof(header)), 0);
  RS_CHECK_EQ(std::memcmp(rx_buf + sizeof(header), prefix, sizeof(prefix)), 0);
  RS_CHECK_EQ(std::memcmp(rx_buf + sizeof(header) + sizeof(prefix), payload, sizeof(payload)), 0);

  // Empty payload segment: header + prefix only.
  const RtpPacket prefix_only{header, sizeof(header), prefix, sizeof(prefix), nullptr, 0U, false};
  auto prefix_only_sent = transport.SendRtp(prefix_only);
  RS_CHECK(prefix_only_sent.has_value());
  RS_CHECK(prefix_only_sent.value() == rs::SendStatus::kSent);
  const ssize_t prefix_len = ::recv(fd, rx_buf, sizeof(rx_buf), 0);
  RS_CHECK_EQ(prefix_len, static_cast<ssize_t>(sizeof(header) + sizeof(prefix)));
  RS_CHECK_EQ(std::memcmp(rx_buf, header, sizeof(header)), 0);
  RS_CHECK_EQ(std::memcmp(rx_buf + sizeof(header), prefix, sizeof(prefix)), 0);

  const uint8_t rtcp[8] = {0x80U, 0xC8U, 0x00U, 0x06U, 0U, 0U, 0U, 0U};
  auto rtcp_sent = transport.SendRtcp(rtcp, sizeof(rtcp));
  RS_CHECK(rtcp_sent.has_value());
  RS_CHECK(rtcp_sent.value() == rs::SendStatus::kSent);
  const ssize_t rtcp_len = ::recv(fd, rx_buf, sizeof(rx_buf), 0);
  RS_CHECK_EQ(rtcp_len, static_cast<ssize_t>(sizeof(rtcp)));
  RS_CHECK_EQ(std::memcmp(rx_buf, rtcp, sizeof(rtcp)), 0);

  auto too_big = transport.SendRtcp(rtcp, 65536U);
  RS_CHECK(too_big.has_value() == false);
  RS_CHECK(too_big.get_error() == TransportError::kTooLarge);

  transport.Close();
  RS_CHECK(transport.IsOpen() == false);
  ::close(fd);
  return 0;
}

}  // namespace

int main() {
  if (TestInterleavedLayout() != 0) {
    return 1;
  }
  if (TestInterleavedBoundaries() != 0) {
    return 1;
  }
  if (TestTxCommitBounds() != 0) {
    return 1;
  }
  if (TestTokenBucket() != 0) {
    return 1;
  }
  if (TestTokenBucketNoOverIssue() != 0) {
    return 1;
  }
  if (TestUdpLoopback() != 0) {
    return 1;
  }
  std::printf("test_rtp_transport: all checks passed\n");
  return 0;
}
