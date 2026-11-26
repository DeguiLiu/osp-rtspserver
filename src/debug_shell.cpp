// Read-only msh-like debug shell for the RTSP server (P0).
//
// Command callbacks run on DebugShell session threads. They only read
// RtspServer::Snapshot(); no socket/session/RTP state is touched here.
// SPDX-License-Identifier: MIT
#include "rs/debug_shell.hpp"

#include "osp/log.hpp"
#include "osp/shell.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <charconv>
#include <string_view>
#include <time.h>

namespace rs {
namespace {

// newosp ShellCmdFn has no context pointer, so the single RTSP server is
// reached through one encapsulated context instead of a file-scope g_server.
class ShellContext {
 public:
  static ShellContext& Instance() noexcept {
    static ShellContext ctx;
    return ctx;
  }

  void Bind(RtspServer* server) noexcept { server_ = server; }
  [[nodiscard]] RtspServer* Server() const noexcept { return server_; }

 private:
  RtspServer* server_ = nullptr;
};

RtspServer* Server() noexcept {
  return ShellContext::Instance().Server();
}

const ServerSnapshot& ShellSnapshot() noexcept {
  static thread_local ServerSnapshot snapshot;
  Server()->CopySnapshot(snapshot);
  return snapshot;
}

bool ParseU32(const char* text, uint32_t& value) noexcept {
  bool ok = text != nullptr;
  if (ok) {
    const std::string_view input{text};
    const char* const first = input.data();
    const char* const last = first + input.size();
    const auto parsed = std::from_chars(first, last, value, 10);
    ok = parsed.ec == std::errc{} && parsed.ptr == last;
  }
  return ok;
}

const char* TransportName(uint8_t transport) noexcept {
  switch (transport) {
    case 0U:
      return "none";
    case 1U:
      return "udp";
    case 2U:
      return "tcp";
    default:
      return "?";
  }
}

const char* LogLevelName(uint8_t level) noexcept {
  switch (static_cast<osp::log::Level>(level)) {
    case osp::log::Level::kError:
      return "error";
    case osp::log::Level::kWarn:
      return "warn";
    case osp::log::Level::kInfo:
      return "info";
    case osp::log::Level::kDebug:
      return "debug";
    case osp::log::Level::kFatal:
      return "fatal";
    case osp::log::Level::kOff:
      return "off";
    default:
      return "?";
  }
}

void PrintIpv4(uint32_t ip) noexcept {
  osp::DebugShell::Printf("%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32 "", static_cast<uint32_t>(ip & 0xFFU),
                          static_cast<uint32_t>((ip >> 8U) & 0xFFU), static_cast<uint32_t>((ip >> 16U) & 0xFFU),
                          static_cast<uint32_t>((ip >> 24U) & 0xFFU));
}

bool RequireServer() noexcept {
  if (Server() == nullptr) {
    osp::DebugShell::Printf("server not bound\r\n");
    return false;
  }
  return true;
}

bool SlotActive(const ServerSnapshot& snapshot, uint32_t slot) noexcept {
  for (uint32_t i = 0U; i < snapshot.client_count; ++i) {
    if (snapshot.clients[i].slot == slot && !snapshot.clients[i].closing) {
      return true;
    }
  }
  return false;
}

int CmdStats(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  const ServerStats& g = s.stats;
  osp::DebugShell::Printf("conns=%" PRIu32 "/%" PRIu32 " playing=%" PRIu32 " accepts=%" PRIu64 " full=%" PRIu64
                          " failed=%" PRIu64 " reqs=%" PRIu64 "\r\n",
                          static_cast<uint32_t>(g.connections_active), static_cast<uint32_t>(g.connections_peak),
                          static_cast<uint32_t>(g.sessions_playing), static_cast<uint64_t>(g.accepts),
                          static_cast<uint64_t>(g.accept_full), static_cast<uint64_t>(g.accept_failed),
                          static_cast<uint64_t>(g.requests));
  osp::DebugShell::Printf("rate limited accept=%" PRIu64 " request=%" PRIu64 "\r\n",
                          static_cast<uint64_t>(g.accept_rate_limited), static_cast<uint64_t>(g.request_rate_limited));
  osp::DebugShell::Printf("au in=%" PRIu64 " ok=%" PRIu64 " drop=%" PRIu64 " gaps=%" PRIu64 " pool_drop=%" PRIu64
                          " qfull=%" PRIu64 "\r\n",
                          static_cast<uint64_t>(g.au_in), static_cast<uint64_t>(g.au_accepted),
                          static_cast<uint64_t>(g.au_dropped), static_cast<uint64_t>(g.input_gaps),
                          static_cast<uint64_t>(g.frame_pool_drop), static_cast<uint64_t>(g.input_queue_full));
  osp::DebugShell::Printf("idr wait=%" PRIu64 " req=%" PRIu64 " timeout=%" PRIu64 " qdrop=%" PRIu64 " tx=%" PRIu64
                          " short=%" PRIu64 " eagain=%" PRIu64 "\r\n",
                          static_cast<uint64_t>(g.idr_waits), static_cast<uint64_t>(g.idr_requests),
                          static_cast<uint64_t>(g.idr_timeouts), static_cast<uint64_t>(g.queue_drops),
                          static_cast<uint64_t>(g.tx_bytes), static_cast<uint64_t>(g.short_writes),
                          static_cast<uint64_t>(g.would_block));
  osp::DebugShell::Printf("loop p50=%" PRIu32 "us p95=%" PRIu32 "us p99=%" PRIu32 "us samples=%" PRIu64
                          " source_stopped=%d loop_failed=%d\r\n",
                          static_cast<uint32_t>(g.loop_p50_us), static_cast<uint32_t>(g.loop_p95_us),
                          static_cast<uint32_t>(g.loop_p99_us), static_cast<uint64_t>(g.loop_samples),
                          g.source_stopped ? 1 : 0, g.loop_failed ? 1 : 0);
  osp::DebugShell::Printf("shell commands=%" PRIu64 " rejected=%" PRIu64 " log=%s fault_drop=%" PRIu32 "\r\n",
                          static_cast<uint64_t>(g.shell_commands), static_cast<uint64_t>(g.shell_rejects),
                          LogLevelName(s.log_level), static_cast<uint32_t>(s.fault_drop_remaining));
  return 0;
}

int CmdClients(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  if (s.client_count == 0U) {
    osp::DebugShell::Printf("no active clients\r\n");
    return 0;
  }
  osp::DebugShell::Printf("slot state   transport peer            rtp/rtcp queue bytes age_ms pkts qdrop close\r\n");
  for (uint32_t i = 0U; i < s.client_count; ++i) {
    const ClientSnapshot& c = s.clients[i];
    const SessionSnapshot& x = c.session;
    osp::DebugShell::Printf("%2u   %-7s %-9s ", static_cast<uint32_t>(c.slot), sess::StateName(x.state),
                            TransportName(x.transport));
    PrintIpv4(x.client_ipv4);
    osp::DebugShell::Printf(" %" PRIu32 "/%" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32
                            " %s\r\n",
                            static_cast<uint32_t>(x.client_rtp_port), static_cast<uint32_t>(x.client_rtcp_port),
                            static_cast<uint32_t>(x.queue_depth), static_cast<uint32_t>(x.queue_bytes),
                            static_cast<uint32_t>(x.oldest_age_ms), static_cast<uint32_t>(x.counters.packets_sent),
                            static_cast<uint32_t>(x.counters.queue_drops), CloseReasonName(x.close_reason));
  }
  return 0;
}

int CmdClient(int argc, char** argv) noexcept {
  if (!RequireServer()) {
    return -1;
  }
  if (argc != 2) {
    osp::DebugShell::Printf("usage: client <slot>\r\n");
    return -1;
  }
  uint32_t slot = 0U;
  if (!ParseU32(argv[1], slot) || slot >= kMaxConnections) {
    osp::DebugShell::Printf("invalid slot\r\n");
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  for (uint32_t i = 0U; i < s.client_count; ++i) {
    const ClientSnapshot& c = s.clients[i];
    if (c.slot != slot) {
      continue;
    }
    const SessionSnapshot& x = c.session;
    osp::DebugShell::Printf("slot=%" PRIu32 " gen=%" PRIu32 " fd=%d closing=%d connected_ms=%" PRIu64 "\r\n",
                            static_cast<uint32_t>(c.slot), static_cast<uint32_t>(c.gen), c.fd, c.closing ? 1 : 0,
                            static_cast<uint64_t>(c.connected_ms));
    osp::DebugShell::Printf("state=%s transport=%s playing=%d active=%d awaiting_rtp_info=%d closed=%d trace=%d\r\n",
                            sess::StateName(x.state), TransportName(x.transport), x.playing ? 1 : 0, x.active ? 1 : 0,
                            x.awaiting_rtp_info ? 1 : 0, x.closed ? 1 : 0, x.trace_enabled ? 1 : 0);
    osp::DebugShell::Printf("peer=");
    PrintIpv4(x.client_ipv4);
    osp::DebugShell::Printf(" client_rtp=%" PRIu32 " client_rtcp=%" PRIu32 " server_rtp=%" PRIu32
                            " server_rtcp=%" PRIu32 "\r\n",
                            static_cast<uint32_t>(x.client_rtp_port), static_cast<uint32_t>(x.client_rtcp_port),
                            static_cast<uint32_t>(x.server_rtp_port), static_cast<uint32_t>(x.server_rtcp_port));
    osp::DebugShell::Printf(
        "channel rtp=%" PRIu32 " rtcp=%" PRIu32 " seq=%" PRIu32 " ts=%" PRIu32 " ssrc=%" PRIu32 "\r\n",
        static_cast<uint32_t>(x.rtp_channel), static_cast<uint32_t>(x.rtcp_channel), static_cast<uint32_t>(x.seq),
        static_cast<uint32_t>(x.rtp_ts), static_cast<uint32_t>(x.ssrc));
    osp::DebugShell::Printf("queue depth=%" PRIu32 " bytes=%" PRIu32 " oldest=%" PRIu32 "ms stall_until=%" PRIu64
                            "\r\n",
                            static_cast<uint32_t>(x.queue_depth), static_cast<uint32_t>(x.queue_bytes),
                            static_cast<uint32_t>(x.oldest_age_ms), static_cast<uint64_t>(c.stall_until_ns));
    osp::DebugShell::Printf("tcp rtt=%" PRIu32 "us cwnd=%" PRIu32 " retrans=%" PRIu32 " unacked=%" PRIu32
                            " notsent=%" PRIu32 " sndbuf=%" PRIu32 " rcvbuf=%" PRIu32 "\r\n",
                            static_cast<uint32_t>(c.tcp_rtt_us), static_cast<uint32_t>(c.tcp_cwnd),
                            static_cast<uint32_t>(c.tcp_retrans), static_cast<uint32_t>(c.tcp_unacked),
                            static_cast<uint32_t>(c.tcp_notsent), static_cast<uint32_t>(c.sndbuf),
                            static_cast<uint32_t>(c.rcvbuf));
    osp::DebugShell::Printf("fault eagain=%" PRIu32 " idr=%" PRIu32 " slow_bps=%" PRIu32 "\r\n",
                            static_cast<uint32_t>(x.fault_eagain), static_cast<uint32_t>(x.fault_idr),
                            static_cast<uint32_t>(x.slow_bps));
    osp::DebugShell::Printf("drops queue=%" PRIu32 " udp=%" PRIu32 " token_wait=%" PRIu32 " interleaved=%" PRIu32
                            " gaps=%" PRIu32 " rtcp_bad=%" PRIu32 "\r\n",
                            static_cast<uint32_t>(x.counters.queue_drops), static_cast<uint32_t>(x.counters.udp_drops),
                            static_cast<uint32_t>(x.counters.token_waits),
                            static_cast<uint32_t>(x.counters.interleaved_errors),
                            static_cast<uint32_t>(x.counters.input_gaps), static_cast<uint32_t>(x.counters.rtcp_bad));
    osp::DebugShell::Printf("frames=%" PRIu32 " packets=%" PRIu32 " close=%s session=%s\r\n",
                            static_cast<uint32_t>(x.counters.frames_completed),
                            static_cast<uint32_t>(x.counters.packets_sent), CloseReasonName(x.close_reason),
                            x.session_id_prefix);
    return 0;
  }
  osp::DebugShell::Printf("slot %" PRIu32 " not active\r\n", static_cast<uint32_t>(slot));
  return -1;
}

int CmdNet(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  osp::DebugShell::Printf("slot rtt_us cwnd retrans unacked notsent sndbuf rcvbuf\r\n");
  for (uint32_t i = 0U; i < s.client_count; ++i) {
    const ClientSnapshot& c = s.clients[i];
    osp::DebugShell::Printf(
        "%2u   %6u %4u %7u %7u %7u %6u %6u\r\n", static_cast<uint32_t>(c.slot), static_cast<uint32_t>(c.tcp_rtt_us),
        static_cast<uint32_t>(c.tcp_cwnd), static_cast<uint32_t>(c.tcp_retrans), static_cast<uint32_t>(c.tcp_unacked),
        static_cast<uint32_t>(c.tcp_notsent), static_cast<uint32_t>(c.sndbuf), static_cast<uint32_t>(c.rcvbuf));
  }
  return 0;
}

int CmdPool(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  osp::DebugShell::Printf("frames slots=%" PRIu32 " free=%" PRIu32 " in_use=%" PRIu32 " input_q=%" PRIu32
                          " empty_q=%" PRIu32 "\r\n",
                          static_cast<uint32_t>(kFrameSlots), static_cast<uint32_t>(s.pool.free_slots),
                          static_cast<uint32_t>(s.pool.in_use_slots), static_cast<uint32_t>(s.pool.input_queued),
                          static_cast<uint32_t>(s.pool.empty_queued));
  osp::DebugShell::Printf("orphan producer=%" PRIu32 " loop=%" PRIu32 "\r\n",
                          static_cast<uint32_t>(s.pool.producer_orphan_drops),
                          static_cast<uint32_t>(s.pool.loop_orphan_drops));
  return 0;
}

int CmdFd(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  osp::DebugShell::Printf("listener fd=%d watchers=%" PRIu32 " timers=%" PRIu32 "\r\n", s.listener_fd,
                          static_cast<uint32_t>(s.watchers_used), static_cast<uint32_t>(s.timers_used));
  for (uint32_t i = 0U; i < s.client_count; ++i) {
    const ClientSnapshot& c = s.clients[i];
    const SessionSnapshot& x = c.session;
    osp::DebugShell::Printf("slot=%" PRIu32 " fd=%d %s ", static_cast<uint32_t>(c.slot), c.fd,
                            TransportName(x.transport));
    PrintIpv4(x.client_ipv4);
    osp::DebugShell::Printf(" udp=%" PRIu32 "/%" PRIu32 " channels=%" PRIu32 "/%" PRIu32 "\r\n",
                            static_cast<uint32_t>(x.server_rtp_port), static_cast<uint32_t>(x.server_rtcp_port),
                            static_cast<uint32_t>(x.rtp_channel), static_cast<uint32_t>(x.rtcp_channel));
  }
  return 0;
}

int CmdTimers(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  const uint64_t age_ms = (s.published_ns == 0U) ? 0U : (osp::SteadyNowNs() - s.published_ns) / kNsPerMs;
  osp::DebugShell::Printf("maintenance=%" PRIu32 "ms timers_used=%" PRIu32 " snapshot_age=%" PRIu64 "ms\r\n",
                          static_cast<uint32_t>(kMaintenancePeriodMs), static_cast<uint32_t>(s.timers_used),
                          static_cast<uint64_t>(age_ms));
  return 0;
}

int CmdStream(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  const ServerStats& g = s.stats;
  osp::DebugShell::Printf("codec=%s param_gen=%" PRIu32 " vps=%" PRIu32 " sps=%" PRIu32 " pps=%" PRIu32
                          " idr_hook=%d\r\n",
                          CodecName(g.codec), static_cast<uint32_t>(g.param_gen), static_cast<uint32_t>(g.vps_len),
                          static_cast<uint32_t>(g.sps_len), static_cast<uint32_t>(g.pps_len), s.idr_hook_set ? 1 : 0);
  osp::DebugShell::Printf(
      "au in=%" PRIu64 " ok=%" PRIu64 " drop=%" PRIu64 " gaps=%" PRIu64 " source_stopped=%d loop_failed=%d\r\n",
      static_cast<uint64_t>(g.au_in), static_cast<uint64_t>(g.au_accepted), static_cast<uint64_t>(g.au_dropped),
      static_cast<uint64_t>(g.input_gaps), g.source_stopped ? 1 : 0, g.loop_failed ? 1 : 0);
  return 0;
}

int CmdRtcp(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  uint32_t sr = 0U;
  uint32_t rr = 0U;
  uint32_t bye = 0U;
  uint32_t bad = 0U;
  for (uint32_t i = 0U; i < s.client_count; ++i) {
    const SessionCounters& c = s.clients[i].session.counters;
    sr += c.rtcp_sr;
    rr += c.rtcp_rr;
    bye += c.rtcp_bye;
    bad += c.rtcp_bad;
  }
  osp::DebugShell::Printf(
      "rtcp sr=%" PRIu32 " rr=%" PRIu32 " bye=%" PRIu32 " bad=%" PRIu32 " (per-client counters)\r\n",
      static_cast<uint32_t>(sr), static_cast<uint32_t>(rr), static_cast<uint32_t>(bye), static_cast<uint32_t>(bad));
  return 0;
}

int CmdLatency(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  osp::DebugShell::Printf("loop p50=%" PRIu32 "us p95=%" PRIu32 "us p99=%" PRIu32 "us samples=%" PRIu64 "\r\n",
                          static_cast<uint32_t>(s.stats.loop_p50_us), static_cast<uint32_t>(s.stats.loop_p95_us),
                          static_cast<uint32_t>(s.stats.loop_p99_us), static_cast<uint64_t>(s.stats.loop_samples));
  return 0;
}

int CmdMem(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  osp::DebugShell::Printf("sizeof server=%" PRIu32 " session=%" PRIu32 " ingress=%" PRIu32 " snapshot=%" PRIu32 "\r\n",
                          static_cast<uint32_t>(sizeof(RtspServer)), static_cast<uint32_t>(sizeof(RtspSession)),
                          static_cast<uint32_t>(sizeof(FrameIngress)), static_cast<uint32_t>(sizeof(ServerSnapshot)));
  osp::DebugShell::Printf("frame_pool=%" PRIu32 " bytes; rx=%" PRIu32 " tx=%" PRIu32 " per_conn; max_conns=%" PRIu32
                          "\r\n",
                          static_cast<uint32_t>(kFrameSlots * kMaxAuBytes), static_cast<uint32_t>(kRtspRxBytes),
                          static_cast<uint32_t>(kRtspTxBytes), static_cast<uint32_t>(kMaxConnections));
  return 0;
}

int CmdConfig(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot& s = ShellSnapshot();
  osp::DebugShell::Printf("port=%" PRIu32 " advertise=%s max_clients=%" PRIu32 " frame_slots=%" PRIu32 "\r\n",
                          static_cast<uint32_t>(s.port), s.advertise_ip, static_cast<uint32_t>(s.max_clients),
                          static_cast<uint32_t>(kFrameSlots));
  osp::DebugShell::Printf("rx=%" PRIu32 " tx=%" PRIu32 " payload=%" PRIu32 " fps=%" PRIu32 " client_rate=%" PRIu32
                          "bps burst=%" PRIu32 "B log=%s\r\n",
                          static_cast<uint32_t>(kRtspRxBytes), static_cast<uint32_t>(kRtspTxBytes),
                          static_cast<uint32_t>(kRtpPayloadMax), static_cast<uint32_t>(kFps),
                          static_cast<uint32_t>(kClientEgressRateBps), static_cast<uint32_t>(kClientEgressBurstBytes),
                          LogLevelName(s.log_level));
  return 0;
}

int CmdTime(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  struct timespec real{};
  static_cast<void>(clock_gettime(CLOCK_REALTIME, &real));
  osp::DebugShell::Printf("steady_ns=%" PRIu64 " realtime=%" PRIi64 ".%09ld\r\n",
                          static_cast<uint64_t>(osp::SteadyNowNs()), static_cast<int64_t>(real.tv_sec), real.tv_nsec);
  return 0;
}

int CmdVersion(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  osp::DebugShell::Printf("rs %s built %s %s\r\n", kVersion, __DATE__, __TIME__);
  osp::DebugShell::Printf("newosp: see build; max_conns=%" PRIu32 " frame_slots=%" PRIu32 "\r\n",
                          static_cast<uint32_t>(kMaxConnections), static_cast<uint32_t>(kFrameSlots));
  return 0;
}

int CmdLog(int argc, char** argv) noexcept {
  if (!RequireServer()) {
    return -1;
  }
  if (argc != 2) {
    osp::DebugShell::Printf("usage: log error|warn|info|debug\r\n");
    return -1;
  }
  osp::log::Level level = osp::log::Level::kInfo;
  if (std::strcmp(argv[1], "error") == 0) {
    level = osp::log::Level::kError;
  } else if (std::strcmp(argv[1], "warn") == 0) {
    level = osp::log::Level::kWarn;
  } else if (std::strcmp(argv[1], "info") == 0) {
    level = osp::log::Level::kInfo;
  } else if (std::strcmp(argv[1], "debug") == 0) {
    level = osp::log::Level::kDebug;
  } else {
    osp::DebugShell::Printf("unknown level\r\n");
    return -1;
  }
  Server()->SetLogLevel(level);
  osp::DebugShell::Printf("log level=%s\r\n", LogLevelName(static_cast<uint8_t>(level)));
  return 0;
}

int CmdKick(int argc, char** argv) noexcept {
  if (!RequireServer()) {
    return -1;
  }
  if (argc != 2) {
    osp::DebugShell::Printf("usage: kick <slot>\r\n");
    return -1;
  }
  uint32_t slot = 0U;
  if (!ParseU32(argv[1], slot) || slot >= kMaxConnections) {
    osp::DebugShell::Printf("invalid slot\r\n");
    return -1;
  }
  if (!SlotActive(ShellSnapshot(), static_cast<uint32_t>(slot))) {
    osp::DebugShell::Printf("slot not active\r\n");
    return -1;
  }
  const bool ok = Server()->PostShellCommand(ShellCommand{ShellCommandId::kKick, static_cast<uint32_t>(slot), 0U});
  osp::DebugShell::Printf(ok ? "kick queued\r\n" : "shell queue busy\r\n");
  return ok ? 0 : -1;
}

int CmdReset(int argc, char** argv) noexcept {
  if (!RequireServer()) {
    return -1;
  }
  ShellCommand cmd;
  if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "stats") == 0)) {
    cmd.id = ShellCommandId::kResetStats;
  } else if (argc == 3 && std::strcmp(argv[1], "client") == 0) {
    uint32_t slot = 0U;
    if (!ParseU32(argv[2], slot) || slot >= kMaxConnections) {
      osp::DebugShell::Printf("invalid slot\r\n");
      return -1;
    }
    if (!SlotActive(ShellSnapshot(), static_cast<uint32_t>(slot))) {
      osp::DebugShell::Printf("slot not active\r\n");
      return -1;
    }
    cmd.id = ShellCommandId::kResetClient;
    cmd.arg0 = static_cast<uint32_t>(slot);
  } else {
    osp::DebugShell::Printf("usage: reset [stats] | reset client <slot>\r\n");
    return -1;
  }
  const bool ok = Server()->PostShellCommand(cmd);
  osp::DebugShell::Printf(ok ? "reset queued\r\n" : "shell queue busy\r\n");
  return ok ? 0 : -1;
}

int CmdTrace(int argc, char** argv) noexcept {
  if (!RequireServer()) {
    return -1;
  }
  if (argc != 3) {
    osp::DebugShell::Printf("usage: trace <slot> on|off\r\n");
    return -1;
  }
  uint32_t slot = 0U;
  if (!ParseU32(argv[1], slot) || slot >= kMaxConnections) {
    osp::DebugShell::Printf("invalid slot\r\n");
    return -1;
  }
  uint32_t enable = 0U;
  if (std::strcmp(argv[2], "on") == 0) {
    enable = 1U;
  } else if (std::strcmp(argv[2], "off") != 0) {
    osp::DebugShell::Printf("usage: trace <slot> on|off\r\n");
    return -1;
  }
  if (!SlotActive(ShellSnapshot(), static_cast<uint32_t>(slot))) {
    osp::DebugShell::Printf("slot not active\r\n");
    return -1;
  }
  const bool ok =
      Server()->PostShellCommand(ShellCommand{ShellCommandId::kSetTrace, static_cast<uint32_t>(slot), enable});
  if (ok) {
    osp::DebugShell::Printf("trace queued (set 'log debug' to see packet lines)\r\n");
  } else {
    osp::DebugShell::Printf("shell queue busy\r\n");
  }
  return ok ? 0 : -1;
}

int CmdIdr(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  if (!ShellSnapshot().idr_hook_set) {
    osp::DebugShell::Printf("idr: no encoder hook registered\r\n");
    return -1;
  }
  const bool ok = Server()->PostShellCommand(ShellCommand{ShellCommandId::kRequestIdr, 0U, 0U});
  osp::DebugShell::Printf(ok ? "idr request queued\r\n" : "shell queue busy\r\n");
  return ok ? 0 : -1;
}

int CmdShutdown(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  Server()->RequestStop();
  osp::DebugShell::Printf("shutdown requested\r\n");
  return 0;
}

#if defined(RS_ENABLE_FAULT_INJECTION)
int CmdFault(int argc, char** argv) noexcept {
  if (!RequireServer()) {
    return -1;
  }
  if (argc < 3) {
    osp::DebugShell::Printf(
        "usage: fault drop <n> | fault stall <slot> <ms> | fault eagain <slot> <n> | fault idr <slot> <n>\r\n");
    return -1;
  }
  if (std::strcmp(argv[1], "drop") == 0 && argc == 3) {
    uint32_t n = 0U;
    if (!ParseU32(argv[2], n)) {
      osp::DebugShell::Printf("invalid count\r\n");
      return -1;
    }
    const bool ok = Server()->PostShellCommand(ShellCommand{ShellCommandId::kFaultDrop, static_cast<uint32_t>(n), 0U});
    osp::DebugShell::Printf(ok ? "fault drop queued\r\n" : "shell queue busy\r\n");
    return ok ? 0 : -1;
  }
  if (argc != 4) {
    osp::DebugShell::Printf(
        "usage: fault drop <n> | fault stall <slot> <ms> | fault eagain <slot> <n> | fault idr <slot> <n>\r\n");
    return -1;
  }
  uint32_t slot = 0U;
  if (!ParseU32(argv[2], slot) || slot >= kMaxConnections) {
    osp::DebugShell::Printf("invalid slot\r\n");
    return -1;
  }
  if (!SlotActive(ShellSnapshot(), static_cast<uint32_t>(slot))) {
    osp::DebugShell::Printf("slot not active\r\n");
    return -1;
  }
  uint32_t arg = 0U;
  if (!ParseU32(argv[3], arg)) {
    osp::DebugShell::Printf("invalid argument\r\n");
    return -1;
  }
  ShellCommand cmd;
  cmd.arg0 = static_cast<uint32_t>(slot);
  cmd.arg1 = static_cast<uint32_t>(arg);
  if (std::strcmp(argv[1], "stall") == 0) {
    cmd.id = ShellCommandId::kFaultStall;
  } else if (std::strcmp(argv[1], "eagain") == 0) {
    cmd.id = ShellCommandId::kFaultEagain;
  } else if (std::strcmp(argv[1], "idr") == 0) {
    cmd.id = ShellCommandId::kFaultIdr;
  } else {
    osp::DebugShell::Printf("unknown fault\r\n");
    return -1;
  }
  const bool ok = Server()->PostShellCommand(cmd);
  osp::DebugShell::Printf(ok ? "fault queued\r\n" : "shell queue busy\r\n");
  return ok ? 0 : -1;
}

int CmdSlow(int argc, char** argv) noexcept {
  if (!RequireServer()) {
    return -1;
  }
  if (argc != 4 || std::strcmp(argv[1], "slow") != 0) {
    osp::DebugShell::Printf("usage: simulate slow <slot> <bps> (0 restores)\r\n");
    return -1;
  }
  uint32_t slot = 0U;
  if (!ParseU32(argv[2], slot) || slot >= kMaxConnections) {
    osp::DebugShell::Printf("invalid slot\r\n");
    return -1;
  }
  if (!SlotActive(ShellSnapshot(), static_cast<uint32_t>(slot))) {
    osp::DebugShell::Printf("slot not active\r\n");
    return -1;
  }
  uint32_t bps = 0U;
  if (!ParseU32(argv[3], bps)) {
    osp::DebugShell::Printf("invalid bps\r\n");
    return -1;
  }
  const bool ok = Server()->PostShellCommand(
      ShellCommand{ShellCommandId::kSlowClient, static_cast<uint32_t>(slot), static_cast<uint32_t>(bps)});
  osp::DebugShell::Printf(ok ? "simulate slow queued\r\n" : "shell queue busy\r\n");
  return ok ? 0 : -1;
}
#endif

void RegisterShellCommands() noexcept {
  static osp::ShellAutoReg stats{"stats", CmdStats, "Show global server counters"};
  static osp::ShellAutoReg clients{"clients", CmdClients, "List active RTSP clients"};
  static osp::ShellAutoReg client{"client", CmdClient, "Show one client: client <slot>"};
  static osp::ShellAutoReg pool{"pool", CmdPool, "Show frame pool and queues"};
  static osp::ShellAutoReg fd{"fd", CmdFd, "Show listener/client fds and watchers"};
  static osp::ShellAutoReg timers{"timers", CmdTimers, "Show timers and snapshot age"};
  static osp::ShellAutoReg stream{"stream", CmdStream, "Show codec and source state"};
  static osp::ShellAutoReg net{"net", CmdNet, "Show TCP RTT/cwnd/retrans/socket buffers"};
  static osp::ShellAutoReg rtcp{"rtcp", CmdRtcp, "Show RTCP status"};
  static osp::ShellAutoReg latency{"latency", CmdLatency, "Show loop latency percentiles"};
  static osp::ShellAutoReg mem{"mem", CmdMem, "Show sizes and static buffers"};
  static osp::ShellAutoReg config{"config", CmdConfig, "Show runtime limits"};
  static osp::ShellAutoReg time{"time", CmdTime, "Show monotonic and wall clock"};
  static osp::ShellAutoReg version{"version", CmdVersion, "Show build version"};
  static osp::ShellAutoReg log{"log", CmdLog, "Set log level", osp::ShellCmdFlags::kAdmin};
  static osp::ShellAutoReg kick{"kick", CmdKick, "Close one client: kick <slot>", osp::ShellCmdFlags::kAdmin};
  static osp::ShellAutoReg reset{"reset", CmdReset, "Reset stats or one client", osp::ShellCmdFlags::kAdmin};
  static osp::ShellAutoReg trace{"trace", CmdTrace, "Toggle packet trace: trace <slot> on|off",
                                 osp::ShellCmdFlags::kAdmin};
  static osp::ShellAutoReg idr{"idr", CmdIdr, "Request an IDR from the registered encoder hook",
                               osp::ShellCmdFlags::kAdmin};
  static osp::ShellAutoReg shutdown{"shutdown", CmdShutdown, "Request graceful shutdown", osp::ShellCmdFlags::kAdmin};
#if defined(RS_ENABLE_FAULT_INJECTION)
  static osp::ShellAutoReg fault{"fault", CmdFault, "Fault injection: drop/stall/eagain/idr", osp::ShellCmdFlags::kAdmin};
  static osp::ShellAutoReg simulate{"simulate", CmdSlow, "Simulate slow client: simulate slow <slot> <bps>",
                                    osp::ShellCmdFlags::kAdmin};
#endif
}

}  // namespace

void BindDebugShell(RtspServer* server) noexcept {
  ShellContext::Instance().Bind(server);
  RegisterShellCommands();
}

}  // namespace rs
