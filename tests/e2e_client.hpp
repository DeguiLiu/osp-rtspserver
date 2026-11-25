// Minimal RTSP + RTP + H.264/H.265 depacketizing test client (cold path only).
// Uses raw BSD sockets with non-blocking I/O and explicit deadlines so a
// missing or stalled server fails the test instead of hanging it.
// SPDX-License-Identifier: MIT
#ifndef RS_TESTS_E2E_CLIENT_HPP_
#define RS_TESTS_E2E_CLIENT_HPP_

#include <cstddef>
#include <cstdint>

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace rse2e {

enum class Codec : uint8_t { kH264 = 0U, kH265 = 1U };

struct Response {
  uint32_t status = 0U;
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;

  // Case-insensitive header lookup; returns an empty string when absent.
  std::string Get(const std::string& name) const;
  bool Has(const std::string& name) const;
};

struct MediaFrame {
  uint8_t channel = 0U;
  std::vector<uint8_t> payload;
};

struct RtpPacket {
  bool valid = false;
  uint8_t payload_type = 0U;
  uint16_t seq = 0U;
  uint32_t timestamp = 0U;
  bool marker = false;
  std::vector<uint8_t> payload;
};

class Client {
 public:
  Client() noexcept = default;
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  bool Connect(const std::string& host, uint16_t port, int timeout_ms, std::string& err);
  void Close() noexcept;
  bool IsOpen() const noexcept { return fd_ >= 0; }
  int Fd() const noexcept { return fd_; }

  bool Request(const std::string& method, const std::string& uri,
               const std::vector<std::string>& headers, const std::string& body, std::string& err);
  bool ReadResponse(Response& out, int timeout_ms, std::string& err);
  bool ReadInterleaved(MediaFrame& out, int timeout_ms, std::string& err);
  bool HasPendingMedia() const noexcept { return !media_.empty(); }

  // True when the peer closed the connection (EOF observed).
  bool PeerClosed() const noexcept { return peer_closed_; }
  size_t Buffered() const noexcept { return buf_.size() - parse_off_; }
  uint32_t interleaved_frames() const noexcept { return interleaved_frames_; }
  uint32_t stray_responses() const noexcept { return stray_responses_; }

 private:
  bool Fill(int timeout_ms, std::string& err);
  bool PumpOnce(bool& consumed, std::string& err);
  bool PumpUntil(bool need_response, int timeout_ms, std::string& err);
  bool SendAll(const std::string& data, int timeout_ms, std::string& err);
  void Compact();

  int fd_ = -1;
  std::vector<uint8_t> buf_;
  size_t parse_off_ = 0U;
  std::deque<MediaFrame> media_;
  std::deque<Response> responses_;
  bool peer_closed_ = false;
  bool awaiting_response_ = false;
  uint32_t interleaved_frames_ = 0U;
  uint32_t stray_responses_ = 0U;
};

bool ParseRtp(const uint8_t* data, size_t len, RtpPacket& out);

// Reassembles single NAL and FU-A payloads; skips STAP-A (never emitted by
// this server) and reserved type 0.
class H264Depacketizer {
 public:
  void Reset();

  // Appends one packet; returns true when `nal` holds a complete NAL unit.
  bool Push(const RtpPacket& pkt, std::vector<uint8_t>& nal);

  uint32_t packets() const noexcept { return packets_; }
  uint32_t seq_gaps() const noexcept { return seq_gaps_; }
  uint32_t truncated_fu() const noexcept { return truncated_fu_; }
  bool HasSeq() const noexcept { return have_seq_; }
  uint16_t LastSeq() const noexcept { return last_seq_; }

 private:
  uint32_t packets_ = 0U;
  uint32_t seq_gaps_ = 0U;
  uint32_t truncated_fu_ = 0U;
  uint16_t last_seq_ = 0U;
  bool have_seq_ = false;
  bool in_fu_ = false;
  std::vector<uint8_t> fu_buf_;
};

// Reassembles RFC 7798 single-NAL and FU (type 49) payloads. Aggregation
// packets (type 48) are counted and dropped: this server never emits them.
class H265Depacketizer {
 public:
  void Reset();

  // Appends one packet; returns true when `nal` holds a complete NAL unit
  // including its 2-byte NAL header.
  bool Push(const RtpPacket& pkt, std::vector<uint8_t>& nal);

  uint32_t packets() const noexcept { return packets_; }
  uint32_t seq_gaps() const noexcept { return seq_gaps_; }
  uint32_t truncated_fu() const noexcept { return truncated_fu_; }
  uint32_t aggregation_packets() const noexcept { return aggregation_packets_; }
  uint32_t fu_packets() const noexcept { return fu_packets_; }
  uint32_t single_nal_packets() const noexcept { return single_nal_packets_; }

 private:
  uint32_t packets_ = 0U;
  uint32_t seq_gaps_ = 0U;
  uint32_t truncated_fu_ = 0U;
  uint32_t aggregation_packets_ = 0U;
  uint32_t fu_packets_ = 0U;
  uint32_t single_nal_packets_ = 0U;
  uint16_t last_seq_ = 0U;
  bool have_seq_ = false;
  bool in_fu_ = false;
  std::vector<uint8_t> fu_buf_;
};

// Codec-specific NAL classification. Parameter-set and delimiter types differ
// between the two formats, so every scan needs the codec that produced it.
uint8_t NalTypeOf(Codec codec, const uint8_t* nal, size_t len) noexcept;
bool IsParamSetType(Codec codec, uint8_t type) noexcept;
bool IsAudType(Codec codec, uint8_t type) noexcept;
bool IsSeiType(Codec codec, uint8_t type) noexcept;
bool IsIrapType(Codec codec, uint8_t type) noexcept;
const char* CodecLabel(Codec codec) noexcept;

// Picks the depacketizer from the SDP the server actually sent: rtpmap must
// name exactly one of H264/H265 (sprop-vps is accepted as H.265 evidence).
bool CodecFromSdp(const std::string& sdp, Codec& out);

struct NalUnit {
  uint8_t type = 0U;
  std::vector<uint8_t> bytes;  // Annex B start code removed.
};

struct AccessUnit {
  bool has_aud = false;
  std::vector<NalUnit> nals;
};

bool HasIrap(const AccessUnit& au, Codec codec) noexcept;

std::vector<NalUnit> SplitAnnexB(const std::vector<uint8_t>& data, Codec codec);
std::vector<AccessUnit> GroupAus(const std::vector<NalUnit>& nals, Codec codec);
// Drops AUD and SEI, which the server is allowed not to forward.
std::vector<NalUnit> NormalizeForCompare(const std::vector<NalUnit>& nals, Codec codec);

struct StreamFile {
  std::vector<NalUnit> nals;
  std::vector<AccessUnit> aus;
};

bool LoadStreamFile(const std::string& path, Codec codec, StreamFile& out, std::string& err);

// Byte comparison that tolerates extra leading SPS/PPS copies on the wire.
bool CompareNals(const std::vector<NalUnit>& expected, const std::vector<NalUnit>& actual, Codec codec,
                 std::string& err);

// fork/exec with bounded wait. Returns false only when the child could not be
// started or reaped; exit_code is valid otherwise.
bool RunProcess(const std::vector<std::string>& argv, int timeout_ms, std::string& stdout_log,
                std::string& stderr_log, int& exit_code);

std::string MakeUri(const std::string& host, uint16_t port, const std::string& path);

}  // namespace rse2e

#endif  // RS_TESTS_E2E_CLIENT_HPP_
