// Static table-driven state machines for the server lifecycle and the
// per-connection RTSP session (design §9.1, §9.2, §9.7).
//
// Both machines are wrappers over osp::TableHsm. The wrapper adds a Dispatch
// that reports whether a row matched, because TableHsm::Dispatch is void and
// the request layer must map "no valid arc" to 455 instead of assuming the
// event was handled.
//
// SPDX-License-Identifier: MIT
#ifndef RS_SESSION_HSM_HPP_
#define RS_SESSION_HSM_HPP_

#include "osp/hsm_table.hpp"

#include <cstdint>

namespace rs {

// Callback order trace: guards run before actions, actions run before the
// exit/entry pair of an external transition. Tests assert that order.
enum class HsmStepKind : uint8_t { kGuard = 0, kAction, kExit, kEntry };

struct HsmTrace {
  static constexpr uint32_t kCapacity = 48U;

  uint8_t kind[kCapacity] = {};
  int32_t state[kCapacity] = {};
  uint32_t count = 0U;

  void Push(HsmStepKind k, int32_t s) noexcept {
    if (count < kCapacity) {
      kind[count] = static_cast<uint8_t>(k);
      state[count] = s;
      ++count;
    }
  }

  void Clear() noexcept { count = 0U; }
};

// ---------------------------------------------------------------------------
// Server lifecycle: Init -> Ready -> Running -> Stopping -> Stopped, plus
// Failed. Any transition not listed is rejected (Dispatch returns false).
// ---------------------------------------------------------------------------
namespace svc {

enum State : int32_t { kRoot = 0, kInit, kReady, kRunning, kStopping, kStopped, kFailed };

constexpr uint32_t kStateCount = 7U;
constexpr uint32_t kTransitionCount = 11U;

enum Event : uint32_t { kStartReady = 1U, kThreadStarted, kStartFail, kStopRequested, kDrainDone, kFatal };

struct Context {
  // Guard inputs, set by the owning thread before Dispatch.
  bool listener_ready = false;
  bool fatal_seen = false;

  // Action results, written by the transition actions.
  bool start_ok = false;
  bool cleanup_done = false;
  uint32_t applied = 0U;
  uint32_t rejects = 0U;
  uint32_t fatal_count = 0U;
  HsmTrace trace;
};

class Hsm {
 public:
  explicit Hsm(Context& ctx) noexcept;

  // Enters Init. A second Start() is a no-op (the machine starts once).
  void Start() noexcept;
  // Re-enters Init from scratch (used when a connection slot is reused).
  void Restart() noexcept;
  // Returns false when no row matched (including before Start()).
  [[nodiscard]] bool Dispatch(uint32_t event) noexcept;

  [[nodiscard]] int32_t State() const noexcept { return hsm_.CurrentState(); }
  [[nodiscard]] const char* StateName() const noexcept { return hsm_.CurrentStateName(); }
  [[nodiscard]] bool IsInState(int32_t s) const noexcept { return hsm_.IsInState(s); }
  [[nodiscard]] bool Started() const noexcept { return started_; }

  // Startup self-check: parent chain valid, no cycle, every row in range.
  [[nodiscard]] static bool TablesValid() noexcept;

 private:
  Context& ctx_;
  osp::TableHsm<Context, kStateCount, kTransitionCount> hsm_;
  bool started_ = false;
};

}  // namespace svc

// ---------------------------------------------------------------------------
// Session: Init -> Active{Ready, Playing} -> Closing -> Init.
// Active is the parent that owns TEARDOWN/Timeout/PeerClosed.
// ---------------------------------------------------------------------------
namespace sess {

enum State : int32_t { kRoot = 0, kInit, kActive, kReady, kPlaying, kClosing };

constexpr uint32_t kStateCount = 6U;
constexpr uint32_t kTransitionCount = 11U;

[[nodiscard]] const char* StateName(int32_t state) noexcept;

enum Event : uint32_t { kSetupOk = 1U, kSetupFail, kPlayOk, kTeardown, kTimeout, kPeerClosed, kCloseDone };

struct Context {
  // Guard input: resources for SETUP were acquired outside the HSM.
  bool setup_ok = false;

  // Action results.
  uint32_t applied = 0U;
  uint32_t rollbacks = 0U;
  uint32_t rejects = 0U;
  uint32_t teardowns = 0U;
  uint32_t timeouts = 0U;
  uint32_t peer_closes = 0U;
  HsmTrace trace;
};

class Hsm {
 public:
  explicit Hsm(Context& ctx) noexcept;

  void Start() noexcept;
  void Restart() noexcept;
  [[nodiscard]] bool Dispatch(uint32_t event) noexcept;

  [[nodiscard]] int32_t State() const noexcept { return hsm_.CurrentState(); }
  [[nodiscard]] const char* StateName() const noexcept { return hsm_.CurrentStateName(); }
  [[nodiscard]] bool IsInState(int32_t s) const noexcept { return hsm_.IsInState(s); }
  [[nodiscard]] bool Started() const noexcept { return started_; }

  [[nodiscard]] static bool TablesValid() noexcept;

 private:
  Context& ctx_;
  osp::TableHsm<Context, kStateCount, kTransitionCount> hsm_;
  bool started_ = false;
};

}  // namespace sess

}  // namespace rs

#endif  // RS_SESSION_HSM_HPP_
