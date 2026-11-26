// Runtime parameter-set update: validation, param_gen bump, old-session close
// and the new SDP reaching a reconnecting client (design §9.4).
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/rtsp_server.hpp"
#include "rs/sdp.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr uint16_t kPort = 18586U;

uint8_t g_old_sps[4] = {0x67, 0x42, 0xC0, 0x1F};
uint8_t g_new_sps[4] = {0x67, 0x42, 0xC0, 0x1E};
uint8_t g_pps[4] = {0x68, 0xCE, 0x3C, 0x80};
std::atomic<uint32_t> g_idr_calls{0U};

void OnIdrRequest(void* ctx) noexcept {
  static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U, std::memory_order_relaxed);
}

rs::TrackParams MakeParams(const uint8_t* sps) {
  rs::TrackParams params;
  params.codec = rs::Codec::kH264;
  params.sps = sps;
  params.sps_len = 4U;
  params.pps = g_pps;
  params.pps_len = 4U;
  return params;
}

bool SendAll(int fd, const char* text) {
  const size_t len = std::strlen(text);
  size_t off = 0U;
  bool ok = true;
  while (ok && off < len) {
    const ssize_t n = ::send(fd, text + off, len - off, 0);
    if (n > 0) {
      off += static_cast<size_t>(n);
    } else {
      ok = false;
    }
  }
  return ok;
}

int ConnectClient() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  timeval tv{};
  tv.tv_sec = 2;
  static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(kPort);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    static_cast<void>(::close(fd));
    return -1;
  }
  return fd;
}

// Reads until the response terminator or the receive timeout.
std::string ReadResponse(int fd) {
  std::string out;
  char buf[1024];
  for (uint32_t round = 0U; round < 16U; ++round) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    out.append(buf, static_cast<size_t>(n));
    if (out.find("\r\n\r\n") != std::string::npos) {
      break;
    }
  }
  return out;
}

bool WaitFor(const rs::RtspServer& server, bool want_param_gen2, uint32_t timeout_ms) {
  const uint32_t step_ms = 20U;
  for (uint32_t waited = 0U; waited <= timeout_ms; waited += step_ms) {
    const rs::ServerSnapshot s = server.Snapshot();
    const bool gen_ok = !want_param_gen2 || s.stats.param_gen == 2U;
    if (gen_ok && s.stats.connections_active == 0U) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
  }
  return false;
}

std::string HeaderValue(const std::string& response, const char* name) {
  const std::string key = std::string("\r\n") + name + ": ";
  const size_t pos = response.find(key);
  if (pos == std::string::npos) {
    return std::string();
  }
  const size_t start = pos + key.size();
  const size_t end = response.find("\r\n", start);
  return response.substr(start, end - start);
}

}  // namespace

int main() {
  rs::ServerConfig cfg;
  cfg.port = kPort;
  cfg.advertise_ip = "127.0.0.1";
  cfg.max_clients = 4U;
  cfg.params = MakeParams(g_old_sps);

  rs::RtspServer server;
  server.SetIdrRequester(&OnIdrRequest, &g_idr_calls);
  RS_CHECK(server.Start(cfg).has_value());
  RS_CHECK_EQ(server.Stats().param_gen, 1U);

  const int client = ConnectClient();
  RS_CHECK(client >= 0);
  char describe[256];
  std::snprintf(describe, sizeof(describe), "DESCRIBE rtsp://127.0.0.1:%" PRIu32 "/live RTSP/1.0\r\nCSeq: 1\r\n\r\n",
                static_cast<uint32_t>(kPort));
  RS_CHECK(SendAll(client, describe));
  const std::string old_sdp = ReadResponse(client);
  RS_CHECK(old_sdp.find("200 OK") != std::string::npos);
  RS_CHECK(old_sdp.find("sprop-parameter-sets=") != std::string::npos);

  // PLAY without any media asks the registered encoder hook for an IDR.
  char setup[320];
  std::snprintf(setup, sizeof(setup),
                "SETUP rtsp://127.0.0.1:%" PRIu32
                "/live/trackID=0 RTSP/1.0\r\nCSeq: 2\r\n"
                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n",
                static_cast<uint32_t>(kPort));
  RS_CHECK(SendAll(client, setup));
  const std::string setup_rsp = ReadResponse(client);
  RS_CHECK(setup_rsp.find("200 OK") != std::string::npos);
  const std::string session = HeaderValue(setup_rsp, "Session");
  RS_CHECK(!session.empty());
  char play[320];
  std::snprintf(play, sizeof(play),
                "PLAY rtsp://127.0.0.1:%" PRIu32
                "/live/trackID=0 RTSP/1.0\r\nCSeq: 3\r\n"
                "Session: %s\r\n\r\n",
                static_cast<uint32_t>(kPort), session.c_str());
  RS_CHECK(SendAll(client, play));
  // The PLAY 200 is deferred until the first IDR, so only the hook is checked.
  for (uint32_t waited = 0U; waited < 1000U && g_idr_calls.load(std::memory_order_relaxed) == 0U; waited += 20U) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  RS_CHECK_EQ(g_idr_calls.load(std::memory_order_relaxed), 1U);
  char teardown[320];
  std::snprintf(teardown, sizeof(teardown),
                "TEARDOWN rtsp://127.0.0.1:%" PRIu32 "/live/trackID=0 RTSP/1.0\r\nCSeq: 4\r\nSession: %s\r\n\r\n",
                static_cast<uint32_t>(kPort), session.c_str());
  RS_CHECK(SendAll(client, teardown));
  const std::string teardown_rsp = ReadResponse(client);
  RS_CHECK(teardown_rsp.find("200 OK") != std::string::npos);

  // Too-long parameter sets are rejected before any copy.
  rs::TrackParams bad = MakeParams(g_new_sps);
  bad.sps_len = rs::kMaxParamSetBytes + 1U;
  RS_CHECK(!server.UpdateTrackParams(bad).has_value());
  RS_CHECK_EQ(server.Stats().param_gen, 1U);

  RS_CHECK(server.UpdateTrackParams(MakeParams(g_new_sps)).has_value());
  RS_CHECK(WaitFor(server, true, 2000U));
  const rs::ServerStats after = server.Stats();
  RS_CHECK_EQ(after.sps_len, 4U);
  RS_CHECK_EQ(after.close_reasons[rs::kCloseParamsChanged], 1U);
  static_cast<void>(::close(client));

  // A reconnecting client must see the new SPS in SDP.
  char old_b64[32];
  char new_b64[32];
  const uint32_t old_len = rs::Base64Encode(g_old_sps, 4U, old_b64, sizeof(old_b64));
  const uint32_t new_len = rs::Base64Encode(g_new_sps, 4U, new_b64, sizeof(new_b64));
  RS_CHECK(old_len < sizeof(old_b64));
  RS_CHECK(new_len < sizeof(new_b64));
  old_b64[old_len] = '\0';
  new_b64[new_len] = '\0';

  const int client2 = ConnectClient();
  RS_CHECK(client2 >= 0);
  RS_CHECK(SendAll(client2, describe));
  const std::string new_sdp = ReadResponse(client2);
  RS_CHECK(new_sdp.find("200 OK") != std::string::npos);
  RS_CHECK(new_sdp.find(new_b64) != std::string::npos);
  RS_CHECK(new_sdp.find(old_b64) == std::string::npos);
  static_cast<void>(::close(client2));

  server.Stop();
  std::printf("test_track_params: all checks passed\n");
  return 0;
}
