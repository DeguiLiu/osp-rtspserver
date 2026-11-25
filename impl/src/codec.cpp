// Annex B start-code scan and H.264/H.265 discrimination (design phase 2).
// SPDX-License-Identifier: MIT
#include "rs/codec.hpp"

namespace rs {
namespace {

constexpr uint32_t kMaxProbeNals = 8U;
constexpr uint8_t kNalSpsH264 = 7U;
constexpr uint8_t kNalSpsH265 = 33U;

struct StartCode {
  uint32_t header = 0U;
  uint32_t data = 0U;
};

// Finds the next 00 00 01 start code at or after `from`, returns false at end.
bool FindStartCode(const uint8_t* p, uint32_t len, uint32_t from, StartCode& out) noexcept {
  for (uint32_t i = from; i + 3U <= len; ++i) {
    if (p[i] == 0U && p[i + 1U] == 0U && p[i + 2U] == 1U) {
      out.header = i;
      out.data = i + 3U;
      return true;
    }
  }
  return false;
}

// H.264 forbids nal_unit_type 0 and reserves 24..31.
bool H264HeaderPlausible(uint8_t b0) noexcept {
  if ((b0 & 0x80U) != 0U) {  // forbidden_zero_bit
    return false;
  }
  const uint8_t type = static_cast<uint8_t>(b0 & 0x1FU);
  return (type != 0U && type <= 12U) || (type >= 14U && type <= 20U);
}

// H.265: nuh_layer_id must be 0 (single-layer stream) and
// nuh_temporal_id_plus1 must be non-zero. Reserved types 41..47 rejected.
bool H265HeaderPlausible(const uint8_t* nal, uint32_t len) noexcept {
  if (len < 2U) {
    return false;
  }
  const uint8_t type = static_cast<uint8_t>((nal[0] >> 1) & 0x3FU);
  const uint8_t layer = static_cast<uint8_t>(((nal[0] & 0x01U) << 5) | (nal[1] >> 3));
  const uint8_t tid1 = static_cast<uint8_t>(nal[1] & 0x07U);
  if ((nal[0] & 0x80U) != 0U || tid1 == 0U || layer != 0U) {
    return false;
  }
  return type <= 40U;
}

}  // namespace

const char* CodecName(Codec codec) noexcept {
  switch (codec) {
    case Codec::kH264:
      return "H264";
    case Codec::kH265:
      return "H265";
    default:
      return "UNKNOWN";
  }
}

bool ValidateParamSets(const TrackParams& params) noexcept {
  if (params.codec == Codec::kH264) {
    if (params.sps == nullptr || params.sps_len < 4U || params.pps == nullptr || params.pps_len < 2U) {
      return false;
    }
    if (kNalSpsH264 != (params.sps[0] & 0x1FU)) {
      return false;
    }
    const uint8_t profile_idc = params.sps[1];
    return profile_idc == 66U || profile_idc == 77U || profile_idc == 100U;
  }
  if (params.codec == Codec::kH265) {
    if (params.vps == nullptr || params.vps_len < 4U || params.sps == nullptr || params.sps_len < 4U ||
        params.pps == nullptr || params.pps_len < 2U) {
      return false;
    }
    if (kNalSpsH265 != ((params.sps[0] >> 1U) & 0x3FU)) {
      return false;
    }
    // general_profile_idc occupies the low 5 bits of the second SPS byte.
    const uint8_t profile_idc = static_cast<uint8_t>(params.sps[1] & 0x1FU);
    return profile_idc == 1U || profile_idc == 3U;  // Main, Main Still Picture.
  }
  return false;
}

bool DetectCodec(const uint8_t* au, uint32_t au_len, Codec& out) noexcept {
  if (au == nullptr || au_len < 4U) {
    return false;
  }
  StartCode sc{};
  if (!FindStartCode(au, au_len, 0U, sc)) {
    return false;
  }
  uint32_t probes = 0U;
  while (probes < kMaxProbeNals) {
    StartCode next{};
    const bool has_next = FindStartCode(au, au_len, sc.data + 1U, next);
    const uint32_t end = has_next ? next.header : au_len;
    const uint32_t nal_len = end - sc.data;
    if (nal_len >= 2U) {
      const uint8_t* nal = au + sc.data;
      const bool h265_ok = H265HeaderPlausible(nal, nal_len);
      const bool h264_ok = H264HeaderPlausible(nal[0]);
      const uint8_t h265_type = static_cast<uint8_t>((nal[0] >> 1) & 0x3FU);
      const uint8_t h264_type = static_cast<uint8_t>(nal[0] & 0x1FU);
      // Parameter-set / delimiter NALs are the unambiguous evidence.
      if (h265_ok && (h265_type == 32U || h265_type == 33U || h265_type == 34U || h265_type == 35U)) {
        out = Codec::kH265;
        return true;
      }
      if (h264_ok && (h264_type == 7U || h264_type == 8U || h264_type == 9U)) {
        out = Codec::kH264;
        return true;
      }
      if (h265_ok && !h264_ok) {
        out = Codec::kH265;
        return true;
      }
      if (h264_ok && !h265_ok) {
        out = Codec::kH264;
        return true;
      }
    }
    if (!has_next) {
      break;
    }
    sc = next;
    ++probes;
  }
  out = Codec::kUnknown;
  return false;
}

}  // namespace rs
