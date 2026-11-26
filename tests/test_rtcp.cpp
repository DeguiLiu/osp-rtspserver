// Unit tests for RTCP sender reports and receiver report parsing.
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/rtcp.hpp"

#include <cstring>

namespace {

osp::writable_byte_span Span(uint8_t* buf, uint32_t cap) {
  return osp::writable_byte_span(reinterpret_cast<std::byte*>(buf), cap);
}

void Put16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value >> 8U);
  p[1] = static_cast<uint8_t>(value);
}

void Put32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value >> 24U);
  p[1] = static_cast<uint8_t>(value >> 16U);
  p[2] = static_cast<uint8_t>(value >> 8U);
  p[3] = static_cast<uint8_t>(value);
}

uint32_t Read32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) |
         (static_cast<uint32_t>(p[2]) << 8U) | static_cast<uint32_t>(p[3]);
}

// Builds an RR with `rc` report blocks; the first block carries the values
// the parser must return.
uint32_t MakeRr(uint8_t* p, uint8_t rc, uint32_t block_ssrc, uint8_t fraction, uint32_t lost24, uint32_t ext_seq,
                uint32_t jitter, uint32_t last_sr, uint32_t delay) {
  const uint32_t bytes = 8U + static_cast<uint32_t>(rc) * 24U;
  p[0] = static_cast<uint8_t>(0x80U | rc);
  p[1] = 201U;
  Put16(p + 2U, static_cast<uint16_t>((bytes / 4U) - 1U));
  Put32(p + 4U, 0xAABBCCDDU);
  Put32(p + 8U, block_ssrc);
  p[12] = fraction;
  p[13] = static_cast<uint8_t>(lost24 >> 16U);
  p[14] = static_cast<uint8_t>(lost24 >> 8U);
  p[15] = static_cast<uint8_t>(lost24);
  Put32(p + 16U, ext_seq);
  Put32(p + 20U, jitter);
  Put32(p + 24U, last_sr);
  Put32(p + 28U, delay);
  return bytes;
}

uint32_t MakeSr(uint8_t* p, uint8_t rc, uint32_t ssrc) {
  const uint32_t bytes = 28U + static_cast<uint32_t>(rc) * 24U;
  p[0] = static_cast<uint8_t>(0x80U | rc);
  p[1] = 200U;
  Put16(p + 2U, static_cast<uint16_t>((bytes / 4U) - 1U));
  Put32(p + 4U, ssrc);
  Put32(p + 8U, 0x83AA7E80U);
  Put32(p + 12U, 0U);
  Put32(p + 16U, 1000U);
  Put32(p + 20U, 10U);
  Put32(p + 24U, 100U);
  return bytes;
}

// BYE with `rc` SSRCs; only the header needs to be well formed for counting.
uint32_t MakeBye(uint8_t* p, uint8_t rc) {
  const uint32_t bytes = 4U + static_cast<uint32_t>(rc) * 4U;
  p[0] = static_cast<uint8_t>(0x80U | rc);
  p[1] = 203U;
  Put16(p + 2U, static_cast<uint16_t>((bytes / 4U) - 1U));
  for (uint32_t i = 0U; i < rc; ++i) {
    Put32(p + 4U + i * 4U, 0x1000U + i);
  }
  return bytes;
}

// An XR (PT=207) packet the compound parser must skip without failing.
uint32_t MakeXr(uint8_t* p) {
  p[0] = 0x80U;
  p[1] = 207U;
  Put16(p + 2U, 1U);
  Put32(p + 4U, 0U);
  return 8U;
}

}  // namespace

int main() {
  // 28-byte SR layout, including NTP conversion.
  {
    uint8_t sr[64] = {};
    const auto r = rs::BuildSenderReport(Span(sr, sizeof(sr)), 0x11223344U, 1500000000ULL, 90000U, 100U, 5000U);
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value(), 28U);
    RS_CHECK_EQ(sr[0], static_cast<uint8_t>(0x80U));
    RS_CHECK_EQ(sr[1], static_cast<uint8_t>(200U));
    RS_CHECK_EQ(sr[2], static_cast<uint8_t>(0U));
    RS_CHECK_EQ(sr[3], static_cast<uint8_t>(6U));
    RS_CHECK_EQ(Read32(sr + 4U), 0x11223344U);
    RS_CHECK_EQ(Read32(sr + 8U), 0x83AA7E81U);   // 1 s after the NTP epoch offset.
    RS_CHECK_EQ(Read32(sr + 12U), 0x80000000U);  // 0.5 s fraction.
    RS_CHECK_EQ(Read32(sr + 16U), 90000U);
    RS_CHECK_EQ(Read32(sr + 20U), 100U);
    RS_CHECK_EQ(Read32(sr + 24U), 5000U);
  }

  // Zero timestamp maps to the exact NTP epoch offset.
  {
    uint8_t sr[28] = {};
    const auto r = rs::BuildSenderReport(Span(sr, sizeof(sr)), 7U, 0ULL, 0U, 0U, 0U);
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(Read32(sr + 8U), 0x83AA7E80U);
    RS_CHECK_EQ(Read32(sr + 12U), 0U);
  }

  // Buffer bounds.
  {
    uint8_t sr[28] = {};
    auto r = rs::BuildSenderReport(Span(sr, 27U), 1U, 0ULL, 0U, 0U, 0U);
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kBufferTooSmall);
    r = rs::BuildSenderReport(osp::writable_byte_span(), 1U, 0ULL, 0U, 0U, 0U);
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kBufferTooSmall);
  }

  // Standalone RR: every field of the first report block.
  {
    uint8_t rr[32] = {};
    MakeRr(rr, 1U, 0xDEADBEEFU, 25U, 0x000002U, 0x00000123U, 300U, 0x00004567U, 0x00000010U);
    const auto r = rs::ParseReceiverReport(rr, sizeof(rr));
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value().ssrc, 0xDEADBEEFU);
    RS_CHECK_EQ(r.value().fraction_lost, static_cast<uint8_t>(25U));
    RS_CHECK_EQ(r.value().cumulative_lost, 2);
    RS_CHECK_EQ(r.value().extended_highest_seq, 0x00000123U);
    RS_CHECK_EQ(r.value().jitter, 300U);
    RS_CHECK_EQ(r.value().last_sr, 0x00004567U);
    RS_CHECK_EQ(r.value().delay_since_sr, 0x00000010U);
  }

  // Negative cumulative loss is sign-extended from 24 bits.
  {
    uint8_t rr[32] = {};
    MakeRr(rr, 1U, 1U, 0U, 0xFFFFFEU, 0U, 0U, 0U, 0U);
    const auto r = rs::ParseReceiverReport(rr, sizeof(rr));
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value().cumulative_lost, -2);
  }

  // Compound SR (RC=0) + RR: the SR is skipped.
  {
    uint8_t compound[128] = {};
    const uint32_t sr_bytes =
        rs::BuildSenderReport(Span(compound, sizeof(compound)), 0x01020304U, 1000ULL, 50U, 1U, 10U).value();
    const uint32_t rr_bytes = MakeRr(compound + sr_bytes, 1U, 0xCAFEBABEU, 3U, 4U, 5U, 6U, 7U, 8U);
    const auto r = rs::ParseReceiverReport(compound, sr_bytes + rr_bytes);
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value().ssrc, 0xCAFEBABEU);
    RS_CHECK_EQ(r.value().jitter, 6U);
    RS_CHECK_EQ(r.value().delay_since_sr, 8U);
  }

  // Compound SR with one report block still skips the whole SR packet.
  {
    uint8_t compound[128] = {};
    const uint32_t sr_bytes = MakeSr(compound, 1U, 0x11111111U);
    RS_CHECK_EQ(sr_bytes, 52U);
    const uint32_t rr_bytes = MakeRr(compound + sr_bytes, 2U, 0x22222222U, 9U, 10U, 11U, 12U, 13U, 14U);
    const auto r = rs::ParseReceiverReport(compound, sr_bytes + rr_bytes);
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value().ssrc, 0x22222222U);
    RS_CHECK_EQ(r.value().fraction_lost, static_cast<uint8_t>(9U));
    RS_CHECK_EQ(r.value().cumulative_lost, 10);
  }

  // Bad packets.
  {
    uint8_t rr[32] = {};
    MakeRr(rr, 1U, 1U, 0U, 0U, 0U, 0U, 0U, 0U);
    // Wrong version.
    rr[0] = static_cast<uint8_t>(0x41U);
    auto r = rs::ParseReceiverReport(rr, sizeof(rr));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
    // Truncated by one byte.
    MakeRr(rr, 1U, 1U, 0U, 0U, 0U, 0U, 0U, 0U);
    r = rs::ParseReceiverReport(rr, sizeof(rr) - 1U);
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
    // RR with no report block.
    MakeRr(rr, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U);
    r = rs::ParseReceiverReport(rr, 8U);
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
    // RR declares two blocks but the packet only holds one.
    MakeRr(rr, 1U, 1U, 0U, 0U, 0U, 0U, 0U, 0U);
    rr[0] = static_cast<uint8_t>(0x82U);
    r = rs::ParseReceiverReport(rr, sizeof(rr));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
    // First packet is not SR/RR.
    uint8_t sdes[8] = {0x81U, 202U, 0x00U, 0x01U, 0U, 0U, 0U, 0U};
    r = rs::ParseReceiverReport(sdes, sizeof(sdes));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
    // SR without a following RR in the compound.
    uint8_t only_sr[28] = {};
    MakeSr(only_sr, 0U, 1U);
    r = rs::ParseReceiverReport(only_sr, sizeof(only_sr));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
    // SR length field exceeds the available bytes.
    only_sr[2] = 0U;
    only_sr[3] = 12U;
    r = rs::ParseReceiverReport(only_sr, sizeof(only_sr));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
    // Empty input.
    r = rs::ParseReceiverReport(nullptr, 0U);
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
  }

  // SR + SDES(CNAME): exact compound bytes, "abc" padded to 32-bit alignment.
  {
    uint8_t buf[128] = {};
    const auto r = rs::BuildSenderReportWithSdes(Span(buf, sizeof(buf)), 0x11223344U, "abc", 0ULL, 90000U, 100U, 5000U);
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value(), 44U);
    // SR header: V=2, PT=200, length=6, SSRC, then sender info.
    RS_CHECK_EQ(buf[0], static_cast<uint8_t>(0x80U));
    RS_CHECK_EQ(buf[1], static_cast<uint8_t>(200U));
    RS_CHECK_EQ(buf[2], static_cast<uint8_t>(0U));
    RS_CHECK_EQ(buf[3], static_cast<uint8_t>(6U));
    RS_CHECK_EQ(Read32(buf + 4U), 0x11223344U);
    RS_CHECK_EQ(Read32(buf + 8U), 0x83AA7E80U);
    RS_CHECK_EQ(Read32(buf + 12U), 0U);
    RS_CHECK_EQ(Read32(buf + 16U), 90000U);
    RS_CHECK_EQ(Read32(buf + 20U), 100U);
    RS_CHECK_EQ(Read32(buf + 24U), 5000U);
    // SDES header: V=2, SC=1, PT=202, length=3 (16 bytes), chunk SSRC.
    RS_CHECK_EQ(buf[28], static_cast<uint8_t>(0x81U));
    RS_CHECK_EQ(buf[29], static_cast<uint8_t>(202U));
    RS_CHECK_EQ(buf[30], static_cast<uint8_t>(0U));
    RS_CHECK_EQ(buf[31], static_cast<uint8_t>(3U));
    RS_CHECK_EQ(Read32(buf + 32U), 0x11223344U);
    // CNAME item: type=1, length=3, "abc", null terminator, zero padding.
    RS_CHECK_EQ(buf[36], static_cast<uint8_t>(1U));
    RS_CHECK_EQ(buf[37], static_cast<uint8_t>(3U));
    RS_CHECK_EQ(buf[38], static_cast<uint8_t>('a'));
    RS_CHECK_EQ(buf[39], static_cast<uint8_t>('b'));
    RS_CHECK_EQ(buf[40], static_cast<uint8_t>('c'));
    RS_CHECK_EQ(buf[41], static_cast<uint8_t>(0U));
    RS_CHECK_EQ(buf[42], static_cast<uint8_t>(0U));
    RS_CHECK_EQ(buf[43], static_cast<uint8_t>(0U));
  }

  // CNAME longer than 255 bytes is truncated to the 8-bit length limit.
  {
    char long_name[300] = {};
    for (uint32_t i = 0U; i < 299U; ++i) {
      long_name[i] = 'x';
    }
    uint8_t buf[512] = {};
    const auto r = rs::BuildSenderReportWithSdes(Span(buf, sizeof(buf)), 1U, long_name, 0ULL, 0U, 0U, 0U);
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value(), 296U);  // 28 SR + 4 SDES header + 264 padded chunk/padding.
    RS_CHECK_EQ(buf[37], static_cast<uint8_t>(255U));
    RS_CHECK_EQ(buf[293], static_cast<uint8_t>(0U));  // First terminator/pad octet.
    const auto summary = rs::ParseCompound(buf, r.value());
    RS_CHECK(summary.has_value());
    RS_CHECK_EQ(summary.value().sender_reports, 1U);
  }

  // SR + SDES builder errors.
  {
    uint8_t buf[64] = {};
    auto r = rs::BuildSenderReportWithSdes(Span(buf, 43U), 1U, "abc", 0ULL, 0U, 0U, 0U);
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kBufferTooSmall);
    r = rs::BuildSenderReportWithSdes(osp::writable_byte_span(), 1U, "abc", 0ULL, 0U, 0U, 0U);
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kBufferTooSmall);
    r = rs::BuildSenderReportWithSdes(Span(buf, sizeof(buf)), 1U, nullptr, 0ULL, 0U, 0U, 0U);
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
    r = rs::BuildSenderReportWithSdes(Span(buf, sizeof(buf)), 1U, "", 0ULL, 0U, 0U, 0U);
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::RtcpError::kMalformed);
  }

  // ParseCompound: SR + SDES + RR (negative cumulative loss) + BYE.
  uint8_t mixed[256] = {};
  uint32_t mixed_bytes = 0U;
  {
    mixed_bytes =
        rs::BuildSenderReportWithSdes(Span(mixed, sizeof(mixed)), 0x01020304U, "cname", 0ULL, 900U, 1U, 10U).value();
    mixed_bytes += MakeRr(mixed + mixed_bytes, 1U, 0xDEADBEEFU, 25U, 0xFFFFFEU, 9U, 300U, 0x4567U, 0x10U);
    mixed_bytes += MakeBye(mixed + mixed_bytes, 1U);
    const auto summary = rs::ParseCompound(mixed, mixed_bytes);
    RS_CHECK(summary.has_value());
    RS_CHECK_EQ(summary.value().sender_reports, 1U);
    RS_CHECK_EQ(summary.value().receiver_reports, 1U);
    RS_CHECK_EQ(summary.value().byes, 1U);
    RS_CHECK(summary.value().has_rr);
    RS_CHECK_EQ(summary.value().first_rr.ssrc, 0xDEADBEEFU);
    RS_CHECK_EQ(summary.value().first_rr.fraction_lost, static_cast<uint8_t>(25U));
    RS_CHECK_EQ(summary.value().first_rr.cumulative_lost, -2);
    RS_CHECK_EQ(summary.value().first_rr.extended_highest_seq, 9U);
    RS_CHECK_EQ(summary.value().first_rr.jitter, 300U);
    RS_CHECK_EQ(summary.value().first_rr.last_sr, 0x4567U);
    RS_CHECK_EQ(summary.value().first_rr.delay_since_sr, 0x10U);
  }

  // A standalone RR is a valid one-packet compound.
  {
    uint8_t rr[32] = {};
    const uint32_t bytes = MakeRr(rr, 1U, 0xCAFEBABEU, 1U, 2U, 3U, 4U, 5U, 6U);
    const auto summary = rs::ParseCompound(rr, bytes);
    RS_CHECK(summary.has_value());
    RS_CHECK_EQ(summary.value().sender_reports, 0U);
    RS_CHECK_EQ(summary.value().receiver_reports, 1U);
    RS_CHECK_EQ(summary.value().byes, 0U);
    RS_CHECK_EQ(summary.value().first_rr.ssrc, 0xCAFEBABEU);
  }

  // Unknown-but-valid PT (XR=207) is counted as neither SR/RR/BYE nor an error.
  {
    uint8_t buf[64] = {};
    const uint32_t rr_bytes = MakeRr(buf, 1U, 7U, 0U, 0U, 0U, 0U, 0U, 0U);
    const uint32_t xr_bytes = MakeXr(buf + rr_bytes);
    const auto summary = rs::ParseCompound(buf, rr_bytes + xr_bytes);
    RS_CHECK(summary.has_value());
    RS_CHECK_EQ(summary.value().receiver_reports, 1U);
    RS_CHECK_EQ(summary.value().sender_reports, 0U);
    RS_CHECK_EQ(summary.value().byes, 0U);
  }

  // Round trip: builder output parses back to a summary with one SR and no RR.
  {
    const auto summary = rs::ParseCompound(mixed, 44U);
    RS_CHECK(summary.has_value());
    RS_CHECK_EQ(summary.value().sender_reports, 1U);
    RS_CHECK_EQ(summary.value().receiver_reports, 0U);
    RS_CHECK_EQ(summary.value().byes, 0U);
    RS_CHECK(summary.value().has_rr == false);
  }

  // Malformed compounds: empty, truncated, bad version/PT, length overflow.
  {
    uint8_t buf[256] = {};
    std::memcpy(buf, mixed, mixed_bytes);
    auto s = rs::ParseCompound(nullptr, 0U);
    RS_CHECK(s.has_value() == false);
    RS_CHECK_EQ(s.get_error(), rs::RtcpError::kMalformed);
    s = rs::ParseCompound(buf, 0U);
    RS_CHECK(s.has_value() == false);
    RS_CHECK_EQ(s.get_error(), rs::RtcpError::kMalformed);
    s = rs::ParseCompound(buf, 4U);
    RS_CHECK(s.has_value() == false);
    RS_CHECK_EQ(s.get_error(), rs::RtcpError::kMalformed);
    s = rs::ParseCompound(buf, mixed_bytes / 2U);
    RS_CHECK(s.has_value() == false);
    RS_CHECK_EQ(s.get_error(), rs::RtcpError::kMalformed);
    buf[0] = static_cast<uint8_t>(0x40U);  // Version 1.
    s = rs::ParseCompound(buf, mixed_bytes);
    RS_CHECK(s.has_value() == false);
    RS_CHECK_EQ(s.get_error(), rs::RtcpError::kMalformed);
    std::memcpy(buf, mixed, mixed_bytes);
    buf[1] = 199U;  // Below the RTCP range.
    s = rs::ParseCompound(buf, mixed_bytes);
    RS_CHECK(s.has_value() == false);
    RS_CHECK_EQ(s.get_error(), rs::RtcpError::kMalformed);
    std::memcpy(buf, mixed, mixed_bytes);
    buf[1] = 208U;  // Above the RTCP range.
    s = rs::ParseCompound(buf, mixed_bytes);
    RS_CHECK(s.has_value() == false);
    RS_CHECK_EQ(s.get_error(), rs::RtcpError::kMalformed);
    std::memcpy(buf, mixed, mixed_bytes);
    buf[2] = 0xFFU;  // Length field (262144 bytes) exceeds the datagram.
    buf[3] = 0xFFU;
    s = rs::ParseCompound(buf, mixed_bytes);
    RS_CHECK(s.has_value() == false);
    RS_CHECK_EQ(s.get_error(), rs::RtcpError::kMalformed);
    // A compound must start with SR or RR.
    uint8_t sdes_first[8] = {0x81U, 202U, 0x00U, 0x01U, 0U, 0U, 0U, 0U};
    s = rs::ParseCompound(sdes_first, sizeof(sdes_first));
    RS_CHECK(s.has_value() == false);
    RS_CHECK_EQ(s.get_error(), rs::RtcpError::kMalformed);
  }

  return 0;
}
