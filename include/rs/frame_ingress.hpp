// Fixed frame-slot pool with intrusive refcounts and SPSC hand-off queues.
// Producer = encoder thread (Acquire/Commit/Abort); consumer = loop thread
// (Pop/AddRef/Release). No allocation after construction.
// SPDX-License-Identifier: MIT
#ifndef RS_FRAME_INGRESS_HPP_
#define RS_FRAME_INGRESS_HPP_

#include "osp/spsc_ringbuffer.hpp"
#include "osp/vocabulary.hpp"
#include "rs/config.hpp"

#include <cstddef>
#include <cstdint>

#include <atomic>

namespace rs {

enum FrameFlags : uint16_t { kFrameIdr = 0x0001U, kFrameHasParams = 0x0002U, kFrameParamChanged = 0x0004U };

struct FrameDesc {
  uint64_t pts_ns = 0U;
  uint64_t input_seq = 0U;
  uint32_t length = 0U;
  uint32_t generation = 0U;
  uint16_t slot = 0U;
  uint16_t flags = 0U;
};

static_assert(sizeof(FrameDesc) <= 32U, "FrameDesc must stay cache-line friendly");
static_assert((sizeof(FrameDesc) % alignof(FrameDesc)) == 0U, "FrameDesc must not contain implicit tail padding");
static_assert(alignof(FrameDesc) <= 8U, "FrameDesc must stay naturally aligned on 32-bit targets");
static_assert((kMaxAuBytes % kCacheLineBytes) == 0U, "frame slot stride must be cache-line aligned");

// Producer -> server notifications. FileSource (or an encoder adapter) uses
// these callbacks; the server implements them without exposing its internals.
struct SourceCallbacks {
  void* ctx = nullptr;
  void (*wake)(void*) noexcept = nullptr;
  void (*drop)(void*) noexcept = nullptr;
  void (*error)(void*, uint32_t) noexcept = nullptr;
};

struct FrameIngressStats {
  uint32_t free_slots = 0U;
  uint32_t in_use_slots = 0U;
  uint32_t input_queued = 0U;
  uint32_t empty_queued = 0U;
  uint32_t producer_orphan_drops = 0U;
  uint32_t loop_orphan_drops = 0U;
};

enum class IngressError : uint8_t { kFull = 0, kStopped, kInvalidAu, kNoSlot };

class FrameIngress {
 public:
  struct Writer {
    FrameDesc desc;
    uint8_t* au = nullptr;
    uint32_t capacity = 0U;
  };

  FrameIngress() noexcept;

  // Encoder thread.
  [[nodiscard]] osp::expected<Writer, IngressError> Acquire(uint32_t max_len) noexcept;
  [[nodiscard]] osp::expected<void, IngressError> Commit(Writer& writer, uint32_t actual_len) noexcept;
  void Abort(Writer& writer) noexcept;
  // Called once by the producer at shutdown to return its local spare slot.
  void ReleaseProducerSlot() noexcept;

  // Loop thread.
  [[nodiscard]] bool Pop(FrameDesc& out) noexcept;
  [[nodiscard]] bool AddRef(const FrameDesc& desc) noexcept;
  void Release(const FrameDesc& desc) noexcept;
  [[nodiscard]] bool Validate(const FrameDesc& desc) const noexcept;
  [[nodiscard]] const uint8_t* Data(const FrameDesc& desc) const noexcept;
  [[nodiscard]] uint32_t QueuedCount() const noexcept;
  [[nodiscard]] uint32_t ProducerOrphanDrops() const noexcept {
    return producer_orphan_drops_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] uint32_t LoopOrphanDrops() const noexcept { return loop_orphan_drops_.load(std::memory_order_relaxed); }
  [[nodiscard]] FrameIngressStats Stats() const noexcept;
  void SetFaultDrop(uint32_t count) noexcept { fault_drop_.store(count, std::memory_order_release); }
  [[nodiscard]] uint32_t FaultDropRemaining() const noexcept { return fault_drop_.load(std::memory_order_acquire); }
  [[nodiscard]] bool ConsumeFaultDrop() noexcept;

  void Stop() noexcept { stopped_.store(true, std::memory_order_release); }
  // Only safe when no producer holds a writer; used to retry a failed Start().
  void Resume() noexcept { stopped_.store(false, std::memory_order_release); }
  [[nodiscard]] bool Stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

 private:
  [[nodiscard]] bool ReturnSlot(uint16_t slot) noexcept;

  alignas(kCacheLineBytes) uint8_t pool_[kFrameSlots][kMaxAuBytes];
  uint32_t slot_gen_[kFrameSlots] = {};
  uint32_t slot_len_[kFrameSlots] = {};
  uint16_t slot_refs_[kFrameSlots] = {};
  uint32_t next_gen_ = 1U;

  osp::SpscRingbuffer<FrameDesc, kInputQueueDepth> input_;
  osp::SpscRingbuffer<uint16_t, kEmptySlotQueueDepth> empty_;

  int32_t spare_slot_ = -1;  // Producer-local slot after Abort/kNoSlot.
  uint64_t input_seq_ = 0U;
  uint64_t last_pts_ns_ = 0U;
  // Orphan slots are counted per writer so no counter is shared between the
  // producer and the loop thread (review P1-10). Lock-free 32-bit counters.
  std::atomic<uint32_t> producer_orphan_drops_{0U};
  std::atomic<uint32_t> loop_orphan_drops_{0U};
  std::atomic<uint32_t> fault_drop_{0U};
  std::atomic<bool> stopped_{false};
};

static_assert(alignof(FrameIngress) >= kCacheLineBytes, "FrameIngress must start on a cache line");

}  // namespace rs

#endif  // RS_FRAME_INGRESS_HPP_
