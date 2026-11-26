// RTSP/RTP/H.264+H.265 test client implementation. Cold path: std containers and
// heap are allowed here; this file never ships in the server binary.
// SPDX-License-Identifier: MIT
#include "e2e_client.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <chrono>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rse2e {
namespace {

constexpr uint32_t kMaxBufferedBytes = 4U * 1024U * 1024U;
constexpr size_t kMaxFrameBytes = 65535U;

int64_t NowMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int RemainingMs(int64_t deadline) noexcept {
  const int64_t left = deadline - NowMs();
  return (left > 0) ? static_cast<int>(left) : 0;
}

std::string Lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

std::string Trim(const std::string& text) {
  size_t first = 0U;
  size_t last = text.size();
  while (first < last && (text[first] == ' ' || text[first] == '\t')) {
    ++first;
  }
  while (last > first && (text[last - 1U] == ' ' || text[last - 1U] == '\t')) {
    --last;
  }
  return text.substr(first, last - first);
}

bool StartsWith(const uint8_t* data, size_t len, const char* text) noexcept {
  const size_t text_len = std::strlen(text);
  return (len >= text_len) && (0 == std::memcmp(data, text, text_len));
}

size_t FindHeaderEnd(const uint8_t* data, size_t len) noexcept {
  size_t found = 0U;
  for (size_t i = 0U; i + 3U < len && 0U == found; ++i) {
    if ('\r' == data[i] && '\n' == data[i + 1U] && '\r' == data[i + 2U] && '\n' == data[i + 3U]) {
      found = i + 4U;
    }
  }
  return found;
}

bool ParseContentLength(const std::vector<std::pair<std::string, std::string>>& headers, size_t& out) noexcept {
  bool ok = true;
  out = 0U;
  for (const auto& header : headers) {
    if (0 == header.first.compare("content-length")) {
      char* end = nullptr;
      const unsigned long value = std::strtoul(header.second.c_str(), &end, 10);
      ok = (nullptr != end) && (0 == *end) && (value <= kMaxBufferedBytes);
      out = ok ? static_cast<size_t>(value) : 0U;
    }
  }
  return ok;
}

std::string ReadFileText(const char* path) {
  std::string text;
  std::FILE* file = std::fopen(path, "rb");
  if (nullptr != file) {
    char chunk[4096];
    size_t n = 0U;
    while ((n = std::fread(chunk, 1U, sizeof(chunk), file)) > 0U) {
      text.append(chunk, n);
    }
    std::fclose(file);
  }
  return text;
}

}  // namespace

std::string Response::Get(const std::string& name) const {
  const std::string wanted = Lower(name);
  std::string value;
  for (const auto& header : headers) {
    if (header.first == wanted) {
      value = header.second;
    }
  }
  return value;
}

bool Response::Has(const std::string& name) const {
  const std::string wanted = Lower(name);
  for (const auto& header : headers) {
    if (header.first == wanted) {
      return true;
    }
  }
  return false;
}

std::string MakeUri(const std::string& host, uint16_t port, const std::string& path) {
  return "rtsp://" + host + ":" + std::to_string(port) + path;
}

Client::~Client() {
  Close();
}

void Client::Close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool Client::Connect(const std::string& host, uint16_t port, int timeout_ms, std::string& err) {
  Close();
  buf_.clear();
  parse_off_ = 0U;
  media_.clear();
  responses_.clear();
  peer_closed_ = false;
  awaiting_response_ = false;
  interleaved_frames_ = 0U;
  stray_responses_ = 0U;

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    err = "cannot resolve " + host + ": " + std::strerror(errno);
    return false;
  }

  // A busy server can delay accept() briefly after a connection burst; retry a
  // bounded number of times so a transient backlog does not fail the suite.
  constexpr uint32_t kConnectAttempts = 3U;
  bool ok = false;
  std::string last_err = "connect failed";
  for (uint32_t attempt = 0U; attempt < kConnectAttempts && ok == false; ++attempt) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      last_err = std::string("socket: ") + std::strerror(errno);
      break;
    }
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    static_cast<void>(::fcntl(fd_, F_SETFL, flags | O_NONBLOCK));
    if (0 == ::connect(fd_, static_cast<const struct sockaddr*>(static_cast<const void*>(&addr)), sizeof(addr))) {
      ok = true;
    } else if (EINPROGRESS != errno) {
      last_err = (ECONNREFUSED == errno) ? "connection refused (listener closed or not bound)" : std::strerror(errno);
    } else {
      struct pollfd pfd{};
      pfd.fd = fd_;
      pfd.events = POLLOUT;
      const int ready = ::poll(&pfd, 1, timeout_ms);
      int so_error = 0;
      socklen_t so_len = static_cast<socklen_t>(sizeof(so_error));
      if (ready > 0 && ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &so_error, &so_len) == 0) {
        ok = (0 == so_error);
        last_err = ok ? std::string()
                      : (ECONNREFUSED == so_error ? "connection refused (listener closed or not bound)"
                                                  : std::strerror(so_error));
      } else if (0 == ready) {
        last_err = "connect timeout";
      } else {
        last_err = std::strerror(errno);
      }
    }
    if (ok == false) {
      Close();
      struct timespec pause{0, 50000000};
      static_cast<void>(::nanosleep(&pause, nullptr));
    }
  }
  if (ok == false) {
    err = "connect to " + host + ":" + std::to_string(port) + " failed: " + last_err;
  } else {
    const int one = 1;
    static_cast<void>(::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)));
  }
  return ok;
}

bool Client::SendAll(const std::string& data, int timeout_ms, std::string& err) {
  const int64_t deadline = NowMs() + timeout_ms;
  size_t sent = 0U;
  bool ok = true;
  while (ok && sent < data.size()) {
    const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
    } else if (n < 0 && (EAGAIN == errno || EWOULDBLOCK == errno || EINTR == errno)) {
      struct pollfd pfd{};
      pfd.fd = fd_;
      pfd.events = POLLOUT;
      const int wait_ms = RemainingMs(deadline);
      if (::poll(&pfd, 1, wait_ms) <= 0) {
        err = "timeout writing request";
        ok = false;
      }
    } else {
      err = std::string("send failed: ") + std::strerror(errno);
      ok = false;
    }
  }
  return ok;
}

bool Client::Request(const std::string& method, const std::string& uri, const std::vector<std::string>& headers,
                     const std::string& body, std::string& err) {
  if (fd_ < 0) {
    err = "request on closed client";
    return false;
  }
  std::string request = method + " " + uri + " RTSP/1.0\r\n";
  for (const auto& header : headers) {
    request += header;
    request += "\r\n";
  }
  if (body.empty() == false || "DESCRIBE" == method) {
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += "\r\n";
  request += body;
  return SendAll(request, 3000, err);
}

void Client::Compact() {
  if (parse_off_ == buf_.size()) {
    buf_.clear();
    parse_off_ = 0U;
  } else if (parse_off_ > 4096U) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(parse_off_));
    parse_off_ = 0U;
  }
}

bool Client::PumpOnce(bool& consumed, std::string& err) {
  consumed = false;
  const size_t avail = buf_.size() - parse_off_;
  const uint8_t* data = buf_.data() + parse_off_;
  bool ok = true;
  if (avail > 0U && '$' == data[0]) {
    if (avail < 4U) {
      return true;
    }
    const uint32_t length = (static_cast<uint32_t>(data[2]) << 8U) | data[3];
    // Interleaved channels are 0..127; the caller decides which one carries
    // RTP (the server supports interleaved=0-1 as well as e.g. 2-3).
    if (0U == length || data[1] > 127U) {
      err = "invalid interleaved frame: channel=" + std::to_string(data[1]) + " length=" + std::to_string(length);
      ok = false;
    } else if (avail < 4U + length) {
      return true;
    } else {
      MediaFrame frame;
      frame.channel = data[1];
      frame.payload.assign(data + 4U, data + 4U + length);
      media_.push_back(std::move(frame));
      ++interleaved_frames_;
      parse_off_ += 4U + length;
      consumed = true;
    }
  } else if (avail > 0U) {
    constexpr size_t kStatusPrefix = 9U;  // "RTSP/1.0 "
    const bool looks_like_status =
        (avail >= kStatusPrefix) ? StartsWith(data, avail, "RTSP/1.0 ") : (0 == std::memcmp(data, "RTSP/1.0 ", avail));
    if (looks_like_status == false) {
      char byte_text[16];
      std::snprintf(byte_text, sizeof(byte_text), "0x%02X", data[0]);
      err = std::string("unexpected byte ") + byte_text + " while waiting for RTSP data";
      ok = false;
    } else if (avail < kStatusPrefix) {
      return true;
    } else {
      const size_t head_len = FindHeaderEnd(data, avail);
      if (0U == head_len) {
        return true;
      }
      Response response;
      std::vector<std::pair<std::string, std::string>> headers;
      const std::string head(static_cast<const char*>(static_cast<const void*>(data)), head_len);
      const size_t first_eol = head.find("\r\n");
      const std::string status_line = head.substr(0U, first_eol);
      char* end = nullptr;
      const size_t space = status_line.find(' ');
      const unsigned long status =
          (std::string::npos != space) ? std::strtoul(status_line.c_str() + space + 1U, &end, 10) : 0UL;
      if (std::string::npos == space || nullptr == end || status < 100UL || status > 999UL) {
        err = "malformed RTSP status line: " + status_line;
        ok = false;
      } else {
        response.status = static_cast<uint32_t>(status);
        response.reason = Trim(status_line.substr(static_cast<size_t>(end - status_line.c_str())));
        size_t pos = first_eol + 2U;
        while (pos < head.size()) {
          const size_t eol = head.find("\r\n", pos);
          const std::string line = head.substr(pos, eol - pos);
          const size_t colon = line.find(':');
          if (std::string::npos != colon) {
            headers.emplace_back(Lower(Trim(line.substr(0U, colon))), Trim(line.substr(colon + 1U)));
          }
          pos = (std::string::npos == eol) ? head.size() : eol + 2U;
        }
        size_t body_len = 0U;
        ok = ParseContentLength(headers, body_len);
        if (ok == false) {
          err = "invalid Content-Length in response";
        } else if (avail < head_len + body_len) {
          return true;
        } else {
          response.headers = std::move(headers);
          response.body.assign(static_cast<const char*>(static_cast<const void*>(data + head_len)), body_len);
          if (awaiting_response_ == false) {
            ++stray_responses_;
          }
          responses_.push_back(std::move(response));
          parse_off_ += head_len + body_len;
          consumed = true;
        }
      }
    }
  }
  if (consumed) {
    Compact();
  }
  return ok;
}

bool Client::Fill(int timeout_ms, std::string& err) {
  const int64_t deadline = NowMs() + timeout_ms;
  bool ok = true;
  bool got_data = false;
  while (ok && got_data == false) {
    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    const int wait_ms = RemainingMs(deadline);
    const int ready = ::poll(&pfd, 1, wait_ms);
    if (0 == ready) {
      err = "timeout waiting for server data";
      ok = false;
    } else if (ready < 0) {
      if (EINTR != errno) {
        err = std::string("poll failed: ") + std::strerror(errno);
        ok = false;
      }
    } else {
      uint8_t chunk[8192];
      const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (0 == n) {
        peer_closed_ = true;
        err = "peer closed the connection";
        ok = false;
      } else if (n > 0) {
        buf_.insert(buf_.end(), chunk, chunk + n);
        got_data = true;
        if (buf_.size() > kMaxBufferedBytes) {
          err = "client receive buffer overflow";
          ok = false;
        }
      } else if (ECONNRESET == errno || EPIPE == errno || ENOTCONN == errno) {
        // A server that closes with RST also counts as a closed peer.
        peer_closed_ = true;
        err = std::string("peer reset the connection: ") + std::strerror(errno);
        ok = false;
      } else if (EAGAIN != errno && EWOULDBLOCK != errno && EINTR != errno) {
        err = std::string("recv failed: ") + std::strerror(errno);
        ok = false;
      }
    }
  }
  return ok;
}

bool Client::PumpUntil(bool need_response, int timeout_ms, std::string& err) {
  const int64_t deadline = NowMs() + timeout_ms;
  bool ok = true;
  for (;;) {
    bool consumed = true;
    while (ok && consumed) {
      ok = PumpOnce(consumed, err);
    }
    const bool ready = need_response ? responses_.empty() == false : media_.empty() == false;
    if (ok == false || ready) {
      break;
    }
    const int wait_ms = RemainingMs(deadline);
    if (0 == wait_ms) {
      err = need_response ? "timeout waiting for RTSP response" : "timeout waiting for interleaved media";
      ok = false;
    } else {
      ok = Fill(wait_ms, err);
    }
  }
  return ok;
}

bool Client::ReadResponse(Response& out, int timeout_ms, std::string& err) {
  bool ok = true;
  if (responses_.empty()) {
    awaiting_response_ = true;
    ok = PumpUntil(true, timeout_ms, err);
    awaiting_response_ = false;
  }
  if (ok && responses_.empty() == false) {
    out = std::move(responses_.front());
    responses_.pop_front();
  } else if (ok) {
    err = "no RTSP response available";
    ok = false;
  }
  return ok;
}

bool Client::ReadInterleaved(MediaFrame& out, int timeout_ms, std::string& err) {
  bool ok = true;
  if (media_.empty()) {
    ok = PumpUntil(false, timeout_ms, err);
  }
  if (ok && media_.empty() == false) {
    out = std::move(media_.front());
    media_.pop_front();
  } else if (ok) {
    err = "no interleaved frame available";
    ok = false;
  }
  return ok;
}

bool ParseRtp(const uint8_t* data, size_t len, RtpPacket& out) {
  out = RtpPacket{};
  bool ok = (len >= 12U) && ((data[0] >> 6U) == 2U);
  size_t header_len = 12U;
  if (ok) {
    header_len += 4U * static_cast<size_t>(data[0] & 0x0FU);
    ok = header_len <= len;
  }
  if (ok && (data[0] & 0x10U) != 0) {
    ok = header_len + 4U <= len;
    if (ok) {
      const size_t ext_words = (static_cast<size_t>(data[header_len + 2U]) << 8U) | data[header_len + 3U];
      header_len += 4U + 4U * ext_words;
      ok = header_len <= len;
    }
  }
  size_t padding = 0U;
  if (ok && (data[0] & 0x20U) != 0) {
    padding = data[len - 1U];
    ok = header_len + padding <= len;
  }
  if (ok && header_len + padding > len) {
    ok = false;
  }
  if (ok) {
    out.valid = true;
    out.payload_type = static_cast<uint8_t>(data[1] & 0x7FU);
    out.marker = (data[1] & 0x80U) != 0;
    out.seq = static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8U) | data[3]);
    out.timestamp = (static_cast<uint32_t>(data[4]) << 24U) | (static_cast<uint32_t>(data[5]) << 16U) |
                    (static_cast<uint32_t>(data[6]) << 8U) | data[7];
    out.payload.assign(data + header_len, data + len - padding);
    out.valid = out.payload.empty() == false;
  }
  return ok && out.valid;
}

void H264Depacketizer::Reset() {
  packets_ = 0U;
  seq_gaps_ = 0U;
  truncated_fu_ = 0U;
  last_seq_ = 0U;
  have_seq_ = false;
  in_fu_ = false;
  fu_buf_.clear();
}

bool H264Depacketizer::Push(const RtpPacket& pkt, std::vector<uint8_t>& nal) {
  ++packets_;
  if (have_seq_) {
    const uint16_t expected = static_cast<uint16_t>(last_seq_ + 1U);
    if (pkt.seq != expected) {
      ++seq_gaps_;
      if (in_fu_) {
        ++truncated_fu_;
        in_fu_ = false;
        fu_buf_.clear();
      }
    }
  }
  last_seq_ = pkt.seq;
  have_seq_ = true;

  bool complete = false;
  if (pkt.payload.empty()) {
    return false;
  }
  const uint8_t type = static_cast<uint8_t>(pkt.payload[0] & 0x1FU);
  if (28U == type) {
    if (pkt.payload.size() < 2U) {
      ++truncated_fu_;
    } else {
      const bool start = (pkt.payload[1] & 0x80U) != 0;
      const bool end = (pkt.payload[1] & 0x40U) != 0;
      if (start) {
        if (in_fu_) {
          ++truncated_fu_;
        }
        fu_buf_.clear();
        fu_buf_.push_back(static_cast<uint8_t>((pkt.payload[0] & 0xE0U) | (pkt.payload[1] & 0x1FU)));
        fu_buf_.insert(fu_buf_.end(), pkt.payload.begin() + 2, pkt.payload.end());
        in_fu_ = true;
      } else if (in_fu_) {
        fu_buf_.insert(fu_buf_.end(), pkt.payload.begin() + 2, pkt.payload.end());
      }
      if (end && in_fu_) {
        nal = fu_buf_;
        fu_buf_.clear();
        in_fu_ = false;
        complete = true;
      }
    }
  } else if (24U != type && 0U != type) {
    nal = pkt.payload;
    complete = true;
  }
  return complete;
}

void H265Depacketizer::Reset() {
  packets_ = 0U;
  seq_gaps_ = 0U;
  truncated_fu_ = 0U;
  aggregation_packets_ = 0U;
  fu_packets_ = 0U;
  single_nal_packets_ = 0U;
  last_seq_ = 0U;
  have_seq_ = false;
  in_fu_ = false;
  fu_buf_.clear();
}

bool H265Depacketizer::Push(const RtpPacket& pkt, std::vector<uint8_t>& nal) {
  ++packets_;
  if (have_seq_) {
    const uint16_t expected = static_cast<uint16_t>(last_seq_ + 1U);
    if (pkt.seq != expected) {
      ++seq_gaps_;
      if (in_fu_) {
        ++truncated_fu_;
        in_fu_ = false;
        fu_buf_.clear();
      }
    }
  }
  last_seq_ = pkt.seq;
  have_seq_ = true;

  bool complete = false;
  if (pkt.payload.empty()) {
    return false;
  }
  // RFC 7798 payload header: F(1) | Type(6) | LayerId_hi(1), LayerId_lo(5) | TID(3).
  const uint8_t type = static_cast<uint8_t>((pkt.payload[0] >> 1U) & 0x3FU);
  if (49U == type) {
    ++fu_packets_;
    // FU: 2-byte payload header, then FU header with S|E|FuType.
    if (pkt.payload.size() < 3U) {
      ++truncated_fu_;
    } else {
      const uint8_t fu_header = pkt.payload[2];
      const bool start = (fu_header & 0x80U) != 0U;
      const bool end = (fu_header & 0x40U) != 0U;
      const uint8_t fu_type = static_cast<uint8_t>(fu_header & 0x3FU);
      if (start) {
        if (in_fu_) {
          ++truncated_fu_;
        }
        fu_buf_.clear();
        // Rebuild the NAL header: F and LayerId_hi from payload[0], Type from
        // FuType, LayerId_lo/TID from payload[1].
        fu_buf_.push_back(static_cast<uint8_t>((pkt.payload[0] & 0x81U) | static_cast<uint8_t>(fu_type << 1U)));
        fu_buf_.push_back(pkt.payload[1]);
        fu_buf_.insert(fu_buf_.end(), pkt.payload.begin() + 3, pkt.payload.end());
        in_fu_ = true;
      } else if (in_fu_) {
        fu_buf_.insert(fu_buf_.end(), pkt.payload.begin() + 3, pkt.payload.end());
      }
      if (end && in_fu_) {
        nal = fu_buf_;
        fu_buf_.clear();
        in_fu_ = false;
        complete = true;
      }
    }
  } else if (48U == type) {
    // Aggregation packet: not emitted by this server, cannot be reassembled here.
    ++aggregation_packets_;
  } else {
    // Single NAL unit packet: the 2-byte payload header is the NAL header.
    ++single_nal_packets_;
    if (pkt.payload.size() < 2U) {
      ++truncated_fu_;
    } else {
      nal = pkt.payload;
      complete = true;
    }
  }
  return complete;
}

uint8_t NalTypeOf(Codec codec, const uint8_t* nal, size_t len) noexcept {
  uint8_t type = 0U;
  if (Codec::kH265 == codec && len >= 2U) {
    type = static_cast<uint8_t>((nal[0] >> 1U) & 0x3FU);
  } else if (Codec::kH264 == codec && len >= 1U) {
    type = static_cast<uint8_t>(nal[0] & 0x1FU);
  }
  return type;
}

bool IsParamSetType(Codec codec, uint8_t type) noexcept {
  if (Codec::kH265 == codec) {
    return 32U == type || 33U == type || 34U == type;
  }
  return 7U == type || 8U == type;
}

bool IsAudType(Codec codec, uint8_t type) noexcept {
  return (Codec::kH265 == codec) ? (35U == type) : (9U == type);
}

bool IsSeiType(Codec codec, uint8_t type) noexcept {
  return (Codec::kH265 == codec) ? (39U == type || 40U == type) : (6U == type);
}

bool IsIrapType(Codec codec, uint8_t type) noexcept {
  if (Codec::kH265 == codec) {
    // BLA/IDR/CRA: 16..23.
    return type >= 16U && type <= 23U;
  }
  return 5U == type;
}

const char* CodecLabel(Codec codec) noexcept {
  return (Codec::kH265 == codec) ? "H265" : "H264";
}

bool CodecFromSdp(const std::string& sdp, Codec& out) {
  const bool h265 = (std::string::npos != sdp.find("H265/")) || (std::string::npos != sdp.find("sprop-vps="));
  const bool h264 = (std::string::npos != sdp.find("H264/"));
  bool ok = false;
  if (h265 && h264 == false) {
    out = Codec::kH265;
    ok = true;
  } else if (h264 && h265 == false) {
    out = Codec::kH264;
    ok = true;
  }
  return ok;
}

bool HasIrap(const AccessUnit& au, Codec codec) noexcept {
  for (const auto& nal : au.nals) {
    if (IsIrapType(codec, nal.type)) {
      return true;
    }
  }
  return false;
}

std::vector<NalUnit> SplitAnnexB(const std::vector<uint8_t>& data, Codec codec) {
  std::vector<NalUnit> nals;
  size_t nal_start = data.size();
  size_t i = 0U;
  while (i + 2U < data.size()) {
    if (0U == data[i] && 0U == data[i + 1U] && 1U == data[i + 2U]) {
      if (nal_start < i) {
        size_t end = i;
        while (end > nal_start && 0U == data[end - 1U]) {
          --end;
        }
        if (end > nal_start) {
          NalUnit nal;
          nal.type = NalTypeOf(codec, data.data() + nal_start, end - nal_start);
          nal.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(nal_start),
                           data.begin() + static_cast<std::ptrdiff_t>(end));
          nals.push_back(std::move(nal));
        }
      }
      i += 3U;
      nal_start = i;
    } else {
      ++i;
    }
  }
  if (nal_start < data.size()) {
    size_t end = data.size();
    while (end > nal_start && 0U == data[end - 1U]) {
      --end;
    }
    if (end > nal_start) {
      NalUnit nal;
      nal.type = NalTypeOf(codec, data.data() + nal_start, end - nal_start);
      nal.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(nal_start),
                       data.begin() + static_cast<std::ptrdiff_t>(end));
      nals.push_back(std::move(nal));
    }
  }
  return nals;
}

std::vector<AccessUnit> GroupAus(const std::vector<NalUnit>& nals, Codec codec) {
  std::vector<AccessUnit> aus;
  for (const auto& nal : nals) {
    const bool aud = IsAudType(codec, nal.type);
    if (aud || aus.empty()) {
      aus.emplace_back();
      aus.back().has_aud = aud;
    }
    aus.back().nals.push_back(nal);
  }
  return aus;
}

std::vector<NalUnit> NormalizeForCompare(const std::vector<NalUnit>& nals, Codec codec) {
  std::vector<NalUnit> out;
  out.reserve(nals.size());
  for (const auto& nal : nals) {
    if (IsAudType(codec, nal.type) == false && IsSeiType(codec, nal.type) == false) {
      NalUnit copy = nal;
      // Design 13.5 allows trailing zero bytes from Annex B framing.
      while (copy.bytes.size() > 1U && 0U == copy.bytes.back()) {
        copy.bytes.pop_back();
      }
      out.push_back(std::move(copy));
    }
  }
  return out;
}

bool LoadStreamFile(const std::string& path, Codec codec, StreamFile& out, std::string& err) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  bool ok = (nullptr != file);
  std::vector<uint8_t> data;
  if (ok) {
    ok = 0 == std::fseek(file, 0L, SEEK_END);
    const long size = ok ? std::ftell(file) : -1L;
    ok = ok && size >= 0 && std::fseek(file, 0L, SEEK_SET) == 0;
    if (ok) {
      data.resize(static_cast<size_t>(size));
      ok = (data.empty()) || (std::fread(data.data(), 1U, data.size(), file) == data.size());
    }
    std::fclose(file);
  }
  if (ok == false) {
    err = "cannot read stream file " + path;
  } else {
    out.nals = SplitAnnexB(data, codec);
    out.aus = GroupAus(out.nals, codec);
    ok = out.nals.empty() == false;
    if (ok == false) {
      err = "stream file " + path + " contains no Annex B NAL units";
    }
  }
  return ok;
}

bool CompareNals(const std::vector<NalUnit>& expected, const std::vector<NalUnit>& actual, Codec codec,
                 std::string& err) {
  size_t actual_index = 0U;
  bool ok = true;
  for (size_t expected_index = 0U; ok && expected_index < expected.size(); ++expected_index) {
    const NalUnit& want = expected[expected_index];
    while (actual_index < actual.size() && IsParamSetType(codec, actual[actual_index].type) &&
           want.type != actual[actual_index].type) {
      ++actual_index;
    }
    if (actual_index >= actual.size()) {
      err = "wire stream ended at expected NAL #" + std::to_string(expected_index) + " (type " +
            std::to_string(want.type) + ")";
      ok = false;
    } else {
      const NalUnit& got = actual[actual_index];
      if (want.type != got.type) {
        err = "NAL #" + std::to_string(expected_index) + " type mismatch: expected " + std::to_string(want.type) +
              ", got " + std::to_string(got.type);
        ok = false;
      } else if (want.bytes != got.bytes) {
        size_t diff = 0U;
        while (diff < want.bytes.size() && diff < got.bytes.size() && want.bytes[diff] == got.bytes[diff]) {
          ++diff;
        }
        err = "NAL #" + std::to_string(expected_index) + " (type " + std::to_string(want.type) + ") differs at byte " +
              std::to_string(diff) + ": expected size " + std::to_string(want.bytes.size()) + ", got " +
              std::to_string(got.bytes.size());
        ok = false;
      } else {
        ++actual_index;
      }
    }
  }
  return ok;
}

bool RunProcess(const std::vector<std::string>& argv, int timeout_ms, std::string& stdout_log, std::string& stderr_log,
                int& exit_code) {
  char out_path[] = "/tmp/rs_proc_out_XXXXXX";
  char err_path[] = "/tmp/rs_proc_err_XXXXXX";
  const int out_fd = ::mkstemp(out_path);
  const int err_fd = ::mkstemp(err_path);
  bool ok = (out_fd >= 0) && (err_fd >= 0) && argv.empty() == false;
  stdout_log.clear();
  stderr_log.clear();
  exit_code = -1;
  if (ok == false) {
    if (out_fd >= 0) {
      ::close(out_fd);
    }
    if (err_fd >= 0) {
      ::close(err_fd);
    }
    ::unlink(out_path);
    ::unlink(err_path);
  } else {
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (0 == pid) {
      // Do not outlive the test process: a killed suite must not leave a
      // script/ffmpeg child behind.
      static_cast<void>(::prctl(PR_SET_PDEATHSIG, SIGKILL));
      if (::getppid() != parent) {
        ::_exit(127);
      }
      static_cast<void>(::dup2(out_fd, STDOUT_FILENO));
      static_cast<void>(::dup2(err_fd, STDERR_FILENO));
      ::close(out_fd);
      ::close(err_fd);
      std::vector<char*> cargv;
      cargv.reserve(argv.size() + 1U);
      for (const auto& arg : argv) {
        cargv.push_back(const_cast<char*>(arg.c_str()));
      }
      cargv.push_back(nullptr);
      ::execvp(cargv[0], cargv.data());
      ::_exit(127);
    }
    ::close(out_fd);
    ::close(err_fd);
    if (pid < 0) {
      ok = false;
    } else {
      const int64_t deadline = NowMs() + timeout_ms;
      int status = 0;
      bool reaped = false;
      while (reaped == false) {
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (pid == waited) {
          reaped = true;
        } else if (waited < 0 && EINTR != errno) {
          ok = false;
          reaped = true;
        } else if (NowMs() > deadline) {
          static_cast<void>(::kill(pid, SIGKILL));
          static_cast<void>(::waitpid(pid, &status, 0));
          reaped = true;
          exit_code = -1;
        } else {
          struct timespec pause{0, 2000000};
          static_cast<void>(::nanosleep(&pause, nullptr));
        }
      }
      if (ok) {
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      }
    }
    stdout_log = ReadFileText(out_path);
    stderr_log = ReadFileText(err_path);
    ::unlink(out_path);
    ::unlink(err_path);
  }
  return ok;
}

}  // namespace rse2e
