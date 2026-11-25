// Annex B scanning and RTP packetization (RFC 7798) for H.265/HEVC.
// Single NAL unit packets and FU packets; AP aggregation is not used.
// Shares RtpPacket / PacketizeError / NalUnit with the H.264 module.
// SPDX-License-Identifier: MIT
#ifndef RS_H265_PACKETIZER_HPP_
#define RS_H265_PACKETIZER_HPP_

#include "rs/config.hpp"
#include "rs/h264_packetizer.hpp"  // shared RtpPacket / PacketizeError / NalUnit

#include "osp/span.hpp"
#include "osp/vocabulary.hpp"

#include <array>
#include <cstdint>

namespace rs {

// HEVC nal_unit_type values used by this implementation.
enum class HevcNalType : uint8_t {
  kTrailN = 0U,
  kTrailR = 1U,
  kTsaN = 2U,
  kTsaR = 3U,
  kStsaN = 4U,
  kStsaR = 5U,
  kRadlN = 6U,
  kRadlR = 7U,
  kRaslN = 8U,
  kRaslR = 9U,
  kBlWlp = 16U,
  kBlWradl = 17U,
  kBlNlp = 18U,
  kIdrWRadl = 19U,
  kIdrNLp = 20U,
  kCra = 21U,
  kIrapVcl23 = 23U,
  kVps = 32U,
  kSps = 33U,
  kPps = 34U,
  kAud = 35U,
  kEos = 36U,
  kEob = 37U,
  kFd = 38U,
  kPrefixSei = 39U,
  kSuffixSei = 40U,
  kFu = 49U
};

struct HevcParamSets {
  const uint8_t* vps = nullptr;
  uint32_t vps_len = 0U;
  const uint8_t* sps = nullptr;
  uint32_t sps_len = 0U;
  const uint8_t* pps = nullptr;
  uint32_t pps_len = 0U;
};

// Scans the next Annex B NAL unit. `offset` points at the 3/4-byte start code
// and is advanced past the NAL. `out.data` is the NAL including its 2-byte
// NAL header.
[[nodiscard]] bool NextHevcNalUnit(const uint8_t* au, uint32_t au_len, uint32_t& offset, NalUnit& out) noexcept;

// True when the AU contains an IRAP (IDR/CRA/BLA) VCL NAL.
[[nodiscard]] bool HevcAuHasIrap(const uint8_t* au, uint32_t au_len) noexcept;

// Per-client HEVC packetization progress (loop thread only).
struct HevcCursor {
  uint32_t nal_offset = 0U;
  uint32_t frag_offset = 0U;
  uint32_t au_len = 0U;
  uint16_t seq = 0U;
  uint32_t ts = 0U;
  uint8_t param_stage = 3U;  // 0 = VPS, 1 = SPS, 2 = PPS, 3 = done.
  bool await_idr = true;
  bool in_au = false;
  bool marker_sent = false;
  std::array<uint8_t, kRtpHeaderBytes> rtp_header{};
  // FU header only (3 bytes: two-byte payload header + FU header); fragment
  // bytes stay in the shared AU.
  std::array<uint8_t, 3U> fu_header{};
};

class H265Packetizer {
 public:
  static void ResetCursor(HevcCursor& cursor) noexcept;

  // inject_params emits VPS/SPS/PPS before the first slice packet.
  static void StartAu(HevcCursor& cursor, uint32_t au_len, uint32_t ts, bool inject_params) noexcept;

  [[nodiscard]] static osp::expected<RtpPacket, PacketizeError> Next(const uint8_t* au, uint32_t au_len,
                                                                     const HevcParamSets& params,
                                                                     HevcCursor& cursor,
                                                                     uint32_t ssrc) noexcept;
};

}  // namespace rs

#endif  // RS_H265_PACKETIZER_HPP_
