// Unit tests for Annex B scanning and RFC 7798 packetization.
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/h265_packetizer.hpp"

#include <cstring>

namespace {

struct PacketCopy {
  uint8_t header[12] = {};
  uint8_t prefix[3] = {};
  uint32_t prefix_len = 0U;
  uint8_t payload[rs::kRtpPayloadMax] = {};
  uint32_t payload_len = 0U;
  uint8_t body[rs::kRtpPayloadMax + 3U] = {};  // prefix | payload, i.e. the RTP payload.
  uint32_t body_len = 0U;
  const uint8_t* payload_ptr = nullptr;
  const uint8_t* prefix_ptr = nullptr;
  bool marker = false;
};

struct AuBuilder {
  uint8_t data[8192] = {};
  uint32_t len = 0U;

  void Start(bool four) {
    if (four) {
      data[len] = 0U;
      ++len;
    }
    data[len] = 0U;
    data[len + 1U] = 0U;
    data[len + 2U] = 1U;
    len += 3U;
  }

  void Nal(const uint8_t* nal, uint32_t nal_len, bool four) {
    Start(four);
    std::memcpy(data + len, nal, nal_len);
    len += nal_len;
  }

  void Zeros(uint32_t n) {
    std::memset(data + len, 0, n);
    len += n;
  }
};

uint16_t Seq(const PacketCopy& p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p.header[2]) << 8U) |
                               static_cast<uint16_t>(p.header[3]));
}

uint32_t Ts(const PacketCopy& p) {
  return (static_cast<uint32_t>(p.header[4]) << 24U) | (static_cast<uint32_t>(p.header[5]) << 16U) |
         (static_cast<uint32_t>(p.header[6]) << 8U) | static_cast<uint32_t>(p.header[7]);
}

uint32_t Ssrc(const PacketCopy& p) {
  return (static_cast<uint32_t>(p.header[8]) << 24U) | (static_cast<uint32_t>(p.header[9]) << 16U) |
         (static_cast<uint32_t>(p.header[10]) << 8U) | static_cast<uint32_t>(p.header[11]);
}

bool Collect(const uint8_t* au, uint32_t au_len, const rs::HevcParamSets& params, rs::HevcCursor& cursor,
             uint32_t ssrc, PacketCopy* out, uint32_t max, uint32_t& count) {
  count = 0U;
  bool ok = true;
  while (ok && (count < max)) {
    const auto r = rs::H265Packetizer::Next(au, au_len, params, cursor, ssrc);
    if (!r.has_value()) {
      ok = (rs::PacketizeError::kEndOfAu == r.get_error());
      break;
    } else {
      const rs::RtpPacket& p = r.value();
      std::memcpy(out[count].header, p.header, p.header_len);
      out[count].prefix_len = p.prefix_len;
      out[count].payload_ptr = p.payload;
      out[count].prefix_ptr = p.prefix;
      if (0U != p.prefix_len) {
        std::memcpy(out[count].prefix, p.prefix, p.prefix_len);
      }
      out[count].payload_len = p.payload_len;
      if (0U != p.payload_len) {
        std::memcpy(out[count].payload, p.payload, p.payload_len);
      }
      out[count].body_len = p.prefix_len + p.payload_len;
      std::memcpy(out[count].body, out[count].prefix, out[count].prefix_len);
      std::memcpy(out[count].body + out[count].prefix_len, out[count].payload, out[count].payload_len);
      out[count].marker = p.marker;
      ++count;
    }
  }
  return ok;
}

// Rebuilds the original NAL (including the two-byte header) from FU packets.
uint32_t Reassemble(const PacketCopy* packets, uint32_t count, uint8_t* out, uint32_t cap) {
  uint32_t len = 0U;
  for (uint32_t i = 0U; i < count; ++i) {
    const uint8_t* payload = packets[i].body;
    const uint32_t payload_len = packets[i].body_len;
    if ((payload_len >= 3U) && (49U == ((payload[0] >> 1U) & 0x3FU))) {
      const uint8_t fu_header = payload[2];
      if (0U != (fu_header & 0x80U)) {
        out[0] = static_cast<uint8_t>((payload[0] & 0x81U) | ((fu_header & 0x3FU) << 1U));
        out[1] = payload[1];
        len = 2U;
      }
      if ((len + payload_len - 3U) <= cap) {
        std::memcpy(out + len, payload + 3U, payload_len - 3U);
        len += payload_len - 3U;
      }
    } else if ((len + payload_len) <= cap) {
      std::memcpy(out + len, payload, payload_len);
      len += payload_len;
    }
  }
  return len;
}

uint32_t MakeNal(uint8_t* nal, uint8_t type, uint8_t tid, uint32_t total_len) {
  nal[0] = static_cast<uint8_t>((type << 1U) & 0x7EU);
  nal[1] = static_cast<uint8_t>(tid & 0x07U);
  for (uint32_t i = 2U; i < total_len; ++i) {
    nal[i] = static_cast<uint8_t>((i * 13U + 5U) & 0xFFU);
  }
  return total_len;
}

// Feeds one NAL with an explicit header and expects kBadNal (returns 0).
int ExpectBadNal(const uint8_t* nal, uint32_t nal_len) {
  AuBuilder au;
  au.Nal(nal, nal_len, true);
  rs::HevcCursor cursor;
  rs::H265Packetizer::StartAu(cursor, au.len, 100U, false);
  const auto r = rs::H265Packetizer::Next(au.data, au.len, rs::HevcParamSets{}, cursor, 1U);
  RS_CHECK(!r.has_value());
  RS_CHECK_EQ(r.get_error(), rs::PacketizeError::kBadNal);
  RS_CHECK(!cursor.in_au);
  return 0;
}

}  // namespace

int main() {
  const uint8_t vps[] = {0x40, 0x01, 0x0C, 0x01, 0xFF};
  const uint8_t sps[] = {0x42, 0x01, 0x01, 0x60, 0x0A};
  const uint8_t pps[] = {0x44, 0x01, 0xC1};
  const rs::HevcParamSets configured = {vps, sizeof(vps), sps, sizeof(sps), pps, sizeof(pps)};
  const rs::HevcParamSets empty = {};

  // Mixed 3/4-byte start codes; trailing zeros are not part of a NAL.
  {
    AuBuilder au;
    au.Nal(vps, sizeof(vps), true);
    au.Nal(sps, sizeof(sps), false);
    au.Zeros(2U);
    au.Nal(pps, sizeof(pps), true);
    uint32_t offset = 0U;
    rs::NalUnit unit;
    RS_CHECK(rs::NextHevcNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(unit.type, static_cast<uint8_t>(32U));  // VPS
    RS_CHECK_EQ(unit.data.size(), sizeof(vps));
    RS_CHECK_EQ(std::memcmp(unit.data.data(), vps, sizeof(vps)), 0);
    RS_CHECK(rs::NextHevcNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(unit.type, static_cast<uint8_t>(33U));  // SPS
    RS_CHECK_EQ(unit.data.size(), sizeof(sps));
    RS_CHECK_EQ(std::memcmp(unit.data.data(), sps, sizeof(sps)), 0);
    RS_CHECK(rs::NextHevcNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(unit.type, static_cast<uint8_t>(34U));  // PPS
    RS_CHECK_EQ(unit.data.size(), sizeof(pps));
    RS_CHECK_EQ(std::memcmp(unit.data.data(), pps, sizeof(pps)), 0);
    RS_CHECK(!rs::NextHevcNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(offset, au.len);
  }

  // Six-bit type extraction: IDR_W_RADL is 19, AUD is 35.
  {
    uint8_t idr[] = {0x26, 0x01, 0x11, 0x22};
    uint8_t aud[] = {0x46, 0x01, 0x50};
    AuBuilder au;
    au.Nal(idr, sizeof(idr), false);
    au.Nal(aud, sizeof(aud), true);
    uint32_t offset = 0U;
    rs::NalUnit unit;
    RS_CHECK(rs::NextHevcNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(unit.type, static_cast<uint8_t>(19U));
    RS_CHECK(rs::NextHevcNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(unit.type, static_cast<uint8_t>(35U));
    RS_CHECK(!rs::NextHevcNalUnit(au.data, au.len, offset, unit));
  }

  // No start code, a start code with no NAL, and a truncated AU.
  {
    const uint8_t random_bytes[] = {0x40, 0x01, 0x02, 0x03};
    uint32_t offset = 0U;
    rs::NalUnit unit;
    RS_CHECK(!rs::NextHevcNalUnit(random_bytes, sizeof(random_bytes), offset, unit));
    RS_CHECK_EQ(offset, sizeof(random_bytes));
    const uint8_t truncated[] = {0x00, 0x00, 0x01};
    offset = 0U;
    RS_CHECK(!rs::NextHevcNalUnit(truncated, sizeof(truncated), offset, unit));
    rs::HevcCursor cursor;
    rs::H265Packetizer::StartAu(cursor, sizeof(truncated), 100U, false);
    const auto r = rs::H265Packetizer::Next(truncated, sizeof(truncated), empty, cursor, 1U);
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::PacketizeError::kBadNal);
    RS_CHECK(!cursor.in_au);
  }

  // Invalid NAL headers are rejected: forbidden bit, TID 0, layer id, type.
  {
    const uint8_t forbidden[] = {0xC0, 0x01};
    RS_CHECK_EQ(ExpectBadNal(forbidden, sizeof(forbidden)), 0);
    const uint8_t tid_zero[] = {0x40, 0x00};
    RS_CHECK_EQ(ExpectBadNal(tid_zero, sizeof(tid_zero)), 0);
    const uint8_t layer_high[] = {0x41, 0x01};
    RS_CHECK_EQ(ExpectBadNal(layer_high, sizeof(layer_high)), 0);
    const uint8_t layer_low[] = {0x40, 0x09};
    RS_CHECK_EQ(ExpectBadNal(layer_low, sizeof(layer_low)), 0);
    const uint8_t type41[] = {0x52, 0x01};
    RS_CHECK_EQ(ExpectBadNal(type41, sizeof(type41)), 0);
    const uint8_t one_byte[] = {0x40};
    RS_CHECK_EQ(ExpectBadNal(one_byte, sizeof(one_byte)), 0);
  }

  // Single NAL exactly at the payload limit (1200 bytes, 2-byte header).
  {
    uint8_t nal[1200] = {};
    MakeNal(nal, 19U, 1U, sizeof(nal));
    AuBuilder au;
    au.Nal(nal, sizeof(nal), true);
    rs::HevcCursor cursor;
    cursor.seq = 0x1000U;
    rs::H265Packetizer::StartAu(cursor, au.len, 9000U, false);
    const auto r = rs::H265Packetizer::Next(au.data, au.len, empty, cursor, 0xAABBCCDDU);
    RS_CHECK(r.has_value());
    const rs::RtpPacket& p = r.value();
    RS_CHECK_EQ(p.header_len, 12U);
    RS_CHECK_EQ(p.payload_len, 1200U);
    RS_CHECK(p.marker);
    RS_CHECK_EQ(p.header[0], static_cast<uint8_t>(0x80U));
    RS_CHECK_EQ(p.header[1], static_cast<uint8_t>(0xE0U));  // marker + PT 96
    RS_CHECK_EQ(static_cast<uint32_t>(p.header[2]), 0x10U);
    RS_CHECK_EQ(static_cast<uint32_t>(p.header[3]), 0x00U);
    RS_CHECK_EQ(static_cast<uint32_t>(p.header[8]), 0xAAU);
    RS_CHECK_EQ(static_cast<uint32_t>(p.header[11]), 0xDDU);
    RS_CHECK_EQ(std::memcmp(p.payload, nal, sizeof(nal)), 0);
    RS_CHECK_EQ(p.prefix_len, 0U);  // Single NAL: no FU prefix.
    RS_CHECK(nullptr == p.prefix);
    // Zero-copy: the payload points at the NAL inside the AU (4-byte start code).
    RS_CHECK(p.payload == au.data + 4U);
    RS_CHECK_EQ(cursor.seq, static_cast<uint16_t>(0x1001U));
    const auto end = rs::H265Packetizer::Next(au.data, au.len, empty, cursor, 0xAABBCCDDU);
    RS_CHECK(!end.has_value());
    RS_CHECK_EQ(end.get_error(), rs::PacketizeError::kEndOfAu);
    RS_CHECK(!cursor.in_au);
  }

  // 1201 bytes: first FU packet is full (3 + 1197), tail holds 2 bytes.
  {
    uint8_t nal[1201] = {};
    MakeNal(nal, 19U, 2U, sizeof(nal));
    AuBuilder au;
    au.Nal(nal, sizeof(nal), true);
    rs::HevcCursor cursor;
    rs::H265Packetizer::StartAu(cursor, au.len, 12345U, false);
    PacketCopy packets[4];
    uint32_t count = 0U;
    RS_CHECK(Collect(au.data, au.len, empty, cursor, 0x01020304U, packets, 4U, count));
    RS_CHECK_EQ(count, 2U);
    RS_CHECK_EQ(packets[0].body_len, 1200U);          // 3-byte prefix + 1197 fragment bytes.
    RS_CHECK_EQ(packets[0].prefix_len, 3U);
    RS_CHECK_EQ(packets[0].payload_len, 1197U);
    RS_CHECK_EQ(packets[0].prefix[0], static_cast<uint8_t>(0x62U));  // F=0, type 49
    RS_CHECK_EQ(packets[0].prefix[1], nal[1]);                       // layer/TID copied
    RS_CHECK_EQ(packets[0].prefix[2], static_cast<uint8_t>(0x93U));  // S=1, type 19
    // Zero-copy: the fragment points into the AU right after the NAL header.
    RS_CHECK(packets[0].payload_ptr == au.data + 6U);
    RS_CHECK(packets[0].prefix_ptr == cursor.fu_header.data());
    RS_CHECK(!packets[0].marker);
    RS_CHECK_EQ(packets[1].body_len, 5U);
    RS_CHECK_EQ(packets[1].prefix_len, 3U);
    RS_CHECK_EQ(packets[1].payload_len, 2U);
    RS_CHECK_EQ(packets[1].prefix[2], static_cast<uint8_t>(0x53U));  // E=1, type 19
    RS_CHECK(packets[1].payload_ptr == au.data + 6U + 1197U);
    RS_CHECK(packets[1].marker);
    RS_CHECK_EQ(Ts(packets[0]), 12345U);
    RS_CHECK_EQ(Ts(packets[1]), 12345U);
    RS_CHECK_EQ(Ssrc(packets[0]), 0x01020304U);
    uint8_t rebuilt[1300] = {};
    uint32_t rebuilt_len = Reassemble(packets, count, rebuilt, sizeof(rebuilt));
    RS_CHECK_EQ(rebuilt_len, sizeof(nal));
    RS_CHECK_EQ(std::memcmp(rebuilt, nal, sizeof(nal)), 0);
    // Fragment bytes alone match the NAL payload after its two-byte header.
    uint8_t payload_only[1300] = {};
    uint32_t payload_len = 0U;
    for (uint32_t i = 0U; i < count; ++i) {
      std::memcpy(payload_only + payload_len, packets[i].payload, packets[i].payload_len);
      payload_len += packets[i].payload_len;
    }
    RS_CHECK_EQ(payload_len, sizeof(nal) - 2U);
    RS_CHECK_EQ(std::memcmp(payload_only, nal + 2U, payload_len), 0);
  }

  // 2400 bytes spans three FU packets (1200 / 1200 / 7).
  {
    uint8_t nal[2400] = {};
    MakeNal(nal, 1U, 1U, sizeof(nal));
    AuBuilder au;
    au.Nal(nal, sizeof(nal), false);  // 3-byte start code variant
    rs::HevcCursor cursor;
    rs::H265Packetizer::StartAu(cursor, au.len, 777U, false);
    PacketCopy packets[6];
    uint32_t count = 0U;
    RS_CHECK(Collect(au.data, au.len, empty, cursor, 9U, packets, 6U, count));
    RS_CHECK_EQ(count, 3U);
    RS_CHECK_EQ(packets[0].body_len, 1200U);
    RS_CHECK_EQ(packets[1].body_len, 1200U);
    RS_CHECK_EQ(packets[2].body_len, 7U);
    RS_CHECK_EQ(packets[0].payload_len, 1197U);
    RS_CHECK_EQ(packets[1].payload_len, 1197U);
    RS_CHECK_EQ(packets[2].payload_len, 4U);
    RS_CHECK_EQ(packets[0].prefix[2], static_cast<uint8_t>(0x81U));  // S=1, type 1
    RS_CHECK_EQ(packets[1].prefix[2], static_cast<uint8_t>(0x01U));  // middle
    RS_CHECK_EQ(packets[2].prefix[2], static_cast<uint8_t>(0x41U));  // E=1, type 1
    // Zero-copy: every fragment is a disjoint view into the AU.
    RS_CHECK(packets[0].payload_ptr == au.data + 5U);
    RS_CHECK(packets[1].payload_ptr == au.data + 1202U);
    RS_CHECK(packets[2].payload_ptr == au.data + 2399U);
    RS_CHECK(!packets[0].marker);
    RS_CHECK(!packets[1].marker);
    RS_CHECK(packets[2].marker);
    uint8_t rebuilt[2600] = {};
    const uint32_t rebuilt_len = Reassemble(packets, count, rebuilt, sizeof(rebuilt));
    RS_CHECK_EQ(rebuilt_len, sizeof(nal));
    RS_CHECK_EQ(std::memcmp(rebuilt, nal, sizeof(nal)), 0);
  }

  // Multi-NAL AU: marker only on the last packet of the last NAL.
  {
    uint8_t aud[] = {0x46, 0x01, 0x50};
    uint8_t idr[1201] = {};
    MakeNal(idr, 19U, 1U, sizeof(idr));
    uint8_t trail[100] = {};
    MakeNal(trail, 1U, 1U, sizeof(trail));
    AuBuilder au;
    au.Nal(vps, sizeof(vps), false);
    au.Nal(sps, sizeof(sps), true);
    au.Nal(pps, sizeof(pps), true);
    au.Nal(aud, sizeof(aud), false);
    au.Nal(idr, sizeof(idr), true);
    au.Nal(trail, sizeof(trail), false);
    rs::HevcCursor cursor;
    cursor.seq = 0x0100U;
    rs::H265Packetizer::StartAu(cursor, au.len, 500U, false);
    PacketCopy packets[10];
    uint32_t count = 0U;
    RS_CHECK(Collect(au.data, au.len, empty, cursor, 0x55U, packets, 10U, count));
    RS_CHECK_EQ(count, 7U);  // VPS, SPS, PPS, AUD, IDR x2, TRAIL
    uint32_t markers = 0U;
    for (uint32_t i = 0U; i < count; ++i) {
      RS_CHECK(packets[i].body_len <= rs::kRtpPayloadMax);
      RS_CHECK_EQ(Ts(packets[i]), 500U);
      RS_CHECK_EQ(Seq(packets[i]), static_cast<uint16_t>(0x0100U + i));
      if (packets[i].marker) {
        ++markers;
        RS_CHECK_EQ(i, count - 1U);
      }
    }
    RS_CHECK_EQ(markers, 1U);
    RS_CHECK_EQ(packets[0].prefix_len, 0U);  // VPS is a single NAL unit packet.
    RS_CHECK_EQ(packets[0].payload_len, sizeof(vps));
    RS_CHECK_EQ(std::memcmp(packets[0].payload, vps, sizeof(vps)), 0);
    RS_CHECK_EQ(packets[3].payload_len, sizeof(aud));
    RS_CHECK_EQ(std::memcmp(packets[3].payload, aud, sizeof(aud)), 0);
    RS_CHECK_EQ(packets[6].payload_len, sizeof(trail));
    RS_CHECK_EQ(std::memcmp(packets[6].payload, trail, sizeof(trail)), 0);
    uint8_t rebuilt[1300] = {};
    const uint32_t rebuilt_len = Reassemble(&packets[4], 2U, rebuilt, sizeof(rebuilt));
    RS_CHECK_EQ(rebuilt_len, sizeof(idr));
    RS_CHECK_EQ(std::memcmp(rebuilt, idr, sizeof(idr)), 0);
  }

  // VPS/SPS/PPS injection precedes the first slice packet, in that order.
  {
    uint8_t idr[100] = {};
    MakeNal(idr, 19U, 1U, sizeof(idr));
    AuBuilder au;
    au.Nal(idr, sizeof(idr), true);
    rs::HevcCursor cursor;
    cursor.seq = 10U;
    rs::H265Packetizer::StartAu(cursor, au.len, 4321U, true);
    PacketCopy packets[4];
    uint32_t count = 0U;
    RS_CHECK(Collect(au.data, au.len, configured, cursor, 0x77U, packets, 4U, count));
    RS_CHECK_EQ(count, 4U);
    RS_CHECK_EQ(packets[0].payload_len, sizeof(vps));
    RS_CHECK_EQ(std::memcmp(packets[0].payload, vps, sizeof(vps)), 0);
    RS_CHECK(!packets[0].marker);
    RS_CHECK_EQ(packets[1].payload_len, sizeof(sps));
    RS_CHECK_EQ(std::memcmp(packets[1].payload, sps, sizeof(sps)), 0);
    RS_CHECK(!packets[1].marker);
    RS_CHECK_EQ(packets[2].payload_len, sizeof(pps));
    RS_CHECK_EQ(std::memcmp(packets[2].payload, pps, sizeof(pps)), 0);
    RS_CHECK(!packets[2].marker);
    RS_CHECK_EQ(packets[3].payload_len, sizeof(idr));
    RS_CHECK(packets[3].marker);
    RS_CHECK_EQ(Seq(packets[0]), static_cast<uint16_t>(10U));
    RS_CHECK_EQ(Seq(packets[1]), static_cast<uint16_t>(11U));
    RS_CHECK_EQ(Seq(packets[2]), static_cast<uint16_t>(12U));
    RS_CHECK_EQ(Seq(packets[3]), static_cast<uint16_t>(13U));
    RS_CHECK_EQ(Ts(packets[3]), 4321U);
  }

  // Injection without parameter sets fails instead of sending an empty NAL.
  {
    uint8_t idr[20] = {};
    MakeNal(idr, 19U, 1U, sizeof(idr));
    AuBuilder au;
    au.Nal(idr, sizeof(idr), true);
    rs::HevcCursor cursor;
    rs::H265Packetizer::StartAu(cursor, au.len, 1U, true);
    const auto r = rs::H265Packetizer::Next(au.data, au.len, empty, cursor, 1U);
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::PacketizeError::kNotConfigured);
    RS_CHECK(!cursor.in_au);
    // VPS + SPS present, PPS missing: fails at the third stage.
    const rs::HevcParamSets no_pps = {vps, sizeof(vps), sps, sizeof(sps), nullptr, 0U};
    rs::HevcCursor partial;
    rs::H265Packetizer::StartAu(partial, au.len, 1U, true);
    const auto first = rs::H265Packetizer::Next(au.data, au.len, no_pps, partial, 1U);
    RS_CHECK(first.has_value());
    const auto second = rs::H265Packetizer::Next(au.data, au.len, no_pps, partial, 1U);
    RS_CHECK(second.has_value());
    const auto third = rs::H265Packetizer::Next(au.data, au.len, no_pps, partial, 1U);
    RS_CHECK(!third.has_value());
    RS_CHECK_EQ(third.get_error(), rs::PacketizeError::kNotConfigured);
    RS_CHECK(!partial.in_au);
  }

  // kEndOfAu is idempotent and leaves no AU in progress.
  {
    uint8_t trail[10] = {};
    MakeNal(trail, 1U, 1U, sizeof(trail));
    AuBuilder au;
    au.Nal(trail, sizeof(trail), true);
    rs::HevcCursor cursor;
    rs::H265Packetizer::StartAu(cursor, au.len, 1U, false);
    PacketCopy packets[2];
    uint32_t count = 0U;
    RS_CHECK(Collect(au.data, au.len, empty, cursor, 1U, packets, 2U, count));
    RS_CHECK_EQ(count, 1U);
    for (uint32_t i = 0U; i < 2U; ++i) {
      const auto r = rs::H265Packetizer::Next(au.data, au.len, empty, cursor, 1U);
      RS_CHECK(!r.has_value());
      RS_CHECK_EQ(r.get_error(), rs::PacketizeError::kEndOfAu);
      RS_CHECK(!cursor.in_au);
    }
  }

  // StartAu field contract and ResetCursor wait-for-IDR behaviour.
  {
    rs::HevcCursor cursor;
    cursor.seq = 0xFFFFU;
    rs::H265Packetizer::StartAu(cursor, 123U, 456U, true);
    RS_CHECK_EQ(cursor.nal_offset, 0U);
    RS_CHECK_EQ(cursor.frag_offset, 0U);
    RS_CHECK_EQ(cursor.au_len, 123U);
    RS_CHECK_EQ(cursor.ts, 456U);
    RS_CHECK(cursor.in_au);
    RS_CHECK_EQ(cursor.param_stage, static_cast<uint8_t>(0));
    RS_CHECK(!cursor.marker_sent);
    rs::H265Packetizer::StartAu(cursor, 123U, 456U, false);
    RS_CHECK_EQ(cursor.param_stage, static_cast<uint8_t>(3));
    rs::H265Packetizer::ResetCursor(cursor);
    RS_CHECK(!cursor.in_au);
    RS_CHECK(cursor.await_idr);
    RS_CHECK_EQ(cursor.frag_offset, 0U);
    RS_CHECK_EQ(cursor.param_stage, static_cast<uint8_t>(3));
    RS_CHECK_EQ(cursor.seq, static_cast<uint16_t>(0xFFFFU));  // Sequence is not reset.
  }

  // 16-bit sequence wrap is natural.
  {
    uint8_t nal[10] = {};
    MakeNal(nal, 1U, 1U, sizeof(nal));
    AuBuilder au;
    au.Nal(nal, sizeof(nal), true);
    rs::HevcCursor cursor;
    cursor.seq = 0xFFFFU;
    rs::H265Packetizer::StartAu(cursor, au.len, 1U, false);
    const auto r = rs::H265Packetizer::Next(au.data, au.len, empty, cursor, 1U);
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(static_cast<uint16_t>((static_cast<uint16_t>(r.value().header[2]) << 8U) |
                                      static_cast<uint16_t>(r.value().header[3])),
                static_cast<uint16_t>(0xFFFFU));
    RS_CHECK_EQ(cursor.seq, static_cast<uint16_t>(0U));
  }

  // IRAP detection: VCL types 16..23 only.
  {
    const uint8_t types[] = {1U, 19U, 20U, 21U, 23U, 32U};
    const bool expected[] = {false, true, true, true, true, false};
    for (uint32_t i = 0U; i < sizeof(types); ++i) {
      uint8_t nal[8] = {};
      MakeNal(nal, types[i], 1U, sizeof(nal));
      AuBuilder au;
      au.Nal(nal, sizeof(nal), true);
      RS_CHECK_EQ(rs::HevcAuHasIrap(au.data, au.len), expected[i]);
    }
    RS_CHECK(!rs::HevcAuHasIrap(nullptr, 0U));
  }

  return 0;
}
