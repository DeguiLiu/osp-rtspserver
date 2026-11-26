// rtspserver_demo: fixed-capacity RTSP server for one H.264/H.265 Annex B file.
// SPDX-License-Identifier: MIT
#include "osp/log.hpp"
#include "osp/platform.hpp"
#include "rs/file_source.hpp"
#include "rs/rtsp_server.hpp"

#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <charconv>
#include <chrono>
#include <memory>
#include <signal.h>
#include <string_view>
#include <thread>

#if defined(RS_ENABLE_DEBUG_SHELL)
#include "osp/shell.hpp"
#include "rs/debug_shell.hpp"
#endif

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int /*sig*/) {
  g_stop = 1;
}

void PrintUsage(const char* prog) {
  // Not a log line: this is multi-line CLI help text. A per-line log prefix
  // would break its alignment, and the exit code (0 for --help, 2 for a bad
  // argument) is the contract a caller scripts against.
  std::fprintf(stderr,
               "usage: %s --port <n> --file <path.h264|path.h265> --ip <advertise_ip> [--clients <n>]\n"
               "                [--codec auto|h264|h265]\n"
               "  --port     RTSP listen port (default %" PRIu32
               ")\n"
               "  --file     Annex B H.264/H.265 stream, AUD-delimited (required)\n"
               "  --ip       address advertised in SDP/Content-Base (required)\n"
               "  --clients  max concurrent RTSP connections, 1..%" PRIu32 " (default %" PRIu32
               ")\n"
               "  --codec    auto detects from the file header (default auto)\n"
#if defined(RS_ENABLE_DEBUG_SHELL)
               "  --shell-port  telnet debug shell port (0 = disabled)\n"
               "  --shell-bind  shell bind address (default 127.0.0.1)\n"
#endif
               ,
               prog, static_cast<uint32_t>(rs::kDefaultRtspPort), static_cast<uint32_t>(rs::kMaxConnections),
               static_cast<uint32_t>(rs::kMaxConnections));
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
  // Not a log line: scripts/stress.sh greps "^\[final\]" out of this, so the
  // format is a machine-readable contract. A log prefix would break it.
  std::printf(
      "[%s] conns=%" PRIu32 "/%" PRIu32 " playing=%" PRIu32 " accepts=%" PRIu64 " full=%" PRIu64 " reqs=%" PRIu64
      " au_in=%" PRIu64 " au_ok=%" PRIu64 " au_drop=%" PRIu64
      " "
      "idr_wait=%" PRIu64 " idr_to=%" PRIu64 " gaps=%" PRIu64 " qdrop=%" PRIu64 " tx_bytes=%" PRIu64 " short=%" PRIu64
      " eagain=%" PRIu64
      " "
      "loop_us=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
      tag, static_cast<uint32_t>(s.connections_active), static_cast<uint32_t>(s.connections_peak),
      static_cast<uint32_t>(s.sessions_playing), static_cast<uint64_t>(s.accepts), static_cast<uint64_t>(s.accept_full),
      static_cast<uint64_t>(s.requests), static_cast<uint64_t>(s.au_in), static_cast<uint64_t>(s.au_accepted),
      static_cast<uint64_t>(s.au_dropped), static_cast<uint64_t>(s.idr_waits), static_cast<uint64_t>(s.idr_timeouts),
      static_cast<uint64_t>(s.input_gaps), static_cast<uint64_t>(s.queue_drops), static_cast<uint64_t>(s.tx_bytes),
      static_cast<uint64_t>(s.short_writes), static_cast<uint64_t>(s.would_block), static_cast<uint32_t>(s.loop_p50_us),
      static_cast<uint32_t>(s.loop_p95_us), static_cast<uint32_t>(s.loop_p99_us));
}

}  // namespace

int main(int argc, char** argv) {
  // The newosp logging backend needs an explicit Init before the first write.
  // The server's own level is mirrored into it by SetLogLevel().
  osp::log::Init();
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
        OSP_LOG_ERROR("main", "--clients must be 1..%" PRIu32, static_cast<uint32_t>(rs::kMaxConnections));
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

  struct sigaction sa{};
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
    OSP_LOG_ERROR("main", "cannot open stream %s (error %" PRIu32 ")", file_path,
                  static_cast<uint32_t>(opened.get_error()));
    return 1;
  }
  cfg.params = opened.value().params;

  rs::RtspServer server;
  auto started = server.Start(cfg);
  if (!started.has_value()) {
    OSP_LOG_ERROR("main", "start failed (error %" PRIu32 "), check --file and --ip",
                  static_cast<uint32_t>(started.get_error()));
    return 1;
  }
  const rs::ServerStats stats = server.Stats();
  OSP_LOG_INFO("main",
               "listening on :%" PRIu32 ", file=%s, advertise=%s, clients=%" PRIu32 ", codec=%s vps=%" PRIu32
               " sps=%" PRIu32 " pps=%" PRIu32,
               cfg.port, file_path, cfg.advertise_ip, static_cast<uint32_t>(cfg.max_clients),
               rs::CodecName(stats.codec), static_cast<uint32_t>(stats.vps_len), static_cast<uint32_t>(stats.sps_len),
               static_cast<uint32_t>(stats.pps_len));

  auto source_started = source.Start(server.Ingress(), server.Callbacks());
  if (!source_started.has_value()) {
    OSP_LOG_ERROR("main", "source thread start failed (error %" PRIu32 ")",
                  static_cast<uint32_t>(source_started.get_error()));
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
      OSP_LOG_ERROR("main", "debug shell start failed on %s:%" PRIu32 " (error %" PRIu32 ")", shell_bind,
                    static_cast<uint32_t>(shell_port), static_cast<uint32_t>(shell_started.get_error()));
      shell.reset();
    } else {
      OSP_LOG_INFO("main", "debug shell on %s:%" PRIu32, shell_bind, static_cast<uint32_t>(shell_port));
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
  // LogWrite instead of OSP_LOG_INFO: the macro needs the GNU
  // ", ##__VA_ARGS__" extension, and -Wpedantic rejects an empty variadic part.
  osp::log::LogWrite(osp::log::Level::kInfo, "main", __FILE__, __LINE__, "stopped");
  return 0;
}
