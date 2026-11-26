// Real-process end-to-end tests: spawn rtspserver_demo, drive it with raw
// RTSP/RTP sockets and compare reassembled H.264/H.265 NAL units with the
// source stream. Every case owns its server process and port; a server is
// stopped and reaped before the next case starts so a broken listener cannot
// cascade into the remaining cases. A missing or broken server fails the test;
// nothing is skipped silently.
// SPDX-License-Identifier: MIT
#include "e2e_client.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef RS_DEMO_BIN
#define RS_DEMO_BIN "rtspserver_demo"
#endif
#ifndef RS_STREAM_TOOL
#define RS_STREAM_TOOL "make_test_stream"
#endif

namespace {

using rse2e::Codec;

constexpr uint32_t kConnectTimeoutMs = 3000U;
constexpr uint32_t kResponseTimeoutMs = 5000U;
constexpr uint32_t kMediaTimeoutMs = 8000U;
constexpr uint32_t kServerStartTimeoutMs = 10000U;
constexpr uint32_t kMaxRtpDatagramBytes = 12U + 1200U;
constexpr uint16_t kExpectedPayloadType = 96U;
constexpr uint32_t kFfmpegFrames = 25U;
constexpr uint32_t kRetryPortStep = 200U;

enum class StreamKind : uint8_t { kSynthH264 = 0U, kSynthH265 = 1U, kRealH264 = 2U, kRealH265 = 3U };

Codec CodecOf(StreamKind kind) noexcept {
  return (StreamKind::kSynthH265 == kind || StreamKind::kRealH265 == kind) ? Codec::kH265 : Codec::kH264;
}

struct Ctx {
  Codec codec = Codec::kH264;
  uint32_t case_index = 0U;
  std::string stream_path;
  std::string server_log_path;
  uint16_t port = 0U;
  pid_t server_pid = -1;
  bool server_exited = false;
  int server_exit_code = -1;
  std::vector<rse2e::AccessUnit> file_aus;
};

struct RtspSession {
  rse2e::Client client;
  uint32_t cseq = 1U;
  Codec sdp_codec = Codec::kH264;
  std::string base_uri;
  std::string track_uri;
  std::string session_id;
  std::string transport;
  uint32_t rtp_info_seq = 0U;
  uint32_t rtp_info_ts = 0U;
  bool have_rtp_info = false;
};

struct MediaCollect {
  Codec codec = Codec::kH264;
  rse2e::H264Depacketizer h264;
  rse2e::H265Depacketizer h265;
  std::vector<rse2e::AccessUnit> aus;
  std::vector<rse2e::NalUnit> current;
  uint32_t packets = 0U;
  uint32_t other_channel = 0U;
  uint32_t channel_frames[128] = {};
  uint32_t max_frame_bytes = 0U;
  uint32_t seq_gaps = 0U;
  uint16_t first_seq = 0U;
  uint32_t first_ts = 0U;
  bool have_first = false;
  std::string err;
};

int64_t NowMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int RemainingMs(int64_t deadline) noexcept {
  const int64_t left = deadline - NowMs();
  return (left > 0) ? static_cast<int>(left) : 0;
}

std::string TrimText(const std::string& text) {
  size_t first = 0U;
  size_t last = text.size();
  while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n')) {
    ++first;
  }
  while (last > first &&
         (text[last - 1U] == ' ' || text[last - 1U] == '\t' || text[last - 1U] == '\r' || text[last - 1U] == '\n')) {
    --last;
  }
  return text.substr(first, last - first);
}

std::string TailText(const std::string& text, size_t keep) {
  if (text.size() <= keep) {
    return text;
  }
  return text.substr(text.size() - keep);
}

std::string BaseName(const std::string& path) {
  const size_t slash = path.rfind('/');
  return (std::string::npos == slash) ? path : path.substr(slash + 1U);
}

// ---------------------------------------------------------------- file/util

bool ReadWholeFile(const std::string& path, std::string& out) {
  out.clear();
  std::FILE* file = std::fopen(path.c_str(), "rb");
  bool ok = (nullptr != file);
  if (ok) {
    char chunk[4096];
    size_t n = 0U;
    while ((n = std::fread(chunk, 1U, sizeof(chunk), file)) > 0U) {
      out.append(chunk, n);
    }
    std::fclose(file);
  }
  return ok;
}

void RemoveFile(const std::string& path) {
  if (!path.empty()) {
    static_cast<void>(::unlink(path.c_str()));
  }
}

uint64_t Fnv1a64(const uint8_t* data, size_t len) noexcept {
  uint64_t hash = UINT64_C(0xCBF29CE484222325);
  for (size_t i = 0U; i < len; ++i) {
    hash ^= data[i];
    hash *= UINT64_C(0x100000001B3);
  }
  return hash;
}

// Rebuilds the Annex B bytes of one access unit exactly as make_test_stream
// wrote them (every NAL uses a 4-byte start code).
std::vector<uint8_t> AnnexBBytes(const rse2e::AccessUnit& au) {
  static const uint8_t kStartCode[4] = {0x00U, 0x00U, 0x00U, 0x01U};
  std::vector<uint8_t> out;
  size_t total = 0U;
  for (const auto& nal : au.nals) {
    total += 4U + nal.bytes.size();
  }
  out.reserve(total);
  for (const auto& nal : au.nals) {
    out.insert(out.end(), kStartCode, kStartCode + 4U);
    out.insert(out.end(), nal.bytes.begin(), nal.bytes.end());
  }
  return out;
}

// Verifies the generator's .expect sidecar against the parsed stream: one line
// per AU with "<index> <is_idr> <bytes> <fnv1a64>" and, for H.265 only, a
// leading "# codec=H265" line. This catches AU-boundary drift between the
// generator and the Annex B scan used by the client.
bool VerifyExpectFile(const Ctx& ctx, std::string& err) {
  const std::string path = ctx.stream_path + ".expect";
  std::string text;
  bool ok = ReadWholeFile(path, text);
  if (!ok) {
    err = "cannot read " + path;
  }
  size_t pos = 0U;
  size_t index = 0U;
  bool first_line = true;
  while (ok && pos < text.size()) {
    const size_t eol = text.find('\n', pos);
    const std::string line = TrimText(text.substr(pos, (std::string::npos == eol) ? std::string::npos : eol - pos));
    pos = (std::string::npos == eol) ? text.size() : eol + 1U;
    if (line.empty()) {
      continue;
    }
    if ('#' == line[0]) {
      const bool want_header = (Codec::kH265 == ctx.codec);
      ok = want_header && first_line && (std::string::npos != line.find("codec=H265"));
      if (!ok) {
        err = "unexpected header line in " + path + ": '" + line + "'";
      }
      first_line = false;
      continue;
    }
    first_line = false;
    unsigned au_index = 0U;
    unsigned is_idr = 0U;
    unsigned long long au_bytes = 0U;
    char hash_text[32] = {};
    const int fields =
        std::sscanf(line.c_str(), "%" PRIu32 " %" PRIu32 " %" PRIu64 " %31s", &au_index, &is_idr, &au_bytes, hash_text);
    ok = (4 == fields) && (index < ctx.file_aus.size()) && (au_index == index);
    if (!ok) {
      err = "malformed or out-of-range expect line '" + line + "'";
    } else {
      const auto bytes = AnnexBBytes(ctx.file_aus[index]);
      const bool want_idr = rse2e::HasIrap(ctx.file_aus[index], ctx.codec);
      const uint64_t hash = Fnv1a64(bytes.data(), bytes.size());
      char want_hash[32];
      std::snprintf(want_hash, sizeof(want_hash), "%016llx", static_cast<uint64_t>(hash));
      ok = (au_bytes == bytes.size()) && ((0U != is_idr) == want_idr) && (0 == std::strcmp(hash_text, want_hash));
      if (!ok) {
        err = "expect line " + std::to_string(index) + " mismatch: file says " + std::to_string(au_bytes) + "/" +
              (is_idr ? "idr" : "non-idr") + "/" + hash_text + ", parsed " + std::to_string(bytes.size()) + "/" +
              (want_idr ? "idr" : "non-idr") + "/" + want_hash;
      } else {
        ++index;
      }
    }
  }
  if (ok && index != ctx.file_aus.size()) {
    err = "expect file has " + std::to_string(index) + " AUs, parsed stream has " + std::to_string(ctx.file_aus.size());
    ok = false;
  }
  return ok;
}

// --------------------------------------------------------- process/port util

void PrintServerLog(const Ctx& ctx) {
  if (ctx.server_log_path.empty()) {
    return;
  }
  std::string log;
  if (ReadWholeFile(ctx.server_log_path, log) && !log.empty()) {
    std::fprintf(stderr, "----- server log (%s, tail) -----\n%s\n", ctx.server_log_path.c_str(),
                 TailText(log, 4000U).c_str());
  } else {
    std::fprintf(stderr, "----- server log: empty or unreadable (%s) -----\n", ctx.server_log_path.c_str());
  }
}

bool PortFree(uint16_t port) noexcept {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  bool free_port = false;
  if (fd >= 0) {
    const int one = 1;
    static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    free_port = 0 == ::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
  }
  return free_port;
}

// Requirement: base = 19000 + (pid % 500) * 2 + case_index, plus a per-process
// random even offset so two suites started from different build directories do
// not walk the same port window.
uint32_t g_port_salt = 0U;

uint32_t CaseBasePort(uint32_t case_index) noexcept {
  return 19000U + ((static_cast<uint32_t>(::getpid()) % 500U) * 2U) + case_index + g_port_salt;
}

// Scans /proc for rtspserver_demo processes that this test did not start. Such
// a process is the most likely source of external SIGTERM/SIGKILL interference
// reported as F-E2E-01/F-E2E-02 in the phase-1 test report.
uint32_t CountForeignDemoProcesses(pid_t exclude_pid, std::vector<std::string>& found) {
  found.clear();
  const std::string wanted = BaseName(RS_DEMO_BIN);
  DIR* dir = ::opendir("/proc");
  if (nullptr == dir) {
    return 0U;
  }
  while (struct dirent* entry = ::readdir(dir)) {
    char* end = nullptr;
    const long pid = std::strtol(entry->d_name, &end, 10);
    if (pid <= 0 || nullptr == end || '\0' != *end || pid == static_cast<long>(exclude_pid)) {
      continue;
    }
    std::string cmdline;
    const std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    if (!ReadWholeFile(path, cmdline) || cmdline.empty()) {
      continue;
    }
    const size_t nul = cmdline.find('\0');
    const std::string argv0 = cmdline.substr(0U, nul);
    if (BaseName(argv0) == wanted) {
      found.push_back(std::to_string(pid) + ":" + argv0);
    }
  }
  static_cast<void>(::closedir(dir));
  return static_cast<uint32_t>(found.size());
}

void ReportForeignDemoProcesses(pid_t own_pid, const char* phase) {
  std::vector<std::string> found;
  const uint32_t count = CountForeignDemoProcesses(own_pid, found);
  // Informational only: the suite never signals a process it did not fork, so
  // a server from another build directory is reported and otherwise ignored.
  std::printf("    [iso] %s: foreign rtspserver_demo processes=%" PRIu32 " (informational)\n", phase, count);
  for (const auto& item : found) {
    std::printf("    [iso]   foreign pid: %s\n", item.c_str());
  }
}

// Even offset added to the pid-derived port base so two suites running from
// different build directories do not share the same port window.
uint32_t InitPortSalt() noexcept {
  uint32_t raw = 0U;
  std::FILE* file = std::fopen("/dev/urandom", "rb");
  if (nullptr != file) {
    if (std::fread(&raw, 1U, sizeof(raw), file) != sizeof(raw)) {
      raw = 0U;
    }
    std::fclose(file);
  }
  if (0U == raw) {
    raw = static_cast<uint32_t>(NowMs()) ^ (static_cast<uint32_t>(::getpid()) << 8U);
  }
  return (raw % 500U) * 2U;
}

bool ReapStrays(std::string& err) {
  bool clean = true;
  for (;;) {
    int status = 0;
    const pid_t pid = ::waitpid(-1, &status, WNOHANG);
    if (pid > 0) {
      clean = false;
      err += " leftover child pid=" + std::to_string(static_cast<int>(pid)) + ";";
    } else {
      break;
    }
  }
  return clean;
}

struct CaseStart {
  bool started = false;
  bool exited = false;
  int exit_code = -1;
  std::string stderr_text;
  std::string err;
};

bool g_server_running = false;

void StopServer(Ctx& ctx);

CaseStart TryStartServer(Ctx& ctx, uint16_t port, const std::vector<std::string>& extra_args) {
  CaseStart out;
  ctx.port = port;
  ctx.server_exited = false;
  ctx.server_exit_code = -1;
  ctx.server_log_path = "/tmp/rs_e2e_server_" + std::to_string(::getpid()) + "_" + std::to_string(ctx.case_index) +
                        "_" + std::to_string(port) + ".log";
  const int log_fd = ::open(ctx.server_log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (log_fd < 0) {
    out.err = "cannot create server log " + ctx.server_log_path;
    return out;
  }
  std::vector<std::string> args{RS_DEMO_BIN,     "--port", std::to_string(port), "--file",
                                ctx.stream_path, "--ip",   "127.0.0.1"};
  args.insert(args.end(), extra_args.begin(), extra_args.end());
  std::vector<char*> cargv;
  cargv.reserve(args.size() + 1U);
  for (const auto& arg : args) {
    cargv.push_back(const_cast<char*>(arg.c_str()));
  }
  cargv.push_back(nullptr);

  const pid_t parent_pid = ::getpid();
  const pid_t pid = ::fork();
  if (0 == pid) {
    // The server must not survive a killed test process; SIGTERM keeps the
    // demo's graceful stop path and its final log line.
    static_cast<void>(::prctl(PR_SET_PDEATHSIG, SIGTERM));
    if (::getppid() != parent_pid) {
      ::_exit(127);
    }
    static_cast<void>(::dup2(log_fd, STDERR_FILENO));
    static_cast<void>(::dup2(log_fd, STDOUT_FILENO));
    ::close(log_fd);
    ::execv(cargv[0], cargv.data());
    ::_exit(127);
  }
  ::close(log_fd);
  if (pid < 0) {
    out.err = "fork failed";
    return out;
  }
  ctx.server_pid = pid;
  g_server_running = true;

  const int64_t deadline = NowMs() + kServerStartTimeoutMs;
  bool decided = false;
  while (!decided && NowMs() < deadline) {
    int status = 0;
    if (::waitpid(ctx.server_pid, &status, WNOHANG) == ctx.server_pid) {
      ctx.server_exited = true;
      ctx.server_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      ctx.server_pid = -1;
      g_server_running = false;
      ReadWholeFile(ctx.server_log_path, out.stderr_text);
      out.exited = true;
      out.exit_code = ctx.server_exit_code;
      decided = true;
    } else {
      const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd >= 0) {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(ctx.port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        out.started = 0 == ::connect(fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
        ::close(fd);
      }
      if (out.started) {
        decided = true;
      } else {
        struct timespec pause{0, 50000000};
        static_cast<void>(::nanosleep(&pause, nullptr));
      }
    }
  }
  if (!out.started && !out.exited) {
    out.err = "server did not accept TCP connections on port " + std::to_string(port) + " within " +
              std::to_string(kServerStartTimeoutMs) + " ms";
  }
  return out;
}

// One server instance at a time; on a busy port retry once on another port.
CaseStart StartCaseServer(Ctx& ctx, const char* codec_flag, bool retry_on_exit) {
  CaseStart out;
  if (g_server_running) {
    out.err = "internal: a server instance is still running";
    return out;
  }
  std::vector<std::string> extra;
  if (nullptr != codec_flag) {
    extra.push_back("--codec");
    extra.push_back(codec_flag);
  }
  const uint32_t base = CaseBasePort(ctx.case_index);
  constexpr uint32_t kPortAttempts = 4U;
  for (uint32_t attempt = 0U; attempt < kPortAttempts; ++attempt) {
    const uint32_t candidate = base + attempt * kRetryPortStep;
    if (candidate > 65000U) {
      continue;
    }
    const uint16_t port = static_cast<uint16_t>(candidate);
    if (!PortFree(port)) {
      out.err = "port " + std::to_string(port) + " already in use";
      continue;
    }
    out = TryStartServer(ctx, port, extra);
    if (out.started) {
      return out;
    }
    if (out.exited && (!retry_on_exit || (kPortAttempts - 1U) == attempt)) {
      return out;
    }
    StopServer(ctx);  // Timed out without accepting: never leave it running.
  }
  return out;
}

void StopServer(Ctx& ctx) {
  if (ctx.server_pid > 0) {
    if (!ctx.server_exited) {
      static_cast<void>(::kill(ctx.server_pid, SIGTERM));
    }
    int status = 0;
    const int64_t deadline = NowMs() + 2000;
    bool reaped = false;
    while (!reaped && NowMs() < deadline) {
      if (::waitpid(ctx.server_pid, &status, WNOHANG) == ctx.server_pid) {
        reaped = true;
      } else {
        struct timespec pause{0, 20000000};
        static_cast<void>(::nanosleep(&pause, nullptr));
      }
    }
    if (!reaped) {
      static_cast<void>(::kill(ctx.server_pid, SIGKILL));
      static_cast<void>(::waitpid(ctx.server_pid, &status, 0));
    }
    ctx.server_pid = -1;
  }
  ctx.server_exited = true;
  g_server_running = false;
}

// ------------------------------------------------------------ stream setup

std::string SourceDir() {
  const std::string file = __FILE__;
  const size_t slash = file.rfind('/');
  return (std::string::npos == slash) ? std::string(".") : file.substr(0U, slash);
}

std::string RealStreamScript() {
  return SourceDir() + "/../scripts/make_real_stream.sh";
}

bool PrepareStream(Ctx& ctx, StreamKind kind, std::string& err) {
  const std::string tag = std::to_string(::getpid()) + "_" + std::to_string(ctx.case_index);
  std::string tool_stdout;
  std::string tool_stderr;
  int exit_code = -1;
  if (StreamKind::kRealH264 == kind || StreamKind::kRealH265 == kind) {
    const bool h265 = (StreamKind::kRealH265 == kind);
    ctx.stream_path = "/tmp/rs_e2e_real_" + tag + (h265 ? ".h265" : ".h264");
    // Run the generator through bash explicitly: the script uses bash arrays,
    // and a host whose /bin/sh is dash must not change the outcome.
    const std::vector<std::string> argv{"/bin/bash", RealStreamScript(), ctx.stream_path, h265 ? "h265" : "h264", "4"};
    if (!rse2e::RunProcess(argv, 120000, tool_stdout, tool_stderr, exit_code)) {
      err = "failed to execute " + RealStreamScript();
      return false;
    }
    if (0 != exit_code) {
      err = "make_real_stream.sh exited with " + std::to_string(exit_code) +
            (-1 == exit_code ? " (timed out after 120 s)" : "") + ": " + TailText(tool_stderr, 1500U);
      return false;
    }
    std::printf("    [stream] %s", TailText(tool_stdout, 400U).c_str());
  } else {
    const bool h265 = (StreamKind::kSynthH265 == kind);
    ctx.stream_path = "/tmp/rs_e2e_synth_" + tag + (h265 ? ".h265" : ".h264");
    std::vector<std::string> argv{RS_STREAM_TOOL, ctx.stream_path, "--aus", "60", "--gop", "25"};
    if (h265) {
      // H.264 keeps the default path (no --codec) so the legacy behaviour is
      // exercised; H.265 selects the codec explicitly.
      argv.push_back("--codec");
      argv.push_back("h265");
    }
    if (!rse2e::RunProcess(argv, 30000, tool_stdout, tool_stderr, exit_code)) {
      err = "failed to execute stream tool " + std::string(RS_STREAM_TOOL);
      return false;
    }
    if (0 != exit_code) {
      err = "stream tool exited with " + std::to_string(exit_code) + ": " + TailText(tool_stderr, 1500U);
      return false;
    }
    std::printf("    [stream] %s", TailText(tool_stdout, 400U).c_str());
  }

  rse2e::StreamFile stream;
  bool ok = rse2e::LoadStreamFile(ctx.stream_path, ctx.codec, stream, err);
  if (ok) {
    ctx.file_aus = std::move(stream.aus);
    ok = !ctx.file_aus.empty();
    if (!ok) {
      err = "generated stream has no access units";
    }
  }
  bool has_irap = false;
  for (const auto& au : ctx.file_aus) {
    has_irap = has_irap || rse2e::HasIrap(au, ctx.codec);
  }
  if (ok && !has_irap) {
    err = "generated stream has no IRAP/IDR access unit";
    ok = false;
  }
  if (ok && (StreamKind::kSynthH264 == kind || StreamKind::kSynthH265 == kind)) {
    ok = VerifyExpectFile(ctx, err);
  }
  if (ok) {
    std::printf("    [stream] %s: %zu AUs, codec=%s\n", ctx.stream_path.c_str(), ctx.file_aus.size(),
                (Codec::kH265 == ctx.codec) ? "H265" : "H264");
  }
  return ok;
}

// ------------------------------------------------------------- ffmpeg probe

bool HaveFfmpeg() {
  std::string out;
  std::string err_text;
  int exit_code = -1;
  if (!rse2e::RunProcess({"ffmpeg", "-hide_banner", "-version"}, 10000, out, err_text, exit_code)) {
    return false;
  }
  return 0 == exit_code && (std::string::npos != out.find("ffmpeg version"));
}

std::string FfmpegVersionLine() {
  std::string out;
  std::string err_text;
  int exit_code = -1;
  if (!rse2e::RunProcess({"ffmpeg", "-hide_banner", "-version"}, 10000, out, err_text, exit_code) || 0 != exit_code) {
    return "unavailable";
  }
  const size_t eol = out.find('\n');
  return TrimText(out.substr(0U, eol));
}

// ------------------------------------------------------------------ RTSP I/O

bool SendRequest(rse2e::Client& client, uint32_t& cseq, const char* method, const std::string& uri,
                 const std::vector<std::string>& headers, const std::string& body, rse2e::Response& response,
                 std::string& err) {
  const uint32_t used = cseq;
  std::vector<std::string> all;
  all.reserve(headers.size() + 1U);
  all.push_back("CSeq: " + std::to_string(used));
  all.insert(all.end(), headers.begin(), headers.end());
  bool ok = client.Request(method, uri, all, body, err);
  if (ok) {
    ok = client.ReadResponse(response, static_cast<int>(kResponseTimeoutMs), err);
  }
  if (ok && response.Get("cseq") != std::to_string(used)) {
    err = std::string(method) + " response CSeq mismatch: expected " + std::to_string(used) + ", got " +
          response.Get("cseq");
    ok = false;
  }
  if (ok) {
    ++cseq;
  }
  return ok;
}

bool ExpectStatus(const rse2e::Response& response, uint32_t want, const char* method, std::string& err) {
  const bool ok = response.status == want;
  if (!ok) {
    err = std::string(method) + " returned " + std::to_string(response.status) + " " + response.reason + ", expected " +
          std::to_string(want);
  }
  return ok;
}

std::string SessionToken(const std::string& value) {
  const size_t semicolon = value.find(';');
  return TrimText(value.substr(0U, semicolon));
}

std::string TransportParam(const std::string& transport, const std::string& key) {
  std::string result;
  size_t pos = 0U;
  while (pos <= transport.size() && result.empty()) {
    const size_t end = transport.find(';', pos);
    const std::string part = TrimText(transport.substr(pos, end - pos));
    const std::string prefix = key + "=";
    if (0 == part.compare(0U, prefix.size(), prefix)) {
      result = part.substr(prefix.size());
    }
    if (std::string::npos == end) {
      break;
    }
    pos = end + 1U;
  }
  return result;
}

bool ParseU32After(const std::string& text, const std::string& key, uint32_t& out) noexcept {
  const size_t at = text.find(key);
  bool ok = (std::string::npos != at);
  if (ok) {
    const char* start = text.c_str() + at + key.size();
    char* end = nullptr;
    const unsigned long value = std::strtoul(start, &end, 10);
    ok = (end != start) && (value <= 0xFFFFFFFFUL);
    if (ok) {
      out = static_cast<uint32_t>(value);
    }
  }
  return ok;
}

bool ParseU32Text(const std::string& text, uint32_t& out) noexcept {
  char* end = nullptr;
  const unsigned long value = std::strtoul(text.c_str(), &end, 10);
  const bool ok = (end != text.c_str()) && ('\0' == *end) && (value <= 0xFFFFFFFFUL);
  if (ok) {
    out = static_cast<uint32_t>(value);
  }
  return ok;
}

bool OpenSession(RtspSession& session, const Ctx& ctx, bool tcp, uint16_t client_rtp_port, rse2e::Response& describe,
                 std::string& err, const char* interleaved = "0-1") {
  session.base_uri = rse2e::MakeUri("127.0.0.1", ctx.port, "/live");
  session.track_uri = session.base_uri + "/trackID=0";
  bool ok = session.client.Connect("127.0.0.1", ctx.port, static_cast<int>(kConnectTimeoutMs), err);

  rse2e::Response options;
  if (ok) {
    ok = SendRequest(session.client, session.cseq, "OPTIONS", "*", {}, "", options, err) &&
         ExpectStatus(options, 200U, "OPTIONS", err);
  }
  if (ok) {
    ok = SendRequest(session.client, session.cseq, "DESCRIBE", session.base_uri, {"Accept: application/sdp"}, "",
                     describe, err) &&
         ExpectStatus(describe, 200U, "DESCRIBE", err);
  }
  if (ok) {
    ok = rse2e::CodecFromSdp(describe.body, session.sdp_codec);
    if (!ok) {
      err = "SDP advertises neither H264 nor H265";
    }
  }
  rse2e::Response setup;
  if (ok) {
    const std::string transport = tcp ? "Transport: RTP/AVP/TCP;unicast;interleaved=" + std::string(interleaved)
                                      : "Transport: RTP/AVP;unicast;client_port=" + std::to_string(client_rtp_port) +
                                            "-" + std::to_string(static_cast<uint32_t>(client_rtp_port) + 1U);
    ok = SendRequest(session.client, session.cseq, "SETUP", session.track_uri, {transport}, "", setup, err) &&
         ExpectStatus(setup, 200U, "SETUP", err);
  }
  if (ok) {
    session.session_id = SessionToken(setup.Get("session"));
    session.transport = setup.Get("transport");
    ok = !session.session_id.empty() && !session.transport.empty();
    if (!ok) {
      err = "SETUP response missing Session or Transport header";
    }
  }
  return ok;
}

bool PlaySession(RtspSession& session, std::string& err) {
  rse2e::Response play;
  bool ok = SendRequest(session.client, session.cseq, "PLAY", session.track_uri,
                        {"Session: " + session.session_id, "Range: npt=0-"}, "", play, err) &&
            ExpectStatus(play, 200U, "PLAY", err);
  if (ok) {
    const std::string info = play.Get("rtp-info");
    ok = !info.empty() && ParseU32After(info, "seq=", session.rtp_info_seq) &&
         ParseU32After(info, "rtptime=", session.rtp_info_ts);
    if (!ok) {
      err = "PLAY response has no usable RTP-Info: '" + info + "'";
    } else {
      session.have_rtp_info = true;
    }
  }
  return ok;
}

void TeardownSession(RtspSession& session) {
  if (session.client.IsOpen() && !session.session_id.empty()) {
    rse2e::Response response;
    std::string err;
    static_cast<void>(SendRequest(session.client, session.cseq, "TEARDOWN", session.track_uri,
                                  {"Session: " + session.session_id}, "", response, err));
    session.session_id.clear();
  }
  session.client.Close();
}

bool CheckSdp(Codec codec, const std::string& sdp, std::string& err) {
  bool ok = false;
  if (Codec::kH265 == codec) {
    ok = (std::string::npos != sdp.find("H265/90000")) && (std::string::npos != sdp.find("sprop-vps=")) &&
         (std::string::npos != sdp.find("sprop-sps=")) && (std::string::npos != sdp.find("sprop-pps=")) &&
         (std::string::npos != sdp.find("m=video 0 RTP/AVP 96")) &&
         (std::string::npos != sdp.find("a=control:trackID=0"));
    if (!ok) {
      err = "H.265 SDP is missing H265/90000, sprop-vps/sps/pps, m=video 0 RTP/AVP 96 or a=control";
    }
  } else {
    ok = (std::string::npos != sdp.find("sprop-parameter-sets=")) &&
         (std::string::npos != sdp.find("packetization-mode=1")) &&
         (std::string::npos != sdp.find("profile-level-id=42")) &&
         (std::string::npos != sdp.find("m=video 0 RTP/AVP 96")) &&
         (std::string::npos != sdp.find("a=control:trackID=0"));
    if (!ok) {
      err = "SDP is missing sprop-parameter-sets/packetization-mode/profile-level-id/m=/a=control";
    }
  }
  return ok;
}

// ------------------------------------------------------------ media handling

bool PushRtp(const rse2e::RtpPacket& packet, MediaCollect& collect, bool& complete) {
  complete = false;
  if (!collect.have_first) {
    collect.first_seq = packet.seq;
    collect.first_ts = packet.timestamp;
    collect.have_first = true;
  }
  ++collect.packets;
  std::vector<uint8_t> nal_bytes;
  const bool reassembled =
      (Codec::kH265 == collect.codec) ? collect.h265.Push(packet, nal_bytes) : collect.h264.Push(packet, nal_bytes);
  if (reassembled) {
    rse2e::NalUnit nal;
    nal.type = rse2e::NalTypeOf(collect.codec, nal_bytes.data(), nal_bytes.size());
    nal.bytes = std::move(nal_bytes);
    collect.current.push_back(std::move(nal));
  }
  if (packet.marker) {
    rse2e::AccessUnit au;
    au.nals = std::move(collect.current);
    collect.current.clear();
    collect.aus.push_back(std::move(au));
    complete = true;
  }
  return true;
}

uint32_t SeqGaps(const MediaCollect& collect) noexcept {
  return (Codec::kH265 == collect.codec) ? collect.h265.seq_gaps() : collect.h264.seq_gaps();
}

bool CollectTcpMedia(rse2e::Client& client, uint32_t wanted_aus, MediaCollect& collect, uint8_t want_channel = 0U) {
  const int64_t deadline = NowMs() + kMediaTimeoutMs;
  bool ok = true;
  while (ok && collect.aus.size() < wanted_aus) {
    const int wait_ms = RemainingMs(deadline);
    if (0 == wait_ms) {
      collect.err = "timeout after " + std::to_string(collect.aus.size()) + " complete AUs and " +
                    std::to_string(collect.packets) + " RTP packets";
      ok = false;
    } else {
      rse2e::MediaFrame frame;
      ok = client.ReadInterleaved(frame, wait_ms, collect.err);
      if (ok) {
        ++collect.channel_frames[frame.channel];
      }
      if (ok && want_channel != frame.channel) {
        ++collect.other_channel;
      } else if (ok) {
        if (frame.payload.size() < 12U || frame.payload.size() > kMaxRtpDatagramBytes) {
          collect.err = "interleaved RTP frame size " + std::to_string(frame.payload.size()) + " outside [12, " +
                        std::to_string(kMaxRtpDatagramBytes) + "]";
          ok = false;
        } else {
          if (frame.payload.size() > collect.max_frame_bytes) {
            collect.max_frame_bytes = static_cast<uint32_t>(frame.payload.size());
          }
          rse2e::RtpPacket packet;
          if (!rse2e::ParseRtp(frame.payload.data(), frame.payload.size(), packet)) {
            collect.err = "malformed RTP header inside interleaved frame";
            ok = false;
          } else if (packet.payload_type != kExpectedPayloadType) {
            collect.err = "unexpected RTP payload type " + std::to_string(packet.payload_type);
            ok = false;
          } else {
            bool complete = false;
            ok = PushRtp(packet, collect, complete);
          }
        }
      }
    }
  }
  collect.seq_gaps = SeqGaps(collect);
  return ok;
}

bool BindUdpPair(uint16_t& base_port, int& rtp_fd, int& rtcp_fd, std::string& err) {
  rtp_fd = -1;
  rtcp_fd = -1;
  bool ok = false;
  for (uint32_t attempt = 0U; attempt < 200U && !ok; ++attempt) {
    uint32_t candidate = 40000U + ((static_cast<uint32_t>(::getpid()) * 37U + attempt * 2U) % 20000U);
    if (0U != (candidate & 1U)) {
      ++candidate;
    }
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(candidate));
    rtp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_fd >= 0 && 0 == ::bind(rtp_fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr))) {
      addr.sin_port = htons(static_cast<uint16_t>(candidate + 1U));
      rtcp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
      ok = rtcp_fd >= 0 && 0 == ::bind(rtcp_fd, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
      if (ok) {
        base_port = static_cast<uint16_t>(candidate);
      } else {
        ::close(rtcp_fd);
        rtcp_fd = -1;
      }
    }
    if (!ok) {
      if (rtp_fd >= 0) {
        ::close(rtp_fd);
        rtp_fd = -1;
      }
    }
  }
  if (!ok) {
    err = "cannot bind an even/odd UDP client port pair";
  }
  return ok;
}

bool RecvUdp(int fd, std::vector<uint8_t>& out, int timeout_ms) {
  struct pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  const int ready = ::poll(&pfd, 1, timeout_ms);
  bool ok = ready > 0;
  if (ok) {
    uint8_t buffer[2048];
    const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
    ok = n > 0;
    if (ok) {
      out.assign(buffer, buffer + n);
    }
  }
  return ok;
}

bool CollectUdpMedia(int rtp_fd, uint32_t wanted_aus, MediaCollect& collect) {
  const int64_t deadline = NowMs() + kMediaTimeoutMs;
  bool ok = true;
  while (ok && collect.aus.size() < wanted_aus) {
    const int wait_ms = RemainingMs(deadline);
    std::vector<uint8_t> datagram;
    ok = (0 != wait_ms) && RecvUdp(rtp_fd, datagram, wait_ms);
    if (!ok) {
      collect.err = "timeout after " + std::to_string(collect.aus.size()) + " complete AUs and " +
                    std::to_string(collect.packets) + " RTP datagrams";
    } else if (0x24U == datagram[0]) {
      collect.err = "UDP RTP datagram starts with '$' (interleaved prefix leaked into UDP)";
      ok = false;
    } else if (datagram.size() > kMaxRtpDatagramBytes) {
      collect.err = "UDP datagram size " + std::to_string(datagram.size()) + " exceeds MTU budget";
      ok = false;
    } else {
      if (datagram.size() > collect.max_frame_bytes) {
        collect.max_frame_bytes = static_cast<uint32_t>(datagram.size());
      }
      rse2e::RtpPacket packet;
      if (!rse2e::ParseRtp(datagram.data(), datagram.size(), packet)) {
        collect.err = "malformed UDP RTP packet";
        ok = false;
      } else if (packet.payload_type != kExpectedPayloadType) {
        collect.err = "unexpected UDP RTP payload type " + std::to_string(packet.payload_type);
        ok = false;
      } else {
        bool complete = false;
        ok = PushRtp(packet, collect, complete);
      }
    }
  }
  collect.seq_gaps = SeqGaps(collect);
  return ok;
}

bool FindFirstIrapAu(const std::vector<rse2e::AccessUnit>& aus, Codec codec, size_t& index) {
  index = aus.size();
  for (size_t i = 0U; i < aus.size() && index == aus.size(); ++i) {
    if (rse2e::HasIrap(aus[i], codec)) {
      index = i;
    }
  }
  return index < aus.size();
}

bool CompareMediaToFile(MediaCollect& collect, const std::vector<rse2e::AccessUnit>& file_aus, size_t wanted_aus,
                        Codec codec, std::string& err) {
  size_t wire_start = 0U;
  bool ok = FindFirstIrapAu(collect.aus, codec, wire_start);
  if (!ok) {
    err = "no IRAP/IDR access unit in the reassembled wire stream";
  } else {
    // PLAY starts at the next IRAP after the current file position, so align
    // the first wire IRAP with the source IRAP whose bytes it matches.
    const auto wire_irap = rse2e::NormalizeForCompare(collect.aus[wire_start].nals, codec);
    size_t file_start = file_aus.size();
    for (size_t i = 0U; i < file_aus.size() && file_start == file_aus.size(); ++i) {
      std::string ignored;
      if (rse2e::HasIrap(file_aus[i], codec) &&
          rse2e::CompareNals(rse2e::NormalizeForCompare(file_aus[i].nals, codec), wire_irap, codec, ignored)) {
        file_start = i;
      }
    }
    if (file_start >= file_aus.size()) {
      err = "first wire IRAP access unit does not match any source IRAP access unit";
      return false;
    }
    size_t wire_count = 0U;
    for (size_t i = wire_start; i < collect.aus.size(); ++i) {
      wire_count += collect.aus[i].nals.empty() ? 0U : 1U;
    }
    const size_t compare_count = (wanted_aus < wire_count ? wanted_aus : wire_count);
    for (size_t i = 0U; ok && i < compare_count && file_start + i < file_aus.size(); ++i) {
      const auto expected = rse2e::NormalizeForCompare(file_aus[file_start + i].nals, codec);
      const auto actual = rse2e::NormalizeForCompare(collect.aus[wire_start + i].nals, codec);
      ok = rse2e::CompareNals(expected, actual, codec, err);
      if (!ok) {
        err = "AU " + std::to_string(i) + " after the first IRAP: " + err;
      }
    }
    if (ok && compare_count < wanted_aus) {
      err =
          "only " + std::to_string(compare_count) + " comparable AUs reassembled, wanted " + std::to_string(wanted_aus);
      ok = false;
    }
  }
  return ok;
}

// ------------------------------------------------------------------- bodies

bool RunTcpPlaybackWith(Ctx& ctx, const char* interleaved, uint8_t want_channel, std::string& err) {
  RtspSession session;
  rse2e::Response describe;
  bool ok = OpenSession(session, ctx, true, 0U, describe, err, interleaved);
  if (ok) {
    ok = CheckSdp(ctx.codec, describe.body, err);
  }
  if (ok && session.sdp_codec != ctx.codec) {
    err = std::string("SDP advertises ") + rse2e::CodecLabel(session.sdp_codec) + ", served stream is " +
          rse2e::CodecLabel(ctx.codec);
    ok = false;
  }
  if (ok) {
    ok = TransportParam(session.transport, "interleaved") == interleaved;
    if (!ok) {
      err = "SETUP Transport lacks interleaved=" + std::string(interleaved) + ": " + session.transport;
    }
  }
  if (ok) {
    ok = PlaySession(session, err);
  }
  MediaCollect collect;
  collect.codec = session.sdp_codec;
  if (ok) {
    ok = CollectTcpMedia(session.client, 3U, collect, want_channel);
    if (!ok) {
      err = collect.err;
    }
  }
  if (ok) {
    ok = collect.channel_frames[want_channel] > 0U;
    if (!ok) {
      err = "no media on interleaved channel " + std::to_string(want_channel);
    }
  }
  if (ok) {
    // Media must arrive only on the negotiated RTP channel; RTCP is not sent
    // over TCP by this server, so the other three channels stay silent.
    for (uint32_t ch = 0U; ch < 128U && ok; ++ch) {
      if (ch != want_channel && 0U != collect.channel_frames[ch]) {
        ok = false;
        err = "unexpected interleaved channel " + std::to_string(ch) + " with " +
              std::to_string(collect.channel_frames[ch]) + " frames";
      }
    }
  }
  if (ok && Codec::kH265 == collect.codec) {
    // Prove the RFC 7798 paths were both exercised: the synthetic stream has
    // small NALs (single-NAL packets) and >1200-byte NALs (FU).
    ok = collect.h265.fu_packets() > 0U && collect.h265.single_nal_packets() > 0U &&
         collect.h265.truncated_fu() == 0U && collect.h265.aggregation_packets() == 0U;
    if (!ok) {
      err = "H.265 packetization coverage: fu=" + std::to_string(collect.h265.fu_packets()) +
            " single_nal=" + std::to_string(collect.h265.single_nal_packets()) +
            " truncated_fu=" + std::to_string(collect.h265.truncated_fu()) +
            " ap=" + std::to_string(collect.h265.aggregation_packets());
    } else {
      std::printf("    [tcp] h265 fu_packets=%" PRIu32 " single_nal_packets=%" PRIu32 "\n", collect.h265.fu_packets(),
                  collect.h265.single_nal_packets());
    }
  }
  if (ok) {
    ok = collect.have_first && collect.first_seq == static_cast<uint16_t>(session.rtp_info_seq) &&
         collect.first_ts == session.rtp_info_ts;
    if (!ok) {
      err = "first RTP packet (seq=" + std::to_string(collect.first_seq) + ", ts=" + std::to_string(collect.first_ts) +
            ") does not match RTP-Info (seq=" + std::to_string(session.rtp_info_seq) +
            ", ts=" + std::to_string(session.rtp_info_ts) + ")";
    }
  }
  if (ok) {
    ok = CompareMediaToFile(collect, ctx.file_aus, 3U, ctx.codec, err);
  }
  if (ok) {
    std::printf("    [tcp] channel=%" PRIu32 " packets=%" PRIu32 " aus=%zu max_rtp=%" PRIu32 " seq_gaps=%" PRIu32 "\n",
                want_channel, collect.packets, collect.aus.size(), collect.max_frame_bytes, collect.seq_gaps);
  }
  if (ok) {
    ok = session.client.Buffered() <= 4U + kMaxRtpDatagramBytes;
    if (!ok) {
      err = "unparsed trailing bytes exceed one interleaved frame: " + std::to_string(session.client.Buffered());
    }
  }
  TeardownSession(session);
  return ok;
}

bool RunTcpPlayback(Ctx& ctx, std::string& err) {
  return RunTcpPlaybackWith(ctx, "0-1", 0U, err);
}

bool RunTcpPlaybackChannel2(Ctx& ctx, std::string& err) {
  return RunTcpPlaybackWith(ctx, "2-3", 2U, err);
}

bool RunUdpPlayback(Ctx& ctx, std::string& err) {
  uint16_t client_port = 0U;
  int rtp_fd = -1;
  int rtcp_fd = -1;
  bool ok = BindUdpPair(client_port, rtp_fd, rtcp_fd, err);
  RtspSession session;
  rse2e::Response describe;
  if (ok) {
    ok = OpenSession(session, ctx, false, client_port, describe, err);
  }
  if (ok) {
    ok = CheckSdp(ctx.codec, describe.body, err);
  }
  if (ok && session.sdp_codec != ctx.codec) {
    err = std::string("SDP advertises ") + rse2e::CodecLabel(session.sdp_codec) + ", served stream is " +
          rse2e::CodecLabel(ctx.codec);
    ok = false;
  }
  if (ok) {
    const std::string server_port = TransportParam(session.transport, "server_port");
    uint32_t rtp_port = 0U;
    uint32_t rtcp_port = 0U;
    const size_t dash = server_port.find('-');
    ok = !server_port.empty() && (std::string::npos != dash) && ParseU32Text(server_port.substr(0U, dash), rtp_port) &&
         ParseU32Text(server_port.substr(dash + 1U), rtcp_port);
    if (!ok) {
      err = "SETUP response lacks a usable server_port pair: " + session.transport;
    } else if (0U != (rtp_port & 1U) || rtcp_port != rtp_port + 1U) {
      ok = false;
      err = "server_port must be <even rtp>-<rtp+1>, got " + server_port;
    } else {
      std::printf("    [udp] server_port=%s (rtp even, rtcp=rtp+1)\n", server_port.c_str());
    }
  }
  if (ok) {
    ok = PlaySession(session, err);
  }
  MediaCollect collect;
  collect.codec = session.sdp_codec;
  if (ok) {
    ok = CollectUdpMedia(rtp_fd, 3U, collect);
    if (!ok) {
      err = collect.err;
    }
  }
  if (ok && Codec::kH265 == collect.codec) {
    ok = collect.h265.fu_packets() > 0U && collect.h265.single_nal_packets() > 0U &&
         collect.h265.truncated_fu() == 0U && collect.h265.aggregation_packets() == 0U;
    if (!ok) {
      err = "H.265 packetization coverage: fu=" + std::to_string(collect.h265.fu_packets()) +
            " single_nal=" + std::to_string(collect.h265.single_nal_packets()) +
            " truncated_fu=" + std::to_string(collect.h265.truncated_fu()) +
            " ap=" + std::to_string(collect.h265.aggregation_packets());
    } else {
      std::printf("    [udp] h265 fu_packets=%" PRIu32 " single_nal_packets=%" PRIu32 "\n", collect.h265.fu_packets(),
                  collect.h265.single_nal_packets());
    }
  }
  if (ok) {
    ok = collect.seq_gaps == 0U;
    if (!ok) {
      err = "UDP RTP sequence gaps: " + std::to_string(collect.seq_gaps);
    }
  }
  if (ok) {
    ok = collect.have_first && collect.first_seq == static_cast<uint16_t>(session.rtp_info_seq);
    if (!ok) {
      err = "first UDP RTP seq " + std::to_string(collect.first_seq) + " does not match RTP-Info seq " +
            std::to_string(session.rtp_info_seq);
    }
  }
  if (ok) {
    ok = CompareMediaToFile(collect, ctx.file_aus, 3U, ctx.codec, err);
  }
  if (ok) {
    std::string rtcp_note = "RTCP: not observed";
    std::vector<uint8_t> rtcp_datagram;
    if (RecvUdp(rtcp_fd, rtcp_datagram, 1000) && rtcp_datagram.size() >= 4U) {
      const uint8_t packet_type = rtcp_datagram[1];
      rtcp_note = (200U == packet_type || 201U == packet_type) ? "RTCP: SR/RR observed" : "RTCP: non-SR/RR observed";
    }
    std::printf("    [udp] %s, packets=%" PRIu32 ", max_datagram=%" PRIu32 ", seq_gaps=%" PRIu32 "\n",
                rtcp_note.c_str(), collect.packets, collect.max_frame_bytes, collect.seq_gaps);
  }
  TeardownSession(session);
  if (rtp_fd >= 0) {
    ::close(rtp_fd);
  }
  if (rtcp_fd >= 0) {
    ::close(rtcp_fd);
  }
  return ok;
}

uint32_t DecodedFrames(const std::string& progress) noexcept {
  uint32_t frames = 0U;
  size_t pos = 0U;
  while ((pos = progress.find("frame=", pos)) != std::string::npos) {
    const char* start = progress.c_str() + pos + 6U;
    char* end = nullptr;
    const unsigned long value = std::strtoul(start, &end, 10);
    if (end != start && value <= 0xFFFFFFFFUL) {
      frames = static_cast<uint32_t>(value);
    }
    pos += 6U;
  }
  return frames;
}

bool RunFfmpegDecode(const Ctx& ctx, const char* transport, std::string& err) {
  const std::string url = rse2e::MakeUri("127.0.0.1", ctx.port, "/live");
  // -progress pipe:1 adds the decoded frame count to stdout; the decode path
  // and the mandated command are otherwise unchanged.
  const std::vector<std::string> argv{"ffmpeg",  "-hide_banner", "-v", "error",     "-rtsp_transport",
                                      transport, "-i",           url,  "-frames:v", std::to_string(kFfmpegFrames),
                                      "-f",      "null",         "-",  "-progress", "pipe:1",
                                      "-nostats"};
  std::string progress;
  std::string ffmpeg_err;
  int exit_code = -1;
  const bool ran = rse2e::RunProcess(argv, 30000, progress, ffmpeg_err, exit_code);
  const uint32_t frames = DecodedFrames(progress);
  bool ok = ran && 0 == exit_code && frames >= kFfmpegFrames;
  if (!ok) {
    err = "ffmpeg -rtsp_transport " + std::string(transport) + " exited rc=" + std::to_string(exit_code) +
          ", decoded frames=" + std::to_string(frames) + (ran ? "" : " (could not run ffmpeg)") +
          "; stderr: " + TailText(TrimText(ffmpeg_err), 1200U);
  } else {
    const std::string warnings = TrimText(ffmpeg_err);
    std::printf("    [ffmpeg] transport=%s decoded_frames=%" PRIu32 " rc=0 stderr_bytes=%zu\n", transport, frames,
                warnings.size());
    if (!warnings.empty()) {
      std::printf("    [ffmpeg] stderr: %s\n", TailText(warnings, 600U).c_str());
    }
  }
  return ok;
}

bool RunH265Decode(Ctx& ctx, std::string& err) {
  bool ok = RunFfmpegDecode(ctx, "tcp", err);
  if (ok) {
    ok = RunFfmpegDecode(ctx, "udp", err);
  }
  return ok;
}

bool RunH264Decode(Ctx& ctx, std::string& err) {
  return RunFfmpegDecode(ctx, "tcp", err);
}

struct ClientJob {
  const Ctx* ctx = nullptr;
  bool ok = false;
  std::string err;
};

void RunOneTcpClient(ClientJob& job) {
  RtspSession session;
  rse2e::Response describe;
  job.ok = OpenSession(session, *job.ctx, true, 0U, describe, job.err) && PlaySession(session, job.err);
  MediaCollect collect;
  collect.codec = session.sdp_codec;
  if (job.ok) {
    job.ok = CollectTcpMedia(session.client, 1U, collect);
    if (!job.ok) {
      job.err = collect.err;
    }
  }
  if (job.ok) {
    size_t irap_index = 0U;
    job.ok = FindFirstIrapAu(collect.aus, collect.codec, irap_index);
    if (!job.ok) {
      job.err = "no IRAP/IDR AU received";
    } else {
      for (const auto& nal : collect.aus[irap_index].nals) {
        const bool header_ok =
            (Codec::kH265 == collect.codec) ? (nal.bytes.size() >= 2U) : (!nal.bytes.empty() && 0U != nal.type);
        job.ok = job.ok && header_ok && 0U == (nal.bytes[0] & 0x80U);
      }
      if (!job.ok) {
        job.err = "reassembled NAL boundary is invalid";
      }
    }
  }
  TeardownSession(session);
}

bool TestConcurrentClients(Ctx& ctx, std::string& err) {
  constexpr uint32_t kClients = 8U;
  std::vector<ClientJob> jobs(kClients);
  std::vector<std::thread> threads;
  threads.reserve(kClients);
  for (auto& job : jobs) {
    job.ctx = &ctx;
  }
  for (auto& job : jobs) {
    threads.emplace_back(RunOneTcpClient, std::ref(job));
  }
  for (auto& thread : threads) {
    thread.join();
  }
  bool ok = true;
  for (size_t i = 0U; i < jobs.size(); ++i) {
    if (!jobs[i].ok && ok) {
      err = "client " + std::to_string(i) + ": " + jobs[i].err;
      ok = false;
    }
  }
  return ok;
}

bool TestConnectionLimit(Ctx& ctx, std::string& err) {
  constexpr uint32_t kMaxConnections = 16U;
  std::vector<RtspSession> sessions(kMaxConnections);
  bool ok = true;
  for (uint32_t i = 0U; ok && i < kMaxConnections; ++i) {
    rse2e::Response options;
    ok = sessions[i].client.Connect("127.0.0.1", ctx.port, static_cast<int>(kConnectTimeoutMs), err) &&
         SendRequest(sessions[i].client, sessions[i].cseq, "OPTIONS", "*", {}, "", options, err) &&
         ExpectStatus(options, 200U, "OPTIONS", err);
    if (!ok) {
      err = "connection " + std::to_string(i + 1U) + " failed: " + err;
    }
  }
  if (ok) {
    rse2e::Client overflow;
    const bool connected = overflow.Connect("127.0.0.1", ctx.port, static_cast<int>(kConnectTimeoutMs), err);
    rse2e::Response options;
    std::string read_err;
    const bool sent = connected && overflow.Request("OPTIONS", "*", {"CSeq: 1"}, "", read_err);
    const bool got_response = sent && overflow.ReadResponse(options, 2000, read_err);
    if (!connected) {
      ok = false;
      err = "17th connection could not be created: " + err;
    } else if (got_response) {
      err = "17th connection received RTSP " + std::to_string(options.status) + " instead of being closed";
      ok = false;
    } else if (!overflow.PeerClosed()) {
      err = "17th connection was not closed within 2 s: " + read_err;
      ok = false;
    } else {
      ok = true;
    }
  }
  for (auto& session : sessions) {
    session.client.Close();
  }
  return ok;
}

bool TestSlowClient(Ctx& ctx, std::string& err) {
  RtspSession slow;
  rse2e::Response describe;
  bool ok = OpenSession(slow, ctx, true, 0U, describe, err) && PlaySession(slow, err);
  RtspSession healthy;
  if (ok) {
    ok = OpenSession(healthy, ctx, true, 0U, describe, err) && PlaySession(healthy, err);
  }

  bool slow_closed = false;
  int64_t max_gap_ms = 0;
  if (ok) {
    int64_t last_frame_ms = NowMs();
    const int64_t deadline = last_frame_ms + 3000;
    uint32_t frames = 0U;
    bool healthy_ok = true;
    while (healthy_ok && NowMs() < deadline && frames < 400U) {
      rse2e::MediaFrame frame;
      std::string read_err;
      const int wait_ms = RemainingMs(deadline);
      healthy_ok = healthy.client.ReadInterleaved(frame, wait_ms > 0 ? wait_ms : 1, read_err);
      if (healthy_ok && 0U == frame.channel) {
        const int64_t now = NowMs();
        const int64_t gap = now - last_frame_ms;
        max_gap_ms = (gap > max_gap_ms) ? gap : max_gap_ms;
        last_frame_ms = now;
        ++frames;
      }
    }

    // POLLRDHUP observes the server's FIN without draining the slow socket.
    struct pollfd pfd{};
    pfd.fd = slow.client.Fd();
    pfd.events = POLLIN | POLLRDHUP | POLLHUP;
    const int64_t close_deadline = NowMs() + 2500;
    while (!slow_closed && NowMs() < close_deadline) {
      const int ready = ::poll(&pfd, 1, RemainingMs(close_deadline));
      slow_closed = ready > 0 && (0 != (pfd.revents & (POLLRDHUP | POLLHUP)));
      if (!slow_closed) {
        struct timespec pause{0, 20000000};
        static_cast<void>(::nanosleep(&pause, nullptr));
      }
    }
    std::printf("    [slow] healthy frames=%" PRIu32 " max_gap=%" PRIi64 " ms slow_closed=%s\n", frames,
                static_cast<int64_t>(max_gap_ms), slow_closed ? "yes" : "no");
    ok = frames > 0U && max_gap_ms < 1500;
    if (0U == frames) {
      err = "healthy client received no media while the slow client was silent";
    } else if (!ok) {
      err = "healthy client stalled for " + std::to_string(max_gap_ms) + " ms while a slow client was silent";
    }
  }
  TeardownSession(healthy);
  slow.client.Close();
  return ok;
}

bool TestErrorPaths(Ctx& ctx, std::string& err) {
  const std::string base = rse2e::MakeUri("127.0.0.1", ctx.port, "/live");
  const std::string track = base + "/trackID=0";

  RtspSession unknown;
  rse2e::Response response;
  bool ok = unknown.client.Connect("127.0.0.1", ctx.port, static_cast<int>(kConnectTimeoutMs), err);
  if (ok) {
    ok = SendRequest(unknown.client, unknown.cseq, "DESCRIBE", rse2e::MakeUri("127.0.0.1", ctx.port, "/does-not-exist"),
                     {"Accept: application/sdp"}, "", response, err) &&
         ExpectStatus(response, 404U, "DESCRIBE unknown URI", err);
  }
  unknown.client.Close();

  RtspSession no_session;
  if (ok) {
    ok = no_session.client.Connect("127.0.0.1", ctx.port, static_cast<int>(kConnectTimeoutMs), err) &&
         SendRequest(no_session.client, no_session.cseq, "PLAY", track, {"Range: npt=0-"}, "", response, err) &&
         ExpectStatus(response, 454U, "PLAY without Session", err);
  }
  no_session.client.Close();

  RtspSession bad_transport;
  if (ok) {
    ok = bad_transport.client.Connect("127.0.0.1", ctx.port, static_cast<int>(kConnectTimeoutMs), err) &&
         SendRequest(bad_transport.client, bad_transport.cseq, "SETUP", track, {"Transport: RTP/AVP;multicast;ttl=16"},
                     "", response, err) &&
         ExpectStatus(response, 461U, "SETUP multicast", err);
  }
  bad_transport.client.Close();

  RtspSession duplicate;
  if (ok) {
    rse2e::Response describe;
    ok = OpenSession(duplicate, ctx, true, 0U, describe, err);
    rse2e::Response second;
    if (ok) {
      ok = SendRequest(duplicate.client, duplicate.cseq, "SETUP", duplicate.track_uri,
                       {"Session: " + duplicate.session_id, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1"}, "",
                       second, err) &&
           ExpectStatus(second, 200U, "duplicate SETUP", err);
      if (!ok) {
        err = "duplicate SETUP must be accepted in Ready state (design 6.3): " + err;
      }
    }
    TeardownSession(duplicate);
  }
  return ok;
}

// -------------------------------------------------------------------- driver

struct Case {
  const char* name;
  const char* matrix;
  StreamKind stream;
  const char* server_codec;  // nullptr = omit --codec (server default)
  bool expect_start_failure;
  bool needs_ffmpeg;
  bool (*body)(Ctx&, std::string&);
};

}  // namespace

int main() {
  const pid_t own_pid = ::getpid();
  std::printf("test_e2e: demo=%s stream_tool=%s pid=%d\n", RS_DEMO_BIN, RS_STREAM_TOOL, static_cast<int>(own_pid));
  const bool ffmpeg_ok = HaveFfmpeg();
  std::printf("test_e2e: ffmpeg=%s (%s)\n", ffmpeg_ok ? "available" : "MISSING", FfmpegVersionLine().c_str());
  g_port_salt = InitPortSalt();
  std::printf("test_e2e: port_base=%" PRIu32 " (salt=%" PRIu32 ")\n", CaseBasePort(0U), g_port_salt);
  ReportForeignDemoProcesses(own_pid, "suite-start");

  const Case cases[] = {
      {"tcp_interleaved_single_client", "T-TRANS-01 T-COMM-01 T-COMM-07 T-COMM-08", StreamKind::kSynthH264, nullptr,
       false, false, RunTcpPlayback},
      {"tcp_interleaved_channel_2", "T-TRANS-01 T-COMM-08", StreamKind::kSynthH264, nullptr, false, false,
       RunTcpPlaybackChannel2},
      {"udp_single_client", "T-TRANS-02 T-COMM-08", StreamKind::kSynthH264, nullptr, false, false, RunUdpPlayback},
      {"eight_concurrent_tcp_clients", "T-COMM-02 T-COMM-10", StreamKind::kSynthH264, nullptr, false, false,
       TestConcurrentClients},
      {"connection_limit_17th", "T-LIMIT-01", StreamKind::kSynthH264, nullptr, false, false, TestConnectionLimit},
      {"slow_client_isolation", "T-LIMIT-02 F-02/F-05", StreamKind::kSynthH264, nullptr, false, false, TestSlowClient},
      {"error_paths", "T-RTSP-404/454/461, duplicate SETUP", StreamKind::kSynthH264, nullptr, false, false,
       TestErrorPaths},
      {"h265_tcp_playback", "T-MEDIA-H265-01 T-COMM-07 T-COMM-08", StreamKind::kSynthH265, "auto", false, false,
       RunTcpPlayback},
      {"h265_udp_playback", "T-MEDIA-H265-02 T-COMM-08", StreamKind::kSynthH265, "auto", false, false, RunUdpPlayback},
      {"h265_ffmpeg_decode", "T-COMM-08 H265 decode tcp+udp", StreamKind::kRealH265, "auto", false, true,
       RunH265Decode},
      {"h264_regression_decode", "T-COMM-08 H264 regression tcp", StreamKind::kRealH264, "h264", false, true,
       RunH264Decode},
      {"codec_mismatch", "T-COMM-09 unsupported codec rejected", StreamKind::kSynthH265, "h264", true, false, nullptr},
  };

  uint32_t passed = 0U;
  uint32_t failed = 0U;
  uint32_t skipped = 0U;
  for (uint32_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    const Case& test = cases[index];
    Ctx ctx;
    ctx.case_index = index;
    ctx.codec = CodecOf(test.stream);
    const int64_t started_ms = NowMs();
    std::string err;
    bool ok = true;
    bool was_skipped = false;

    if (ok && test.needs_ffmpeg && !ffmpeg_ok) {
      err = "SKIP: ffmpeg not available in PATH; case cannot be executed (reported as not executed)";
      ok = false;
      was_skipped = true;
    }
    if (ok) {
      ok = PrepareStream(ctx, test.stream, err);
    }
    CaseStart start;
    if (ok) {
      // An unsupported codec exits immediately and must not be retried; a
      // normal case may retry once on another port when the bind raced with a
      // concurrent test run.
      start = StartCaseServer(ctx, test.server_codec, !test.expect_start_failure);
      if (test.expect_start_failure) {
        ok = start.exited && start.exit_code != 0 && !TrimText(start.stderr_text).empty();
        if (ok) {
          std::printf("    [expect] server refused to start: exit=%d\n", start.exit_code);
          std::printf("    [expect] server stderr: %s\n", TailText(TrimText(start.stderr_text), 300U).c_str());
        } else if (!start.exited) {
          err = "server started for an unsupported codec combination (exit expected)";
        } else if (0 == start.exit_code) {
          err = "server exited with code 0 for an unsupported codec combination";
        } else {
          err = "server exited with code " + std::to_string(start.exit_code) + " but printed no reason";
        }
      } else {
        ok = start.started;
        if (!ok) {
          err = start.exited
                    ? ("server exited before accepting connections (exit=" + std::to_string(start.exit_code) + ")")
                    : (start.err.empty() ? "server did not start" : start.err);
        }
      }
    }
    if (ok && nullptr != test.body) {
      ok = test.body(ctx, err);
    }

    StopServer(ctx);
    std::string stray_err;
    if (!ReapStrays(stray_err)) {
      ok = false;
      err += " process isolation failure:" + stray_err;
    }
    ReportForeignDemoProcesses(own_pid, "case-end");
    RemoveFile(ctx.stream_path);
    RemoveFile(ctx.stream_path + ".expect");

    const int64_t elapsed_ms = NowMs() - started_ms;
    if (ok) {
      ++passed;
      std::printf("[PASS] %s (%s) %" PRIi64 " ms\n", test.name, test.matrix, static_cast<int64_t>(elapsed_ms));
    } else {
      ++failed;
      if (was_skipped) {
        ++skipped;
        std::printf("[FAIL][SKIP] %s (%s) %" PRIi64 " ms\n  reason: %s\n", test.name, test.matrix,
                    static_cast<int64_t>(elapsed_ms), err.c_str());
      } else {
        std::printf("[FAIL] %s (%s) %" PRIi64 " ms\n", test.name, test.matrix, static_cast<int64_t>(elapsed_ms));
        std::fprintf(stderr, "  reason: %s\n", err.c_str());
        PrintServerLog(ctx);
      }
    }
    if (!was_skipped) {
      RemoveFile(ctx.server_log_path);
    }
  }

  std::printf("test_e2e: %" PRIu32 " passed, %" PRIu32 " failed (%" PRIu32 " skipped), %" PRIu32 " cases\n", passed,
              failed, skipped, static_cast<uint32_t>(sizeof(cases) / sizeof(cases[0])));
  return (0U == failed) ? 0 : 1;
}
