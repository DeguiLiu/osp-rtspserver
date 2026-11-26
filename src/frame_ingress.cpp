// Fixed frame-slot pool: producer-side Acquire/Commit/Abort, consumer-side
// Pop/AddRef/Release (design §4.1, §4.2).
//
// Ownership: after Commit the loop owns one reference. Each PLAY client adds
// one reference; the loop drops its own reference once the frame is handed to
// every client. The last Release returns the slot to the empty-slot queue,
// which only the loop produces and only the encoder consumes.
// SPDX-License-Identifier: MIT
#include "rs/frame_ingress.hpp"

namespace rs {
static_assert(std::atomic<uint32_t>::is_always_lock_free, "orphan counters require lock-free 32-bit atomics");

FrameIngress::FrameIngress() noexcept {
  for (uint32_t i = 0U; i < kFrameSlots; ++i) {
    slot_gen_[i] = 0U;
    slot_len_[i] = 0U;
    slot_refs_[i] = 0U;
    // Prefill before the producer or loop thread exists.
    static_cast<void>(empty_.Push(static_cast<uint16_t>(i)));
  }
}

osp::expected<FrameIngress::Writer, IngressError> FrameIngress::Acquire(uint32_t max_len) noexcept {
  // Every attempt consumes an input sequence number, including the failures:
  // the loop uses the gap to force AwaitIDR on all clients (design §5.3).
  const uint64_t seq = ++input_seq_;
  if (Stopped()) {
    return osp::expected<Writer, IngressError>::error(IngressError::kStopped);
  }
  if (max_len == 0U || max_len > kMaxAuBytes) {
    return osp::expected<Writer, IngressError>::error(IngressError::kInvalidAu);
  }

  int32_t slot = spare_slot_;
  if (slot < 0) {
    uint16_t popped = 0U;
    if (!empty_.Pop(popped)) {
      return osp::expected<Writer, IngressError>::error(IngressError::kNoSlot);
    }
    slot = static_cast<int32_t>(popped);
  }
  spare_slot_ = -1;

  uint32_t gen = next_gen_++;
  if (gen == 0U) {
    gen = next_gen_++;  // Generation 0 is reserved for "never published".
  }
  slot_gen_[slot] = gen;
  slot_len_[slot] = 0U;
  slot_refs_[slot] = 0U;

  Writer writer;
  writer.desc = FrameDesc{};
  writer.desc.input_seq = seq;
  writer.desc.generation = gen;
  writer.desc.slot = static_cast<uint16_t>(slot);
  writer.au = &pool_[slot][0];
  writer.capacity = max_len;
  return osp::expected<Writer, IngressError>::success(writer);
}

osp::expected<void, IngressError> FrameIngress::Commit(Writer& writer, uint32_t actual_len) noexcept {
  if (Stopped()) {
    return osp::expected<void, IngressError>::error(IngressError::kStopped);
  }
  if (writer.au == nullptr || writer.desc.slot >= kFrameSlots || writer.capacity == 0U ||
      writer.desc.generation != slot_gen_[writer.desc.slot]) {
    return osp::expected<void, IngressError>::error(IngressError::kInvalidAu);
  }
  if (actual_len == 0U || actual_len > writer.capacity || actual_len > kMaxAuBytes) {
    return osp::expected<void, IngressError>::error(IngressError::kInvalidAu);
  }
  if (writer.desc.pts_ns < last_pts_ns_) {
    return osp::expected<void, IngressError>::error(IngressError::kInvalidAu);
  }

  const uint16_t slot = writer.desc.slot;
  slot_len_[slot] = actual_len;
  slot_refs_[slot] = 1U;  // Reference held by the loop once queued.
  writer.desc.length = actual_len;
  if (!input_.Push(writer.desc)) {
    slot_len_[slot] = 0U;
    slot_refs_[slot] = 0U;
    return osp::expected<void, IngressError>::error(IngressError::kFull);
  }

  last_pts_ns_ = writer.desc.pts_ns;
  writer.au = nullptr;  // Ownership transferred; the producer must not touch it.
  writer.capacity = 0U;
  return osp::expected<void, IngressError>::success();
}

void FrameIngress::Abort(Writer& writer) noexcept {
  if (writer.au == nullptr || writer.desc.slot >= kFrameSlots) {
    writer.au = nullptr;
    writer.capacity = 0U;
    return;
  }
  const uint16_t slot = writer.desc.slot;
  if (writer.desc.generation == slot_gen_[slot]) {
    slot_len_[slot] = 0U;
    slot_refs_[slot] = 0U;
    if (spare_slot_ < 0) {
      spare_slot_ = static_cast<int32_t>(slot);
    } else {
      // Only reachable with two live writers, which the single-producer
      // contract forbids. Dropping the slot is safer than pushing onto a queue
      // the producer does not own.
      static_cast<void>(producer_orphan_drops_.fetch_add(1U, std::memory_order_relaxed));
    }
  }
  writer.au = nullptr;
  writer.capacity = 0U;
}

void FrameIngress::ReleaseProducerSlot() noexcept {
  if (spare_slot_ < 0) {
    return;
  }
  const uint16_t slot = static_cast<uint16_t>(spare_slot_);
  spare_slot_ = -1;
  static_cast<void>(ReturnSlot(slot));
}

bool FrameIngress::Pop(FrameDesc& out) noexcept {
  return input_.Pop(out);
}

bool FrameIngress::AddRef(const FrameDesc& desc) noexcept {
  if (!Validate(desc)) {
    return false;
  }
  const uint16_t slot = desc.slot;
  if (slot_refs_[slot] == 0xFFFFU) {
    return false;
  }
  ++slot_refs_[slot];
  return true;
}

void FrameIngress::Release(const FrameDesc& desc) noexcept {
  if (desc.slot >= kFrameSlots) {
    return;
  }
  const uint16_t slot = desc.slot;
  if (slot_gen_[slot] != desc.generation || slot_refs_[slot] == 0U) {
    return;
  }
  --slot_refs_[slot];
  if (slot_refs_[slot] == 0U) {
    slot_len_[slot] = 0U;
    static_cast<void>(ReturnSlot(slot));
  }
}

bool FrameIngress::Validate(const FrameDesc& desc) const noexcept {
  if (desc.slot >= kFrameSlots) {
    return false;
  }
  if (slot_gen_[desc.slot] != desc.generation || desc.generation == 0U) {
    return false;
  }
  if (slot_len_[desc.slot] != desc.length || desc.length == 0U || desc.length > kMaxAuBytes) {
    return false;
  }
  return slot_refs_[desc.slot] > 0U;
}

const uint8_t* FrameIngress::Data(const FrameDesc& desc) const noexcept {
  if (!Validate(desc)) {
    return nullptr;
  }
  return &pool_[desc.slot][0];
}

uint32_t FrameIngress::QueuedCount() const noexcept {
  return static_cast<uint32_t>(input_.Size());
}

bool FrameIngress::ConsumeFaultDrop() noexcept {
  uint32_t count = fault_drop_.load(std::memory_order_acquire);
  while (count != 0U && !fault_drop_.compare_exchange_weak(count, count - 1U, std::memory_order_acq_rel)) {}
  return count != 0U;
}

FrameIngressStats FrameIngress::Stats() const noexcept {
  FrameIngressStats stats;
  stats.free_slots = static_cast<uint32_t>(empty_.Size());
  stats.in_use_slots = (kFrameSlots >= stats.free_slots) ? (kFrameSlots - stats.free_slots) : 0U;
  stats.input_queued = static_cast<uint32_t>(input_.Size());
  stats.empty_queued = stats.free_slots;
  stats.producer_orphan_drops = ProducerOrphanDrops();
  stats.loop_orphan_drops = LoopOrphanDrops();
  return stats;
}

bool FrameIngress::ReturnSlot(uint16_t slot) noexcept {
  if (slot >= kFrameSlots) {
    return false;
  }
  slot_len_[slot] = 0U;
  slot_refs_[slot] = 0U;
  if (!empty_.Push(slot)) {
    // The queue covers every slot, so this only happens if the SPSC contract
    // was broken. Keep the slot out of circulation rather than corrupt state.
    static_cast<void>(loop_orphan_drops_.fetch_add(1U, std::memory_order_relaxed));
    return false;
  }
  return true;
}

}  // namespace rs
