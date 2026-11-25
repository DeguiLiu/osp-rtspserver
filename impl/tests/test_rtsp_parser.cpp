// Unit tests for the incremental RTSP parser (design section 6.2).
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/rtsp_parser.hpp"

#include <cstring>

namespace {

struct Capture {
  uint32_t requests = 0U;
  uint32_t frames = 0U;
  rs::Method method = rs::Method::kUnknown;
  char method_raw[24] = {};
  char uri[320] = {};
  char version[24] = {};
  uint32_t body_len = 0U;
  char body[64] = {};
  uint32_t header_count = 0U;
  uint32_t cseq = 0U;
  bool cseq_ok = false;
  uint32_t case_value = 0U;
  bool case_ok = false;
  bool copy_ok = false;
  uint32_t copy_len = 0U;
  char copy[8] = {};
  bool tight_ok = true;
  bool dup_ok = false;
  uint32_t dup_len = 0U;
  char dup[16] = {};
  bool null_name_ok = true;
  uint8_t channel = 0U;
  uint32_t frame_len = 0U;
  uint8_t frame[32] = {};
};

void CopyText(osp::byte_span span, char* dst, uint32_t cap) {
  uint32_t n = static_cast<uint32_t>(span.size());
  if (n >= cap) {
    n = cap - 1U;
  }
  if (n > 0U) {
    std::memcpy(dst, span.data(), n);
  }
  dst[n] = '\0';
}

void OnRequest(void* ctx, const rs::RtspRequest& req) {
  Capture& c = *static_cast<Capture*>(ctx);
  ++c.requests;
  c.method = req.method;
  CopyText(req.method_raw, c.method_raw, sizeof(c.method_raw));
  CopyText(req.uri, c.uri, sizeof(c.uri));
  CopyText(req.version, c.version, sizeof(c.version));
  c.header_count = req.header_count;
  c.cseq_ok = rs::GetHeaderU32(req, "CSeq", c.cseq);
  c.case_ok = rs::GetHeaderU32(req, "X-Case", c.case_value);
  c.copy_ok = rs::CopyHeader(req, "X-Long", c.copy, sizeof(c.copy), c.copy_len);
  c.tight_ok = rs::CopyHeader(req, "X-Long", c.copy, 5U, c.copy_len);
  c.dup_ok = rs::CopyHeader(req, "X-Dup", c.dup, sizeof(c.dup), c.dup_len);
  osp::byte_span unused;
  c.null_name_ok = rs::GetHeader(req, nullptr, unused);
  c.body_len = static_cast<uint32_t>(req.body.size());
  const uint32_t body_n = (c.body_len < sizeof(c.body)) ? c.body_len : static_cast<uint32_t>(sizeof(c.body) - 1U);
  if (body_n > 0U) {
    std::memcpy(c.body, req.body.data(), body_n);
  }
  c.body[body_n] = '\0';
}

void OnFrame(void* ctx, const rs::InterleavedFrame& frame) {
  Capture& c = *static_cast<Capture*>(ctx);
  ++c.frames;
  c.channel = frame.channel;
  c.frame_len = static_cast<uint32_t>(frame.payload.size());
  if (c.frame_len > sizeof(c.frame)) {
    c.frame_len = static_cast<uint32_t>(sizeof(c.frame));
  }
  if (c.frame_len > 0U) {
    std::memcpy(c.frame, frame.payload.data(), c.frame_len);
  }
}

rs::ParserCallbacks Callbacks(Capture& c) {
  rs::ParserCallbacks cb;
  cb.ctx = &c;
  cb.on_request = OnRequest;
  cb.on_interleaved = OnFrame;
  return cb;
}

osp::expected<void, rs::ParseError> FeedText(rs::RtspParser& parser, Capture& c, const char* text) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(text);
  return parser.Feed(bytes, static_cast<uint32_t>(std::strlen(text)), Callbacks(c));
}

}  // namespace

int main() {
  // Simple request, no headers.
  {
    rs::RtspParser parser;
    Capture c;
    const auto r = FeedText(parser, c, "OPTIONS * RTSP/1.0\r\n\r\n");
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.requests, 1U);
    RS_CHECK_EQ(c.method, rs::Method::kOptions);
    RS_CHECK_EQ(std::strcmp(c.uri, "*"), 0);
    RS_CHECK_EQ(std::strcmp(c.version, "RTSP/1.0"), 0);
    RS_CHECK_EQ(c.header_count, 0U);
    RS_CHECK(!c.null_name_ok);
    RS_CHECK_EQ(parser.buffered(), 0U);
  }

  // Per-byte feed must assemble exactly one request and retain partial state.
  {
    rs::RtspParser parser;
    Capture c;
    const char* text = "DESCRIBE rtsp://192.0.2.10/live RTSP/1.0\r\nCSeq: 7\r\nAccept: application/sdp\r\n\r\n";
    const uint32_t len = static_cast<uint32_t>(std::strlen(text));
    bool always_ok = true;
    for (uint32_t i = 0U; i < len; ++i) {
      const auto r = parser.Feed(reinterpret_cast<const uint8_t*>(text + i), 1U, Callbacks(c));
      always_ok = always_ok && r.has_value();
    }
    RS_CHECK(always_ok);
    RS_CHECK_EQ(c.requests, 1U);
    RS_CHECK_EQ(c.method, rs::Method::kDescribe);
    RS_CHECK_EQ(std::strcmp(c.uri, "rtsp://192.0.2.10/live"), 0);
    RS_CHECK_EQ(c.header_count, 2U);
    RS_CHECK_EQ(c.cseq, 7U);
  }

  // Two pipelined requests in one Feed.
  {
    rs::RtspParser parser;
    Capture c;
    const auto r = FeedText(parser, c,
                            "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n"
                            "PLAY /live/trackID=0 RTSP/1.0\r\nCSeq: 2\r\nSession: 1234\r\n\r\n");
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.requests, 2U);
    RS_CHECK_EQ(c.method, rs::Method::kPlay);
    RS_CHECK_EQ(c.cseq, 2U);
    RS_CHECK_EQ(c.header_count, 2U);
    RS_CHECK_EQ(parser.buffered(), 0U);
  }

  // Body split across two Feeds with Content-Length.
  {
    rs::RtspParser parser;
    Capture c;
    auto r = FeedText(parser, c, "SETUP /live/trackID=0 RTSP/1.0\r\nContent-Length: 5\r\n\r\nhel");
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.requests, 0U);
    RS_CHECK(parser.buffered() > 0U);
    r = FeedText(parser, c, "lo");
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.requests, 1U);
    RS_CHECK_EQ(c.body_len, 5U);
    RS_CHECK_EQ(std::strcmp(c.body, "hello"), 0);
  }

  // Interleaved frame followed by a request in the same feed.
  {
    rs::RtspParser parser;
    Capture c;
    static const char head[] = "$\x02\x00\x03\xAA\xBB\xCC""OPTIONS * RTSP/1.0\r\nCSeq: 9\r\n\r\n";
    const auto r =
        parser.Feed(reinterpret_cast<const uint8_t*>(head), sizeof(head) - 1U, Callbacks(c));
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.frames, 1U);
    RS_CHECK_EQ(c.channel, static_cast<uint8_t>(2U));
    RS_CHECK_EQ(c.frame_len, 3U);
    RS_CHECK_EQ(c.frame[0], static_cast<uint8_t>(0xAAU));
    RS_CHECK_EQ(c.frame[2], static_cast<uint8_t>(0xCCU));
    RS_CHECK_EQ(c.requests, 1U);
    RS_CHECK_EQ(c.cseq, 9U);
  }

  // Partial interleaved header/payload retained across feeds.
  {
    rs::RtspParser parser;
    Capture c;
    auto r = parser.Feed(reinterpret_cast<const uint8_t*>("$\x01\x00"), 3U, Callbacks(c));
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(parser.buffered(), 3U);
    r = parser.Feed(reinterpret_cast<const uint8_t*>("\x02\x10\x20"), 3U, Callbacks(c));
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.frames, 1U);
    RS_CHECK_EQ(c.channel, static_cast<uint8_t>(1U));
    RS_CHECK_EQ(c.frame_len, 2U);
    RS_CHECK_EQ(c.frame[1], static_cast<uint8_t>(0x20U));
  }

  // A '$' byte inside a Content-Length body is not an interleaved frame.
  {
    rs::RtspParser parser;
    Capture c;
    const auto r = FeedText(parser, c, "SETUP /x RTSP/1.0\r\nContent-Length: 1\r\n\r\n$"
                                       "OPTIONS * RTSP/1.0\r\n\r\n");
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.frames, 0U);
    RS_CHECK_EQ(c.requests, 2U);
    RS_CHECK_EQ(c.method, rs::Method::kOptions);
  }

  // Header names are case-insensitive; values keep their exact bytes.
  {
    rs::RtspParser parser;
    Capture c;
    const auto r = FeedText(parser, c,
                            "PLAY /x RTSP/1.0\r\ncseq: 42\r\nx-cASE: 77\r\n"
                            "X-Long: 1234567\r\nX-Dup: first\r\nX-Dup: second\r\n\r\n");
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.requests, 1U);
    RS_CHECK(c.cseq_ok);
    RS_CHECK_EQ(c.cseq, 42U);
    RS_CHECK(c.case_ok);
    RS_CHECK_EQ(c.case_value, 77U);
    RS_CHECK(c.copy_ok);
    RS_CHECK_EQ(c.copy_len, 7U);
    RS_CHECK_EQ(std::strcmp(c.copy, "1234567"), 0);
    RS_CHECK(!c.tight_ok);  // cap 5 cannot hold 7 bytes plus NUL.
    RS_CHECK(c.dup_ok);
    RS_CHECK_EQ(c.dup_len, 5U);
    RS_CHECK_EQ(std::strcmp(c.dup, "first"), 0);
    RS_CHECK_EQ(c.header_count, 5U);
  }

  // Duplicate Content-Length values must agree.
  {
    rs::RtspParser parser;
    Capture c;
    auto r = FeedText(parser, c, "SETUP /x RTSP/1.0\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nhi");
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.body_len, 2U);
    rs::RtspParser bad;
    Capture c2;
    r = FeedText(bad, c2, "SETUP /x RTSP/1.0\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nhi");
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kMalformed);
  }

  // CSeq overflow is rejected by GetHeaderU32, not silently wrapped.
  {
    rs::RtspParser parser;
    Capture c;
    const auto r = FeedText(parser, c, "OPTIONS * RTSP/1.0\r\nCSeq: 4294967296\r\n\r\n");
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.requests, 1U);
    RS_CHECK(!c.cseq_ok);
  }

  // Malformed start lines.
  {
    rs::RtspParser parser;
    Capture c;
    auto r = FeedText(parser, c, "OPTIONS *\r\n\r\n");
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kMalformed);
    rs::RtspParser parser2;
    Capture c2;
    r = FeedText(parser2, c2, "OPTIONS  * RTSP/1.0\r\n\r\n");  // empty URI token
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kMalformed);
  }

  // Oversized method and URI are rejected.
  {
    rs::RtspParser parser;
    Capture c;
    auto r = FeedText(parser, c, "ABCDEFGHIJKLMNOPQ * RTSP/1.0\r\n\r\n");  // 17-byte method
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kMalformed);
    char long_uri[300] = {};
    long_uri[0] = '/';
    std::memset(long_uri + 1, 'a', 280U);
    char line[360] = {};
    std::snprintf(line, sizeof(line), "OPTIONS %s RTSP/1.0\r\n\r\n", long_uri);
    rs::RtspParser parser2;
    Capture c2;
    r = FeedText(parser2, c2, line);
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kMalformed);
  }

  // Bare LF, NUL and obs-fold are rejected.
  {
    rs::RtspParser parser;
    Capture c;
    auto r = FeedText(parser, c, "OPTIONS * RTSP/1.0\r\nX: 1\nY: 2\r\n\r\n");
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kMalformed);
    rs::RtspParser parser2;
    Capture c2;
    r = FeedText(parser2, c2, "OPTIONS * RTSP/1.0\r\nX: a\r\n\r\n");
    RS_CHECK(r.has_value());
    rs::RtspParser parser3;
    Capture c3;
    char nul[32] = "OPTIONS * RTSP/1.0\r\n\r\n";
    nul[0] = '\0';
    r = parser3.Feed(reinterpret_cast<const uint8_t*>(nul), 22U, Callbacks(c3));
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kMalformed);
    rs::RtspParser parser4;
    Capture c4;
    r = FeedText(parser4, c4, "OPTIONS * RTSP/1.0\r\nX: 1\r\n folded\r\n\r\n");
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kMalformed);
  }

  // Header count limit (kMaxHeaderCount = 24) reports kTooManyHeaders.
  {
    rs::RtspParser parser;
    Capture c;
    char text[1024] = "OPTIONS * RTSP/1.0\r\n";
    uint32_t used = static_cast<uint32_t>(std::strlen(text));
    for (uint32_t i = 0U; i < (rs::kMaxHeaderCount + 1U); ++i) {
      const int n = std::snprintf(text + used, sizeof(text) - used, "X-%u: 1\r\n", i);
      used += static_cast<uint32_t>(n);
    }
    std::snprintf(text + used, sizeof(text) - used, "\r\n");
    const auto r = parser.Feed(reinterpret_cast<const uint8_t*>(text), static_cast<uint32_t>(std::strlen(text)),
                               Callbacks(c));
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kTooManyHeaders);
  }

  // A single header line over 512 bytes is malformed.
  {
    rs::RtspParser parser;
    Capture c;
    char text[800] = "OPTIONS * RTSP/1.0\r\nX: ";
    uint32_t used = static_cast<uint32_t>(std::strlen(text));
    std::memset(text + used, 'a', 520U);
    used += 520U;
    std::memcpy(text + used, "\r\n\r\n", 4U);
    used += 4U;
    const auto r = parser.Feed(reinterpret_cast<const uint8_t*>(text), used, Callbacks(c));
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kMalformed);
  }

  // Interleaved budget violations.
  {
    rs::RtspParser parser;
    Capture c;
    auto r = parser.Feed(reinterpret_cast<const uint8_t*>("$\x01\xFF\xFF"), 4U, Callbacks(c));
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kBadInterleaved);
    rs::RtspParser parser2;
    Capture c2;
    r = parser2.Feed(reinterpret_cast<const uint8_t*>("$\x01\x00\x00"), 4U, Callbacks(c2));
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::ParseError::kBadInterleaved);
  }

  // Unknown method is syntactically valid and maps to kUnknown.
  {
    rs::RtspParser parser;
    Capture c;
    const auto r = FeedText(parser, c, "FOO /x RTSP/1.0\r\n\r\n");
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(c.requests, 1U);
    RS_CHECK_EQ(c.method, rs::Method::kUnknown);
    RS_CHECK_EQ(std::strcmp(c.method_raw, "FOO"), 0);
  }

  // Method helpers.
  RS_CHECK_EQ(rs::ParseMethod(osp::byte_span(reinterpret_cast<const std::byte*>("GET_PARAMETER"), 13U)),
              rs::Method::kGetParameter);
  RS_CHECK_EQ(rs::ParseMethod(osp::byte_span(reinterpret_cast<const std::byte*>("get_parameter"), 13U)),
              rs::Method::kUnknown);
  RS_CHECK_EQ(std::strcmp(rs::MethodName(rs::Method::kTeardown), "TEARDOWN"), 0);
  RS_CHECK_EQ(std::strcmp(rs::MethodName(rs::Method::kUnknown), "UNKNOWN"), 0);

  // Reset drops any retained partial message.
  {
    rs::RtspParser parser;
    Capture c;
    const auto r = parser.Feed(reinterpret_cast<const uint8_t*>("OPTIONS * RTSP/1.0\r\nCSeq: "), 25U, Callbacks(c));
    RS_CHECK(r.has_value());
    RS_CHECK(parser.buffered() > 0U);
    parser.Reset();
    RS_CHECK_EQ(parser.buffered(), 0U);
  }

  return 0;
}
