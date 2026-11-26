// Annex B scanning and RTP packetization (single NAL + FU-A) for H.264.
// Produces zero-copy views: the RTP header and the FU indicator/header live in
// the per-client cursor, while the payload always points into the shared AU
// slot or the parameter sets.
// SPDX-License-Identifier: MIT
#ifndef RS_H264_PACKETIZER_HPP_
#define RS_H264_PACKETIZER_HPP_

#include "osp/span.hpp"
#include "osp/vocabulary.hpp"
#include "rs/config.hpp"

#include <cstdint>

#include <array>
#include <type_traits>

namespace rs {

enum class NalType : uint8_t {
  kNonIdrSlice = 1U,
  kPartitionA = 2U,
  kPartitionB = 3U,
  kPartitionC = 4U,
  kIdrSlice = 5U,
  kSei = 6U,
  kSps = 7U,
  kPps = 8U,
  kAud = 9U
};

struct NalUnit {
  osp::byte_span data;  // NAL payload without the start code.
  uint8_t type = 0U;
};

struct ParamSets {
  const uint8_t* sps = nullptr;
  uint32_t sps_len = 0U;
  const uint8_t* pps = nullptr;
  uint32_t pps_len = 0U;
};

struct RtpPacket {
  const uint8_t* header = nullptr;  // 12-byte RTP header (per-client scratch).
  uint32_t header_len = 0U;
  const uint8_t* prefix = nullptr;  // FU indicator/header (per-client scratch); 0 bytes for a single NAL.
  uint32_t prefix_len = 0U;
  const uint8_t* payload = nullptr;  // RTP payload bytes (shared AU, read-only, zero-copy).
  uint32_t payload_len = 0U;
  bool marker = false;
};

static_assert(std::is_standard_layout<RtpPacket>::value, "RtpPacket is a plain wire view");

enum class PacketizeError : uint8_t {
  kEndOfAu = 0,       // All packets of this AU have been produced.
  kBadNal = 1,        // No start code / truncated NAL / reserved type 0.
  kTooLarge = 2,      // AU exceeds kMaxAuBytes while scanning.
  kNotConfigured = 3  // IDR without SPS/PPS available.
};

// Scans the next Annex B NAL unit starting at `offset` (offset points at the
// 3/4-byte start code). Returns false at end of AU. Advances `offset`.
[[nodiscard]] bool NextNalUnit(const uint8_t* au, uint32_t au_len, uint32_t& offset, NalUnit& out) noexcept;

// True when the AU contains an IDR slice (and a prefix start code is valid).
[[nodiscard]] bool AuHasIdr(const uint8_t* au, uint32_t au_len) noexcept;

// Per-client packetization progress. Owned by the session; loop-thread only.
struct ClientCursor {
  uint32_t nal_offset = 0U;   // Offset of the current NAL start code in the AU.
  uint32_t frag_offset = 0U;  // Bytes of the current NAL already packetized.
  uint32_t au_len = 0U;
  uint16_t seq = 0U;
  uint32_t ts = 0U;
  uint8_t param_stage = 2U;  // 0 = send SPS, 1 = send PPS, 2 = done.
  bool await_idr = true;
  bool in_au = false;
  bool marker_sent = false;
  std::array<uint8_t, kRtpHeaderBytes> rtp_header{};
  // FU-A indicator/header only (2 bytes used); the fragment bytes stay in the
  // shared AU and are exposed through RtpPacket::payload.
  std::array<uint8_t, 3U> fu_header{};
};

class H264Packetizer {
 public:
  // Resets progress and forces "wait for IDR" (used on drop/backpressure).
  static void ResetCursor(ClientCursor& cursor) noexcept;

  // Begins a new AU. inject_params emits SPS/PPS before the first slice packet.
  static void StartAu(ClientCursor& cursor, uint32_t au_len, uint32_t ts, bool inject_params) noexcept;

  // Produces the next RTP packet view for this AU, or kEndOfAu.
  [[nodiscard]] static osp::expected<RtpPacket, PacketizeError> Next(const uint8_t* au, uint32_t au_len,
                                                                     const ParamSets& params, ClientCursor& cursor,
                                                                     uint32_t ssrc) noexcept;
};

}  // namespace rs

#endif  // RS_H264_PACKETIZER_HPP_
