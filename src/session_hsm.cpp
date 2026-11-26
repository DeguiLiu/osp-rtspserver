// Static state tables for the service lifecycle and the RTSP session.
// SPDX-License-Identifier: MIT
#include "rs/session_hsm.hpp"

#include <new>

namespace rs {
namespace {

// Finds the first row for (current state or one of its ancestors, event) whose
// guard passes. Mirrors osp::TableHsm's lookup so Dispatch can report
// "unmatched" without changing newosp. Guards must stay side-effect free
// because this pre-check may evaluate them once more than TableHsm does.
template <typename Context>
bool MatchRow(const osp::StateDef<Context>* states, const osp::TransitionDef<Context>* trans, uint32_t count,
              int32_t current, uint32_t event, Context& ctx, const void* data) noexcept {
  for (int32_t state = current; state >= 0; state = states[state].parent) {
    for (uint32_t i = 0U; i < count; ++i) {
      if (trans[i].from != state || trans[i].event != event) {
        continue;
      }
      if (trans[i].guard == nullptr || trans[i].guard(ctx, data)) {
        return true;
      }
    }
  }
  return false;
}

template <typename Context>
bool TablesAreValid(const osp::StateDef<Context>* states, uint32_t state_count,
                    const osp::TransitionDef<Context>* trans, uint32_t trans_count) noexcept {
  if (state_count == 0U || states == nullptr || trans == nullptr) {
    return false;
  }
  for (uint32_t i = 0U; i < state_count; ++i) {
    if (states[i].parent >= static_cast<int32_t>(state_count)) {
      return false;
    }
    int32_t walk = states[i].parent;
    uint32_t depth = 0U;
    while (walk >= 0 && depth <= state_count) {
      walk = states[walk].parent;
      ++depth;
    }
    if (depth > state_count) {
      return false;  // Parent chain cycle or runaway depth.
    }
  }
  for (uint32_t i = 0U; i < trans_count; ++i) {
    if (trans[i].event == 0U) {
      return false;
    }
    if (trans[i].from < 0 || trans[i].from >= static_cast<int32_t>(state_count)) {
      return false;
    }
    if (trans[i].to < 0 || trans[i].to >= static_cast<int32_t>(state_count)) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ===========================================================================
// Service lifecycle
// ===========================================================================
namespace svc {
namespace {

void OnEntry(Context& ctx, int32_t state) noexcept {
  ctx.trace.Push(HsmStepKind::kEntry, state);
}
void OnExit(Context& ctx, int32_t state) noexcept {
  ctx.trace.Push(HsmStepKind::kExit, state);
}

void EnterRoot(Context& c) noexcept {
  OnEntry(c, kRoot);
}
void EnterInit(Context& c) noexcept {
  OnEntry(c, kInit);
}
void EnterReady(Context& c) noexcept {
  OnEntry(c, kReady);
}
void EnterRunning(Context& c) noexcept {
  OnEntry(c, kRunning);
}
void EnterStopping(Context& c) noexcept {
  OnEntry(c, kStopping);
}
void EnterStopped(Context& c) noexcept {
  OnEntry(c, kStopped);
}
void EnterFailed(Context& c) noexcept {
  OnEntry(c, kFailed);
}

void LeaveRoot(Context& c) noexcept {
  OnExit(c, kRoot);
}
void LeaveInit(Context& c) noexcept {
  OnExit(c, kInit);
}
void LeaveReady(Context& c) noexcept {
  OnExit(c, kReady);
}
void LeaveRunning(Context& c) noexcept {
  OnExit(c, kRunning);
}
void LeaveStopping(Context& c) noexcept {
  OnExit(c, kStopping);
}
void LeaveStopped(Context& c) noexcept {
  OnExit(c, kStopped);
}
void LeaveFailed(Context& c) noexcept {
  OnExit(c, kFailed);
}

bool GuardListenerReady(Context& c, const void* /*data*/) noexcept {
  return c.listener_ready;
}

// Actions validate their precondition again and record the outcome; the target
// state is data, never a return value.
void ActStartReady(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kReady);
  if (c.listener_ready == false) {
    ++c.rejects;
    return;
  }
  c.start_ok = true;
  ++c.applied;
}

void ActStartFail(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kFailed);
  c.start_ok = false;
  ++c.applied;
}

void ActThreadStarted(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kRunning);
  ++c.applied;
}

void ActStopRequested(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kStopping);
  ++c.applied;
}

void ActDrainDone(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kStopped);
  c.cleanup_done = true;
  ++c.applied;
}

void ActFatal(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kFailed);
  c.fatal_seen = true;
  ++c.fatal_count;
  ++c.applied;
}

constexpr osp::StateDef<Context> kStates[kStateCount] = {
    {"Root", -1, EnterRoot, LeaveRoot},
    {"Init", kRoot, EnterInit, LeaveInit},
    {"Ready", kRoot, EnterReady, LeaveReady},
    {"Running", kRoot, EnterRunning, LeaveRunning},
    {"Stopping", kRoot, EnterStopping, LeaveStopping},
    {"Stopped", kRoot, EnterStopped, LeaveStopped},
    {"Failed", kRoot, EnterFailed, LeaveFailed},
};

// Field order: {from, event, to, kind, action, guard}.
constexpr osp::TransitionDef<Context> kTransitions[kTransitionCount] = {
    {kInit, kStartReady, kReady, osp::TransitionKind::kExternal, ActStartReady, GuardListenerReady},
    {kInit, kStartReady, kFailed, osp::TransitionKind::kExternal, ActStartFail, nullptr},
    {kInit, kStopRequested, kStopping, osp::TransitionKind::kExternal, ActStopRequested, nullptr},
    {kReady, kThreadStarted, kRunning, osp::TransitionKind::kExternal, ActThreadStarted, nullptr},
    {kReady, kStartFail, kFailed, osp::TransitionKind::kExternal, ActStartFail, nullptr},
    {kReady, kFatal, kFailed, osp::TransitionKind::kExternal, ActFatal, nullptr},
    {kReady, kStopRequested, kStopping, osp::TransitionKind::kExternal, ActStopRequested, nullptr},
    {kRunning, kStopRequested, kStopping, osp::TransitionKind::kExternal, ActStopRequested, nullptr},
    {kRunning, kFatal, kFailed, osp::TransitionKind::kExternal, ActFatal, nullptr},
    {kStopping, kDrainDone, kStopped, osp::TransitionKind::kExternal, ActDrainDone, nullptr},
    {kFailed, kDrainDone, kStopped, osp::TransitionKind::kExternal, ActDrainDone, nullptr},
};

}  // namespace

Hsm::Hsm(Context& ctx) noexcept : ctx_(ctx), hsm_(ctx, kStates, kStateCount, kTransitions, kTransitionCount) {
  hsm_.SetInitialState(kInit);
}

void Hsm::Start() noexcept {
  if (started_) {
    return;
  }
  started_ = true;
  ctx_.trace.Clear();
  hsm_.Start();
}

void Hsm::Restart() noexcept {
  using Machine = osp::TableHsm<Context, kStateCount, kTransitionCount>;
  hsm_.~Machine();
  ::new (&hsm_) Machine(ctx_, kStates, kStateCount, kTransitions, kTransitionCount);
  hsm_.SetInitialState(kInit);
  ctx_ = Context{};
  started_ = false;
  Start();
}

bool Hsm::Dispatch(uint32_t event) noexcept {
  if (started_ == false) {
    ++ctx_.rejects;
    return false;
  }
  if (MatchRow(kStates, kTransitions, kTransitionCount, hsm_.CurrentState(), event, ctx_, nullptr) == false) {
    ++ctx_.rejects;
    return false;
  }
  hsm_.Dispatch(osp::Event{event, nullptr});
  return true;
}

bool Hsm::TablesValid() noexcept {
  return TablesAreValid(kStates, kStateCount, kTransitions, kTransitionCount);
}

}  // namespace svc

// ===========================================================================
// RTSP session
// ===========================================================================
namespace sess {
namespace {

void OnEntry(Context& ctx, int32_t state) noexcept {
  ctx.trace.Push(HsmStepKind::kEntry, state);
}
void OnExit(Context& ctx, int32_t state) noexcept {
  ctx.trace.Push(HsmStepKind::kExit, state);
}

void EnterRoot(Context& c) noexcept {
  OnEntry(c, kRoot);
}
void EnterInit(Context& c) noexcept {
  OnEntry(c, kInit);
}
void EnterActive(Context& c) noexcept {
  OnEntry(c, kActive);
}
void EnterReady(Context& c) noexcept {
  OnEntry(c, kReady);
}
void EnterPlaying(Context& c) noexcept {
  OnEntry(c, kPlaying);
}
void EnterClosing(Context& c) noexcept {
  OnEntry(c, kClosing);
}

void LeaveRoot(Context& c) noexcept {
  OnExit(c, kRoot);
}
void LeaveInit(Context& c) noexcept {
  OnExit(c, kInit);
}
void LeaveActive(Context& c) noexcept {
  OnExit(c, kActive);
}
void LeaveReady(Context& c) noexcept {
  OnExit(c, kReady);
}
void LeavePlaying(Context& c) noexcept {
  OnExit(c, kPlaying);
}
void LeaveClosing(Context& c) noexcept {
  OnExit(c, kClosing);
}

bool GuardSetupOk(Context& c, const void* /*data*/) noexcept {
  return c.setup_ok;
}

// Re-checks the guard input so a caller that forgets the guard cannot publish
// Ready. On failure the state is unchanged and the rollback is recorded.
void ActSetupOk(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kReady);
  if (c.setup_ok == false) {
    ++c.rollbacks;
    return;
  }
  ++c.applied;
}

void ActSetupRollback(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kInit);
  ++c.rollbacks;
}

void ActPlayOk(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kPlaying);
  ++c.applied;
}

void ActTeardown(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kClosing);
  ++c.teardowns;
  ++c.applied;
}

void ActTimeout(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kClosing);
  ++c.timeouts;
  ++c.applied;
}

void ActPeerClosed(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kClosing);
  ++c.peer_closes;
  ++c.applied;
}

void ActCloseDone(Context& c, const void* /*data*/) noexcept {
  c.trace.Push(HsmStepKind::kAction, kInit);
  ++c.applied;
}

constexpr osp::StateDef<Context> kStates[kStateCount] = {
    {"Root", -1, EnterRoot, LeaveRoot},
    {"Init", kRoot, EnterInit, LeaveInit},
    {"Active", kRoot, EnterActive, LeaveActive},
    {"Ready", kActive, EnterReady, LeaveReady},
    {"Playing", kActive, EnterPlaying, LeavePlaying},
    {"Closing", kRoot, EnterClosing, LeaveClosing},
};

// Field order: {from, event, to, kind, action, guard}.
constexpr osp::TransitionDef<Context> kTransitions[kTransitionCount] = {
    {kInit, kSetupOk, kReady, osp::TransitionKind::kExternal, ActSetupOk, GuardSetupOk},
    // Repeated SETUP while Ready replaces the transport without a state change.
    {kReady, kSetupOk, kReady, osp::TransitionKind::kInternal, ActSetupOk, GuardSetupOk},
    {kInit, kSetupFail, kInit, osp::TransitionKind::kInternal, ActSetupRollback, nullptr},
    {kReady, kSetupFail, kReady, osp::TransitionKind::kInternal, ActSetupRollback, nullptr},
    {kReady, kPlayOk, kPlaying, osp::TransitionKind::kExternal, ActPlayOk, nullptr},
    // Active is the parent: TEARDOWN/Timeout/PeerClosed apply to Ready and Playing.
    {kActive, kTeardown, kClosing, osp::TransitionKind::kExternal, ActTeardown, nullptr},
    {kActive, kTimeout, kClosing, osp::TransitionKind::kExternal, ActTimeout, nullptr},
    {kActive, kPeerClosed, kClosing, osp::TransitionKind::kExternal, ActPeerClosed, nullptr},
    {kInit, kPeerClosed, kClosing, osp::TransitionKind::kExternal, ActPeerClosed, nullptr},
    {kInit, kTimeout, kClosing, osp::TransitionKind::kExternal, ActTimeout, nullptr},
    {kClosing, kCloseDone, kInit, osp::TransitionKind::kExternal, ActCloseDone, nullptr},
};

}  // namespace

const char* StateName(int32_t state) noexcept {
  switch (state) {
    case kRoot:
      return "Root";
    case kInit:
      return "Init";
    case kActive:
      return "Active";
    case kReady:
      return "Ready";
    case kPlaying:
      return "Playing";
    case kClosing:
      return "Closing";
    default:
      return "?";
  }
}

Hsm::Hsm(Context& ctx) noexcept : ctx_(ctx), hsm_(ctx, kStates, kStateCount, kTransitions, kTransitionCount) {
  hsm_.SetInitialState(kInit);
}

void Hsm::Start() noexcept {
  if (started_) {
    return;
  }
  started_ = true;
  ctx_.trace.Clear();
  hsm_.Start();
}

void Hsm::Restart() noexcept {
  using Machine = osp::TableHsm<Context, kStateCount, kTransitionCount>;
  hsm_.~Machine();
  ::new (&hsm_) Machine(ctx_, kStates, kStateCount, kTransitions, kTransitionCount);
  hsm_.SetInitialState(kInit);
  ctx_ = Context{};
  started_ = false;
  Start();
}

bool Hsm::Dispatch(uint32_t event) noexcept {
  if (started_ == false) {
    ++ctx_.rejects;
    return false;
  }
  if (MatchRow(kStates, kTransitions, kTransitionCount, hsm_.CurrentState(), event, ctx_, nullptr) == false) {
    ++ctx_.rejects;
    return false;
  }
  hsm_.Dispatch(osp::Event{event, nullptr});
  return true;
}

bool Hsm::TablesValid() noexcept {
  return TablesAreValid(kStates, kStateCount, kTransitions, kTransitionCount);
}

}  // namespace sess

}  // namespace rs
