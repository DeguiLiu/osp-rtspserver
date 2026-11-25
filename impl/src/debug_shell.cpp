// Read-only msh-like debug shell for the RTSP server (P0).
//
// Command callbacks run on DebugShell session threads. They only read
// RtspServer::Snapshot(); no socket/session/RTP state is touched here.
// SPDX-License-Identifier: MIT
#include "rs/debug_shell.hpp"

#include "osp/shell.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
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

RtspServer* Server() noexcept { return ShellContext::Instance().Server(); }

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
  switch (level) {
    case static_cast<uint8_t>(LogLevel::kError):
      return "error";
    case static_cast<uint8_t>(LogLevel::kWarn):
      return "warn";
    case static_cast<uint8_t>(LogLevel::kInfo):
      return "info";
    case static_cast<uint8_t>(LogLevel::kDebug):
      return "debug";
    default:
      return "?";
  }
}

void PrintIpv4(uint32_t ip) noexcept {
  osp::DebugShell::Printf("%u.%u.%u.%u", static_cast<unsigned>(ip & 0xFFU),
                          static_cast<unsigned>((ip >> 8U) & 0xFFU),
                          static_cast<unsigned>((ip >> 16U) & 0xFFU),
                          static_cast<unsigned>((ip >> 24U) & 0xFFU));
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
  const ServerSnapshot s = Server()->Snapshot();
  const ServerStats& g = s.stats;
  osp::DebugShell::Printf("conns=%u/%u playing=%u accepts=%llu full=%llu failed=%llu reqs=%llu\r\n",
                          static_cast<unsigned>(g.connections_active), static_cast<unsigned>(g.connections_peak),
                          static_cast<unsigned>(g.sessions_playing), static_cast<unsigned long long>(g.accepts),
                          static_cast<unsigned long long>(g.accept_full),
                          static_cast<unsigned long long>(g.accept_failed),
                          static_cast<unsigned long long>(g.requests));
  osp::DebugShell::Printf("rate limited accept=%llu request=%llu\r\n",
                          static_cast<unsigned long long>(g.accept_rate_limited),
                          static_cast<unsigned long long>(g.request_rate_limited));
  osp::DebugShell::Printf("au in=%llu ok=%llu drop=%llu gaps=%llu pool_drop=%llu qfull=%llu\r\n",
                          static_cast<unsigned long long>(g.au_in),
                          static_cast<unsigned long long>(g.au_accepted),
                          static_cast<unsigned long long>(g.au_dropped),
                          static_cast<unsigned long long>(g.input_gaps),
                          static_cast<unsigned long long>(g.frame_pool_drop),
                          static_cast<unsigned long long>(g.input_queue_full));
  osp::DebugShell::Printf("idr wait=%llu req=%llu timeout=%llu qdrop=%llu tx=%llu short=%llu eagain=%llu\r\n",
                          static_cast<unsigned long long>(g.idr_waits),
                          static_cast<unsigned long long>(g.idr_requests),
                          static_cast<unsigned long long>(g.idr_timeouts),
                          static_cast<unsigned long long>(g.queue_drops),
                          static_cast<unsigned long long>(g.tx_bytes),
                          static_cast<unsigned long long>(g.short_writes),
                          static_cast<unsigned long long>(g.would_block));
  osp::DebugShell::Printf("loop p50=%uus p95=%uus p99=%uus samples=%llu source_stopped=%d loop_failed=%d\r\n",
                          static_cast<unsigned>(g.loop_p50_us), static_cast<unsigned>(g.loop_p95_us),
                          static_cast<unsigned>(g.loop_p99_us),
                          static_cast<unsigned long long>(g.loop_samples), g.source_stopped ? 1 : 0,
                          g.loop_failed ? 1 : 0);
  osp::DebugShell::Printf("shell commands=%llu rejected=%llu log=%s fault_drop=%u\r\n",
                          static_cast<unsigned long long>(g.shell_commands),
                          static_cast<unsigned long long>(g.shell_rejects), LogLevelName(s.log_level),
                          static_cast<unsigned>(s.fault_drop_remaining));
  return 0;
}

int CmdClients(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot s = Server()->Snapshot();
  if (s.client_count == 0U) {
    osp::DebugShell::Printf("no active clients\r\n");
    return 0;
  }
  osp::DebugShell::Printf("slot state   transport peer            rtp/rtcp queue bytes age_ms pkts qdrop close\r\n");
  for (uint32_t i = 0U; i < s.client_count; ++i) {
    const ClientSnapshot& c = s.clients[i];
    const SessionSnapshot& x = c.session;
    osp::DebugShell::Printf("%2u   %-7s %-9s ", static_cast<unsigned>(c.slot),
                            sess::StateName(x.state), TransportName(x.transport));
    PrintIpv4(x.client_ipv4);
    osp::DebugShell::Printf(" %u/%u %u %u %u %u %u %s\r\n", static_cast<unsigned>(x.client_rtp_port),
                            static_cast<unsigned>(x.client_rtcp_port), static_cast<unsigned>(x.queue_depth),
                            static_cast<unsigned>(x.queue_bytes), static_cast<unsigned>(x.oldest_age_ms),
                            static_cast<unsigned>(x.counters.packets_sent),
                            static_cast<unsigned>(x.counters.queue_drops), CloseReasonName(x.close_reason));
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
  const ServerSnapshot s = Server()->Snapshot();
  for (uint32_t i = 0U; i < s.client_count; ++i) {
    const ClientSnapshot& c = s.clients[i];
    if (c.slot != slot) {
      continue;
    }
    const SessionSnapshot& x = c.session;
    osp::DebugShell::Printf("slot=%u gen=%u fd=%d closing=%d connected_ms=%llu\r\n",
                            static_cast<unsigned>(c.slot), static_cast<unsigned>(c.gen), c.fd,
                            c.closing ? 1 : 0, static_cast<unsigned long long>(c.connected_ms));
    osp::DebugShell::Printf("state=%s transport=%s playing=%d active=%d awaiting_rtp_info=%d closed=%d trace=%d\r\n",
                            sess::StateName(x.state), TransportName(x.transport), x.playing ? 1 : 0,
                            x.active ? 1 : 0, x.awaiting_rtp_info ? 1 : 0, x.closed ? 1 : 0,
                            x.trace_enabled ? 1 : 0);
    osp::DebugShell::Printf("peer=");
    PrintIpv4(x.client_ipv4);
    osp::DebugShell::Printf(" client_rtp=%u client_rtcp=%u server_rtp=%u server_rtcp=%u\r\n",
                            static_cast<unsigned>(x.client_rtp_port), static_cast<unsigned>(x.client_rtcp_port),
                            static_cast<unsigned>(x.server_rtp_port), static_cast<unsigned>(x.server_rtcp_port));
    osp::DebugShell::Printf("channel rtp=%u rtcp=%u seq=%u ts=%u ssrc=%u\r\n", static_cast<unsigned>(x.rtp_channel),
                            static_cast<unsigned>(x.rtcp_channel), static_cast<unsigned>(x.seq),
                            static_cast<unsigned>(x.rtp_ts), static_cast<unsigned>(x.ssrc));
    osp::DebugShell::Printf("queue depth=%u bytes=%u oldest=%ums stall_until=%llu\r\n",
                            static_cast<unsigned>(x.queue_depth), static_cast<unsigned>(x.queue_bytes),
                            static_cast<unsigned>(x.oldest_age_ms),
                            static_cast<unsigned long long>(c.stall_until_ns));
    osp::DebugShell::Printf("tcp rtt=%uus cwnd=%u retrans=%u unacked=%u notsent=%u sndbuf=%u rcvbuf=%u\r\n",
                            static_cast<unsigned>(c.tcp_rtt_us), static_cast<unsigned>(c.tcp_cwnd),
                            static_cast<unsigned>(c.tcp_retrans), static_cast<unsigned>(c.tcp_unacked),
                            static_cast<unsigned>(c.tcp_notsent), static_cast<unsigned>(c.sndbuf),
                            static_cast<unsigned>(c.rcvbuf));
    osp::DebugShell::Printf("fault eagain=%u idr=%u slow_bps=%u\r\n", static_cast<unsigned>(x.fault_eagain),
                            static_cast<unsigned>(x.fault_idr), static_cast<unsigned>(x.slow_bps));
    osp::DebugShell::Printf("drops queue=%u udp=%u token_wait=%u interleaved=%u gaps=%u rtcp_bad=%u\r\n",
                            static_cast<unsigned>(x.counters.queue_drops), static_cast<unsigned>(x.counters.udp_drops),
                            static_cast<unsigned>(x.counters.token_waits),
                            static_cast<unsigned>(x.counters.interleaved_errors),
                            static_cast<unsigned>(x.counters.input_gaps),
                            static_cast<unsigned>(x.counters.rtcp_bad));
    osp::DebugShell::Printf("frames=%u packets=%u close=%s session=%s\r\n",
                            static_cast<unsigned>(x.counters.frames_completed),
                            static_cast<unsigned>(x.counters.packets_sent), CloseReasonName(x.close_reason),
                            x.session_id_prefix);
    return 0;
  }
  osp::DebugShell::Printf("slot %u not active\r\n", static_cast<unsigned>(slot));
  return -1;
}

int CmdNet(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot s = Server()->Snapshot();
  osp::DebugShell::Printf("slot rtt_us cwnd retrans unacked notsent sndbuf rcvbuf\r\n");
  for (uint32_t i = 0U; i < s.client_count; ++i) {
    const ClientSnapshot& c = s.clients[i];
    osp::DebugShell::Printf("%2u   %6u %4u %7u %7u %7u %6u %6u\r\n", static_cast<unsigned>(c.slot),
                            static_cast<unsigned>(c.tcp_rtt_us), static_cast<unsigned>(c.tcp_cwnd),
                            static_cast<unsigned>(c.tcp_retrans), static_cast<unsigned>(c.tcp_unacked),
                            static_cast<unsigned>(c.tcp_notsent), static_cast<unsigned>(c.sndbuf),
                            static_cast<unsigned>(c.rcvbuf));
  }
  return 0;
}

int CmdPool(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot s = Server()->Snapshot();
  osp::DebugShell::Printf("frames slots=%u free=%u in_use=%u input_q=%u empty_q=%u\r\n",
                          static_cast<unsigned>(kFrameSlots), static_cast<unsigned>(s.pool.free_slots),
                          static_cast<unsigned>(s.pool.in_use_slots), static_cast<unsigned>(s.pool.input_queued),
                          static_cast<unsigned>(s.pool.empty_queued));
  osp::DebugShell::Printf("orphan producer=%u loop=%u\r\n", static_cast<unsigned>(s.pool.producer_orphan_drops),
                          static_cast<unsigned>(s.pool.loop_orphan_drops));
  return 0;
}

int CmdFd(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot s = Server()->Snapshot();
  osp::DebugShell::Printf("listener fd=%d watchers=%u timers=%u\r\n", s.listener_fd,
                          static_cast<unsigned>(s.watchers_used), static_cast<unsigned>(s.timers_used));
  for (uint32_t i = 0U; i < s.client_count; ++i) {
    const ClientSnapshot& c = s.clients[i];
    const SessionSnapshot& x = c.session;
    osp::DebugShell::Printf("slot=%u fd=%d %s ", static_cast<unsigned>(c.slot), c.fd, TransportName(x.transport));
    PrintIpv4(x.client_ipv4);
    osp::DebugShell::Printf(" udp=%u/%u channels=%u/%u\r\n", static_cast<unsigned>(x.server_rtp_port),
                            static_cast<unsigned>(x.server_rtcp_port), static_cast<unsigned>(x.rtp_channel),
                            static_cast<unsigned>(x.rtcp_channel));
  }
  return 0;
}

int CmdTimers(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot s = Server()->Snapshot();
  const uint64_t age_ms = (s.published_ns == 0U) ? 0U : (osp::SteadyNowNs() - s.published_ns) / kNsPerMs;
  osp::DebugShell::Printf("maintenance=%ums timers_used=%u snapshot_age=%llums\r\n",
                          static_cast<unsigned>(kMaintenancePeriodMs), static_cast<unsigned>(s.timers_used),
                          static_cast<unsigned long long>(age_ms));
  return 0;
}

int CmdStream(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot s = Server()->Snapshot();
  const ServerStats& g = s.stats;
  osp::DebugShell::Printf("codec=%s param_gen=%u vps=%u sps=%u pps=%u idr_hook=%d\r\n", CodecName(g.codec),
                          static_cast<unsigned>(g.param_gen),
                          static_cast<unsigned>(g.vps_len), static_cast<unsigned>(g.sps_len),
                          static_cast<unsigned>(g.pps_len), s.idr_hook_set ? 1 : 0);
  osp::DebugShell::Printf("au in=%llu ok=%llu drop=%llu gaps=%llu source_stopped=%d loop_failed=%d\r\n",
                          static_cast<unsigned long long>(g.au_in),
                          static_cast<unsigned long long>(g.au_accepted),
                          static_cast<unsigned long long>(g.au_dropped),
                          static_cast<unsigned long long>(g.input_gaps), g.source_stopped ? 1 : 0,
                          g.loop_failed ? 1 : 0);
  return 0;
}

int CmdRtcp(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot s = Server()->Snapshot();
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
  osp::DebugShell::Printf("rtcp sr=%u rr=%u bye=%u bad=%u (per-client counters)\r\n",
                          static_cast<unsigned>(sr), static_cast<unsigned>(rr), static_cast<unsigned>(bye),
                          static_cast<unsigned>(bad));
  return 0;
}

int CmdLatency(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot s = Server()->Snapshot();
  osp::DebugShell::Printf("loop p50=%uus p95=%uus p99=%uus samples=%llu\r\n",
                          static_cast<unsigned>(s.stats.loop_p50_us), static_cast<unsigned>(s.stats.loop_p95_us),
                          static_cast<unsigned>(s.stats.loop_p99_us),
                          static_cast<unsigned long long>(s.stats.loop_samples));
  return 0;
}

int CmdMem(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  osp::DebugShell::Printf("sizeof server=%u session=%u ingress=%u snapshot=%u\r\n",
                          static_cast<unsigned>(sizeof(RtspServer)), static_cast<unsigned>(sizeof(RtspSession)),
                          static_cast<unsigned>(sizeof(FrameIngress)), static_cast<unsigned>(sizeof(ServerSnapshot)));
  osp::DebugShell::Printf("frame_pool=%u bytes; rx=%u tx=%u per_conn; max_conns=%u\r\n",
                          static_cast<unsigned>(kFrameSlots * kMaxAuBytes), static_cast<unsigned>(kRtspRxBytes),
                          static_cast<unsigned>(kRtspTxBytes), static_cast<unsigned>(kMaxConnections));
  return 0;
}

int CmdConfig(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  if (!RequireServer()) {
    return -1;
  }
  const ServerSnapshot s = Server()->Snapshot();
  osp::DebugShell::Printf("port=%u advertise=%s max_clients=%u frame_slots=%u\r\n",
                          static_cast<unsigned>(s.port), s.advertise_ip, static_cast<unsigned>(s.max_clients),
                          static_cast<unsigned>(kFrameSlots));
  osp::DebugShell::Printf("rx=%u tx=%u payload=%u fps=%u client_rate=%ubps burst=%uB log=%s\r\n",
                          static_cast<unsigned>(kRtspRxBytes), static_cast<unsigned>(kRtspTxBytes),
                          static_cast<unsigned>(kRtpPayloadMax), static_cast<unsigned>(kFps),
                          static_cast<unsigned>(kClientEgressRateBps),
                          static_cast<unsigned>(kClientEgressBurstBytes), LogLevelName(s.log_level));
  return 0;
}

int CmdTime(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  struct timespec real {};
  static_cast<void>(clock_gettime(CLOCK_REALTIME, &real));
  osp::DebugShell::Printf("steady_ns=%llu realtime=%lld.%09ld\r\n",
                          static_cast<unsigned long long>(osp::SteadyNowNs()),
                          static_cast<long long>(real.tv_sec), real.tv_nsec);
  return 0;
}

int CmdVersion(int argc, char** argv) noexcept {
  static_cast<void>(argc);
  static_cast<void>(argv);
  osp::DebugShell::Printf("rs %s built %s %s\r\n", kVersion, __DATE__, __TIME__);
  osp::DebugShell::Printf("newosp: see build; max_conns=%u frame_slots=%u\r\n",
                          static_cast<unsigned>(kMaxConnections), static_cast<unsigned>(kFrameSlots));
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
  LogLevel level = LogLevel::kInfo;
  if (std::strcmp(argv[1], "error") == 0) {
    level = LogLevel::kError;
  } else if (std::strcmp(argv[1], "warn") == 0) {
    level = LogLevel::kWarn;
  } else if (std::strcmp(argv[1], "info") == 0) {
    level = LogLevel::kInfo;
  } else if (std::strcmp(argv[1], "debug") == 0) {
    level = LogLevel::kDebug;
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
  if (!SlotActive(Server()->Snapshot(), static_cast<uint32_t>(slot))) {
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
    if (!SlotActive(Server()->Snapshot(), static_cast<uint32_t>(slot))) {
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
  if (!SlotActive(Server()->Snapshot(), static_cast<uint32_t>(slot))) {
    osp::DebugShell::Printf("slot not active\r\n");
    return -1;
  }
  const bool ok = Server()->PostShellCommand(
      ShellCommand{ShellCommandId::kSetTrace, static_cast<uint32_t>(slot), enable});
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
  if (!Server()->Snapshot().idr_hook_set) {
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
  if (!SlotActive(Server()->Snapshot(), static_cast<uint32_t>(slot))) {
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
  if (!SlotActive(Server()->Snapshot(), static_cast<uint32_t>(slot))) {
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
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  auto& reg = osp::detail::GlobalCmdRegistry::Instance();
  static_cast<void>(reg.Register("stats", CmdStats, "Show global server counters"));
  static_cast<void>(reg.Register("clients", CmdClients, "List active RTSP clients"));
  static_cast<void>(reg.Register("client", CmdClient, "Show one client: client <slot>"));
  static_cast<void>(reg.Register("pool", CmdPool, "Show frame pool and queues"));
  static_cast<void>(reg.Register("fd", CmdFd, "Show listener/client fds and watchers"));
  static_cast<void>(reg.Register("timers", CmdTimers, "Show timers and snapshot age"));
  static_cast<void>(reg.Register("stream", CmdStream, "Show codec and source state"));
  static_cast<void>(reg.Register("net", CmdNet, "Show TCP RTT/cwnd/retrans/socket buffers"));
  static_cast<void>(reg.Register("rtcp", CmdRtcp, "Show RTCP status"));
  static_cast<void>(reg.Register("latency", CmdLatency, "Show loop latency percentiles"));
  static_cast<void>(reg.Register("mem", CmdMem, "Show sizes and static buffers"));
  static_cast<void>(reg.Register("config", CmdConfig, "Show runtime limits"));
  static_cast<void>(reg.Register("time", CmdTime, "Show monotonic and wall clock"));
  static_cast<void>(reg.Register("version", CmdVersion, "Show build version"));
  static_cast<void>(reg.Register("log", CmdLog, "Set log level", osp::ShellCmdFlags::kAdmin));
  static_cast<void>(reg.Register("kick", CmdKick, "Close one client: kick <slot>", osp::ShellCmdFlags::kAdmin));
  static_cast<void>(reg.Register("reset", CmdReset, "Reset stats or one client", osp::ShellCmdFlags::kAdmin));
  static_cast<void>(
      reg.Register("trace", CmdTrace, "Toggle packet trace: trace <slot> on|off", osp::ShellCmdFlags::kAdmin));
  static_cast<void>(reg.Register("idr", CmdIdr, "Request an IDR from the registered encoder hook",
                                 osp::ShellCmdFlags::kAdmin));
  static_cast<void>(reg.Register("shutdown", CmdShutdown, "Request graceful shutdown",
                                 osp::ShellCmdFlags::kAdmin));
#if defined(RS_ENABLE_FAULT_INJECTION)
  static_cast<void>(reg.Register("fault", CmdFault, "Fault injection: drop/stall/eagain/idr",
                                 osp::ShellCmdFlags::kAdmin));
  static_cast<void>(reg.Register("simulate", CmdSlow, "Simulate slow client: simulate slow <slot> <bps>",
                                 osp::ShellCmdFlags::kAdmin));
#endif
}

}  // namespace

void BindDebugShell(RtspServer* server) noexcept {
  ShellContext::Instance().Bind(server);
  RegisterShellCommands();
}

}  // namespace rs
