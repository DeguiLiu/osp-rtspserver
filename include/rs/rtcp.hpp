// Minimal RTCP: build a Sender Report, parse a Receiver Report block.
// SPDX-License-Identifier: MIT
#ifndef RS_RTCP_HPP_
#define RS_RTCP_HPP_

#include "osp/span.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>

namespace rs {

enum class RtcpError : uint8_t { kBufferTooSmall = 0, kMalformed };

struct RrReport {
  uint32_t ssrc = 0U;
  uint8_t fraction_lost = 0U;
  int32_t cumulative_lost = 0;
  uint32_t extended_highest_seq = 0U;
  uint32_t jitter = 0U;
  uint32_t last_sr = 0U;
  uint32_t delay_since_sr = 0U;
};

// 28 bytes: header + sender info (no report blocks).
[[nodiscard]] osp::expected<uint32_t, RtcpError> BuildSenderReport(osp::writable_byte_span out, uint32_t ssrc,
                                                                   uint64_t ntp_now_ns, uint32_t rtp_ts,
                                                                   uint32_t packet_count,
                                                                   uint32_t octet_count) noexcept;

// Summary of one received compound RTCP packet.
struct RtcpSummary {
  uint32_t sender_reports = 0U;
  uint32_t receiver_reports = 0U;
  uint32_t byes = 0U;
  RrReport first_rr{};
  bool has_rr = false;
};

// Accepts a compound packet and returns the first RR report block found.
[[nodiscard]] osp::expected<RrReport, RtcpError> ParseReceiverReport(const uint8_t* data, uint32_t len) noexcept;

// Parses every packet in a compound datagram (SR/RR/BYE); malformed input is
// rejected as a whole so a bad length cannot desynchronise the parser.
[[nodiscard]] osp::expected<RtcpSummary, RtcpError> ParseCompound(const uint8_t* data, uint32_t len) noexcept;

// SR + SDES(CNAME) compound, the periodic sender report required for unicast.
[[nodiscard]] osp::expected<uint32_t, RtcpError> BuildSenderReportWithSdes(osp::writable_byte_span out, uint32_t ssrc,
                                                                           const char* cname, uint64_t ntp_now_ns,
                                                                           uint32_t rtp_ts, uint32_t packet_count,
                                                                           uint32_t octet_count) noexcept;

}  // namespace rs

#endif  // RS_RTCP_HPP_
