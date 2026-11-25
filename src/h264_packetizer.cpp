// Annex B scanning and H.264 RTP packetization (design sections 7.2 and 7.3).
// SPDX-License-Identifier: MIT
#include "rs/h264_packetizer.hpp"

namespace rs {
namespace {

constexpr uint8_t kFuAType = 28U;
constexpr uint32_t kFuHeaderBytes = 2U;
constexpr uint32_t kMaxFragment = kRtpPayloadMax - kFuHeaderBytes;
constexpr uint8_t kFuStartMask = 0x80U;
constexpr uint8_t kFuEndMask = 0x40U;
constexpr uint8_t kNalTypeMask = 0x1FU;

// Finds the next 00 00 01 pattern at or after `from`. A four-byte start code
// (00 00 00 01) is matched at its last three bytes, so the NAL still starts
// after the full prefix; the leading zero is trimmed as a trailing zero of the
// previous NAL.
bool FindStartCode(const uint8_t* au, uint32_t from, uint32_t len, uint32_t& pos) noexcept {
  bool found = false;
  if (nullptr != au && from < len) {
    for (uint32_t i = from; !found && (i + 2U) < len; ++i) {
      if (0U == au[i] && 0U == au[i + 1U] && 1U == au[i + 2U]) {
        pos = i;
        found = true;
      }
    }
  }
  return found;
}

bool HasNextNal(const uint8_t* au, uint32_t au_len, uint32_t offset) noexcept {
  NalUnit next;
  uint32_t probe = offset;
  return (offset < au_len) && NextNalUnit(au, au_len, probe, next);
}

bool OnlyZeros(const uint8_t* au, uint32_t from, uint32_t len) noexcept {
  bool zeros = true;
  for (uint32_t i = from; zeros && i < len; ++i) {
    zeros = (0U == au[i]);
  }
  return zeros;
}

void WriteRtpHeader(ClientCursor& cursor, uint32_t ssrc, bool marker) noexcept {
  cursor.rtp_header[0] = kRtpVersionByte;  // V=2, P=0, X=0, CC=0.
  cursor.rtp_header[1] = static_cast<uint8_t>(kRtpPayloadType | (marker ? kRtpMarkerMask : 0x00U));
  cursor.rtp_header[2] = static_cast<uint8_t>(cursor.seq >> 8U);
  cursor.rtp_header[3] = static_cast<uint8_t>(cursor.seq & 0x00FFU);
  cursor.rtp_header[4] = static_cast<uint8_t>(cursor.ts >> 24U);
  cursor.rtp_header[5] = static_cast<uint8_t>(cursor.ts >> 16U);
  cursor.rtp_header[6] = static_cast<uint8_t>(cursor.ts >> 8U);
  cursor.rtp_header[7] = static_cast<uint8_t>(cursor.ts);
  cursor.rtp_header[8] = static_cast<uint8_t>(ssrc >> 24U);
  cursor.rtp_header[9] = static_cast<uint8_t>(ssrc >> 16U);
  cursor.rtp_header[10] = static_cast<uint8_t>(ssrc >> 8U);
  cursor.rtp_header[11] = static_cast<uint8_t>(ssrc);
  cursor.seq = static_cast<uint16_t>(cursor.seq + 1U);
}

}  // namespace

bool NextNalUnit(const uint8_t* au, uint32_t au_len, uint32_t& offset, NalUnit& out) noexcept {
  uint32_t sc = 0U;
  const bool found = (offset <= au_len) && FindStartCode(au, offset, au_len, sc);
  uint32_t nal_start = 0U;
  uint32_t next = au_len;
  bool has_next = false;
  if (found) {
    nal_start = sc + 3U;
    has_next = FindStartCode(au, nal_start, au_len, next);
  }
  uint32_t nal_end = has_next ? next : au_len;
  // Strip trailing zero bytes (they may be the fourth byte of the next start
  // code, or trailing_zero_8bits); they are not part of the NAL payload.
  while (found && nal_end > nal_start && 0U == au[nal_end - 1U]) {
    --nal_end;
  }
  if (found) {
    out.data = osp::byte_span(reinterpret_cast<const std::byte*>(au + nal_start), nal_end - nal_start);
    out.type = (nal_end > nal_start) ? static_cast<uint8_t>(au[nal_start] & kNalTypeMask) : 0U;
    offset = has_next ? next : au_len;
  } else {
    out.data = osp::byte_span();
    out.type = 0U;
    offset = au_len;
  }
  return found;
}

bool AuHasIdr(const uint8_t* au, uint32_t au_len) noexcept {
  uint32_t offset = 0U;
  NalUnit nal;
  bool found = false;
  while (!found && NextNalUnit(au, au_len, offset, nal)) {
    found = (static_cast<uint8_t>(NalType::kIdrSlice) == nal.type);
  }
  return found;
}

void H264Packetizer::ResetCursor(ClientCursor& cursor) noexcept {
  cursor.nal_offset = 0U;
  cursor.frag_offset = 0U;
  cursor.au_len = 0U;
  cursor.param_stage = 2U;
  cursor.await_idr = true;
  cursor.in_au = false;
  cursor.marker_sent = false;
}

void H264Packetizer::StartAu(ClientCursor& cursor, uint32_t au_len, uint32_t ts, bool inject_params) noexcept {
  cursor.nal_offset = 0U;
  cursor.frag_offset = 0U;
  cursor.au_len = au_len;
  cursor.ts = ts;
  cursor.param_stage = inject_params ? 0U : 2U;
  cursor.in_au = true;
  cursor.marker_sent = false;
}

osp::expected<RtpPacket, PacketizeError> H264Packetizer::Next(const uint8_t* au, uint32_t au_len,
                                                              const ParamSets& params, ClientCursor& cursor,
                                                              uint32_t ssrc) noexcept {
  if (!cursor.in_au) {
    return osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kEndOfAu);
  }
  if (cursor.param_stage < 2U) {
    const uint8_t* set = (0U == cursor.param_stage) ? params.sps : params.pps;
    const uint32_t set_len = (0U == cursor.param_stage) ? params.sps_len : params.pps_len;
    if (nullptr == set || 0U == set_len) {
      cursor.in_au = false;
      return osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kNotConfigured);
    }
    if (set_len > kRtpPayloadMax) {
      cursor.in_au = false;
      return osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kTooLarge);
    }
    WriteRtpHeader(cursor, ssrc, false);
    ++cursor.param_stage;
    RtpPacket packet;
    packet.header = cursor.rtp_header.data();
    packet.header_len = kRtpHeaderBytes;
    packet.payload = set;
    packet.payload_len = set_len;
    packet.marker = false;
    return osp::expected<RtpPacket, PacketizeError>::success(packet);
  }
  if (nullptr == au || cursor.au_len > au_len || cursor.au_len > kMaxAuBytes) {
    cursor.in_au = false;
    const bool too_large = (nullptr != au) && (cursor.au_len <= au_len);
    return osp::expected<RtpPacket, PacketizeError>::error(too_large ? PacketizeError::kTooLarge
                                                                    : PacketizeError::kBadNal);
  }
  uint32_t next_offset = cursor.nal_offset;
  NalUnit nal;
  const bool have_nal = NextNalUnit(au, cursor.au_len, next_offset, nal);
  if (!have_nal) {
    cursor.in_au = false;
    return osp::expected<RtpPacket, PacketizeError>::error(
        OnlyZeros(au, cursor.nal_offset, cursor.au_len) ? PacketizeError::kEndOfAu : PacketizeError::kBadNal);
  }
  if (nal.data.empty() || 0U == nal.type) {
    cursor.in_au = false;
    return osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kBadNal);
  }
  const uint32_t nal_len = static_cast<uint32_t>(nal.data.size());
  if (0U == cursor.frag_offset && nal_len <= kRtpPayloadMax) {
    const bool last = !HasNextNal(au, cursor.au_len, next_offset);
    WriteRtpHeader(cursor, ssrc, last);
    RtpPacket packet;
    packet.header = cursor.rtp_header.data();
    packet.header_len = kRtpHeaderBytes;
    packet.payload = reinterpret_cast<const uint8_t*>(nal.data.data());
    packet.payload_len = nal_len;
    packet.marker = last;
    cursor.nal_offset = next_offset;
    cursor.frag_offset = 0U;
    if (last) {
      cursor.in_au = false;
      cursor.marker_sent = true;
    }
    return osp::expected<RtpPacket, PacketizeError>::success(packet);
  }
  // FU-A: the FU indicator/header is staged in the cursor, the fragment bytes
  // stay in the shared AU (no copy).
  const uint32_t start = (0U == cursor.frag_offset) ? 1U : cursor.frag_offset;
  if (start >= nal_len) {
    cursor.in_au = false;
    return osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kBadNal);
  }
  uint32_t chunk = nal_len - start;
  if (chunk > kMaxFragment) {
    chunk = kMaxFragment;
  }
  const bool first = (0U == cursor.frag_offset);
  const uint32_t frag_end = start + chunk;
  const bool last_fragment = (frag_end >= nal_len);
  const uint8_t nal_header = static_cast<uint8_t>(nal.data[0]);
  cursor.fu_header[0] = static_cast<uint8_t>((nal_header & 0xE0U) | kFuAType);
  cursor.fu_header[1] = static_cast<uint8_t>((first ? kFuStartMask : 0x00U) |
                                             (last_fragment ? kFuEndMask : 0x00U) | (nal_header & kNalTypeMask));
  const bool last = last_fragment && !HasNextNal(au, cursor.au_len, next_offset);
  WriteRtpHeader(cursor, ssrc, last);
  RtpPacket packet;
  packet.header = cursor.rtp_header.data();
  packet.header_len = kRtpHeaderBytes;
  packet.prefix = cursor.fu_header.data();
  packet.prefix_len = kFuHeaderBytes;
  packet.payload = reinterpret_cast<const uint8_t*>(nal.data.data() + start);
  packet.payload_len = chunk;
  packet.marker = last;
  if (last_fragment) {
    cursor.nal_offset = next_offset;
    cursor.frag_offset = 0U;
  } else {
    cursor.frag_offset = frag_end;
  }
  if (last) {
    cursor.in_au = false;
    cursor.marker_sent = true;
  }
  return osp::expected<RtpPacket, PacketizeError>::success(packet);
}

}  // namespace rs
