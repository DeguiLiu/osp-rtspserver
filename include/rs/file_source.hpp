// Annex B file source adapter: codec/parameter-set scan and paced AU producer.
// RtspServer does not depend on this class; the demo main owns it.
// SPDX-License-Identifier: MIT
#ifndef RS_FILE_SOURCE_HPP_
#define RS_FILE_SOURCE_HPP_

#include "rs/codec.hpp"
#include "rs/config.hpp"
#include "rs/frame_ingress.hpp"

#include "osp/vocabulary.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <pthread.h>

namespace rs {

enum class FileSourceError : uint8_t {
  kOpenFailed = 0,
  kBadCodec,
  kMissingParams,
  kAlreadyOpen,
  kAlreadyStarted,
  kThreadFailed
};

struct FileSourceInfo {
  TrackParams params;
};

class FileSource final {
 public:
  FileSource() noexcept = default;
  ~FileSource() noexcept { Stop(); }
  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;

  // Opens the file and scans codec/SPS/PPS/VPS. The returned parameter-set
  // pointers remain valid until Stop()/destruction.
  [[nodiscard]] osp::expected<FileSourceInfo, FileSourceError> Open(const char* path, Codec expected) noexcept;

  // Starts the paced producer. `callbacks` must stay valid until Stop().
  [[nodiscard]] osp::expected<void, FileSourceError> Start(FrameIngress& ingress, SourceCallbacks callbacks) noexcept;
  void Stop() noexcept;

 private:
  static void* ThreadEntry(void* self) noexcept;
  void SourceMain() noexcept;
  [[nodiscard]] bool LoadParamSets() noexcept;
  void FillSourceBuffer() noexcept;
  [[nodiscard]] bool ReadNextAu(uint32_t& out_len) noexcept;
  void ConsumeSourceAu(uint32_t len) noexcept;
  void SubmitAu(uint32_t start, uint32_t len, uint64_t pts_ns) noexcept;
  void NotifyDrop() noexcept;
  void NotifyError(uint32_t error) noexcept;

  std::FILE* file_ = nullptr;
  TrackParams params_{};
  uint8_t vps_[kMaxParamSetBytes] = {};
  uint8_t sps_[kMaxParamSetBytes] = {};
  uint8_t pps_[kMaxParamSetBytes] = {};

  FrameIngress* ingress_ = nullptr;
  SourceCallbacks callbacks_{};
  std::atomic<bool> stop_{false};
  pthread_t thread_{};
  bool thread_started_ = false;

  alignas(kCacheLineBytes) uint8_t au_buf_[kMaxAuBytes + 8192U] = {};
  uint32_t au_off_ = 0U;
  uint32_t au_len_ = 0U;
  static_assert(((kMaxAuBytes + 8192U) % kCacheLineBytes) == 0U, "source buffer stride must be cache-line aligned");
  bool au_eof_ = false;
  bool au_resync_ = false;
  uint64_t pts_ns_ = 0U;
};

static_assert(alignof(FileSource) >= kCacheLineBytes, "FileSource must start on a cache line");

}  // namespace rs

#endif  // RS_FILE_SOURCE_HPP_
