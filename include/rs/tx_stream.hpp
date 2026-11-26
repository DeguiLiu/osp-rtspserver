// Per-connection serial outbound byte stream (control responses + TCP media).
// The stream is bounded and never allocates; short writes leave the head
// offset untouched so a partially written interleaved frame cannot be split.
// SPDX-License-Identifier: MIT
#ifndef RS_TX_STREAM_HPP_
#define RS_TX_STREAM_HPP_

#include "osp/span.hpp"
#include "rs/config.hpp"

#include <cstdint>
#include <cstring>

namespace rs {

class TxStream final {
 public:
  static constexpr uint32_t kCapacity = kRtspTxBytes;
  static_assert((kCapacity % kCacheLineBytes) == 0U, "TxStream capacity must be cache-line aligned");

  void Reset() noexcept {
    len_ = 0U;
    off_ = 0U;
    progress_ns_ = 0U;
  }

  [[nodiscard]] uint32_t Unflushed() const noexcept { return len_ - off_; }
  [[nodiscard]] bool Empty() const noexcept { return len_ == off_; }
  [[nodiscard]] bool WantsWritable() const noexcept { return Unflushed() > 0U; }
  [[nodiscard]] uint64_t ProgressNs() const noexcept { return progress_ns_; }

  void MarkProgressIfEmpty(uint64_t now_ns) noexcept {
    if (Empty()) {
      progress_ns_ = now_ns;
    }
  }

  [[nodiscard]] bool Reserve(uint32_t bytes) noexcept {
    if ((kCapacity - len_) < bytes && off_ > 0U) {
      Compact();
    }
    return (kCapacity - len_) >= bytes;
  }

  [[nodiscard]] osp::writable_byte_span Tail() noexcept {
    return osp::writable_byte_span(reinterpret_cast<std::byte*>(buf_ + len_), kCapacity - len_);
  }

  [[nodiscard]] bool Commit(uint32_t bytes) noexcept {
    if (bytes > (kCapacity - len_)) {
      return false;
    }
    len_ += bytes;
    return true;
  }

  [[nodiscard]] osp::byte_span Pending() const noexcept {
    return osp::byte_span(reinterpret_cast<const std::byte*>(buf_ + off_), Unflushed());
  }

  void Consume(uint32_t bytes, uint64_t now_ns) noexcept {
    const uint32_t pending = Unflushed();
    const uint32_t take = (bytes < pending) ? bytes : pending;
    off_ += take;
    progress_ns_ = now_ns;
    if (off_ == len_) {
      off_ = 0U;
      len_ = 0U;
    }
  }

 private:
  void Compact() noexcept {
    if (off_ == 0U) {
      return;
    }
    const uint32_t remain = Unflushed();
    if (remain > 0U) {
      std::memmove(buf_, buf_ + off_, remain);
    }
    off_ = 0U;
    len_ = remain;
  }

  alignas(kCacheLineBytes) uint8_t buf_[kCapacity] = {};
  uint32_t len_ = 0U;
  uint32_t off_ = 0U;
  uint64_t progress_ns_ = 0U;
};

static_assert(alignof(TxStream) >= kCacheLineBytes, "TxStream must start on a cache line");

}  // namespace rs

#endif  // RS_TX_STREAM_HPP_
