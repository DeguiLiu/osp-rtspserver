// Unit tests for bounded SDP generation and base64 encoding.
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/sdp.hpp"

#include <cstring>

namespace {

constexpr uint8_t kSps[] = {0x67, 0x42, 0xC0, 0x1F, 0x8C, 0x8D, 0x40, 0x50};
constexpr uint8_t kPps[] = {0x68, 0xCE, 0x3C, 0x80};
constexpr uint8_t kH265Vps[] = {0x40, 0x01, 0x0C};
constexpr uint8_t kH265Sps[] = {0x42, 0x01, 0x01, 0x60};
constexpr uint8_t kH265Pps[] = {0x44, 0x01, 0xC1};

osp::writable_byte_span Span(char* buf, uint32_t cap) {
  return osp::writable_byte_span(static_cast<std::byte*>(static_cast<void*>(buf)), cap);
}

rs::SdpParams Params() {
  rs::SdpParams p;
  p.server_ip = "192.0.2.10";
  p.session_name = "live";
  p.port = 0U;
  p.ssrc = 0x12345678U;
  p.sps = kSps;
  p.sps_len = sizeof(kSps);
  p.pps = kPps;
  p.pps_len = sizeof(kPps);
  p.clock_rate = 90000U;
  p.payload_type = 96U;
  return p;
}

rs::SdpParams Params265() {
  rs::SdpParams p = Params();
  p.codec = rs::Codec::kH265;
  p.vps = kH265Vps;
  p.vps_len = sizeof(kH265Vps);
  p.sps = kH265Sps;
  p.sps_len = sizeof(kH265Sps);
  p.pps = kH265Pps;
  p.pps_len = sizeof(kH265Pps);
  return p;
}

}  // namespace

int main() {
  {
    constexpr uint8_t vps[] = {0x40U, 0x01U, 0x0CU, 0x01U};
    constexpr uint8_t sps_main[] = {0x42U, 0x01U, 0x01U, 0x01U};
    constexpr uint8_t sps_still[] = {0x42U, 0x01U, 0x01U, 0x03U};
    constexpr uint8_t sps_main10[] = {0x42U, 0x01U, 0x01U, 0x02U};
    constexpr uint8_t pps[] = {0x44U, 0x01U};
    rs::TrackParams params;
    params.codec = rs::Codec::kH265;
    params.vps = vps;
    params.vps_len = sizeof(vps);
    params.sps = sps_main;
    params.sps_len = sizeof(sps_main);
    params.pps = pps;
    params.pps_len = sizeof(pps);
    RS_CHECK(rs::ValidateParamSets(params));
    params.sps = sps_still;
    RS_CHECK(rs::ValidateParamSets(params));
    params.sps = sps_main10;
    RS_CHECK(false == rs::ValidateParamSets(params));
    params.sps_len = 3U;
    RS_CHECK(false == rs::ValidateParamSets(params));
  }
  // RFC 4648 base64 vectors.
  {
    char out[32] = {};
    std::memset(out, 'X', sizeof(out));
    RS_CHECK_EQ(rs::Base64Encode(static_cast<const uint8_t*>(static_cast<const void*>("f")), 1U, out, sizeof(out)), 4U);
    RS_CHECK_EQ(std::memcmp(out, "Zg==", 4U), 0);
    RS_CHECK_EQ(out[4], 'X');  // No trailing NUL is written.
    std::memset(out, 'X', sizeof(out));
    RS_CHECK_EQ(rs::Base64Encode(static_cast<const uint8_t*>(static_cast<const void*>("fo")), 2U, out, sizeof(out)),
                4U);
    RS_CHECK_EQ(std::memcmp(out, "Zm8=", 4U), 0);
    std::memset(out, 'X', sizeof(out));
    RS_CHECK_EQ(rs::Base64Encode(static_cast<const uint8_t*>(static_cast<const void*>("foo")), 3U, out, sizeof(out)),
                4U);
    RS_CHECK_EQ(std::memcmp(out, "Zm9v", 4U), 0);
    std::memset(out, 'X', sizeof(out));
    RS_CHECK_EQ(rs::Base64Encode(static_cast<const uint8_t*>(static_cast<const void*>("foobar")), 6U, out, sizeof(out)),
                8U);
    RS_CHECK_EQ(std::memcmp(out, "Zm9vYmFy", 8U), 0);
  }

  // Empty input, insufficient capacity, null input.
  {
    char out[8] = {};
    RS_CHECK_EQ(rs::Base64Encode(static_cast<const uint8_t*>(static_cast<const void*>("")), 0U, out, sizeof(out)), 0U);
    RS_CHECK_EQ(rs::Base64Encode(static_cast<const uint8_t*>(static_cast<const void*>("f")), 1U, out, 3U), 0U);
    RS_CHECK_EQ(rs::Base64Encode(nullptr, 1U, out, sizeof(out)), 0U);
    RS_CHECK_EQ(rs::Base64Encode(kSps, sizeof(kSps), nullptr, sizeof(out)), 0U);
  }

  // Full byte-level SDP assertion.
  const char* expected =
      "v=0\r\n"
      "o=- 305419896 0 IN IP4 192.0.2.10\r\n"
      "s=live\r\n"
      "c=IN IP4 192.0.2.10\r\n"
      "t=0 0\r\n"
      "a=control:*\r\n"
      "a=range:npt=0-\r\n"
      "m=video 0 RTP/AVP 96\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 packetization-mode=1;profile-level-id=42c01f;"
      "sprop-parameter-sets=Z0LAH4yNQFA=,aM48gA==\r\n"
      "a=control:trackID=0\r\n"
      "a=ssrc:305419896\r\n";
  const uint32_t expected_len = static_cast<uint32_t>(std::strlen(expected));
  {
    char buf[1024] = {};
    std::memset(buf, 'Z', sizeof(buf));
    const auto r = rs::BuildSdp(Params(), Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value(), expected_len);
    RS_CHECK_EQ(std::memcmp(buf, expected, expected_len), 0);
    RS_CHECK_EQ(buf[expected_len], 'Z');  // No terminator added.
  }

  // Exact-fit buffer succeeds; one byte short fails.
  {
    char buf[1024] = {};
    auto r = rs::BuildSdp(Params(), Span(buf, expected_len));
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value(), expected_len);
    r = rs::BuildSdp(Params(), Span(buf, expected_len - 1U));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBufferTooSmall);
    r = rs::BuildSdp(Params(), Span(buf, 0U));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBufferTooSmall);
  }

  // Non-default port, payload type and session name are rendered verbatim.
  {
    rs::SdpParams p = Params();
    p.port = 5000U;
    p.payload_type = 97U;
    p.session_name = "camera";
    p.ssrc = 1U;
    char buf[1024] = {};
    const auto r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value());
    buf[r.value()] = '\0';
    RS_CHECK(nullptr != std::strstr(buf, "s=camera\r\n"));
    RS_CHECK(nullptr != std::strstr(buf, "m=video 5000 RTP/AVP 97\r\n"));
    RS_CHECK(nullptr != std::strstr(buf, "a=rtpmap:97 H264/90000\r\n"));
    RS_CHECK(nullptr != std::strstr(buf, "a=ssrc:1\r\n"));
  }

  // Missing fields and unusable parameter sets.
  {
    char buf[1024] = {};
    rs::SdpParams p = Params();
    p.server_ip = nullptr;
    auto r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kMissingField);
    p = Params();
    p.session_name = nullptr;
    r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kMissingField);
    p = Params();
    p.sps = nullptr;
    r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBadParamSet);
    p = Params();
    p.sps_len = 0U;
    r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBadParamSet);
    p = Params();
    p.sps_len = 3U;  // profile-level-id needs bytes 1..3.
    r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBadParamSet);
    p = Params();
    p.pps = nullptr;
    r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBadParamSet);
    p = Params();
    p.pps_len = 0U;
    r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBadParamSet);
  }

  // H.265 SDP: rtpmap H265/90000 plus three sprop base64 fields.
  {
    const char* expected265 =
        "v=0\r\n"
        "o=- 305419896 0 IN IP4 192.0.2.10\r\n"
        "s=live\r\n"
        "c=IN IP4 192.0.2.10\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "a=range:npt=0-\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H265/90000\r\n"
        "a=fmtp:96 sprop-vps=QAEM;sprop-sps=QgEBYA==;sprop-pps=RAHB\r\n"
        "a=control:trackID=0\r\n"
        "a=ssrc:305419896\r\n";
    const uint32_t expected_len = static_cast<uint32_t>(std::strlen(expected265));
    char buf[1024] = {};
    std::memset(buf, 'Z', sizeof(buf));
    const auto r = rs::BuildSdp(Params265(), Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(r.value(), expected_len);
    RS_CHECK_EQ(std::memcmp(buf, expected265, expected_len), 0);
    RS_CHECK_EQ(buf[expected_len], 'Z');  // No terminator added.
    RS_CHECK(nullptr != std::strstr(buf, "a=rtpmap:96 H265/90000\r\n"));
    RS_CHECK(nullptr != std::strstr(buf, "sprop-vps=QAEM;"));
    RS_CHECK(nullptr != std::strstr(buf, "sprop-sps=QgEBYA==;"));
    RS_CHECK(nullptr != std::strstr(buf, "sprop-pps=RAHB\r\n"));
    RS_CHECK(nullptr != std::strstr(buf, "a=control:trackID=0\r\n"));
    RS_CHECK(nullptr != std::strstr(buf, "a=ssrc:305419896\r\n"));
  }

  // H.265 tolerates short parameter sets and renders a custom payload type.
  {
    rs::SdpParams p = Params265();
    uint8_t short_sps[] = {0x42, 0x01};
    p.sps = short_sps;
    p.sps_len = sizeof(short_sps);
    p.payload_type = 97U;
    p.port = 5000U;
    char buf[1024] = {};
    const auto r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value());
    buf[r.value()] = '\0';
    RS_CHECK(nullptr != std::strstr(buf, "m=video 5000 RTP/AVP 97\r\n"));
    RS_CHECK(nullptr != std::strstr(buf, "a=rtpmap:97 H265/90000\r\n"));
    RS_CHECK(nullptr != std::strstr(buf, "a=fmtp:97 sprop-vps=QAEM;sprop-sps=QgE=;sprop-pps=RAHB\r\n"));
  }

  // Missing VPS/SPS/PPS and a short buffer fail for H.265.
  {
    char buf[1024] = {};
    rs::SdpParams p = Params265();
    p.vps = nullptr;
    p.vps_len = 0U;
    auto r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBadParamSet);
    p = Params265();
    p.sps = nullptr;
    p.sps_len = 0U;
    r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBadParamSet);
    p = Params265();
    p.pps_len = 0U;
    r = rs::BuildSdp(p, Span(buf, sizeof(buf)));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBadParamSet);
    const char* probe =
        "v=0\r\n"
        "o=- 305419896 0 IN IP4 192.0.2.10\r\n"
        "s=live\r\n"
        "c=IN IP4 192.0.2.10\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "a=range:npt=0-\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H265/90000\r\n"
        "a=fmtp:96 sprop-vps=QAEM;sprop-sps=QgEBYA==;sprop-pps=RAHB\r\n"
        "a=control:trackID=0\r\n"
        "a=ssrc:305419896\r\n";
    const uint32_t full_len = static_cast<uint32_t>(std::strlen(probe));
    r = rs::BuildSdp(Params265(), Span(buf, full_len - 1U));
    RS_CHECK(r.has_value() == false);
    RS_CHECK_EQ(r.get_error(), rs::SdpError::kBufferTooSmall);
  }

  // DetectCodec: parameter-set NALs are the unambiguous evidence.
  {
    const uint8_t h264_au[] = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F,  //
                               0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,  //
                               0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};
    const uint8_t h265_au[] = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C,        //
                               0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x60,  //
                               0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xC1,        //
                               0x00, 0x00, 0x00, 0x01, 0x46, 0x01, 0x50};
    const uint8_t no_start_code[] = {0x67, 0x42, 0xC0, 0x1F};
    const uint8_t random_bytes[] = {0x12, 0x34, 0x56, 0x78, 0x9A};
    const uint8_t implausible_nal[] = {0x00, 0x00, 0x01, 0x00, 0x00};
    rs::Codec codec = rs::Codec::kUnknown;
    RS_CHECK(rs::DetectCodec(h264_au, sizeof(h264_au), codec));
    RS_CHECK_EQ(codec, rs::Codec::kH264);
    codec = rs::Codec::kUnknown;
    RS_CHECK(rs::DetectCodec(h265_au, sizeof(h265_au), codec));
    RS_CHECK_EQ(codec, rs::Codec::kH265);
    codec = rs::Codec::kH264;
    RS_CHECK(rs::DetectCodec(no_start_code, sizeof(no_start_code), codec) == false);
    codec = rs::Codec::kH264;
    RS_CHECK(rs::DetectCodec(random_bytes, sizeof(random_bytes), codec) == false);
    // A scanned NAL with no plausible header form reports kUnknown.
    RS_CHECK(rs::DetectCodec(implausible_nal, sizeof(implausible_nal), codec) == false);
    RS_CHECK_EQ(codec, rs::Codec::kUnknown);
  }

  return 0;
}
