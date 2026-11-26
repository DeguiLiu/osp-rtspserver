// Annex B file source adapter (demo layer). No dependency on RtspServer.
// SPDX-License-Identifier: MIT
#include "rs/file_source.hpp"

#include "rs/h265_packetizer.hpp"

#include <cerrno>
#include <cstring>

#include <time.h>

namespace rs {
namespace {

constexpr uint32_t kNotFound = 0xFFFFFFFFU;
constexpr uint32_t kParamScanLimit = 1U << 20U;
constexpr uint32_t kSourceChunk = 8192U;
constexpr uint32_t kSourceBufBytes = kMaxAuBytes + kSourceChunk;
constexpr uint32_t kSourcePollNs = 2000000U;
constexpr uint64_t kFramePeriodNs = 1000000000ULL / kFps;

uint32_t FindNalStart(const uint8_t* p, uint32_t len, uint32_t from, uint8_t nal_type) noexcept {
  if (len < 4U) {
    return kNotFound;
  }
  for (uint32_t i = from; i + 3U < len; ++i) {
    if (p[i] == 0U && p[i + 1U] == 0U && p[i + 2U] == 1U && (p[i + 3U] & 0x1FU) == nal_type) {
      return i;
    }
  }
  return kNotFound;
}

bool HevcHeaderValid(uint8_t b0, uint8_t b1) noexcept {
  const uint8_t layer = static_cast<uint8_t>(((b0 & 0x01U) << 5U) | (b1 >> 3U));
  return (b0 & 0x80U) == 0U && layer == 0U && (b1 & 0x07U) != 0U;
}

uint8_t HevcType(uint8_t b0) noexcept {
  return static_cast<uint8_t>((b0 >> 1U) & 0x3FU);
}

uint32_t FindStartCodePos(const uint8_t* p, uint32_t len, uint32_t from) noexcept {
  for (uint32_t i = from; i + 3U <= len; ++i) {
    if (p[i] == 0U && p[i + 1U] == 0U && p[i + 2U] == 1U) {
      return i;
    }
  }
  return kNotFound;
}

uint32_t ParamSetBit(Codec codec, const uint8_t* nal, uint32_t len) noexcept {
  uint32_t bit = 0U;
  if (codec == Codec::kH265) {
    if (len >= 2U && HevcHeaderValid(nal[0], nal[1])) {
      const uint8_t type = HevcType(nal[0]);
      bit = (type == 32U) ? 1U : ((type == 33U) ? 2U : ((type == 34U) ? 4U : 0U));
    }
  } else if (len >= 1U) {
    const uint8_t type = static_cast<uint8_t>(nal[0] & 0x1FU);
    bit = (type == 7U) ? 2U : ((type == 8U) ? 4U : 0U);
  }
  return bit;
}

uint32_t FindHevcNalStart(const uint8_t* p, uint32_t len, uint32_t from, uint8_t type) noexcept {
  for (uint32_t i = from; i + 5U <= len; ++i) {
    if (p[i] == 0U && p[i + 1U] == 0U && p[i + 2U] == 1U && HevcHeaderValid(p[i + 3U], p[i + 4U]) &&
        HevcType(p[i + 3U]) == type) {
      return i;
    }
  }
  return kNotFound;
}

uint32_t FindHevcParamStart(const uint8_t* p, uint32_t len, uint32_t from) noexcept {
  for (uint32_t i = from; i + 5U <= len; ++i) {
    if (p[i] == 0U && p[i + 1U] == 0U && p[i + 2U] == 1U && HevcHeaderValid(p[i + 3U], p[i + 4U])) {
      const uint8_t type = HevcType(p[i + 3U]);
      if (type == 32U || type == 33U || type == 34U) {
        return i;
      }
    }
  }
  return kNotFound;
}

uint16_t ScanAuFlagsH265(const uint8_t* p, uint32_t len) noexcept {
  bool vps = false;
  bool sps = false;
  bool pps = false;
  uint32_t offset = 0U;
  NalUnit nal;
  while (NextHevcNalUnit(p, len, offset, nal)) {
    vps = vps || (nal.type == 32U);
    sps = sps || (nal.type == 33U);
    pps = pps || (nal.type == 34U);
  }
  uint16_t flags = 0U;
  if (HevcAuHasIrap(p, len)) {
    flags = static_cast<uint16_t>(flags | kFrameIdr);
  }
  if (vps && sps && pps) {
    flags = static_cast<uint16_t>(flags | kFrameHasParams);
  }
  return flags;
}

uint16_t ScanAuFlagsH264(const uint8_t* p, uint32_t len) noexcept {
  bool idr = false;
  bool sps = false;
  bool pps = false;
  uint32_t i = 0U;
  while (i + 3U < len) {
    if (p[i] == 0U && p[i + 1U] == 0U && p[i + 2U] == 1U) {
      const uint8_t type = static_cast<uint8_t>(p[i + 3U] & 0x1FU);
      idr = idr || (type == 5U);
      sps = sps || (type == 7U);
      pps = pps || (type == 8U);
      i += 3U;
    } else {
      ++i;
    }
  }
  uint16_t flags = 0U;
  if (idr) {
    flags = static_cast<uint16_t>(flags | kFrameIdr);
  }
  if (sps && pps) {
    flags = static_cast<uint16_t>(flags | kFrameHasParams);
  }
  return flags;
}

uint16_t ScanAuFlags(const uint8_t* p, uint32_t len, Codec codec) noexcept {
  return (codec == Codec::kH265) ? ScanAuFlagsH265(p, len) : ScanAuFlagsH264(p, len);
}

}  // namespace

osp::expected<FileSourceInfo, FileSourceError> FileSource::Open(const char* path, Codec expected) noexcept {
  if (file_ != nullptr) {
    return osp::expected<FileSourceInfo, FileSourceError>::error(FileSourceError::kAlreadyOpen);
  }
  if (path == nullptr) {
    return osp::expected<FileSourceInfo, FileSourceError>::error(FileSourceError::kOpenFailed);
  }
  file_ = std::fopen(path, "rb");
  if (file_ == nullptr) {
    return osp::expected<FileSourceInfo, FileSourceError>::error(FileSourceError::kOpenFailed);
  }

  Codec detected = Codec::kUnknown;
  const size_t probe_len = std::fread(au_buf_, 1U, 65536U, file_);
  if (probe_len < 4U || DetectCodec(au_buf_, static_cast<uint32_t>(probe_len), detected) == false ||
      detected == Codec::kUnknown || (expected != Codec::kUnknown && expected != detected)) {
    std::fclose(file_);
    file_ = nullptr;
    return osp::expected<FileSourceInfo, FileSourceError>::error(FileSourceError::kBadCodec);
  }
  if (std::fseek(file_, 0, SEEK_SET) != 0) {
    std::fclose(file_);
    file_ = nullptr;
    return osp::expected<FileSourceInfo, FileSourceError>::error(FileSourceError::kOpenFailed);
  }

  params_ = TrackParams{};
  params_.codec = detected;
  if (LoadParamSets() == false || ValidateParamSets(params_) == false) {
    std::fclose(file_);
    file_ = nullptr;
    return osp::expected<FileSourceInfo, FileSourceError>::error(FileSourceError::kMissingParams);
  }
  if (std::fseek(file_, 0, SEEK_SET) != 0) {
    std::fclose(file_);
    file_ = nullptr;
    return osp::expected<FileSourceInfo, FileSourceError>::error(FileSourceError::kOpenFailed);
  }
  au_off_ = 0U;
  au_len_ = 0U;
  au_eof_ = false;
  au_resync_ = false;
  pts_ns_ = 0U;
  FileSourceInfo info;
  info.params = params_;
  return osp::expected<FileSourceInfo, FileSourceError>::success(info);
}

bool FileSource::LoadParamSets() noexcept {
  const Codec codec = params_.codec;
  const bool h265 = (codec == Codec::kH265);
  const uint32_t need = h265 ? 0x7U : 0x6U;
  uint32_t got = 0U;
  uint8_t* const chunk = au_buf_;
  constexpr uint32_t kScanBufferBytes = 16384U;
  static_assert(kScanBufferBytes <= (kMaxAuBytes + 8192U), "scan buffer must fit in source storage");
  uint32_t carry = 0U;
  uint64_t scanned = 0U;
  bool eof = false;
  bool ok = true;
  while (ok && eof == false && got != need && scanned < kParamScanLimit) {
    const size_t n = std::fread(chunk + carry, 1U, kScanBufferBytes - carry, file_);
    eof = (n == 0U);
    const uint32_t len = carry + static_cast<uint32_t>(n);
    uint32_t i = 0U;
    while (ok && i + 4U <= len) {
      if (chunk[i] != 0U || chunk[i + 1U] != 0U || chunk[i + 2U] != 1U) {
        ++i;
        continue;
      }
      const uint32_t ns = i + 3U;
      if (ns >= len || (h265 && ns + 1U >= len)) {
        break;
      }
      const uint32_t bit = ParamSetBit(codec, chunk + ns, len - ns);
      uint32_t next = FindStartCodePos(chunk, len, ns + (h265 ? 2U : 1U));
      if (next == kNotFound) {
        if (eof == false) {
          if (bit != 0U && (len - ns) > kMaxParamSetBytes) {
            ok = false;
          } else if (bit == 0U) {
            i = (len > 3U) ? (len - 3U) : len;
          }
          break;
        }
        next = len;
      }
      uint32_t end = next;
      while (end > ns && chunk[end - 1U] == 0U) {
        --end;
      }
      const uint32_t nal_len = end - ns;
      if (bit != 0U && (got & bit) == 0U) {
        if (nal_len == 0U || nal_len > kMaxParamSetBytes) {
          ok = false;
          break;
        }
        if (bit == 1U) {
          std::memcpy(vps_, chunk + ns, nal_len);
          params_.vps = vps_;
          params_.vps_len = nal_len;
        } else if (bit == 2U) {
          std::memcpy(sps_, chunk + ns, nal_len);
          params_.sps = sps_;
          params_.sps_len = nal_len;
        } else {
          std::memcpy(pps_, chunk + ns, nal_len);
          params_.pps = pps_;
          params_.pps_len = nal_len;
        }
        got |= bit;
      }
      i = (next == len) ? len : next;
    }
    if (eof) {
      break;
    }
    carry = len - i;
    if (carry > 0U) {
      std::memmove(chunk, chunk + i, carry);
    }
    scanned += n;
  }
  return ok && got == need;
}

osp::expected<void, FileSourceError> FileSource::Start(FrameIngress& ingress, SourceCallbacks callbacks,
                                                       const osp::ThreadOptions& options) noexcept {
  if (file_ == nullptr) {
    return osp::expected<void, FileSourceError>::error(FileSourceError::kOpenFailed);
  }
  if (thread_.joinable()) {
    return osp::expected<void, FileSourceError>::error(FileSourceError::kAlreadyStarted);
  }
  ingress_ = &ingress;
  callbacks_ = callbacks;
  stop_.store(false, std::memory_order_release);
  osp::ThreadOptions thread_options = options;
  thread_options.name = "rtsp-source";
  if (false == thread_.Start(thread_options, [this]() noexcept { SourceMain(); })) {
    ingress_ = nullptr;
    callbacks_ = SourceCallbacks{};
    return osp::expected<void, FileSourceError>::error(FileSourceError::kThreadFailed);
  }
  return osp::expected<void, FileSourceError>::success();
}

void FileSource::Stop() noexcept {
  stop_.store(true, std::memory_order_release);
  thread_.join();
  if (file_ != nullptr) {
    static_cast<void>(std::fclose(file_));
    file_ = nullptr;
  }
  ingress_ = nullptr;
  callbacks_ = SourceCallbacks{};
}

void FileSource::NotifyDrop() noexcept {
  if (callbacks_.drop != nullptr) {
    callbacks_.drop(callbacks_.ctx);
  }
}

void FileSource::NotifyError(uint32_t error) noexcept {
  if (callbacks_.error != nullptr) {
    callbacks_.error(callbacks_.ctx, error);
  }
}

void FileSource::FillSourceBuffer() noexcept {
  // Compact only when the tail is nearly full; normal 20 KiB AUs advance the
  // read offset without moving the remaining bytes every frame.
  if (au_off_ != 0U && (kSourceBufBytes - au_len_) < kSourceChunk) {
    const uint32_t remain = au_len_ - au_off_;
    if (remain > 0U) {
      std::memmove(au_buf_, au_buf_ + au_off_, remain);
    }
    au_len_ = remain;
    au_off_ = 0U;
  }
  const uint32_t space = kSourceBufBytes - au_len_;
  if (space == 0U || au_eof_) {
    au_eof_ = true;
    return;
  }
  const uint32_t want = (space < kSourceChunk) ? space : kSourceChunk;
  const size_t n = std::fread(au_buf_ + au_len_, 1, want, file_);
  if (n == 0U) {
    if (std::ferror(file_) != 0) {
      NotifyError(static_cast<uint32_t>(errno));
    }
    au_eof_ = true;
    return;
  }
  au_len_ += static_cast<uint32_t>(n);
}

bool FileSource::ReadNextAu(uint32_t& out_len) noexcept {
  const bool h265 = (params_.codec == Codec::kH265);
  for (;;) {
    const uint32_t available = au_len_ - au_off_;
    if (available > 4U) {
      const uint32_t from = au_off_ + 4U;
      uint32_t boundary =
          h265 ? FindHevcNalStart(au_buf_, au_len_, from, 35U) : FindNalStart(au_buf_, au_len_, from, 9U);
      if (boundary == kNotFound && (au_eof_ || au_len_ >= kSourceBufBytes)) {
        boundary = h265 ? FindHevcParamStart(au_buf_, au_len_, from) : FindNalStart(au_buf_, au_len_, from, 7U);
      }
      if (boundary != kNotFound) {
        if (au_resync_) {
          au_off_ = boundary;
          au_resync_ = false;
          continue;
        }
        out_len = boundary - au_off_;
        return true;
      }
      if (available > kMaxAuBytes) {
        NotifyDrop();
        au_off_ = 0U;
        au_len_ = 0U;
        au_resync_ = true;
      }
    }
    if (au_eof_) {
      if (available == 0U) {
        return false;
      }
      out_len = available;
      return true;
    }
    FillSourceBuffer();
  }
}

void FileSource::ConsumeSourceAu(uint32_t len) noexcept {
  const uint32_t remaining = au_len_ - au_off_;
  if (len >= remaining) {
    au_off_ = 0U;
    au_len_ = 0U;
    return;
  }
  au_off_ += len;
}

void FileSource::SubmitAu(uint32_t start, uint32_t len, uint64_t pts_ns) noexcept {
  if (ingress_ == nullptr) {
    return;
  }
  auto acquired = ingress_->Acquire(len);
  if (acquired.has_value() == false) {
    NotifyDrop();
    return;
  }
  if (ingress_->ConsumeFaultDrop()) {
    ingress_->Abort(acquired.value());
    NotifyDrop();
    return;
  }
  FrameIngress::Writer writer = acquired.value();
  std::memcpy(writer.au, au_buf_ + start, len);
  writer.desc.pts_ns = pts_ns;
  writer.desc.flags = ScanAuFlags(au_buf_ + start, len, params_.codec);
  auto committed = ingress_->Commit(writer, len);
  if (committed.has_value() == false) {
    ingress_->Abort(writer);
    NotifyDrop();
    return;
  }
  if (callbacks_.wake != nullptr) {
    callbacks_.wake(callbacks_.ctx);
  }
}

void FileSource::SourceMain() noexcept {
  uint64_t next_ns = osp::SteadyNowNs();
  while (stop_.load(std::memory_order_acquire) == false) {
    uint32_t len = 0U;
    if (ReadNextAu(len) == false) {
      if (std::fseek(file_, 0, SEEK_SET) != 0) {
        NotifyError(static_cast<uint32_t>(errno));
        break;
      }
      au_eof_ = false;
      au_off_ = 0U;
      au_len_ = 0U;
      au_resync_ = false;
      if (ReadNextAu(len) == false) {
        break;
      }
    }
    SubmitAu(au_off_, len, pts_ns_);
    ConsumeSourceAu(len);
    pts_ns_ += kFramePeriodNs;

    next_ns += kFramePeriodNs;
    const uint64_t now = osp::SteadyNowNs();
    if (now > next_ns + 1000000000ULL) {
      next_ns = now;
    }
    while (stop_.load(std::memory_order_acquire) == false && osp::SteadyNowNs() < next_ns) {
      const struct timespec pause{0, static_cast<long>(kSourcePollNs)};
      static_cast<void>(nanosleep(&pause, nullptr));
    }
  }
}

}  // namespace rs
