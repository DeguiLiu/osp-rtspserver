// EncoderSource demo: a producer thread plays the role of a hardware/software
// encoder and feeds complete access units through the public FrameIngress
// boundary. It shows the three integration points an encoder adapter needs:
//   1. Acquire -> fill w.au -> Commit -> Wake() for every AU.
//   2. SetIdrRequester() so the server can ask for a keyframe.
//   3. UpdateTrackParams() when SPS/PPS change (old sessions are closed).
// The slice payload is synthetic; only the call sequence is meant to be copied.
// SPDX-License-Identifier: MIT
#include "osp/platform.hpp"
#include "rs/rtsp_server.hpp"

#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <chrono>
#include <signal.h>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int /*sig*/) {
  g_stop = 1;
}

// Baseline-profile H.264 parameter sets. Version 2 changes level_idc so the
// new SDP is visibly different while staying inside ValidateParamSets().
constexpr uint8_t kSpsV1[] = {0x67, 0x42, 0x00, 0x1F, 0xE9, 0x02, 0x83, 0xF0, 0x0A, 0x0F, 0xA8, 0x0F, 0x10, 0x00,
                              0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xC8, 0xF1, 0x83, 0x19, 0x60};
constexpr uint8_t kSpsV2[] = {0x67, 0x42, 0x00, 0x1E, 0xE9, 0x02, 0x83, 0xF0, 0x0A, 0x0F, 0xA8, 0x0F, 0x10, 0x00,
                              0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xC8, 0xF1, 0x83, 0x19, 0x60};
constexpr uint8_t kPps[] = {0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0};
constexpr uint8_t kAud[] = {0x09, 0xF0};

constexpr uint32_t kFrameIntervalMs = 40U;  // 25 fps.
constexpr uint32_t kGopFrames = 25U;
constexpr uint32_t kParamsRefreshFrames = 100U;  // 4 s.

struct EncoderState {
  std::atomic<bool> idr_pending{false};
  std::atomic<uint32_t> idr_requests{0U};
};

// Runs on the loop thread: keep it non-blocking. A real encoder driver usually
// sets a flag or posts to its own control queue, exactly like this.
void OnIdrRequest(void* ctx) noexcept {
  auto* state = static_cast<EncoderState*>(ctx);
  state->idr_pending.store(true, std::memory_order_release);
  state->idr_requests.fetch_add(1U, std::memory_order_relaxed);
}

uint32_t AppendNal(uint8_t* dst, uint32_t cap, const uint8_t* nal, uint32_t len) {
  if (cap < len + 4U) {
    return 0U;
  }
  dst[0] = 0x00U;
  dst[1] = 0x00U;
  dst[2] = 0x00U;
  dst[3] = 0x01U;
  std::memcpy(dst + 4U, nal, len);
  return len + 4U;
}

// AUD [+ SPS/PPS on IDR] + one synthetic slice. 0x21..0x9F avoids start codes.
uint32_t BuildAu(uint8_t* dst, uint32_t cap, bool idr, uint32_t frame_index, const uint8_t* sps, uint32_t sps_len) {
  uint32_t off = AppendNal(dst, cap, kAud, sizeof(kAud));
  if (off == 0U) {
    return 0U;
  }
  if (idr) {
    const uint32_t n1 = AppendNal(dst + off, cap - off, sps, sps_len);
    if (n1 == 0U) {
      return 0U;
    }
    off += n1;
    const uint32_t n2 = AppendNal(dst + off, cap - off, kPps, sizeof(kPps));
    if (n2 == 0U) {
      return 0U;
    }
    off += n2;
  }
  const uint32_t payload = idr ? 4000U : 900U;
  if (cap - off < payload + 5U) {
    return 0U;
  }
  dst[off + 0U] = 0x00U;
  dst[off + 1U] = 0x00U;
  dst[off + 2U] = 0x00U;
  dst[off + 3U] = 0x01U;
  dst[off + 4U] = idr ? 0x65U : 0x41U;
  for (uint32_t i = 0U; i < payload; ++i) {
    dst[off + 5U + i] = static_cast<uint8_t>(0x21U + ((frame_index + i) % 0x7FU));
  }
  return off + 5U + payload;
}

void ProducerMain(rs::RtspServer& server, EncoderState& state) {
  const rs::SourceCallbacks cb = server.Callbacks();
  const uint8_t* sps = kSpsV1;
  uint32_t sps_len = sizeof(kSpsV1);
  uint64_t pts_ns = osp::SteadyNowNs();
  uint32_t frame_index = 0U;
  uint32_t dropped = 0U;

  const uint64_t refresh_at = pts_ns + static_cast<uint64_t>(kParamsRefreshFrames) * kFrameIntervalMs * rs::kNsPerMs;
  const uint64_t period_ns = static_cast<uint64_t>(kFrameIntervalMs) * rs::kNsPerMs;

  while (g_stop == 0 && !server.StopRequested()) {
    auto writer = server.Ingress().Acquire(rs::kMaxAuBytes);
    if (!writer.has_value()) {
      // A dropped AU leaves a sequence gap; the server moves clients to
      // AwaitIDR and the request below produces the recovery keyframe.
      ++dropped;
      if (cb.drop != nullptr) {
        cb.drop(cb.ctx);
      }
      state.idr_pending.store(true, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(kFrameIntervalMs));
      continue;
    }

    const bool forced = state.idr_pending.exchange(false, std::memory_order_acq_rel);
    const bool idr = forced || (frame_index % kGopFrames == 0U);
    const uint32_t len = BuildAu(writer.value().au, writer.value().capacity, idr, frame_index, sps, sps_len);
    if (len == 0U) {
      server.Ingress().Abort(writer.value());
      std::this_thread::sleep_for(std::chrono::milliseconds(kFrameIntervalMs));
      continue;
    }
    writer.value().desc.pts_ns = pts_ns;
    writer.value().desc.flags = idr ? static_cast<uint16_t>(rs::kFrameIdr) : 0U;
    if (!server.Ingress().Commit(writer.value(), len).has_value()) {
      server.Ingress().Abort(writer.value());
      ++dropped;
      if (cb.drop != nullptr) {
        cb.drop(cb.ctx);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kFrameIntervalMs));
      continue;
    }
    if (cb.wake != nullptr) {
      cb.wake(cb.ctx);
    }

    if (frame_index != 0U && (frame_index % kGopFrames) == 0U) {
      std::printf("encoder_source_demo: frame=%" PRIu32 " idr_requests=%" PRIu32 " dropped=%" PRIu32
                  " param_gen=%" PRIu32 "\n",
                  frame_index, state.idr_requests.load(std::memory_order_relaxed), dropped,
                  static_cast<uint32_t>(server.Stats().param_gen));
      std::fflush(stdout);
    }
    ++frame_index;
    pts_ns += period_ns;

    // Parameter refresh: copy succeeded, then switch the encoder output and
    // force an IDR so reconnecting clients get matching in-band SPS/PPS.
    if (pts_ns >= refresh_at && sps == kSpsV1) {
      rs::TrackParams params;
      params.codec = rs::Codec::kH264;
      params.sps = kSpsV2;
      params.sps_len = sizeof(kSpsV2);
      params.pps = kPps;
      params.pps_len = sizeof(kPps);
      const auto updated = server.UpdateTrackParams(params);
      std::printf("encoder_source_demo: UpdateTrackParams -> %s (applied on the loop thread)\n",
                  updated.has_value() ? "staged" : "rejected");
      sps = kSpsV2;
      sps_len = sizeof(kSpsV2);
      state.idr_pending.store(true, std::memory_order_release);
    }

    const uint64_t now = osp::SteadyNowNs();
    if (pts_ns > now) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(pts_ns - now));
    }
  }
  std::printf("encoder_source_demo: producer stopped, frames=%" PRIu32 " idr_requests=%" PRIu32 " dropped=%" PRIu32
              "\n",
              frame_index, state.idr_requests.load(std::memory_order_relaxed), dropped);
}

}  // namespace

int main(int argc, char** argv) {
  rs::ServerConfig cfg;
  cfg.port = 8554U;
  cfg.advertise_ip = "127.0.0.1";
  cfg.max_clients = rs::kMaxConnections;
  cfg.params.codec = rs::Codec::kH264;
  cfg.params.sps = kSpsV1;
  cfg.params.sps_len = sizeof(kSpsV1);
  cfg.params.pps = kPps;
  cfg.params.pps_len = sizeof(kPps);
  for (int32_t i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "--ip") == 0 && i + 1 < argc) {
      cfg.advertise_ip = argv[++i];
    } else if (std::strcmp(arg, "--port") == 0 && i + 1 < argc) {
      cfg.port = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else {
      std::fprintf(stderr, "usage: %s [--ip <advertise_ip>] [--port <n>]\n", argv[0]);
      return 2;
    }
  }

  struct sigaction sa{};
  sa.sa_handler = &OnSignal;
  static_cast<void>(sigemptyset(&sa.sa_mask));
  static_cast<void>(sigaction(SIGINT, &sa, nullptr));
  static_cast<void>(sigaction(SIGTERM, &sa, nullptr));

  EncoderState state;
  rs::RtspServer server;
  server.SetIdrRequester(&OnIdrRequest, &state);
  const auto started = server.Start(cfg);
  if (!started.has_value()) {
    std::fprintf(stderr, "encoder_source_demo: start failed (error %" PRIu32 ")\n",
                 static_cast<uint32_t>(started.get_error()));
    return 1;
  }
  std::printf("encoder_source_demo: rtsp://%s:%" PRIu32 "/live (Ctrl-C to stop)\n", cfg.advertise_ip,
              static_cast<uint32_t>(cfg.port));
  std::fflush(stdout);

  std::thread producer(ProducerMain, std::ref(server), std::ref(state));
  while (g_stop == 0 && !server.StopRequested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  producer.join();
  server.Stop();
  return 0;
}
