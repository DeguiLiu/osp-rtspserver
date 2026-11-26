// Per-client media state: bounded AU queue, packetization cursor, RTP time
// base and egress token bucket. RtspSession owns the session/protocol facade
// and calls this class from the loop thread only.
// SPDX-License-Identifier: MIT
#ifndef RS_MEDIA_SENDER_HPP_
#define RS_MEDIA_SENDER_HPP_

#include "osp/vocabulary.hpp"
#include "rs/codec.hpp"
#include "rs/config.hpp"
#include "rs/frame_ingress.hpp"
#include "rs/h264_packetizer.hpp"
#include "rs/h265_packetizer.hpp"
#include "rs/rtp_transport.hpp"

#include <cstdint>

#include <variant>

namespace rs {

class MediaSender final {
 public:
  static constexpr uint32_t kFrameQueueDepth = 4U;

  enum class EnqueueResult : uint8_t { kEnqueued = 0, kQueueFull, kNoRef };

  struct Snapshot {
    uint32_t queue_depth = 0U;
    uint32_t queue_bytes = 0U;
    uint32_t oldest_age_ms = 0U;
    uint16_t seq = 0U;
    uint32_t rtp_ts = 0U;
    uint32_t slow_bps = 0U;
    bool has_current = false;
    bool need_idr = true;
  };

  void Configure(FrameIngress& ingress, const TrackParams& params) noexcept;
  void Reset() noexcept;
  void RequireIdr() noexcept { need_idr_ = true; }
  [[nodiscard]] bool NeedIdr() const noexcept { return need_idr_; }

  // Returns whether the AU was queued. Queue-full drops media and enters
  // AwaitIDR; the caller owns counter updates.
  [[nodiscard]] EnqueueResult OnFrame(const FrameDesc& desc, uint64_t now_ns) noexcept;
  void OnInputGap() noexcept;

  [[nodiscard]] bool TakeNext(uint64_t now_ns, uint16_t next_seq, uint32_t ssrc) noexcept;
  [[nodiscard]] bool HasCurrent() const noexcept { return has_current_; }
  [[nodiscard]] const FrameDesc& Current() const noexcept { return current_; }
  [[nodiscard]] const uint8_t* Data() const noexcept;
  void ReleaseCurrent() noexcept;
  void FinishCurrent() noexcept { ReleaseCurrent(); }
  [[nodiscard]] osp::expected<RtpPacket, PacketizeError> NextPacket(uint32_t ssrc) noexcept;
  void DropToAwaitIdr() noexcept;

  [[nodiscard]] bool Admit(uint32_t bytes, uint64_t now_ns) noexcept;
  void SetSlowBps(uint32_t bps, uint64_t now_ns) noexcept;
  [[nodiscard]] bool SlowMode() const noexcept { return slow_mode_; }

  [[nodiscard]] uint16_t CursorSeq() const noexcept;
  [[nodiscard]] uint32_t CursorTs() const noexcept;
  [[nodiscard]] Snapshot GetSnapshot(uint64_t now_ns) const noexcept;

 private:
  void ReleaseFrames() noexcept;
  void ResetCursorState() noexcept;
  void StartCursor(uint32_t au_len, uint32_t ts, bool inject_params, uint16_t next_seq) noexcept;
  [[nodiscard]] uint32_t RtpTs(uint64_t pts_ns) const noexcept;

  FrameIngress* ingress_ = nullptr;
  Codec codec_ = Codec::kUnknown;
  ParamSets h264_params_{};
  HevcParamSets h265_params_{};
  TokenBucket client_bucket_;
  bool slow_mode_ = false;
  uint32_t slow_bps_ = 0U;

  FrameDesc queue_[kFrameQueueDepth] = {};
  uint64_t queue_ns_[kFrameQueueDepth] = {};
  uint32_t queue_head_ = 0U;
  uint32_t queue_count_ = 0U;
  FrameDesc current_ = {};
  uint64_t current_ns_ = 0U;
  bool has_current_ = false;
  bool need_idr_ = true;

  using CodecCursor = std::variant<ClientCursor, HevcCursor>;
  CodecCursor cursor_{};
  uint64_t rtp_base_pts_ns_ = 0U;
  uint32_t rtp_base_ts_ = 0U;
  bool rtp_base_set_ = false;
};

}  // namespace rs

#endif  // RS_MEDIA_SENDER_HPP_
