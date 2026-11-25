// External producer mode: start without a file source, submit one AU through
// the public Ingress() boundary, then stop cleanly.
// SPDX-License-Identifier: MIT
#include "check.hpp"
#include "e2e_client.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#include "rs/rtsp_server.hpp"
#pragma GCC diagnostic pop

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

namespace {

std::atomic<uint32_t> g_idr_calls{0U};

void OnIdrRequest(void* ctx) noexcept {
  static_cast<std::atomic<uint32_t>*>(ctx)->fetch_add(1U, std::memory_order_relaxed);
}

bool WaitForIdr(uint32_t previous, uint32_t timeout_ms) {
  for (uint32_t i = 0U; i < timeout_ms; i += 10U) {
    if (g_idr_calls.load(std::memory_order_relaxed) > previous) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool StartPlay(rse2e::Client& client, uint16_t port) {
  std::string err;
  rse2e::Response response;
  const std::string uri = "rtsp://127.0.0.1:" + std::to_string(port) + "/live/trackID=0";
  if (!client.Connect("127.0.0.1", port, 1000, err) ||
      !client.Request("SETUP", uri, {"CSeq: 1", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1"}, "", err) ||
      !client.ReadResponse(response, 1000, err) || response.status != 200U) {
    return false;
  }
  const std::string session = response.Get("Session");
  return !session.empty() &&
         client.Request("PLAY", uri, {"CSeq: 2", "Session: " + session}, "", err);
}

}  // namespace

int main() {
  uint8_t sps[4] = {0x67, 0x42, 0xC0, 0x1F};
  uint8_t pps[4] = {0x68, 0xCE, 0x3C, 0x80};
  rs::TrackParams params;
  params.codec = rs::Codec::kH264;
  params.sps = sps;
  params.sps_len = sizeof(sps);
  params.pps = pps;
  params.pps_len = sizeof(pps);

  rs::ServerConfig cfg;
  cfg.port = 18579U;
  cfg.params = params;
  cfg.advertise_ip = "127.0.0.1";
  cfg.max_clients = 4U;

  rs::RtspServer server;
  server.SetIdrRequester(&OnIdrRequest, &g_idr_calls);
  const auto started = server.Start(cfg);
  RS_CHECK(started.has_value());
  RS_CHECK(server.Running());

  rse2e::Client first;
  rse2e::Client second;
  RS_CHECK(StartPlay(first, cfg.port));
  RS_CHECK(WaitForIdr(0U, 500U));
  // Two sessions waiting for one IDR coalesce into one encoder request.
  RS_CHECK(StartPlay(second, cfg.port));
  RS_CHECK(!WaitForIdr(1U, 250U));
  // Missing IDR is retried once per rate-limit window.
  RS_CHECK(WaitForIdr(1U, 1100U));

  auto writer = server.Ingress().Acquire(8U);
  RS_CHECK(writer.has_value());
  rs::FrameIngress::Writer w = writer.value();
  const uint8_t au[8] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00};
  std::memcpy(w.au, au, sizeof(au));
  w.desc.pts_ns = 1000U;
  w.desc.flags = rs::kFrameIdr;
  RS_CHECK(server.Ingress().Commit(w, sizeof(au)).has_value());
  server.Wake();

  std::string err;
  rse2e::Response response;
  RS_CHECK(first.ReadResponse(response, 1000, err));
  RS_CHECK_EQ(response.status, 200U);
  RS_CHECK(second.ReadResponse(response, 1000, err));
  RS_CHECK_EQ(response.status, 200U);
  RS_CHECK(!WaitForIdr(2U, 1100U));

  // A rejected source AU leaves a sequence gap; the next predictive AU
  // must trigger a fresh IDR request for the playing sessions.
  // Let the shared rate-limit window expire first.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  RS_CHECK(!server.Ingress().Acquire(0U).has_value());
  auto predictive = server.Ingress().Acquire(sizeof(au));
  RS_CHECK(predictive.has_value());
  rs::FrameIngress::Writer p = predictive.value();
  std::memcpy(p.au, au, sizeof(au));
  p.au[4] = 0x41U;
  p.desc.pts_ns = 2000U;
  p.desc.flags = 0U;
  RS_CHECK(server.Ingress().Commit(p, sizeof(au)).has_value());
  server.Wake();
  RS_CHECK(WaitForIdr(2U, 500U));

  auto recovery = server.Ingress().Acquire(sizeof(au));
  RS_CHECK(recovery.has_value());
  rs::FrameIngress::Writer r = recovery.value();
  std::memcpy(r.au, au, sizeof(au));
  r.desc.pts_ns = 3000U;
  r.desc.flags = rs::kFrameIdr;
  RS_CHECK(server.Ingress().Commit(r, sizeof(au)).has_value());
  server.Wake();
  RS_CHECK(!WaitForIdr(3U, 1100U));
  server.Stop();
  RS_CHECK(!server.Running());
  std::printf("test_server_external: all checks passed\n");
  return 0;
}
