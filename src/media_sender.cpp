// Per-client media queue/cursor/token-bucket implementation.
// SPDX-License-Identifier: MIT
#include "rs/media_sender.hpp"

#include <cstring>

namespace rs {
namespace {

struct ResetCursorVisitor {
  void operator()(ClientCursor& c) const noexcept { H264Packetizer::ResetCursor(c); }
  void operator()(HevcCursor& c) const noexcept { H265Packetizer::ResetCursor(c); }
};

struct CursorSeqVisitor {
  uint16_t operator()(const ClientCursor& c) const noexcept { return c.seq; }
  uint16_t operator()(const HevcCursor& c) const noexcept { return c.seq; }
};

struct CursorTsVisitor {
  uint32_t operator()(const ClientCursor& c) const noexcept { return c.ts; }
  uint32_t operator()(const HevcCursor& c) const noexcept { return c.ts; }
};

struct StartCursorVisitor {
  uint16_t seq;
  uint32_t ts;
  uint32_t au_len;
  bool inject;
  void operator()(ClientCursor& c) const noexcept {
    c.seq = seq;
    H264Packetizer::StartAu(c, au_len, ts, inject);
    c.seq = seq;
    c.ts = ts;
    c.au_len = au_len;
    c.in_au = true;
  }
  void operator()(HevcCursor& c) const noexcept {
    c.seq = seq;
    H265Packetizer::StartAu(c, au_len, ts, inject);
    c.seq = seq;
    c.ts = ts;
    c.au_len = au_len;
    c.in_au = true;
  }
};

struct NextPacketVisitor {
  const uint8_t* au;
  uint32_t au_len;
  uint32_t ssrc;
  const ParamSets& h264;
  const HevcParamSets& h265;
  osp::expected<RtpPacket, PacketizeError> operator()(ClientCursor& c) const noexcept {
    return H264Packetizer::Next(au, au_len, h264, c, ssrc);
  }
  osp::expected<RtpPacket, PacketizeError> operator()(HevcCursor& c) const noexcept {
    return H265Packetizer::Next(au, au_len, h265, c, ssrc);
  }
};

}  // namespace

void MediaSender::Configure(FrameIngress& ingress, const TrackParams& params) noexcept {
  ingress_ = &ingress;
  codec_ = params.codec;
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
  if (codec_ == Codec::kH265) {
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
  std::visit(ResetCursorVisitor{}, cursor_);
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
  if (!ingress_->AddRef(desc)) {
    return EnqueueResult::kNoRef;
  }
  if (queue_count_ >= kFrameQueueDepth) {
    ingress_->Release(desc);
    DropToAwaitIdr();
    return EnqueueResult::kQueueFull;
  }
  need_idr_ = false;
  const uint32_t tail = (queue_head_ + queue_count_) % kFrameQueueDepth;
  queue_[tail] = desc;
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
  if (!rtp_base_set_) {
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
  if (!has_current_ || ingress_ == nullptr) {
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
  if (!has_current_) {
    return osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kEndOfAu);
  }
  const uint8_t* au = Data();
  return std::visit(NextPacketVisitor{au, current_.length, ssrc, h264_params_, h265_params_}, cursor_);
}

void MediaSender::DropToAwaitIdr() noexcept {
  ReleaseFrames();
  ResetCursorState();
  need_idr_ = true;
}

bool MediaSender::Admit(uint32_t bytes, uint64_t now_ns) noexcept {
  return !client_bucket_.IsConfigured() || client_bucket_.TryConsume(bytes, now_ns);
}

void MediaSender::SetSlowBps(uint32_t bps, uint64_t now_ns) noexcept {
  slow_bps_ = bps;
  slow_mode_ = (bps != 0U);
  const uint32_t rate = (bps != 0U) ? bps : kClientEgressRateBps;
  client_bucket_.Configure(rate, kClientEgressBurstBytes, now_ns);
}

uint16_t MediaSender::CursorSeq() const noexcept {
  return std::visit(CursorSeqVisitor{}, cursor_);
}

uint32_t MediaSender::CursorTs() const noexcept {
  return std::visit(CursorTsVisitor{}, cursor_);
}

void MediaSender::StartCursor(uint32_t au_len, uint32_t ts, bool inject_params, uint16_t next_seq) noexcept {
  std::visit(StartCursorVisitor{next_seq, ts, au_len, inject_params}, cursor_);
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
