// Annex B scanning and H.265 RTP packetization (RFC 7798).
// Single NAL unit packets and FU packets; AP aggregation is not implemented.
// SPDX-License-Identifier: MIT
#include "rs/h265_packetizer.hpp"

namespace rs {
namespace {

constexpr uint32_t kHevcNalHeaderBytes = 2U;
constexpr uint32_t kFuHeaderBytes = 3U;  // Two-byte payload header + FU header.
constexpr uint32_t kMaxFragment = kRtpPayloadMax - kFuHeaderBytes;
constexpr uint8_t kFuType = static_cast<uint8_t>(HevcNalType::kFu);
constexpr uint8_t kMaxNalType = 40U;  // 41..47 are reserved, 48 (AP) is unused.
constexpr uint8_t kParamStageDone = 3U;
constexpr uint8_t kIrapTypeFirst = static_cast<uint8_t>(HevcNalType::kBlWlp);
constexpr uint8_t kIrapTypeLast = static_cast<uint8_t>(HevcNalType::kIrapVcl23);
constexpr uint8_t kFuStartMask = 0x80U;
constexpr uint8_t kFuEndMask = 0x40U;
constexpr uint8_t kHevcTypeShift = 1U;
constexpr uint8_t kHevcTypeMask = 0x3FU;
constexpr uint8_t kHevcLayerHighMask = 0x01U;
constexpr uint8_t kHevcLayerHighShift = 5U;
constexpr uint8_t kHevcLayerLowShift = 3U;
constexpr uint8_t kHevcTidMask = 0x07U;
constexpr uint8_t kHevcForbiddenMask = 0x80U;
constexpr uint8_t kHevcFuKeepMask = 0x81U;  // F bit + nuh_layer_id high bit.

// Finds the next 00 00 01 pattern at or after `from`. A four-byte start code
// is matched at its last three bytes; the leading zero is trimmed as a
// trailing zero of the previous NAL.
bool FindStartCode(const uint8_t* au, uint32_t from, uint32_t len, uint32_t& pos) noexcept {
  bool found = false;
  if ((nullptr != au) && (from < len)) {
    for (uint32_t i = from; !found && (i + 2U) < len; ++i) {
      if ((0U == au[i]) && (0U == au[i + 1U]) && (1U == au[i + 2U])) {
        pos = i;
        found = true;
      }
    }
  }
  return found;
}

bool OnlyZeros(const uint8_t* au, uint32_t from, uint32_t len) noexcept {
  bool zeros = true;
  for (uint32_t i = from; zeros && (i < len); ++i) {
    zeros = (0U == au[i]);
  }
  return zeros;
}

bool HasNextNal(const uint8_t* au, uint32_t au_len, uint32_t offset) noexcept {
  NalUnit next;
  uint32_t probe = offset;
  return (offset < au_len) && NextHevcNalUnit(au, au_len, probe, next);
}

void WriteRtpHeader(HevcCursor& cursor, uint32_t ssrc, bool marker) noexcept {
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

// Validates the two-byte NAL header and returns the 6-bit nal_unit_type.
// Reserved types above 40 (41..47) and AP (48) are rejected.
bool ParseNalHeader(const uint8_t* data, uint32_t len, uint8_t& type) noexcept {
  bool valid = false;
  if ((nullptr != data) && (len >= kHevcNalHeaderBytes)) {
    const uint8_t layer_id = static_cast<uint8_t>(((data[0] & kHevcLayerHighMask) << kHevcLayerHighShift) | (data[1] >> kHevcLayerLowShift));
    const uint8_t tid_plus1 = static_cast<uint8_t>(data[1] & kHevcTidMask);
    type = static_cast<uint8_t>((data[0] >> kHevcTypeShift) & kHevcTypeMask);
    valid = (0U == (data[0] & kHevcForbiddenMask)) && (0U != tid_plus1) && (0U == layer_id) && (type <= kMaxNalType);
  }
  return valid;
}

osp::expected<RtpPacket, PacketizeError> MakePacket(HevcCursor& cursor, uint32_t ssrc, const uint8_t* payload,
                                                    uint32_t payload_len, bool marker) noexcept {
  WriteRtpHeader(cursor, ssrc, marker);
  RtpPacket packet;
  packet.header = cursor.rtp_header.data();
  packet.header_len = kRtpHeaderBytes;
  packet.payload = payload;
  packet.payload_len = payload_len;
  packet.marker = marker;
  return osp::expected<RtpPacket, PacketizeError>::success(packet);
}

// Emits VPS, SPS or PPS as a single NAL unit packet, one per call.
osp::expected<RtpPacket, PacketizeError> EmitParamSet(const HevcParamSets& params, HevcCursor& cursor,
                                                      uint32_t ssrc) noexcept {
  const uint8_t* const sets[3] = {params.vps, params.sps, params.pps};
  const uint32_t lens[3] = {params.vps_len, params.sps_len, params.pps_len};
  const uint32_t index = cursor.param_stage;
  const uint8_t* set = sets[index];
  const uint32_t set_len = lens[index];
  osp::expected<RtpPacket, PacketizeError> result =
      osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kNotConfigured);
  if ((nullptr == set) || (0U == set_len)) {
    cursor.in_au = false;
  } else if (set_len > kRtpPayloadMax) {
    cursor.in_au = false;
    result = osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kTooLarge);
  } else {
    ++cursor.param_stage;
    result = MakePacket(cursor, ssrc, set, set_len, false);
  }
  return result;
}

osp::expected<RtpPacket, PacketizeError> EmitSingle(const uint8_t* au, uint32_t next_offset, const NalUnit& nal,
                                                    HevcCursor& cursor, uint32_t ssrc) noexcept {
  const bool last = !HasNextNal(au, cursor.au_len, next_offset);
  cursor.nal_offset = next_offset;
  cursor.frag_offset = 0U;
  if (last) {
    cursor.in_au = false;
    cursor.marker_sent = true;
  }
  return MakePacket(cursor, ssrc, reinterpret_cast<const uint8_t*>(nal.data.data()),
                    static_cast<uint32_t>(nal.data.size()), last);
}

// FU packet: two-byte payload header carrying the original NAL header layout
// (type replaced by 49) plus a one-byte FU header with S/E/FuType. Only those
// three bytes are staged in the cursor; fragment bytes stay in the shared AU.
osp::expected<RtpPacket, PacketizeError> EmitFu(const uint8_t* au, uint32_t next_offset, const NalUnit& nal,
                                                uint8_t type, HevcCursor& cursor, uint32_t ssrc) noexcept {
  osp::expected<RtpPacket, PacketizeError> result =
      osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kBadNal);
  const uint32_t nal_len = static_cast<uint32_t>(nal.data.size());
  const uint32_t start = (0U == cursor.frag_offset) ? kHevcNalHeaderBytes : cursor.frag_offset;
  if (start < nal_len) {
    const bool first = (0U == cursor.frag_offset);
    uint32_t chunk = nal_len - start;
    if (chunk > kMaxFragment) {
      chunk = kMaxFragment;
    }
    const uint32_t frag_end = start + chunk;
    const bool last_fragment = (frag_end >= nal_len);
    cursor.fu_header[0] = static_cast<uint8_t>((static_cast<uint8_t>(nal.data[0]) & kHevcFuKeepMask) |
                                               static_cast<uint8_t>(kFuType << kHevcTypeShift));
    cursor.fu_header[1] = static_cast<uint8_t>(nal.data[1]);
    cursor.fu_header[2] = static_cast<uint8_t>((first ? kFuStartMask : 0x00U) |
                                               (last_fragment ? kFuEndMask : 0x00U) | type);
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
    result = osp::expected<RtpPacket, PacketizeError>::success(packet);
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
  } else {
    cursor.in_au = false;
  }
  return result;
}

osp::expected<RtpPacket, PacketizeError> EmitNal(const uint8_t* au, HevcCursor& cursor, uint32_t ssrc) noexcept {
  uint32_t next_offset = cursor.nal_offset;
  NalUnit nal;
  const bool have_nal = NextHevcNalUnit(au, cursor.au_len, next_offset, nal);
  uint8_t type = 0U;
  osp::expected<RtpPacket, PacketizeError> result =
      osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kBadNal);
  if (!have_nal) {
    cursor.in_au = false;
    result = osp::expected<RtpPacket, PacketizeError>::error(
        OnlyZeros(au, cursor.nal_offset, cursor.au_len) ? PacketizeError::kEndOfAu : PacketizeError::kBadNal);
  } else if (!ParseNalHeader(reinterpret_cast<const uint8_t*>(nal.data.data()),
                             static_cast<uint32_t>(nal.data.size()), type)) {
    cursor.in_au = false;
  } else if ((0U == cursor.frag_offset) && (nal.data.size() <= kRtpPayloadMax)) {
    result = EmitSingle(au, next_offset, nal, cursor, ssrc);
  } else {
    result = EmitFu(au, next_offset, nal, type, cursor, ssrc);
  }
  return result;
}

}  // namespace

bool NextHevcNalUnit(const uint8_t* au, uint32_t au_len, uint32_t& offset, NalUnit& out) noexcept {
  uint32_t sc = 0U;
  const bool found = (offset <= au_len) && FindStartCode(au, offset, au_len, sc);
  const uint32_t nal_start = found ? sc + 3U : 0U;
  uint32_t next = au_len;
  const bool has_next = found && FindStartCode(au, nal_start, au_len, next);
  uint32_t nal_end = has_next ? next : au_len;
  // Trailing zeros may be the fourth byte of the next start code or
  // trailing_zero_8bits; neither belongs to the NAL payload.
  while (found && (nal_end > nal_start) && (0U == au[nal_end - 1U])) {
    --nal_end;
  }
  // A start code without a two-byte NAL header is truncated, not a NAL unit.
  const bool ok = found && ((nal_end - nal_start) >= 2U);
  if (ok) {
    out.data = osp::byte_span(reinterpret_cast<const std::byte*>(au + nal_start), nal_end - nal_start);
    out.type = static_cast<uint8_t>((au[nal_start] >> kHevcTypeShift) & kHevcTypeMask);
    offset = has_next ? next : au_len;
  } else {
    out.data = osp::byte_span();
    out.type = 0U;
    offset = au_len;
  }
  return ok;
}

bool HevcAuHasIrap(const uint8_t* au, uint32_t au_len) noexcept {
  uint32_t offset = 0U;
  NalUnit nal;
  bool found = false;
  while (!found && NextHevcNalUnit(au, au_len, offset, nal)) {
    found = (nal.type >= kIrapTypeFirst) && (nal.type <= kIrapTypeLast);
  }
  return found;
}

void H265Packetizer::ResetCursor(HevcCursor& cursor) noexcept {
  cursor.nal_offset = 0U;
  cursor.frag_offset = 0U;
  cursor.au_len = 0U;
  cursor.param_stage = kParamStageDone;
  cursor.await_idr = true;
  cursor.in_au = false;
  cursor.marker_sent = false;
}

void H265Packetizer::StartAu(HevcCursor& cursor, uint32_t au_len, uint32_t ts, bool inject_params) noexcept {
  cursor.nal_offset = 0U;
  cursor.frag_offset = 0U;
  cursor.au_len = au_len;
  cursor.ts = ts;
  cursor.param_stage = inject_params ? 0U : kParamStageDone;
  cursor.in_au = true;
  cursor.marker_sent = false;
}

osp::expected<RtpPacket, PacketizeError> H265Packetizer::Next(const uint8_t* au, uint32_t au_len,
                                                              const HevcParamSets& params, HevcCursor& cursor,
                                                              uint32_t ssrc) noexcept {
  osp::expected<RtpPacket, PacketizeError> result =
      osp::expected<RtpPacket, PacketizeError>::error(PacketizeError::kEndOfAu);
  if (cursor.in_au) {
    if (cursor.param_stage < kParamStageDone) {
      result = EmitParamSet(params, cursor, ssrc);
    } else if ((nullptr == au) || (cursor.au_len > au_len) || (cursor.au_len > kMaxAuBytes)) {
      const bool too_large = (nullptr != au) && (cursor.au_len <= au_len);
      cursor.in_au = false;
      result = osp::expected<RtpPacket, PacketizeError>::error(too_large ? PacketizeError::kTooLarge
                                                                        : PacketizeError::kBadNal);
    } else {
      result = EmitNal(au, cursor, ssrc);
    }
  }
  return result;
}

}  // namespace rs
