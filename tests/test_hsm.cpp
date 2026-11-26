// Service and session HSM tests (design §9.1, §9.2, §9.7).
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/session_hsm.hpp"

namespace {

// Finds the index of the first trace entry with the given kind, or -1.
int FindStep(const rs::HsmTrace& t, rs::HsmStepKind kind) {
  for (uint32_t i = 0U; i < t.count; ++i) {
    if (t.kind[i] == static_cast<uint8_t>(kind)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int TestServiceTables() {
  RS_CHECK(rs::svc::Hsm::TablesValid());
  rs::svc::Context ctx;
  rs::svc::Hsm hsm(ctx);
  // Dispatch before Start is rejected.
  RS_CHECK(!hsm.Dispatch(rs::svc::kStartReady));
  RS_CHECK(!hsm.Started());
  hsm.Start();
  hsm.Start();  // Duplicate Start is a no-op.
  RS_CHECK_EQ(hsm.State(), rs::svc::kInit);
  return 0;
}

int TestServiceHappyPath() {
  rs::svc::Context ctx;
  rs::svc::Hsm hsm(ctx);
  hsm.Start();
  ctx.listener_ready = true;
  RS_CHECK(hsm.Dispatch(rs::svc::kStartReady));
  RS_CHECK_EQ(hsm.State(), rs::svc::kReady);
  RS_CHECK(ctx.start_ok);

  // A second start from Ready has no arc.
  RS_CHECK(!hsm.Dispatch(rs::svc::kStartReady));
  RS_CHECK_EQ(hsm.State(), rs::svc::kReady);

  RS_CHECK(hsm.Dispatch(rs::svc::kThreadStarted));
  RS_CHECK_EQ(hsm.State(), rs::svc::kRunning);
  RS_CHECK(hsm.Dispatch(rs::svc::kStopRequested));
  RS_CHECK_EQ(hsm.State(), rs::svc::kStopping);
  // A duplicate stop request from Stopping is rejected.
  RS_CHECK(!hsm.Dispatch(rs::svc::kStopRequested));
  RS_CHECK(hsm.Dispatch(rs::svc::kDrainDone));
  RS_CHECK_EQ(hsm.State(), rs::svc::kStopped);
  RS_CHECK(ctx.cleanup_done);
  RS_CHECK(!hsm.Dispatch(rs::svc::kDrainDone));
  return 0;
}

int TestServiceGuardRejection() {
  rs::svc::Context ctx;
  rs::svc::Hsm hsm(ctx);
  hsm.Start();
  ctx.listener_ready = false;  // Registration failed.
  RS_CHECK(hsm.Dispatch(rs::svc::kStartReady));
  RS_CHECK_EQ(hsm.State(), rs::svc::kFailed);
  RS_CHECK(!ctx.start_ok);
  RS_CHECK(hsm.Dispatch(rs::svc::kDrainDone));
  RS_CHECK_EQ(hsm.State(), rs::svc::kStopped);
  return 0;
}

int TestServiceFatal() {
  rs::svc::Context ctx;
  rs::svc::Hsm hsm(ctx);
  hsm.Start();
  ctx.listener_ready = true;
  RS_CHECK(hsm.Dispatch(rs::svc::kStartReady));
  RS_CHECK(hsm.Dispatch(rs::svc::kThreadStarted));
  RS_CHECK(hsm.Dispatch(rs::svc::kFatal));
  RS_CHECK_EQ(hsm.State(), rs::svc::kFailed);
  RS_CHECK(ctx.fatal_seen);
  RS_CHECK_EQ(ctx.fatal_count, 1U);
  RS_CHECK(!hsm.Dispatch(rs::svc::kThreadStarted));
  return 0;
}

int TestServiceCallbackOrder() {
  rs::svc::Context ctx;
  rs::svc::Hsm hsm(ctx);
  hsm.Start();
  ctx.trace.Clear();
  ctx.listener_ready = true;
  RS_CHECK(hsm.Dispatch(rs::svc::kStartReady));
  // Action -> exit(source) -> entry(target). Guards are pure and not traced.
  RS_CHECK_EQ(ctx.trace.count, 3U);
  RS_CHECK_EQ(ctx.trace.kind[0], static_cast<uint8_t>(rs::HsmStepKind::kAction));
  RS_CHECK_EQ(ctx.trace.kind[1], static_cast<uint8_t>(rs::HsmStepKind::kExit));
  RS_CHECK_EQ(ctx.trace.kind[2], static_cast<uint8_t>(rs::HsmStepKind::kEntry));
  RS_CHECK_EQ(ctx.trace.state[1], rs::svc::kInit);
  RS_CHECK_EQ(ctx.trace.state[2], rs::svc::kReady);
  return 0;
}

int TestSessionTables() {
  RS_CHECK(rs::sess::Hsm::TablesValid());
  rs::sess::Context ctx;
  rs::sess::Hsm hsm(ctx);
  hsm.Start();
  RS_CHECK_EQ(hsm.State(), rs::sess::kInit);
  RS_CHECK(hsm.IsInState(rs::sess::kInit));
  RS_CHECK(!hsm.IsInState(rs::sess::kActive));
  return 0;
}

int TestSessionSetupGuardAndRollback() {
  rs::sess::Context ctx;
  rs::sess::Hsm hsm(ctx);
  hsm.Start();
  // Guard refuses: no arc, state untouched.
  ctx.setup_ok = false;
  RS_CHECK(!hsm.Dispatch(rs::sess::kSetupOk));
  RS_CHECK_EQ(hsm.State(), rs::sess::kInit);
  RS_CHECK_EQ(ctx.rejects, 1U);

  // Explicit rollback keeps Init and records the failure (SETUP resource
  // acquisition failed outside the HSM).
  RS_CHECK(hsm.Dispatch(rs::sess::kSetupFail));
  RS_CHECK_EQ(hsm.State(), rs::sess::kInit);
  RS_CHECK_EQ(ctx.rollbacks, 1U);

  ctx.setup_ok = true;
  RS_CHECK(hsm.Dispatch(rs::sess::kSetupOk));
  RS_CHECK_EQ(hsm.State(), rs::sess::kReady);
  RS_CHECK(hsm.IsInState(rs::sess::kActive));

  // A repeated SETUP that fails rolls back without leaving Ready.
  RS_CHECK(hsm.Dispatch(rs::sess::kSetupFail));
  RS_CHECK_EQ(hsm.State(), rs::sess::kReady);
  RS_CHECK_EQ(ctx.rollbacks, 2U);
  return 0;
}

int TestSessionPlayAndTeardown() {
  rs::sess::Context ctx;
  rs::sess::Hsm hsm(ctx);
  hsm.Start();
  ctx.setup_ok = true;
  RS_CHECK(hsm.Dispatch(rs::sess::kSetupOk));
  RS_CHECK(hsm.Dispatch(rs::sess::kPlayOk));
  RS_CHECK_EQ(hsm.State(), rs::sess::kPlaying);

  // Repeated PLAY is not a valid arc (455 at the request layer).
  RS_CHECK(!hsm.Dispatch(rs::sess::kPlayOk));
  // SETUP while Playing is not valid either.
  RS_CHECK(!hsm.Dispatch(rs::sess::kSetupOk));
  RS_CHECK_EQ(hsm.State(), rs::sess::kPlaying);

  RS_CHECK(hsm.Dispatch(rs::sess::kTeardown));
  RS_CHECK_EQ(hsm.State(), rs::sess::kClosing);
  RS_CHECK(hsm.Dispatch(rs::sess::kCloseDone));
  RS_CHECK_EQ(hsm.State(), rs::sess::kInit);
  RS_CHECK_EQ(ctx.teardowns, 1U);
  return 0;
}

int TestSessionParentInheritance() {
  // Active parent handles Timeout/PeerClosed for both children.
  {
    rs::sess::Context ctx;
    rs::sess::Hsm hsm(ctx);
    hsm.Start();
    ctx.setup_ok = true;
    RS_CHECK(hsm.Dispatch(rs::sess::kSetupOk));
    RS_CHECK(hsm.Dispatch(rs::sess::kTimeout));
    RS_CHECK_EQ(hsm.State(), rs::sess::kClosing);
    RS_CHECK_EQ(ctx.timeouts, 1U);
  }
  {
    rs::sess::Context ctx;
    rs::sess::Hsm hsm(ctx);
    hsm.Start();
    ctx.setup_ok = true;
    RS_CHECK(hsm.Dispatch(rs::sess::kSetupOk));
    RS_CHECK(hsm.Dispatch(rs::sess::kPlayOk));
    RS_CHECK(hsm.Dispatch(rs::sess::kPeerClosed));
    RS_CHECK_EQ(hsm.State(), rs::sess::kClosing);
    RS_CHECK_EQ(ctx.peer_closes, 1U);
    RS_CHECK(hsm.Dispatch(rs::sess::kCloseDone));
    RS_CHECK_EQ(hsm.State(), rs::sess::kInit);
  }
  // Init can also be closed by the peer.
  {
    rs::sess::Context ctx;
    rs::sess::Hsm hsm(ctx);
    hsm.Start();
    RS_CHECK(hsm.Dispatch(rs::sess::kPeerClosed));
    RS_CHECK_EQ(hsm.State(), rs::sess::kClosing);
    RS_CHECK(hsm.Dispatch(rs::sess::kCloseDone));
    RS_CHECK_EQ(hsm.State(), rs::sess::kInit);
  }
  return 0;
}

int TestSessionOrderAndRestart() {
  rs::sess::Context ctx;
  rs::sess::Hsm hsm(ctx);
  hsm.Start();
  ctx.trace.Clear();
  ctx.setup_ok = true;
  RS_CHECK(hsm.Dispatch(rs::sess::kSetupOk));
  // Init -> Ready crosses the Active parent, so entry runs top-down for both.
  RS_CHECK_EQ(ctx.trace.count, 4U);
  RS_CHECK_EQ(ctx.trace.kind[0], static_cast<uint8_t>(rs::HsmStepKind::kAction));
  RS_CHECK_EQ(ctx.trace.kind[1], static_cast<uint8_t>(rs::HsmStepKind::kExit));
  RS_CHECK_EQ(ctx.trace.kind[2], static_cast<uint8_t>(rs::HsmStepKind::kEntry));
  RS_CHECK_EQ(ctx.trace.kind[3], static_cast<uint8_t>(rs::HsmStepKind::kEntry));
  RS_CHECK_EQ(ctx.trace.state[2], rs::sess::kActive);
  RS_CHECK_EQ(ctx.trace.state[3], rs::sess::kReady);
  RS_CHECK(FindStep(ctx.trace, rs::HsmStepKind::kAction) < FindStep(ctx.trace, rs::HsmStepKind::kExit));

  // Slot reuse re-enters Init with a clean context.
  hsm.Restart();
  RS_CHECK_EQ(hsm.State(), rs::sess::kInit);
  RS_CHECK_EQ(ctx.applied, 0U);
  ctx.setup_ok = true;
  RS_CHECK(hsm.Dispatch(rs::sess::kSetupOk));
  RS_CHECK_EQ(hsm.State(), rs::sess::kReady);
  return 0;
}

int TestSixteenIndependentSessions() {
  struct Bundle {
    rs::sess::Context ctx;
    rs::sess::Hsm hsm;
    Bundle() : hsm(ctx) {}
  };
  Bundle bundles[16];
  for (uint32_t i = 0U; i < 16U; ++i) {
    bundles[i].hsm.Start();
    bundles[i].ctx.setup_ok = true;
    RS_CHECK(bundles[i].hsm.Dispatch(rs::sess::kSetupOk));
  }
  for (uint32_t i = 0U; i < 16U; i += 2U) {
    RS_CHECK(bundles[i].hsm.Dispatch(rs::sess::kPlayOk));
  }
  for (uint32_t i = 0U; i < 16U; ++i) {
    const int32_t want = (i % 2U == 0U) ? rs::sess::kPlaying : rs::sess::kReady;
    RS_CHECK_EQ(bundles[i].hsm.State(), want);
  }
  // Tear down one session; the others keep their own state.
  RS_CHECK(bundles[0].hsm.Dispatch(rs::sess::kTeardown));
  RS_CHECK_EQ(bundles[0].hsm.State(), rs::sess::kClosing);
  RS_CHECK_EQ(bundles[2].hsm.State(), rs::sess::kPlaying);
  RS_CHECK_EQ(bundles[1].hsm.State(), rs::sess::kReady);
  return 0;
}

}  // namespace

int main() {
  if (TestServiceTables() != 0)
    return 1;
  if (TestServiceHappyPath() != 0)
    return 1;
  if (TestServiceGuardRejection() != 0)
    return 1;
  if (TestServiceFatal() != 0)
    return 1;
  if (TestServiceCallbackOrder() != 0)
    return 1;
  if (TestSessionTables() != 0)
    return 1;
  if (TestSessionSetupGuardAndRollback() != 0)
    return 1;
  if (TestSessionPlayAndTeardown() != 0)
    return 1;
  if (TestSessionParentInheritance() != 0)
    return 1;
  if (TestSessionOrderAndRestart() != 0)
    return 1;
  if (TestSixteenIndependentSessions() != 0)
    return 1;
  std::printf("test_hsm: all checks passed\n");
  return 0;
}
