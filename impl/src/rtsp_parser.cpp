// Incremental RTSP 1.0 request parser (design section 6.2).
// Socket-free, allocation-free and bounded: callbacks receive views into the
// parser buffer and must consume them before returning.
// SPDX-License-Identifier: MIT
#include "rs/rtsp_parser.hpp"

#include <charconv>

#include <cstring>

namespace rs {
namespace {

// The parser owns a fixed kRtspRxBytes buffer; a request body that cannot fit
// is rejected before any copy. 4 KiB matches the design limit.
constexpr uint32_t kMaxBodyBytes = 4096U;
constexpr uint32_t kMaxHeaderLineBytes = 512U;
constexpr uint32_t kMaxMethodBytes = 16U;
constexpr uint32_t kMaxVersionBytes = 16U;
constexpr uint32_t kMaxInterleavedPayload = kRtspRxBytes - 4U;

uint8_t ToLower(uint8_t c) noexcept {
  return (c >= static_cast<uint8_t>('A') && c <= static_cast<uint8_t>('Z'))
             ? static_cast<uint8_t>(c + static_cast<uint8_t>(32U))
             : c;
}

bool IsTokenChar(uint8_t c) noexcept {
  if ((c >= static_cast<uint8_t>('0') && c <= static_cast<uint8_t>('9')) ||
      (c >= static_cast<uint8_t>('A') && c <= static_cast<uint8_t>('Z')) ||
      (c >= static_cast<uint8_t>('a') && c <= static_cast<uint8_t>('z'))) {
    return true;
  }
  constexpr char kSpecials[] = "!#$%&'*+-.^_`|~";
  for (uint32_t i = 0U; '\0' != kSpecials[i]; ++i) {
    if (c == static_cast<uint8_t>(kSpecials[i])) {
      return true;
    }
  }
  return false;
}

bool EqualsLiteral(osp::byte_span value, const char* text) noexcept {
  uint32_t n = 0U;
  while ('\0' != text[n]) {
    ++n;
  }
  bool equal = (value.size() == static_cast<size_t>(n));
  for (uint32_t i = 0U; equal && i < n; ++i) {
    equal = (static_cast<uint8_t>(value[i]) == static_cast<uint8_t>(text[i]));
  }
  return equal;
}

bool EqualsIgnoreCase(osp::byte_span value, const char* text) noexcept {
  uint32_t n = 0U;
  while ('\0' != text[n]) {
    ++n;
  }
  bool equal = (value.size() == static_cast<size_t>(n));
  for (uint32_t i = 0U; equal && i < n; ++i) {
    equal = (ToLower(static_cast<uint8_t>(value[i])) == ToLower(static_cast<uint8_t>(text[i])));
  }
  return equal;
}

// Strict unsigned decimal via std::from_chars (locale-free, no exceptions).
bool ParseDecimal(osp::byte_span value, uint32_t& out) noexcept {
  if (0U == value.size()) {
    return false;
  }
  const char* const first = reinterpret_cast<const char*>(value.data());
  const char* const last = first + value.size();
  uint32_t parsed = 0U;
  const std::from_chars_result rc = std::from_chars(first, last, parsed, 10);
  if (rc.ec != std::errc() || rc.ptr != last) {
    return false;
  }
  out = parsed;
  return true;
}

bool FindCrlf(const uint8_t* p, uint32_t start, uint32_t end, uint32_t& out) noexcept {
  for (uint32_t i = start; i + 1U < end; ++i) {
    if ('\r' == p[i] && '\n' == p[i + 1U]) {
      out = i;
      return true;
    }
  }
  return false;
}

bool FindCrlfCrlf(const uint8_t* p, uint32_t start, uint32_t end, uint32_t& out) noexcept {
  for (uint32_t i = start; i + 3U < end; ++i) {
    if ('\r' == p[i] && '\n' == p[i + 1U] && '\r' == p[i + 2U] && '\n' == p[i + 3U]) {
      out = i;
      return true;
    }
  }
  return false;
}

// Rejects NUL, bare LF/CR and obs-fold, and counts header lines. The head is
// guaranteed to end with CRLFCRLF; anything after the blank line is malformed.
bool CountHeaderLines(const uint8_t* p, uint32_t len, uint32_t& count) noexcept {
  bool ok = true;
  for (uint32_t i = 0U; ok && i < len; ++i) {
    if (0 == p[i]) {
      ok = false;
    } else if ('\n' == p[i] && (0U == i || '\r' != p[i - 1U])) {
      ok = false;
    } else if ('\r' == p[i] && (i + 1U >= len || '\n' != p[i + 1U])) {
      ok = false;
    }
  }
  uint32_t eol = 0U;
  if (ok) {
    ok = FindCrlf(p, 0U, len, eol);
  }
  uint32_t pos = ok ? eol + 2U : len;
  uint32_t n = 0U;
  while (ok && pos < len) {
    if ('\r' == p[pos]) {
      ok = (pos + 2U == len);
      pos = len;
    } else if (' ' == p[pos] || '\t' == p[pos]) {
      ok = false;
    } else if (!FindCrlf(p, pos, len, eol)) {
      ok = false;
    } else {
      ++n;
      pos = eol + 2U;
    }
  }
  if (ok) {
    count = n;
  }
  return ok;
}

bool ParseStartLine(const uint8_t* p, uint32_t line_len, RtspRequest& req) noexcept {
  uint32_t sp1 = 0U;
  while (sp1 < line_len && ' ' != p[sp1]) {
    ++sp1;
  }
  uint32_t sp2 = (sp1 < line_len) ? sp1 + 1U : line_len;
  while (sp2 < line_len && ' ' != p[sp2]) {
    ++sp2;
  }
  const uint32_t method_len = (sp1 < line_len) ? sp1 : 0U;
  const uint32_t uri_len = (sp2 > sp1 + 1U) ? (sp2 - sp1 - 1U) : 0U;
  const uint32_t ver_start = (sp2 < line_len) ? sp2 + 1U : line_len;
  const uint32_t ver_len = line_len - ver_start;
  bool ok = (0U != method_len) && (0U != uri_len) && (0U != ver_len) && (method_len <= kMaxMethodBytes) &&
            (uri_len <= kMaxUriBytes) && (ver_len <= kMaxVersionBytes);
  for (uint32_t i = 0U; ok && i < method_len; ++i) {
    ok = IsTokenChar(p[i]);
  }
  for (uint32_t i = 0U; ok && i < uri_len; ++i) {
    const uint8_t c = p[sp1 + 1U + i];
    ok = (c >= 0x21 && c <= 0x7E);
  }
  for (uint32_t i = 0U; ok && i < ver_len; ++i) {
    const uint8_t c = p[ver_start + i];
    ok = (c >= 0x21 && c <= 0x7E);
  }
  if (ok) {
    req.method_raw = osp::byte_span(reinterpret_cast<const std::byte*>(p), method_len);
    req.uri = osp::byte_span(reinterpret_cast<const std::byte*>(p + sp1 + 1U), uri_len);
    req.version = osp::byte_span(reinterpret_cast<const std::byte*>(p + ver_start), ver_len);
    req.method = ParseMethod(req.method_raw);
  }
  return ok;
}

bool ParseHeaderLine(const uint8_t* p, uint32_t start, uint32_t eol, HeaderView& out) noexcept {
  uint32_t colon = start;
  while (colon < eol && ':' != p[colon]) {
    ++colon;
  }
  bool ok = (colon > start) && (colon < eol) && ((eol - start) <= kMaxHeaderLineBytes);
  for (uint32_t i = start; ok && i < colon; ++i) {
    ok = IsTokenChar(p[i]);
  }
  uint32_t vs = colon + 1U;
  uint32_t ve = eol;
  while (vs < ve && (' ' == p[vs] || '\t' == p[vs])) {
    ++vs;
  }
  while (ve > vs && (' ' == p[ve - 1U] || '\t' == p[ve - 1U])) {
    --ve;
  }
  for (uint32_t i = vs; ok && i < ve; ++i) {
    ok = (p[i] >= 0x20 || '\t' == p[i]) && (0x7F != p[i]);
  }
  if (ok) {
    out.name = osp::byte_span(reinterpret_cast<const std::byte*>(p + start), colon - start);
    out.value = osp::byte_span(reinterpret_cast<const std::byte*>(p + vs), ve - vs);
  }
  return ok;
}

}  // namespace

RtspParser::RtspParser() noexcept = default;

void RtspParser::Reset() noexcept { len_ = 0U; }

osp::expected<void, ParseError> RtspParser::Feed(const uint8_t* data, uint32_t len,
                                                 const ParserCallbacks& cb) noexcept {
  if (nullptr == data && 0U != len) {
    return osp::expected<void, ParseError>::error(ParseError::kMalformed);
  }
  // Make room by consuming complete messages before declaring overflow.
  if (len > (kRtspRxBytes - len_)) {
    const auto drained = Drain(cb);
    if (!drained) {
      return drained;
    }
  }
  if (len > (kRtspRxBytes - len_)) {
    return osp::expected<void, ParseError>::error(ParseError::kOverflow);
  }
  if (0U != len) {
    std::memcpy(buf_ + len_, data, len);
    len_ += len;
  }
  return Drain(cb);
}

osp::expected<void, ParseError> RtspParser::Drain(const ParserCallbacks& cb) noexcept {
  uint32_t pos = 0U;
  bool ok = true;
  ParseError err = ParseError::kMalformed;
  while (ok && pos < len_) {
    if ('$' == buf_[pos]) {
      if ((len_ - pos) < 4U) {
        break;  // Header not fully received; keep it for the next Feed().
      }
      const uint32_t payload_len = (static_cast<uint32_t>(buf_[pos + 2U]) << 8U) |
                                   static_cast<uint32_t>(buf_[pos + 3U]);
      if (0U == payload_len || payload_len > kMaxInterleavedPayload) {
        ok = false;
        err = ParseError::kBadInterleaved;
      } else if ((len_ - pos) < (4U + payload_len)) {
        break;  // Payload not fully received.
      } else {
        if (nullptr != cb.on_interleaved) {
          InterleavedFrame frame;
          frame.channel = buf_[pos + 1U];
          frame.payload = osp::byte_span(reinterpret_cast<const std::byte*>(buf_ + pos + 4U), payload_len);
          cb.on_interleaved(cb.ctx, frame);
        }
        pos += 4U + payload_len;
      }
    } else {
      uint32_t head_end = 0U;
      if (!FindCrlfCrlf(buf_, pos, len_, head_end)) {
        if ((len_ - pos) > kMaxHeaderBytes) {
          ok = false;
        }
        break;
      }
      const uint32_t head_len = head_end + 4U - pos;
      uint32_t header_count = 0U;
      if (head_len > kMaxHeaderBytes || !CountHeaderLines(buf_ + pos, head_len, header_count)) {
        ok = false;
      } else if (header_count > kMaxHeaderCount) {
        ok = false;
        err = ParseError::kTooManyHeaders;
      } else {
        RtspRequest req;
        if (!ParseHead(reinterpret_cast<const char*>(buf_ + pos), head_len, req)) {
          ok = false;
        } else {
          uint32_t body_len = 0U;
          bool have_length = false;
          for (uint8_t i = 0U; ok && i < req.header_count; ++i) {
            if (EqualsIgnoreCase(req.headers[i].name, "content-length")) {
              uint32_t value = 0U;
              if (!ParseDecimal(req.headers[i].value, value) || (have_length && value != body_len)) {
                ok = false;
              } else {
                body_len = value;
                have_length = true;
              }
            }
          }
          if (ok && body_len > kMaxBodyBytes) {
            ok = false;
          }
          if (ok) {
            const uint32_t total = head_len + body_len;
            if ((len_ - pos) < total) {
              break;  // Body not fully received.
            }
            req.body = osp::byte_span(reinterpret_cast<const std::byte*>(buf_ + pos + head_len), body_len);
            if (nullptr != cb.on_request) {
              cb.on_request(cb.ctx, req);
            }
            pos += total;
          }
        }
      }
    }
  }
  if (!ok) {
    len_ = 0U;  // The connection is closed by the owner; drop the partial bytes.
    return osp::expected<void, ParseError>::error(err);
  }
  if (pos > 0U) {
    const uint32_t remain = len_ - pos;
    if (remain > 0U) {
      std::memmove(buf_, buf_ + pos, remain);
    }
    len_ = remain;
  }
  return osp::expected<void, ParseError>::success();
}

bool RtspParser::ParseHead(const char* head, uint32_t head_len, RtspRequest& req) noexcept {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(head);
  uint32_t eol = 0U;
  if (!FindCrlf(p, 0U, head_len, eol) || !ParseStartLine(p, eol, req)) {
    return false;
  }
  uint32_t pos = eol + 2U;
  bool ok = true;
  while (ok && pos < head_len) {
    if ('\r' == p[pos]) {
      ok = (pos + 2U == head_len);
      pos = head_len;
    } else {
      uint32_t line_end = 0U;
      if (!FindCrlf(p, pos, head_len, line_end)) {
        ok = false;
      } else if (req.header_count >= kMaxHeaderCount) {
        ok = false;
      } else if (!ParseHeaderLine(p, pos, line_end, req.headers[req.header_count])) {
        ok = false;
      } else {
        ++req.header_count;
        pos = line_end + 2U;
      }
    }
  }
  return ok && (pos == head_len);
}

bool GetHeader(const RtspRequest& req, const char* name, osp::byte_span& out) noexcept {
  bool found = false;
  for (uint8_t i = 0U; nullptr != name && !found && i < req.header_count; ++i) {
    if (EqualsIgnoreCase(req.headers[i].name, name)) {
      out = req.headers[i].value;
      found = true;
    }
  }
  return found;
}

bool GetHeaderU32(const RtspRequest& req, const char* name, uint32_t& out) noexcept {
  osp::byte_span value;
  return GetHeader(req, name, value) && ParseDecimal(value, out);
}

bool CopyHeader(const RtspRequest& req, const char* name, char* dst, uint32_t cap, uint32_t& out_len) noexcept {
  osp::byte_span value;
  bool ok = (nullptr != dst) && (0U != cap) && GetHeader(req, name, value) &&
            (value.size() < static_cast<size_t>(cap));
  if (ok) {
    out_len = static_cast<uint32_t>(value.size());
    if (out_len > 0U) {
      std::memcpy(dst, value.data(), out_len);
    }
    dst[out_len] = '\0';
  }
  return ok;
}

const char* MethodName(Method m) noexcept {
  const char* name = "UNKNOWN";
  switch (m) {
    case Method::kOptions:
      name = "OPTIONS";
      break;
    case Method::kDescribe:
      name = "DESCRIBE";
      break;
    case Method::kSetup:
      name = "SETUP";
      break;
    case Method::kPlay:
      name = "PLAY";
      break;
    case Method::kPause:
      name = "PAUSE";
      break;
    case Method::kTeardown:
      name = "TEARDOWN";
      break;
    case Method::kGetParameter:
      name = "GET_PARAMETER";
      break;
    case Method::kUnknown:
    default:
      name = "UNKNOWN";
      break;
  }
  return name;
}

Method ParseMethod(osp::byte_span raw) noexcept {
  Method method = Method::kUnknown;
  if (EqualsLiteral(raw, "OPTIONS")) {
    method = Method::kOptions;
  } else if (EqualsLiteral(raw, "DESCRIBE")) {
    method = Method::kDescribe;
  } else if (EqualsLiteral(raw, "SETUP")) {
    method = Method::kSetup;
  } else if (EqualsLiteral(raw, "PLAY")) {
    method = Method::kPlay;
  } else if (EqualsLiteral(raw, "PAUSE")) {
    method = Method::kPause;
  } else if (EqualsLiteral(raw, "TEARDOWN")) {
    method = Method::kTeardown;
  } else if (EqualsLiteral(raw, "GET_PARAMETER")) {
    method = Method::kGetParameter;
  }
  return method;
}

}  // namespace rs
