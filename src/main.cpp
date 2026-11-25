// rtspserver_demo: fixed-capacity RTSP server for one H.264/H.265 Annex B file.
// SPDX-License-Identifier: MIT
#include "rs/file_source.hpp"
#include "rs/rtsp_server.hpp"

#include "osp/platform.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>

#include <signal.h>

#if defined(RS_ENABLE_DEBUG_SHELL)
#include "rs/debug_shell.hpp"
#include "osp/shell.hpp"
#endif

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int /*sig*/) { g_stop = 1; }

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s --port <n> --file <path.h264|path.h265> --ip <advertise_ip> [--clients <n>]\n"
               "                [--codec auto|h264|h265]\n"
               "  --port     RTSP listen port (default %u)\n"
               "  --file     Annex B H.264/H.265 stream, AUD-delimited (required)\n"
               "  --ip       address advertised in SDP/Content-Base (required)\n"
               "  --clients  max concurrent RTSP connections, 1..%u (default %u)\n"
               "  --codec    auto detects from the file header (default auto)\n"
#if defined(RS_ENABLE_DEBUG_SHELL)
               "  --shell-port  telnet debug shell port (0 = disabled)\n"
               "  --shell-bind  shell bind address (default 127.0.0.1)\n"
#endif
               ,
               prog, static_cast<unsigned>(rs::kDefaultRtspPort), static_cast<unsigned>(rs::kMaxConnections),
               static_cast<unsigned>(rs::kMaxConnections));
}

bool ParseU32(const char* text, uint32_t& out) {
  if (text == nullptr) {
    return false;
  }
  const char* const last = text + std::strlen(text);
  uint32_t parsed = 0U;
  const std::from_chars_result rc = std::from_chars(text, last, parsed, 10);
  if (rc.ec != std::errc() || rc.ptr != last || 0U == parsed) {
    return false;
  }
  out = parsed;
  return true;
}

void PrintStats(const char* tag, const rs::ServerStats& s) {
  std::printf(
      "[%s] conns=%u/%u playing=%u accepts=%llu full=%llu reqs=%llu au_in=%llu au_ok=%llu au_drop=%llu "
      "idr_wait=%llu idr_to=%llu gaps=%llu qdrop=%llu tx_bytes=%llu short=%llu eagain=%llu "
      "loop_us=%u/%u/%u\n",
      tag, static_cast<unsigned>(s.connections_active), static_cast<unsigned>(s.connections_peak),
      static_cast<unsigned>(s.sessions_playing), static_cast<unsigned long long>(s.accepts),
      static_cast<unsigned long long>(s.accept_full), static_cast<unsigned long long>(s.requests),
      static_cast<unsigned long long>(s.au_in), static_cast<unsigned long long>(s.au_accepted),
      static_cast<unsigned long long>(s.au_dropped), static_cast<unsigned long long>(s.idr_waits),
      static_cast<unsigned long long>(s.idr_timeouts), static_cast<unsigned long long>(s.input_gaps),
      static_cast<unsigned long long>(s.queue_drops),
      static_cast<unsigned long long>(s.tx_bytes), static_cast<unsigned long long>(s.short_writes),
      static_cast<unsigned long long>(s.would_block), static_cast<unsigned>(s.loop_p50_us),
      static_cast<unsigned>(s.loop_p95_us), static_cast<unsigned>(s.loop_p99_us));
}

}  // namespace

int main(int argc, char** argv) {
  rs::ServerConfig cfg;
  bool have_file = false;
  bool have_ip = false;
  const char* file_path = nullptr;
#if defined(RS_ENABLE_DEBUG_SHELL)
  uint16_t shell_port = 0U;
  const char* shell_bind = "127.0.0.1";
#endif
  for (int32_t i = 1; i < static_cast<int32_t>(argc); ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    }
    const bool has_value = (i + 1) < argc;
    if (arg == "--port" && has_value) {
      uint32_t port = 0U;
      if (!ParseU32(argv[++i], port) || port > rs::kMaxPortNumber) {
        PrintUsage(argv[0]);
        return 2;
      }
      cfg.port = static_cast<uint16_t>(port);
    } else if (arg == "--file" && has_value) {
      file_path = argv[++i];
      have_file = true;
    } else if (arg == "--ip" && has_value) {
      cfg.advertise_ip = argv[++i];
      have_ip = true;
    } else if (arg == "--clients" && has_value) {
      uint32_t clients = 0U;
      if (!ParseU32(argv[++i], clients) || clients > rs::kMaxConnections) {
        std::fprintf(stderr, "--clients must be 1..%u\n", static_cast<unsigned>(rs::kMaxConnections));
        return 2;
      }
      cfg.max_clients = clients;
    } else if (arg == "--codec" && has_value) {
      const std::string_view codec{argv[++i]};
      if (codec == "auto") {
        cfg.codec = rs::Codec::kUnknown;
      } else if (codec == "h264") {
        cfg.codec = rs::Codec::kH264;
      } else if (codec == "h265") {
        cfg.codec = rs::Codec::kH265;
      } else {
        PrintUsage(argv[0]);
        return 2;
      }
#if defined(RS_ENABLE_DEBUG_SHELL)
    } else if (arg == "--shell-port" && has_value) {
      uint32_t port = 0U;
      if (!ParseU32(argv[++i], port) || port > rs::kMaxPortNumber) {
        PrintUsage(argv[0]);
        return 2;
      }
      shell_port = static_cast<uint16_t>(port);
    } else if (arg == "--shell-bind" && has_value) {
      shell_bind = argv[++i];
#endif
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }
  if (!have_file || !have_ip) {
    PrintUsage(argv[0]);
    return 2;
  }

  struct sigaction sa {};
  sa.sa_handler = &OnSignal;
  static_cast<void>(sigemptyset(&sa.sa_mask));
  sa.sa_flags = 0;
  static_cast<void>(sigaction(SIGINT, &sa, nullptr));
  static_cast<void>(sigaction(SIGTERM, &sa, nullptr));

  // The source owns the parameter-set storage that cfg.params points at, so it
  // must be constructed first and destroyed last.
  rs::FileSource source;
  auto opened = source.Open(file_path, cfg.codec);
  if (!opened.has_value()) {
    std::fprintf(stderr, "rtspserver_demo: cannot open stream %s (error %u)\n", file_path,
                 static_cast<unsigned>(opened.get_error()));
    return 1;
  }
  cfg.params = opened.value().params;

  rs::RtspServer server;
  auto started = server.Start(cfg);
  if (!started.has_value()) {
    std::fprintf(stderr, "rtspserver_demo: start failed (error %u), check --file and --ip\n",
                 static_cast<unsigned>(started.get_error()));
    return 1;
  }
  const rs::ServerStats stats = server.Stats();
  std::printf(
      "rtspserver_demo: listening on :%u, file=%s, advertise=%s, clients=%u, codec=%s vps=%u sps=%u pps=%u\n",
      cfg.port, file_path, cfg.advertise_ip, static_cast<unsigned>(cfg.max_clients),
      rs::CodecName(stats.codec), static_cast<unsigned>(stats.vps_len), static_cast<unsigned>(stats.sps_len),
      static_cast<unsigned>(stats.pps_len));
  std::fflush(stdout);

  auto source_started = source.Start(server.Ingress(), server.Callbacks());
  if (!source_started.has_value()) {
    std::fprintf(stderr, "rtspserver_demo: source thread start failed (error %u)\n",
                 static_cast<unsigned>(source_started.get_error()));
    server.Stop();
    return 1;
  }

#if defined(RS_ENABLE_DEBUG_SHELL)
  std::unique_ptr<osp::DebugShell> shell;
  if (shell_port != 0U) {
    rs::BindDebugShell(&server);
    osp::DebugShell::Config shell_cfg;
    shell_cfg.port = shell_port;
    shell_cfg.max_connections = 1U;
    shell_cfg.prompt = "rs> ";
    shell_cfg.banner = "RS500 RTSP debug shell\r\n";
    shell_cfg.bind_ip = shell_bind;
    shell = std::make_unique<osp::DebugShell>(shell_cfg);
    auto shell_started = shell->Start();
    if (!shell_started.has_value()) {
      std::fprintf(stderr, "rtspserver_demo: debug shell start failed on %s:%u (error %u)\n", shell_bind,
                   static_cast<unsigned>(shell_port), static_cast<unsigned>(shell_started.get_error()));
      shell.reset();
    } else {
      std::printf("rtspserver_demo: debug shell on %s:%u\n", shell_bind, static_cast<unsigned>(shell_port));
      std::fflush(stdout);
    }
  }
#endif

  const uint64_t period = static_cast<uint64_t>(rs::kStatsLogPeriodMs) * rs::kNsPerMs;
  uint64_t next_log = osp::SteadyNowNs() + period;
  while (g_stop == 0 && !server.StopRequested()) {
    uint32_t sleep_ms = rs::kMaintenancePeriodMs;
    const uint64_t now = osp::SteadyNowNs();
    if (now >= next_log) {
      PrintStats("stats", server.Stats());
      std::fflush(stdout);
      next_log += period;
      if (next_log < now) {
        next_log = now + period;
      }
      sleep_ms = 1U;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }

#if defined(RS_ENABLE_DEBUG_SHELL)
  if (shell) {
    shell->Stop();
  }
#endif
  source.Stop();
  server.Stop();
  PrintStats("final", server.Stats());
  std::printf("rtspserver_demo: stopped\n");
  return 0;
}
