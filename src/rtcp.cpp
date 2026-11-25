// Minimal RTCP sender report / receiver report support (design section 7.5).
// SPDX-License-Identifier: MIT
#include "rs/rtcp.hpp"

#include <cstring>

namespace rs {
namespace {

constexpr uint32_t kSenderReportBytes = 28U;
constexpr uint32_t kReportBlockBytes = 24U;
constexpr uint32_t kReceiverReportHeaderBytes = 8U;
constexpr uint32_t kNtpEpochOffsetSeconds = 2208988800U;
constexpr uint8_t kRtcpVersion = 2U;
constexpr uint8_t kPacketTypeSr = 200U;
constexpr uint8_t kPacketTypeRr = 201U;
constexpr uint8_t kPacketTypeSdes = 202U;
constexpr uint8_t kPacketTypeBye = 203U;
constexpr uint8_t kPacketTypeXr = 207U;
constexpr uint32_t kMinCompoundBytes = 8U;
constexpr uint32_t kByeHeaderBytes = 4U;
constexpr uint32_t kSdesHeaderBytes = 4U;
constexpr uint32_t kSdesChunkFixedBytes = 6U;
constexpr uint32_t kMaxCnameBytes = 255U;
constexpr uint8_t kCnameItemType = 1U;

uint16_t ReadBe16(const uint8_t* p) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8U) | static_cast<uint16_t>(p[1]));
}

uint32_t ReadBe32(const uint8_t* p) noexcept {
  return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) |
         (static_cast<uint32_t>(p[2]) << 8U) | static_cast<uint32_t>(p[3]);
}

void WriteBe16(uint8_t* p, uint16_t value) noexcept {
  p[0] = static_cast<uint8_t>(value >> 8U);
  p[1] = static_cast<uint8_t>(value);
}

void WriteBe32(uint8_t* p, uint32_t value) noexcept {
  p[0] = static_cast<uint8_t>(value >> 24U);
  p[1] = static_cast<uint8_t>(value >> 16U);
  p[2] = static_cast<uint8_t>(value >> 8U);
  p[3] = static_cast<uint8_t>(value);
}

uint32_t AlignUp4(uint32_t value) noexcept { return (value + 3U) & ~3U; }

// CNAME length as carried in SDES: limited to 255 bytes by the 8-bit field.
uint32_t CnameLength(const char* cname) noexcept {
  uint32_t length = 0U;
  if (nullptr != cname) {
    while ((kMaxCnameBytes > length) && ('\0' != cname[length])) {
      ++length;
    }
  }
  return length;
}

// Fills one 24-byte report block; cumulative LSR is sign-extended from 24 bits.
void ReadReportBlock(const uint8_t* block, RrReport& report) noexcept {
  report.ssrc = ReadBe32(block);
  report.fraction_lost = block[4];
  const uint32_t lost = (static_cast<uint32_t>(block[5]) << 16U) |
                        (static_cast<uint32_t>(block[6]) << 8U) | static_cast<uint32_t>(block[7]);
  report.cumulative_lost =
      (0U != (lost & 0x00800000U)) ? (static_cast<int32_t>(lost) - 0x01000000) : static_cast<int32_t>(lost);
  report.extended_highest_seq = ReadBe32(block + 8U);
  report.jitter = ReadBe32(block + 12U);
  report.last_sr = ReadBe32(block + 16U);
  report.delay_since_sr = ReadBe32(block + 20U);
}

// Validates one RTCP packet header and folds its statistics into @p summary.
bool ParsePacket(const uint8_t* packet, uint32_t bytes, bool first, RtcpSummary& summary) noexcept {
  const uint8_t version = static_cast<uint8_t>(packet[0] >> 6U);
  const uint8_t packet_type = packet[1];
  const uint8_t block_count = static_cast<uint8_t>(packet[0] & 0x1FU);
  bool ok = (kRtcpVersion == version) && (kPacketTypeSr <= packet_type) && (packet_type <= kPacketTypeXr);
  if (ok && first) {
    ok = (kPacketTypeSr == packet_type) || (kPacketTypeRr == packet_type);
  }
  if (ok) {
    if (kPacketTypeSr == packet_type) {
      ok = (kSenderReportBytes + static_cast<uint32_t>(block_count) * kReportBlockBytes) <= bytes;
      if (ok) {
        ++summary.sender_reports;
      }
    } else if (kPacketTypeRr == packet_type) {
      ok = (kReceiverReportHeaderBytes + static_cast<uint32_t>(block_count) * kReportBlockBytes) <= bytes;
      if (ok) {
        ++summary.receiver_reports;
        if ((0U < block_count) && !summary.has_rr) {
          ReadReportBlock(packet + kReceiverReportHeaderBytes, summary.first_rr);
          summary.has_rr = true;
        }
      }
    } else if (kPacketTypeBye == packet_type) {
      ok = (kByeHeaderBytes + static_cast<uint32_t>(block_count) * 4U) <= bytes;
      if (ok) {
        ++summary.byes;
      }
    }
    // SDES/APP/RTPFB/PSFB/XR carry no statistics here and are skipped by length.
  }
  return ok;
}

}  // namespace

osp::expected<uint32_t, RtcpError> BuildSenderReport(osp::writable_byte_span out, uint32_t ssrc,
                                                     uint64_t ntp_now_ns, uint32_t rtp_ts,
                                                     uint32_t packet_count, uint32_t octet_count) noexcept {
  if (nullptr == out.data() || out.size() < static_cast<size_t>(kSenderReportBytes)) {
    return osp::expected<uint32_t, RtcpError>::error(RtcpError::kBufferTooSmall);
  }
  uint8_t* p = reinterpret_cast<uint8_t*>(out.data());
  const uint64_t seconds = ntp_now_ns / 1000000000ULL;
  const uint64_t fraction_ns = ntp_now_ns % 1000000000ULL;
  const uint32_t ntp_seconds = static_cast<uint32_t>(seconds) + kNtpEpochOffsetSeconds;
  const uint32_t ntp_fraction = static_cast<uint32_t>((fraction_ns << 32U) / 1000000000ULL);
  p[0] = static_cast<uint8_t>((kRtcpVersion << 6U) | 0U);  // V=2, RC=0.
  p[1] = kPacketTypeSr;
  WriteBe16(p + 2U, static_cast<uint16_t>((kSenderReportBytes / 4U) - 1U));
  WriteBe32(p + 4U, ssrc);
  WriteBe32(p + 8U, ntp_seconds);
  WriteBe32(p + 12U, ntp_fraction);
  WriteBe32(p + 16U, rtp_ts);
  WriteBe32(p + 20U, packet_count);
  WriteBe32(p + 24U, octet_count);
  return osp::expected<uint32_t, RtcpError>::success(kSenderReportBytes);
}

osp::expected<RrReport, RtcpError> ParseReceiverReport(const uint8_t* data, uint32_t len) noexcept {
  const auto summary = ParseCompound(data, len);
  if (!summary.has_value() || !summary.value().has_rr) {
    return osp::expected<RrReport, RtcpError>::error(RtcpError::kMalformed);
  }
  return osp::expected<RrReport, RtcpError>::success(summary.value().first_rr);
}

osp::expected<RtcpSummary, RtcpError> ParseCompound(const uint8_t* data, uint32_t len) noexcept {
  RtcpSummary summary;
  bool ok = (nullptr != data) && (kMinCompoundBytes <= len);
  uint32_t offset = 0U;
  bool first = true;
  while (ok && (offset < len)) {
    const uint32_t remaining = len - offset;
    if (4U > remaining) {
      ok = false;
      break;
    }
    const uint32_t packet_bytes = (static_cast<uint32_t>(ReadBe16(data + offset + 2U)) + 1U) * 4U;
    if (packet_bytes > remaining) {
      ok = false;
      break;
    }
    ok = ParsePacket(data + offset, packet_bytes, first, summary);
    first = false;
    offset += packet_bytes;
  }
  if (!ok) {
    return osp::expected<RtcpSummary, RtcpError>::error(RtcpError::kMalformed);
  }
  return osp::expected<RtcpSummary, RtcpError>::success(summary);
}

osp::expected<uint32_t, RtcpError> BuildSenderReportWithSdes(osp::writable_byte_span out, uint32_t ssrc,
                                                             const char* cname, uint64_t ntp_now_ns,
                                                             uint32_t rtp_ts, uint32_t packet_count,
                                                             uint32_t octet_count) noexcept {
  const uint32_t cname_length = CnameLength(cname);
  const uint32_t sdes_bytes = kSdesHeaderBytes + AlignUp4(kSdesChunkFixedBytes + cname_length + 1U);
  const uint32_t total_bytes = kSenderReportBytes + sdes_bytes;
  if (0U == cname_length) {
    return osp::expected<uint32_t, RtcpError>::error(RtcpError::kMalformed);
  }
  if ((nullptr == out.data()) || (out.size() < static_cast<size_t>(total_bytes))) {
    return osp::expected<uint32_t, RtcpError>::error(RtcpError::kBufferTooSmall);
  }
  const auto sr = BuildSenderReport(out, ssrc, ntp_now_ns, rtp_ts, packet_count, octet_count);
  if (!sr.has_value()) {
    return osp::expected<uint32_t, RtcpError>::error(sr.get_error());
  }
  uint8_t* sdes = reinterpret_cast<uint8_t*>(out.data()) + kSenderReportBytes;
  sdes[0] = static_cast<uint8_t>((kRtcpVersion << 6U) | 1U);  // V=2, SC=1 chunk.
  sdes[1] = kPacketTypeSdes;
  WriteBe16(sdes + 2U, static_cast<uint16_t>((sdes_bytes / 4U) - 1U));
  WriteBe32(sdes + 4U, ssrc);
  sdes[8] = kCnameItemType;
  sdes[9] = static_cast<uint8_t>(cname_length);
  std::memcpy(sdes + 10U, cname, cname_length);
  std::memset(sdes + 10U + cname_length, 0, sdes_bytes - 10U - cname_length);
  return osp::expected<uint32_t, RtcpError>::success(total_bytes);
}
}  // namespace rs
