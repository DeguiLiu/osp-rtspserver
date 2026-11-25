// Codec identity and parameter-set bundle shared by SDP, server and session.
// Phase 2 adds H.265 alongside the existing H.264 path.
// SPDX-License-Identifier: MIT
#ifndef RS_CODEC_HPP_
#define RS_CODEC_HPP_

#include "rs/config.hpp"

#include <cstdint>

namespace rs {

enum class Codec : uint8_t { kH264 = 0, kH265 = 1, kUnknown = 255 };

// Codec-agnostic parameter sets. VPS is meaningful for H.265 only; the
// pointers refer to storage owned by the server for the process lifetime.
struct TrackParams {
  Codec codec = Codec::kUnknown;
  const uint8_t* vps = nullptr;
  uint32_t vps_len = 0U;
  const uint8_t* sps = nullptr;
  uint32_t sps_len = 0U;
  const uint8_t* pps = nullptr;
  uint32_t pps_len = 0U;
};

[[nodiscard]] const char* CodecName(Codec codec) noexcept;

// Detects H.264 vs H.265 from the first Annex B NAL units. Returns false when
// neither header form is plausible. H.264 SPS/PPS bytes are not valid H.265
// headers (reserved nal_unit_type values) and vice versa.
[[nodiscard]] bool DetectCodec(const uint8_t* au, uint32_t au_len, Codec& out) noexcept;

// Rejects parameter sets this build cannot stream (review P1-7): H.264
// Baseline/Main/High 8-bit 4:2:0 and H.265 Main / Main Still 8-bit 4:2:0.
// Deeper SPS parsing (chroma_format_idc, bit depth, field coding) is not done
// here; the server only accepts local files, so a full check stays a TODO.
[[nodiscard]] bool ValidateParamSets(const TrackParams& params) noexcept;

}  // namespace rs

#endif  // RS_CODEC_HPP_
