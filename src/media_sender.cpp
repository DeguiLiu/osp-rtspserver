// Per-client media queue/cursor/token-bucket implementation.
// SPDX-License-Identifier: MIT
#include "rs/media_sender.hpp"

#include <cstring>

#include <type_traits>

namespace rs {
namespace {

// The variant alternative that carries each codec, as its compile-time
// index. newosp provides this facility as osp::VariantIndex, but it lives
// in osp/bus.hpp, which carries the whole AsyncBus; using it here would
// pull that in for an index lookup. The local copy below is the same
// recursive definition. When newosp moves VariantIndex into
// osp/vocabulary.hpp, replace this with osp::VariantIndex.
namespace detail {

template <typename T, size_t I, typename Variant>
struct CursorIndexImpl;

template <typename T, size_t I>
struct CursorIndexImpl<T, I, std::variant<>> {
  static constexpr size_t value = static_cast<size_t>(-1);
};

template <typename T, size_t I, typename First, typename... Rest>
struct CursorIndexImpl<T, I, std::variant<First, Rest...>> {
  static constexpr size_t value =
      std::is_same<T, First>::value ? I : CursorIndexImpl<T, I + 1, std::variant<Rest...>>::value;
};

}  // namespace detail

template <typename T, typename Variant>
struct CursorIndex;

template <typename T, typename... Types>
struct CursorIndex<T, std::variant<Types...>> {
  static constexpr size_t value = detail::CursorIndexImpl<T, 0, std::variant<Types...>>::value;
};

constexpr size_t kH264CursorIndex = CursorIndex<ClientCursor, CodecCursor>::value;
constexpr size_t kH265CursorIndex = CursorIndex<HevcCursor, CodecCursor>::value;
static_assert(kH264CursorIndex != static_cast<size_t>(-1), "ClientCursor must be a variant alternative");
static_assert(kH265CursorIndex != static_cast<size_t>(-1), "HevcCursor must be a variant alternative");
static_assert(kH264CursorIndex != kH265CursorIndex, "the codec cursors must be distinct alternatives");

// The codec enumerators and the variant alternatives must agree, so that a
// codec value can select an alternative without a runtime search. The
// variant order follows the codec order.
static_assert(static_cast<uint8_t>(Codec::kH264) == kH264CursorIndex, "Codec::kH264 must select the H.264 cursor");
static_assert(static_cast<uint8_t>(Codec::kH265) == kH265CursorIndex, "Codec::kH265 must select the H.265 cursor");

// ---------------------------------------------------------------------------
// Common cursor logic. These are plain (non-template) functions over the
// shared PacketizeCursor base, so the behaviour exists once instead of once
// per codec. Only the packetizer call differs between H.264 and H.265, and
// that difference is confined to the thin lambdas further down.
// ---------------------------------------------------------------------------

// Starts a new access unit. The codec's StartAu already resets the AU walk
// (nal_offset, frag_offset, au_len, ts, in_au, marker_sent) and stages the
// parameter sets; the sequence number is the only shared field it does not
// own, because it belongs to the transport rather than to the codec.
void StartCommonFields(PacketizeCursor& cursor, uint16_t seq) noexcept {
  cursor.seq = seq;
}

}  // namespace

void MediaSender::Configure(FrameIngress& ingress, const TrackParams& params) noexcept {
  ingress_ = &ingress;
  h264_params_ = ParamSets{};
  h264_params_.sps = params.sps;
  h264_params_.sps_len = params.sps_len;
  h264_params_.pps = params.pps;
  h264_params_.pps_len = params.pps_len;
  h265_params_ = HevcParamSets{};
  h265_params_.vps = params.vps;
  h265_params_.vps_len = params.vps_len;
  h265_params_.sps = params.sps;
  h265_params_.sps_len = params.sps_len;
  h265_params_.pps = params.pps;
  h265_params_.pps_len = params.pps_len;
  // Select the alternative that matches the codec. The static_asserts above
  // fix the codec-to-alternative mapping at compile time, so this only has to
  // pick between the two known alternatives.
  if (params.codec == Codec::kH265) {
    cursor_.emplace<HevcCursor>();
  } else {
    cursor_.emplace<ClientCursor>();
  }
  Reset();
}

void MediaSender::ReleaseFrames() noexcept {
  if (ingress_ != nullptr) {
    if (has_current_) {
      ingress_->Release(current_);
    }
    for (uint32_t i = 0U; i < queue_count_; ++i) {
      ingress_->Release(queue_[(queue_head_ + i) % kFrameQueueDepth]);
    }
  }
  has_current_ = false;
  current_ns_ = 0U;
  queue_count_ = 0U;
  queue_head_ = 0U;
}

void MediaSender::ResetCursorState() noexcept {
  // The codec's ResetCursor already resets every shared field plus its own
  // parameter-set staging, so the lambda only picks the packetizer.
  std::visit(
      [](auto& cursor) {
        using Cursor = std::decay_t<decltype(cursor)>;
        if constexpr (std::is_same_v<Cursor, ClientCursor>) {
          H264Packetizer::ResetCursor(cursor);
        } else {
          H265Packetizer::ResetCursor(cursor);
        }
      },
      cursor_);
}

void MediaSender::Reset() noexcept {
  ReleaseFrames();
  ResetCursorState();
  need_idr_ = true;
  rtp_base_set_ = false;
  slow_bps_ = 0U;
  slow_mode_ = false;
}

void MediaSender::OnInputGap() noexcept {
  ReleaseFrames();
  ResetCursorState();
  need_idr_ = true;
}

MediaSender::EnqueueResult MediaSender::OnFrame(const FrameDesc& desc, uint64_t now_ns) noexcept {
  if (ingress_ == nullptr) {
    return EnqueueResult::kNoRef;
  }
  if (ingress_->AddRef(desc) == false) {
    return EnqueueResult::kNoRef;
  }
  if (queue_count_ >= kFrameQueueDepth) {
    ingress_->Release(desc);
    DropToAwaitIdr();
    return EnqueueResult::kQueueFull;
  }
  // The first IDR after PLAY (or after a gap) is where a decoder (re)starts:
  // send the stored parameter sets in band even when the encoder does not
  // repeat them in this AU. The flag is forced on the per-client queue entry,
  // never on the shared frame, so other clients are unaffected.
  const bool is_idr = (desc.flags & kFrameIdr) != 0U;
  const bool inject_params = need_idr_ && is_idr;
  if (is_idr) {
    need_idr_ = false;
  }
  const uint32_t tail = (queue_head_ + queue_count_) % kFrameQueueDepth;
  queue_[tail] = desc;
  if (inject_params) {
    queue_[tail].flags = static_cast<uint16_t>(queue_[tail].flags | kFrameHasParams);
  }
  queue_ns_[tail] = now_ns;
  ++queue_count_;
  return EnqueueResult::kEnqueued;
}

uint32_t MediaSender::RtpTs(uint64_t pts_ns) const noexcept {
  const uint64_t delta = pts_ns - rtp_base_pts_ns_;
  const uint64_t whole = (delta / 1000000000ULL) * 90000ULL;
  const uint64_t frac = ((delta % 1000000000ULL) * 90000ULL) / 1000000000ULL;
  return rtp_base_ts_ + static_cast<uint32_t>(whole + frac);
}

bool MediaSender::TakeNext(uint64_t now_ns, uint16_t next_seq, uint32_t ssrc) noexcept {
  static_cast<void>(now_ns);
  if (queue_count_ == 0U) {
    return false;
  }
  current_ = queue_[queue_head_];
  current_ns_ = queue_ns_[queue_head_];
  queue_head_ = (queue_head_ + 1U) % kFrameQueueDepth;
  --queue_count_;
  has_current_ = true;
  if (rtp_base_set_ == false) {
    rtp_base_pts_ns_ = current_.pts_ns;
    rtp_base_ts_ = ssrc ^ 0x5A5A0000U;
    rtp_base_set_ = true;
  }
  const uint32_t ts = RtpTs(current_.pts_ns);
  const bool inject = (current_.flags & kFrameHasParams) != 0U;
  StartCursor(current_.length, ts, inject, next_seq);
  return true;
}

const uint8_t* MediaSender::Data() const noexcept {
  if (has_current_ == false || ingress_ == nullptr) {
    return nullptr;
  }
  return ingress_->Data(current_);
}

void MediaSender::ReleaseCurrent() noexcept {
  if (has_current_ && ingress_ != nullptr) {
    ingress_->Release(current_);
  }
  has_current_ = false;
  current_ns_ = 0U;
}

osp::expected<RtpPacket, PacketizeError> MediaSender::NextPacket(uint32_t ssrc) noexcept {
  if (has_current_ == false) {
    return osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kEndOfAu);
  }
  const uint8_t* au = Data();
  // Thin lambda: the only per-codec part is which packetizer walks the AU
  // and which parameter set it reads.
  return std::visit(
      [&](auto& cursor) {
        using Cursor = std::decay_t<decltype(cursor)>;
        if constexpr (std::is_same_v<Cursor, ClientCursor>) {
          return H264Packetizer::Next(au, current_.length, h264_params_, cursor, ssrc);
        } else {
          return H265Packetizer::Next(au, current_.length, h265_params_, cursor, ssrc);
        }
      },
      cursor_);
}

void MediaSender::DropToAwaitIdr() noexcept {
  ReleaseFrames();
  ResetCursorState();
  need_idr_ = true;
}

bool MediaSender::Admit(uint32_t bytes, uint64_t now_ns) noexcept {
  return client_bucket_.IsConfigured() == false || client_bucket_.TryConsume(bytes, now_ns);
}

void MediaSender::SetSlowBps(uint32_t bps, uint64_t now_ns) noexcept {
  slow_bps_ = bps;
  slow_mode_ = (bps != 0U);
  const uint32_t rate = (bps != 0U) ? bps : kClientEgressRateBps;
  client_bucket_.Configure(rate, kClientEgressBurstBytes, now_ns);
}

uint16_t MediaSender::CursorSeq() const noexcept {
  // Fully common: the sequence number lives in the shared base.
  return std::visit([](const auto& cursor) { return cursor.seq; }, cursor_);
}

uint32_t MediaSender::CursorTs() const noexcept {
  // Fully common: the timestamp lives in the shared base.
  return std::visit([](const auto& cursor) { return cursor.ts; }, cursor_);
}

void MediaSender::StartCursor(uint32_t au_len, uint32_t ts, bool inject_params, uint16_t next_seq) noexcept {
  // Common: the shared progress fields. Thin lambda: the codec's own
  // parameter-set staging is the only per-codec part.
  std::visit(
      [&](auto& cursor) {
        using Cursor = std::decay_t<decltype(cursor)>;
        StartCommonFields(cursor, next_seq);
        if constexpr (std::is_same_v<Cursor, ClientCursor>) {
          H264Packetizer::StartAu(cursor, au_len, ts, inject_params);
        } else {
          H265Packetizer::StartAu(cursor, au_len, ts, inject_params);
        }
      },
      cursor_);
}

MediaSender::Snapshot MediaSender::GetSnapshot(uint64_t now_ns) const noexcept {
  Snapshot out;
  out.queue_depth = queue_count_ + (has_current_ ? 1U : 0U);
  out.queue_bytes = has_current_ ? current_.length : 0U;
  for (uint32_t i = 0U; i < queue_count_; ++i) {
    out.queue_bytes += queue_[(queue_head_ + i) % kFrameQueueDepth].length;
  }
  uint64_t oldest_ns = 0U;
  if (has_current_ && current_ns_ != 0U) {
    oldest_ns = current_ns_;
  }
  if (queue_count_ != 0U) {
    const uint64_t head_ns = queue_ns_[queue_head_];
    if (head_ns != 0U && (oldest_ns == 0U || head_ns < oldest_ns)) {
      oldest_ns = head_ns;
    }
  }
  if (oldest_ns != 0U && now_ns > oldest_ns) {
    out.oldest_age_ms = static_cast<uint32_t>((now_ns - oldest_ns) / 1000000ULL);
  }
  out.seq = CursorSeq();
  out.rtp_ts = CursorTs();
  out.slow_bps = slow_bps_;
  out.has_current = has_current_;
  out.need_idr = need_idr_;
  return out;
}

}  // namespace rs
