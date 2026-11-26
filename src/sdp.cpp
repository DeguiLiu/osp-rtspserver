// Bounded SDP generation for a single H.264/H.265 video track (design 6.6).
// SPDX-License-Identifier: MIT
#include "rs/sdp.hpp"

#include <cstring>

namespace rs {
namespace {

constexpr char kBase64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kHexDigits[] = "0123456789abcdef";

// Append-only view helper: every write is checked against the caller's span.
class Writer {
 public:
  explicit Writer(osp::writable_byte_span out) noexcept : out_(out) {}

  bool Append(const char* text) noexcept {
    uint32_t n = 0U;
    while ('\0' != text[n]) {
      ++n;
    }
    return AppendRaw(text, n);
  }

  bool AppendRaw(const char* text, uint32_t n) noexcept {
    const bool fits = (n <= Remaining());
    if (fits) {
      std::memcpy(out_.data() + pos_, text, n);
      pos_ += n;
    }
    return fits;
  }

  bool AppendU32(uint32_t value) noexcept {
    char digits[10] = {};
    uint32_t n = 0U;
    do {
      digits[n] = static_cast<char>(static_cast<uint8_t>('0') + static_cast<uint8_t>(value % 10U));
      ++n;
      value /= 10U;
    } while (0U != value);
    bool fits = (n <= Remaining());
    for (uint32_t i = 0U; fits && i < n; ++i) {
      out_[pos_ + i] = static_cast<std::byte>(static_cast<uint8_t>(digits[n - 1U - i]));
    }
    if (fits) {
      pos_ += n;
    }
    return fits;
  }

  bool AppendHexByte(uint8_t value) noexcept {
    const bool fits = (2U <= Remaining());
    if (fits) {
      out_[pos_] = static_cast<std::byte>(static_cast<uint8_t>(kHexDigits[(value >> 4U) & 0x0FU]));
      out_[pos_ + 1U] = static_cast<std::byte>(static_cast<uint8_t>(kHexDigits[value & 0x0FU]));
      pos_ += 2U;
    }
    return fits;
  }

  char* Tail() noexcept { return reinterpret_cast<char*>(out_.data() + pos_); }
  uint32_t Remaining() const noexcept { return static_cast<uint32_t>(out_.size()) - pos_; }
  void Advance(uint32_t n) noexcept { pos_ += n; }
  uint32_t size() const noexcept { return pos_; }

 private:
  osp::writable_byte_span out_;
  uint32_t pos_ = 0U;
};

// Appends one base64-encoded parameter set; zero capacity or zero length
// reads as "does not fit" and fails the enclosing SDP build.
bool AppendBase64(Writer& w, const uint8_t* src, uint32_t len) noexcept {
  bool ok = false;
  if ((nullptr != src) && (0U != len)) {
    const uint32_t encoded = Base64Encode(src, len, w.Tail(), w.Remaining());
    if (0U != encoded) {
      w.Advance(encoded);
      ok = true;
    }
  }
  return ok;
}

}  // namespace

uint32_t Base64Encode(const uint8_t* src, uint32_t len, char* dst, uint32_t cap) noexcept {
  if (nullptr == dst || (nullptr == src && 0U != len)) {
    return 0U;
  }
  const uint32_t groups = len / 3U;
  const uint32_t rem = len % 3U;
  const uint64_t need = static_cast<uint64_t>(groups) * 4U + (0U != rem ? 4U : 0U);
  if (need > static_cast<uint64_t>(cap)) {
    return 0U;
  }
  uint32_t in = 0U;
  uint32_t out = 0U;
  for (uint32_t g = 0U; g < groups; ++g) {
    const uint32_t word = (static_cast<uint32_t>(src[in]) << 16U) | (static_cast<uint32_t>(src[in + 1U]) << 8U) |
                          static_cast<uint32_t>(src[in + 2U]);
    in += 3U;
    dst[out] = kBase64Table[(word >> 18U) & 0x3FU];
    dst[out + 1U] = kBase64Table[(word >> 12U) & 0x3FU];
    dst[out + 2U] = kBase64Table[(word >> 6U) & 0x3FU];
    dst[out + 3U] = kBase64Table[word & 0x3FU];
    out += 4U;
  }
  if (1U == rem) {
    const uint32_t word = static_cast<uint32_t>(src[in]) << 16U;
    dst[out] = kBase64Table[(word >> 18U) & 0x3FU];
    dst[out + 1U] = kBase64Table[(word >> 12U) & 0x3FU];
    dst[out + 2U] = '=';
    dst[out + 3U] = '=';
    out += 4U;
  } else if (2U == rem) {
    const uint32_t word = (static_cast<uint32_t>(src[in]) << 16U) | (static_cast<uint32_t>(src[in + 1U]) << 8U);
    dst[out] = kBase64Table[(word >> 18U) & 0x3FU];
    dst[out + 1U] = kBase64Table[(word >> 12U) & 0x3FU];
    dst[out + 2U] = kBase64Table[(word >> 6U) & 0x3FU];
    dst[out + 3U] = '=';
    out += 4U;
  }
  return out;
}

osp::expected<uint32_t, SdpError> BuildSdp(const SdpParams& params, osp::writable_byte_span out) noexcept {
  const char* ip = params.server_ip;
  const char* name = params.session_name;
  const bool h265 = (Codec::kH265 == params.codec);
  if (nullptr == ip || '\0' == ip[0] || nullptr == name) {
    return osp::expected<uint32_t, SdpError>::error(SdpError::kMissingField);
  }
  // H.264 profile-level-id is built from SPS bytes 1..3, so four bytes are the
  // minimum there. H.265 requires all three parameter sets, any length > 0.
  const bool params_ok = h265 ? ((nullptr != params.vps) && (0U != params.vps_len) && (nullptr != params.sps) &&
                                 (0U != params.sps_len) && (nullptr != params.pps) && (0U != params.pps_len))
                              : ((nullptr != params.sps) && (0U != params.sps_len) && (params.sps_len >= 4U) &&
                                 (nullptr != params.pps) && (0U != params.pps_len));
  if (params_ok == false) {
    return osp::expected<uint32_t, SdpError>::error(SdpError::kBadParamSet);
  }
  Writer w(out);
  bool ok = true;
  ok = ok && w.Append("v=0\r\n");
  // RTSP DESCRIBE has no separate session id parameter; the per-client SSRC is
  // the stable, non-zero origin identity for this track.
  ok = ok && w.Append("o=- ") && w.AppendU32(params.ssrc) && w.Append(" 0 IN IP4 ") && w.Append(ip) && w.Append("\r\n");
  ok = ok && w.Append("s=") && w.Append(name) && w.Append("\r\n");
  ok = ok && w.Append("c=IN IP4 ") && w.Append(ip) && w.Append("\r\n");
  ok = ok && w.Append("t=0 0\r\n");
  ok = ok && w.Append("a=control:*\r\n");
  ok = ok && w.Append("a=range:npt=0-\r\n");
  ok = ok && w.Append("m=video ") && w.AppendU32(params.port) && w.Append(" RTP/AVP ") &&
       w.AppendU32(params.payload_type) && w.Append("\r\n");
  if (h265) {
    ok = ok && w.Append("a=rtpmap:") && w.AppendU32(params.payload_type) && w.Append(" H265/") &&
         w.AppendU32(params.clock_rate) && w.Append("\r\n");
    ok = ok && w.Append("a=fmtp:") && w.AppendU32(params.payload_type) && w.Append(" sprop-vps=") &&
         AppendBase64(w, params.vps, params.vps_len) && w.Append(";sprop-sps=") &&
         AppendBase64(w, params.sps, params.sps_len) && w.Append(";sprop-pps=") &&
         AppendBase64(w, params.pps, params.pps_len);
  } else {
    ok = ok && w.Append("a=rtpmap:") && w.AppendU32(params.payload_type) && w.Append(" H264/") &&
         w.AppendU32(params.clock_rate) && w.Append("\r\n");
    ok = ok && w.Append("a=fmtp:") && w.AppendU32(params.payload_type) &&
         w.Append(" packetization-mode=1;profile-level-id=") && w.AppendHexByte(params.sps[1]) &&
         w.AppendHexByte(params.sps[2]) && w.AppendHexByte(params.sps[3]) && w.Append(";sprop-parameter-sets=") &&
         AppendBase64(w, params.sps, params.sps_len) && w.Append(",") && AppendBase64(w, params.pps, params.pps_len);
  }
  ok = ok && w.Append("\r\n");
  ok = ok && w.Append("a=control:trackID=0\r\n");
  ok = ok && w.Append("a=ssrc:") && w.AppendU32(params.ssrc) && w.Append("\r\n");
  if (ok == false) {
    return osp::expected<uint32_t, SdpError>::error(SdpError::kBufferTooSmall);
  }
  return osp::expected<uint32_t, SdpError>::success(w.size());
}

}  // namespace rs
