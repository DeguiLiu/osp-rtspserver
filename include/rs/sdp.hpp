// Bounded SDP generation for a single H.264 video track.
// SPDX-License-Identifier: MIT
#ifndef RS_SDP_HPP_
#define RS_SDP_HPP_

#include "osp/span.hpp"
#include "osp/vocabulary.hpp"
#include "rs/codec.hpp"

#include <cstdint>

namespace rs {

struct SdpParams {
  Codec codec = Codec::kH264;       // Selects rtpmap/fmtp form.
  const char* server_ip = nullptr;  // Advertised c= address.
  const char* session_name = "live";
  uint16_t port = 0U;  // RTP port (RTCP = port + 1).
  uint32_t ssrc = 0U;
  const uint8_t* vps = nullptr;  // H.265 only.
  uint32_t vps_len = 0U;
  const uint8_t* sps = nullptr;
  uint32_t sps_len = 0U;
  const uint8_t* pps = nullptr;
  uint32_t pps_len = 0U;
  uint32_t clock_rate = 90000U;
  uint8_t payload_type = 96U;
};

enum class SdpError : uint8_t { kBufferTooSmall = 0, kBadParamSet, kMissingField };

// Writes a complete SDP body. Returns the number of bytes written.
[[nodiscard]] osp::expected<uint32_t, SdpError> BuildSdp(const SdpParams& params, osp::writable_byte_span out) noexcept;

// RFC 4648 base64 without padding-free variants; returns written length.
[[nodiscard]] uint32_t Base64Encode(const uint8_t* src, uint32_t len, char* dst, uint32_t cap) noexcept;

}  // namespace rs

#endif  // RS_SDP_HPP_
