// Frame pool lifecycle: Acquire/Commit/Pop/AddRef/Release, generation reuse,
// producer-local Abort slot, pool exhaustion and PTS monotonicity.
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/frame_ingress.hpp"

#include <cstring>

namespace {

using rs::FrameDesc;
using rs::FrameIngress;
using rs::IngressError;

bool CommitOne(FrameIngress& ing, uint64_t pts_ns, uint32_t len, uint16_t flags, FrameDesc* out) {
  auto w = ing.Acquire(len);
  if (w.has_value() == false) {
    return false;
  }
  FrameIngress::Writer writer = w.value();
  writer.desc.pts_ns = pts_ns;
  writer.desc.flags = flags;
  std::memset(writer.au, 0x5A, len);
  auto c = ing.Commit(writer, len);
  if (c.has_value() == false) {
    ing.Abort(writer);
    return false;
  }
  if (out != nullptr) {
    *out = writer.desc;
  }
  return true;
}

int TestLifecycle() {
  FrameIngress ing;
  RS_CHECK_EQ(ing.QueuedCount(), 0U);

  FrameDesc desc{};
  RS_CHECK(CommitOne(ing, 1000U, 128U, rs::kFrameIdr, &desc));
  RS_CHECK_EQ(ing.QueuedCount(), 1U);

  FrameDesc popped{};
  RS_CHECK(ing.Pop(popped));
  RS_CHECK_EQ(ing.QueuedCount(), 0U);
  RS_CHECK(ing.Validate(popped));
  RS_CHECK_EQ(popped.length, 128U);
  RS_CHECK_EQ(popped.pts_ns, 1000U);
  RS_CHECK_EQ(popped.generation, desc.generation);
  RS_CHECK_EQ(popped.slot, desc.slot);
  const uint8_t* data = ing.Data(popped);
  RS_CHECK(data != nullptr);
  RS_CHECK_EQ(data[0], 0x5AU);

  // Loop reference + one client reference.
  RS_CHECK(ing.AddRef(popped));
  ing.Release(popped);             // Client done.
  RS_CHECK(ing.Validate(popped));  // Loop reference still alive.
  ing.Release(popped);             // Loop reference: slot returns to the pool.
  RS_CHECK(ing.Validate(popped) == false);
  RS_CHECK(ing.Data(popped) == nullptr);

  // Release on a stale descriptor is a no-op, not a corruption.
  ing.Release(popped);
  RS_CHECK(ing.AddRef(popped) == false);
  return 0;
}

int TestGenerationReuse() {
  FrameIngress ing;
  // Fill the whole pool so the empty-slot queue cycles back to slot 0.
  FrameDesc first{};
  for (uint32_t i = 0U; i < rs::kFrameSlots; ++i) {
    FrameDesc d{};
    RS_CHECK(CommitOne(ing, 10U + i, 64U, 0U, &d));
    if (i == 0U) {
      first = d;
    }
  }
  FrameDesc popped{};
  for (uint32_t i = 0U; i < rs::kFrameSlots; ++i) {
    RS_CHECK(ing.Pop(popped));
    ing.Release(popped);
  }
  // Slot 0 is handed out again with a fresh generation.
  FrameDesc second{};
  RS_CHECK(CommitOne(ing, 100U, 64U, 0U, &second));
  RS_CHECK_EQ(second.slot, first.slot);
  RS_CHECK(second.generation != first.generation);
  RS_CHECK(ing.Validate(first) == false);
  RS_CHECK(ing.Pop(popped));
  RS_CHECK_EQ(popped.generation, second.generation);
  ing.Release(popped);
  return 0;
}

int TestAbortKeepsLocalSlot() {
  FrameIngress ing;
  auto a = ing.Acquire(256U);
  RS_CHECK(a.has_value());
  const uint16_t slot = a.value().desc.slot;
  const uint32_t gen = a.value().desc.generation;
  ing.Abort(a.value());
  RS_CHECK(a.value().au == nullptr);
  RS_CHECK(ing.Validate(a.value().desc) == false);

  auto b = ing.Acquire(256U);
  RS_CHECK(b.has_value());
  RS_CHECK_EQ(b.value().desc.slot, slot);      // Local spare reused first.
  RS_CHECK(b.value().desc.generation != gen);  // Never reuse a generation.
  FrameIngress::Writer w = b.value();
  w.desc.pts_ns = 1U;
  std::memset(w.au, 1, 8U);
  RS_CHECK(ing.Commit(w, 8U).has_value());
  // Abort is idempotent after a successful commit.
  ing.Abort(w);
  RS_CHECK_EQ(ing.QueuedCount(), 1U);
  FrameDesc popped{};
  RS_CHECK(ing.Pop(popped));
  ing.Release(popped);
  return 0;
}

int TestPoolExhaustion() {
  FrameIngress ing;
  for (uint32_t i = 0U; i < rs::kFrameSlots; ++i) {
    RS_CHECK(CommitOne(ing, 100U + i, 32U, 0U, nullptr));
  }
  RS_CHECK_EQ(ing.QueuedCount(), rs::kFrameSlots);
  // The descriptor queue and the slot pool have the same depth, so the pool
  // empties before Commit can ever see a full queue.
  auto ninth = ing.Acquire(32U);
  RS_CHECK(ninth.has_value() == false);
  RS_CHECK(ninth.get_error() == IngressError::kNoSlot);

  FrameDesc popped{};
  for (uint32_t i = 0U; i < rs::kFrameSlots; ++i) {
    RS_CHECK(ing.Pop(popped));
    ing.Release(popped);
  }
  RS_CHECK(CommitOne(ing, 500U, 32U, 0U, nullptr));
  RS_CHECK(ing.Pop(popped));
  ing.Release(popped);
  return 0;
}

int TestPtsRegressionRejected() {
  FrameIngress ing;
  RS_CHECK(CommitOne(ing, 1000U, 16U, 0U, nullptr));
  FrameDesc popped{};
  RS_CHECK(ing.Pop(popped));
  ing.Release(popped);

  auto w = ing.Acquire(16U);
  RS_CHECK(w.has_value());
  FrameIngress::Writer writer = w.value();
  writer.desc.pts_ns = 999U;
  std::memset(writer.au, 0, 16U);
  auto back = ing.Commit(writer, 16U);
  RS_CHECK(back.has_value() == false);
  RS_CHECK(back.get_error() == IngressError::kInvalidAu);
  // The writer still owns its slot; Abort keeps it for the next Acquire.
  ing.Abort(writer);
  RS_CHECK_EQ(ing.QueuedCount(), 0U);

  auto retry = ing.Acquire(16U);
  RS_CHECK(retry.has_value());
  FrameIngress::Writer second = retry.value();
  second.desc.pts_ns = 1001U;
  std::memset(second.au, 0, 16U);
  RS_CHECK(ing.Commit(second, 16U).has_value());
  RS_CHECK(ing.Pop(popped));
  ing.Release(popped);
  return 0;
}

int TestInvalidLengthAndStopped() {
  FrameIngress ing;
  auto w = ing.Acquire(64U);
  RS_CHECK(w.has_value());
  FrameIngress::Writer writer = w.value();
  std::memset(writer.au, 0, 64U);
  RS_CHECK(ing.Commit(writer, 0U).has_value() == false);
  RS_CHECK(ing.Commit(writer, 65U).has_value() == false);
  ing.Abort(writer);

  RS_CHECK(CommitOne(ing, 1U, rs::kMaxAuBytes, rs::kFrameIdr, nullptr));
  FrameDesc full{};
  RS_CHECK(ing.Pop(full));
  RS_CHECK(ing.Validate(full));
  RS_CHECK_EQ(full.length, rs::kMaxAuBytes);
  RS_CHECK_EQ(ing.Data(full)[rs::kMaxAuBytes - 1U], 0x5AU);
  ing.Release(full);
  RS_CHECK(ing.Acquire(rs::kMaxAuBytes + 1U).has_value() == false);

  ing.Stop();
  auto stopped = ing.Acquire(16U);
  RS_CHECK(stopped.has_value() == false);
  RS_CHECK(stopped.get_error() == IngressError::kStopped);
  return 0;
}

int TestStats() {
  FrameIngress ing;
  auto s0 = ing.Stats();
  RS_CHECK_EQ(s0.free_slots, rs::kFrameSlots);
  RS_CHECK_EQ(s0.in_use_slots, 0U);
  RS_CHECK_EQ(s0.input_queued, 0U);
  RS_CHECK_EQ(s0.empty_queued, rs::kFrameSlots);

  auto w = ing.Acquire(64U);
  RS_CHECK(w.has_value());
  auto s1 = ing.Stats();
  RS_CHECK_EQ(s1.free_slots, rs::kFrameSlots - 1U);
  RS_CHECK_EQ(s1.in_use_slots, 1U);
  ing.Abort(w.value());
  auto s2 = ing.Stats();
  RS_CHECK_EQ(s2.free_slots, rs::kFrameSlots - 1U);  // producer-local spare
  ing.ReleaseProducerSlot();
  auto s3 = ing.Stats();
  RS_CHECK_EQ(s3.free_slots, rs::kFrameSlots);

  FrameDesc desc{};
  RS_CHECK(CommitOne(ing, 10U, 8U, 0U, &desc));
  auto s4 = ing.Stats();
  RS_CHECK_EQ(s4.free_slots, rs::kFrameSlots - 1U);
  FrameDesc popped{};
  RS_CHECK(ing.Pop(popped));
  ing.Release(popped);
  auto s5 = ing.Stats();
  RS_CHECK_EQ(s5.free_slots, rs::kFrameSlots);
  return 0;
}

int TestProducerSlotHandoff() {
  FrameIngress ing;
  auto w = ing.Acquire(32U);
  RS_CHECK(w.has_value());
  const uint16_t slot = w.value().desc.slot;
  ing.Abort(w.value());
  ing.ReleaseProducerSlot();

  // The slot went back through the empty-slot queue: all slots are available.
  uint32_t committed = 0U;
  for (uint32_t i = 0U; i < rs::kFrameSlots; ++i) {
    if (CommitOne(ing, 10U + i, 8U, 0U, nullptr) == false) {
      break;
    }
    ++committed;
  }
  RS_CHECK_EQ(committed, rs::kFrameSlots);
  static_cast<void>(slot);
  return 0;
}

int TestRepeatedFrameReclamation() {
  FrameIngress ing;
  for (uint32_t round = 0U; round < 100000U; ++round) {
    FrameDesc desc{};
    RS_CHECK(CommitOne(ing, round, 16U, 0U, nullptr));
    RS_CHECK(ing.Pop(desc));
    for (uint32_t client = 0U; client < rs::kMaxConnections; ++client) {
      RS_CHECK(ing.AddRef(desc));
    }
    for (uint32_t client = 0U; client < rs::kMaxConnections; ++client) {
      ing.Release(desc);
    }
    ing.Release(desc);
  }
  const auto stats = ing.Stats();
  RS_CHECK_EQ(stats.free_slots, rs::kFrameSlots);
  RS_CHECK_EQ(stats.in_use_slots, 0U);
  RS_CHECK_EQ(stats.producer_orphan_drops, 0U);
  RS_CHECK_EQ(stats.loop_orphan_drops, 0U);
  return 0;
}

}  // namespace

int main() {
  if (TestLifecycle() != 0)
    return 1;
  if (TestGenerationReuse() != 0)
    return 1;
  if (TestAbortKeepsLocalSlot() != 0)
    return 1;
  if (TestPoolExhaustion() != 0)
    return 1;
  if (TestPtsRegressionRejected() != 0)
    return 1;
  if (TestInvalidLengthAndStopped() != 0)
    return 1;
  if (TestStats() != 0)
    return 1;
  if (TestProducerSlotHandoff() != 0)
    return 1;
  if (TestRepeatedFrameReclamation() != 0)
    return 1;
  std::printf("test_frame_ingress: all checks passed\n");
  return 0;
}
