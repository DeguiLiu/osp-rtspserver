// Incremental RTSP 1.0 request parser with interleaved ('$') frame support.
// Socket-free, allocation-free, bounded: the owner feeds bytes and receives
// views into the parser's internal buffer for the duration of the callback.
// SPDX-License-Identifier: MIT
#ifndef RS_RTSP_PARSER_HPP_
#define RS_RTSP_PARSER_HPP_

#include "osp/span.hpp"
#include "osp/vocabulary.hpp"
#include "rs/config.hpp"

#include <cstdint>

namespace rs {

enum class Method : uint8_t { kOptions = 0, kDescribe, kSetup, kPlay, kPause, kTeardown, kGetParameter, kUnknown };

struct HeaderView {
  osp::byte_span name;
  osp::byte_span value;
};

struct RtspRequest {
  Method method = Method::kUnknown;
  osp::byte_span method_raw;
  osp::byte_span uri;
  osp::byte_span version;
  HeaderView headers[kMaxHeaderCount] = {};
  uint8_t header_count = 0;
  osp::byte_span body;
};

struct InterleavedFrame {
  uint8_t channel = 0;
  osp::byte_span payload;
};

struct ParserCallbacks {
  void* ctx = nullptr;
  void (*on_request)(void*, const RtspRequest&) = nullptr;
  void (*on_interleaved)(void*, const InterleavedFrame&) = nullptr;
};

enum class ParseError : uint8_t {
  kOverflow = 0,    // Buffer full without a complete message.
  kMalformed,       // Start line / header syntax error.
  kBadInterleaved,  // '$' frame length exceeds the buffer budget.
  kTooManyHeaders
};

// Case-insensitive helpers over a parsed request.
bool GetHeader(const RtspRequest& req, const char* name, osp::byte_span& out) noexcept;
bool GetHeaderU32(const RtspRequest& req, const char* name, uint32_t& out) noexcept;
// Copies a header value verbatim (NUL-terminated) into dst; false if absent or too long.
bool CopyHeader(const RtspRequest& req, const char* name, char* dst, uint32_t cap, uint32_t& out_len) noexcept;
const char* MethodName(Method m) noexcept;
Method ParseMethod(osp::byte_span raw) noexcept;

// Feed-driven parser. One instance per connection, used only on the loop thread.
class RtspParser {
 public:
  RtspParser() noexcept;
  RtspParser(const RtspParser&) = delete;
  RtspParser& operator=(const RtspParser&) = delete;

  void Reset() noexcept;

  // Consumes all complete messages in the buffer, invoking callbacks for each.
  // Incomplete trailing bytes are retained for the next Feed().
  [[nodiscard]] osp::expected<void, ParseError> Feed(const uint8_t* data, uint32_t len,
                                                     const ParserCallbacks& cb) noexcept;

  uint32_t buffered() const noexcept { return len_; }

 private:
  [[nodiscard]] osp::expected<void, ParseError> Drain(const ParserCallbacks& cb) noexcept;
  [[nodiscard]] static bool ParseHead(const char* head, uint32_t head_len, RtspRequest& req) noexcept;

  alignas(kCacheLineBytes) uint8_t buf_[kRtspRxBytes] = {};
  RtspRequest request_{};
  uint32_t len_ = 0U;
  static_assert((kRtspRxBytes % kCacheLineBytes) == 0U, "RTSP rx buffer must be cache-line aligned");
};

static_assert(alignof(RtspParser) >= kCacheLineBytes, "RtspParser must start on a cache line");

}  // namespace rs

#endif  // RS_RTSP_PARSER_HPP_
