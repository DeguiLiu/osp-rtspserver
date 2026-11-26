// Unit tests for Annex B scanning and RTP packetization (design 7.2/7.3).
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/h264_packetizer.hpp"

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
  return static_cast<uint16_t>((static_cast<uint16_t>(p.header[2]) << 8U) | static_cast<uint16_t>(p.header[3]));
}

uint32_t Ts(const PacketCopy& p) {
  return (static_cast<uint32_t>(p.header[4]) << 24U) | (static_cast<uint32_t>(p.header[5]) << 16U) |
         (static_cast<uint32_t>(p.header[6]) << 8U) | static_cast<uint32_t>(p.header[7]);
}

uint32_t Ssrc(const PacketCopy& p) {
  return (static_cast<uint32_t>(p.header[8]) << 24U) | (static_cast<uint32_t>(p.header[9]) << 16U) |
         (static_cast<uint32_t>(p.header[10]) << 8U) | static_cast<uint32_t>(p.header[11]);
}

bool Collect(const uint8_t* au, uint32_t au_len, const rs::ParamSets& params, rs::ClientCursor& cursor, uint32_t ssrc,
             PacketCopy* out, uint32_t max, uint32_t& count) {
  count = 0U;
  bool ok = true;
  while (ok && count < max) {
    const auto r = rs::H264Packetizer::Next(au, au_len, params, cursor, ssrc);
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

uint32_t Reassemble(const PacketCopy* packets, uint32_t count, uint8_t* out, uint32_t cap) {
  uint32_t len = 0U;
  for (uint32_t i = 0U; i < count; ++i) {
    const uint8_t* payload = packets[i].body;
    const uint32_t payload_len = packets[i].body_len;
    if (payload_len >= 2U && 28U == (payload[0] & 0x1FU)) {
      const uint8_t indicator = payload[0];
      const uint8_t fu_header = payload[1];
      if (0U != (fu_header & 0x80U)) {
        out[0] = static_cast<uint8_t>((indicator & 0xE0U) | (fu_header & 0x1FU));
        len = 1U;
      }
      if ((len + payload_len - 2U) <= cap) {
        std::memcpy(out + len, payload + 2U, payload_len - 2U);
        len += payload_len - 2U;
      }
    } else if ((len + payload_len) <= cap) {
      std::memcpy(out + len, payload, payload_len);
      len += payload_len;
    }
  }
  return len;
}

uint32_t MakeNal(uint8_t* nal, uint8_t header, uint32_t total_len) {
  nal[0] = header;
  for (uint32_t i = 1U; i < total_len; ++i) {
    nal[i] = static_cast<uint8_t>((i * 7U + 3U) & 0xFFU);
  }
  return total_len;
}

}  // namespace

int main() {
  const uint8_t sps[] = {0x67, 0x42, 0xC0, 0x1F};
  const uint8_t pps[] = {0x68, 0xCE};
  const rs::ParamSets configured = {sps, sizeof(sps), pps, sizeof(pps)};
  const rs::ParamSets empty = {};

  // Mixed 3/4-byte start codes; trailing zeros are not part of a NAL.
  {
    uint8_t nal7[] = {0x67, 0x11, 0x22};
    uint8_t nal8[] = {0x68, 0x33};
    uint8_t nal5[] = {0x65, 0x01, 0x02, 0x03, 0x04};
    AuBuilder au;
    au.Nal(nal7, sizeof(nal7), true);
    au.Nal(nal8, sizeof(nal8), false);
    au.Zeros(2U);
    au.Nal(nal5, sizeof(nal5), true);
    uint32_t offset = 0U;
    rs::NalUnit unit;
    RS_CHECK(rs::NextNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(unit.type, static_cast<uint8_t>(7U));
    RS_CHECK_EQ(unit.data.size(), sizeof(nal7));
    RS_CHECK_EQ(std::memcmp(unit.data.data(), nal7, sizeof(nal7)), 0);
    RS_CHECK(rs::NextNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(unit.type, static_cast<uint8_t>(8U));
    RS_CHECK_EQ(unit.data.size(), sizeof(nal8));
    RS_CHECK_EQ(std::memcmp(unit.data.data(), nal8, sizeof(nal8)), 0);
    RS_CHECK(rs::NextNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(unit.type, static_cast<uint8_t>(5U));
    RS_CHECK_EQ(unit.data.size(), sizeof(nal5));
    RS_CHECK_EQ(std::memcmp(unit.data.data(), nal5, sizeof(nal5)), 0);
    RS_CHECK(!rs::NextNalUnit(au.data, au.len, offset, unit));
    RS_CHECK_EQ(offset, au.len);
    RS_CHECK(rs::AuHasIdr(au.data, au.len));
  }

  // No IDR and empty AU are reported as such.
  {
    uint8_t nal1[] = {0x41, 0x01, 0x02};
    AuBuilder au;
    au.Nal(nal1, sizeof(nal1), false);
    RS_CHECK(!rs::AuHasIdr(au.data, au.len));
    RS_CHECK(!rs::AuHasIdr(au.data, 0U));
  }

  // NAL type 0 is rejected by Next().
  {
    uint8_t nal0[] = {0x00, 0xAA, 0xBB};
    AuBuilder au;
    au.Nal(nal0, sizeof(nal0), true);
    rs::ClientCursor cursor;
    rs::H264Packetizer::StartAu(cursor, au.len, 100U, false);
    const auto r = rs::H264Packetizer::Next(au.data, au.len, empty, cursor, 1U);
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::PacketizeError::kBadNal);
    RS_CHECK(!cursor.in_au);
  }

  // A start code with no NAL payload is truncated, not a valid empty NAL.
  {
    const uint8_t truncated[] = {0x00, 0x00, 0x01};
    rs::ClientCursor cursor;
    rs::H264Packetizer::StartAu(cursor, sizeof(truncated), 100U, false);
    const auto r = rs::H264Packetizer::Next(truncated, sizeof(truncated), empty, cursor, 1U);
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::PacketizeError::kBadNal);
  }

  // Single NAL exactly at the payload limit (1200 bytes).
  {
    uint8_t nal[1200] = {};
    MakeNal(nal, 0x65, sizeof(nal));
    AuBuilder au;
    au.Nal(nal, sizeof(nal), true);
    rs::ClientCursor cursor;
    cursor.seq = 0x1000U;
    rs::H264Packetizer::StartAu(cursor, au.len, 9000U, false);
    const auto r = rs::H264Packetizer::Next(au.data, au.len, empty, cursor, 0xAABBCCDDU);
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
    const auto end = rs::H264Packetizer::Next(au.data, au.len, empty, cursor, 0xAABBCCDDU);
    RS_CHECK(!end.has_value());
    RS_CHECK_EQ(end.get_error(), rs::PacketizeError::kEndOfAu);
    RS_CHECK(!cursor.in_au);
  }

  // 1201 bytes: one full FU-A fragment plus a two-byte tail.
  {
    uint8_t nal[1201] = {};
    MakeNal(nal, 0x65, sizeof(nal));
    AuBuilder au;
    au.Nal(nal, sizeof(nal), true);
    rs::ClientCursor cursor;
    rs::H264Packetizer::StartAu(cursor, au.len, 12345U, false);
    PacketCopy packets[4];
    uint32_t count = 0U;
    RS_CHECK(Collect(au.data, au.len, empty, cursor, 0x01020304U, packets, 4U, count));
    RS_CHECK_EQ(count, 2U);
    RS_CHECK_EQ(packets[0].body_len, 1200U);  // 2-byte prefix + 1198 fragment bytes.
    RS_CHECK_EQ(packets[0].prefix_len, 2U);
    RS_CHECK_EQ(packets[0].payload_len, 1198U);
    RS_CHECK_EQ(packets[0].prefix[0], static_cast<uint8_t>(0x7CU));  // FU indicator: NRI | 28
    RS_CHECK_EQ(packets[0].prefix[1], static_cast<uint8_t>(0x85U));  // S=1, type 5
    // Zero-copy: the fragment points into the AU right after the NAL header.
    RS_CHECK(packets[0].payload_ptr == au.data + 5U);
    RS_CHECK(packets[0].prefix_ptr == cursor.fu_header.data());
    RS_CHECK(!packets[0].marker);
    RS_CHECK_EQ(packets[1].body_len, 4U);
    RS_CHECK_EQ(packets[1].prefix_len, 2U);
    RS_CHECK_EQ(packets[1].payload_len, 2U);
    RS_CHECK_EQ(packets[1].prefix[1], static_cast<uint8_t>(0x45U));  // E=1, type 5
    RS_CHECK(packets[1].payload_ptr == au.data + 5U + 1198U);
    RS_CHECK(packets[1].marker);
    RS_CHECK_EQ(Ts(packets[0]), 12345U);
    RS_CHECK_EQ(Ts(packets[1]), 12345U);
    RS_CHECK_EQ(Ssrc(packets[0]), 0x01020304U);
    uint8_t rebuilt[1300] = {};
    const uint32_t rebuilt_len = Reassemble(packets, count, rebuilt, sizeof(rebuilt));
    RS_CHECK_EQ(rebuilt_len, sizeof(nal));
    RS_CHECK_EQ(std::memcmp(rebuilt, nal, sizeof(nal)), 0);
  }

  // 2400 bytes spans three FU-A packets.
  {
    uint8_t nal[2400] = {};
    MakeNal(nal, 0x65, sizeof(nal));
    AuBuilder au;
    au.Nal(nal, sizeof(nal), false);  // 3-byte start code variant
    rs::ClientCursor cursor;
    rs::H264Packetizer::StartAu(cursor, au.len, 777U, false);
    PacketCopy packets[6];
    uint32_t count = 0U;
    RS_CHECK(Collect(au.data, au.len, empty, cursor, 9U, packets, 6U, count));
    RS_CHECK_EQ(count, 3U);
    RS_CHECK_EQ(packets[0].body_len, 1200U);
    RS_CHECK_EQ(packets[1].body_len, 1200U);
    RS_CHECK_EQ(packets[2].body_len, 5U);
    RS_CHECK_EQ(packets[0].payload_len, 1198U);
    RS_CHECK_EQ(packets[1].payload_len, 1198U);
    RS_CHECK_EQ(packets[2].payload_len, 3U);
    RS_CHECK_EQ(packets[0].prefix[1], static_cast<uint8_t>(0x85U));  // first
    RS_CHECK_EQ(packets[1].prefix[1], static_cast<uint8_t>(0x05U));  // middle
    RS_CHECK_EQ(packets[2].prefix[1], static_cast<uint8_t>(0x45U));  // last
    // Zero-copy: every fragment is a disjoint view into the AU.
    RS_CHECK(packets[0].payload_ptr == au.data + 4U);
    RS_CHECK(packets[1].payload_ptr == au.data + 1202U);
    RS_CHECK(packets[2].payload_ptr == au.data + 2400U);
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
    uint8_t idr[1201] = {};
    MakeNal(idr, 0x65, sizeof(idr));
    uint8_t p_frame[100] = {};
    MakeNal(p_frame, 0x41, sizeof(p_frame));
    AuBuilder au;
    au.Nal(sps, sizeof(sps), false);
    au.Nal(pps, sizeof(pps), true);
    au.Nal(idr, sizeof(idr), true);
    au.Nal(p_frame, sizeof(p_frame), false);
    rs::ClientCursor cursor;
    rs::H264Packetizer::StartAu(cursor, au.len, 500U, false);
    PacketCopy packets[8];
    uint32_t count = 0U;
    RS_CHECK(Collect(au.data, au.len, empty, cursor, 0x55U, packets, 8U, count));
    RS_CHECK_EQ(count, 5U);
    uint32_t markers = 0U;
    for (uint32_t i = 0U; i < count; ++i) {
      RS_CHECK(packets[i].body_len <= rs::kRtpPayloadMax);
      RS_CHECK_EQ(Ts(packets[i]), 500U);
      RS_CHECK_EQ(Seq(packets[i]), static_cast<uint16_t>(i));
      if (packets[i].marker) {
        ++markers;
        RS_CHECK_EQ(i, count - 1U);
      }
    }
    RS_CHECK_EQ(markers, 1U);
    RS_CHECK_EQ(packets[0].prefix_len, 0U);  // SPS is a single NAL unit packet.
    RS_CHECK_EQ(packets[0].payload_len, sizeof(sps));
    RS_CHECK_EQ(std::memcmp(packets[0].payload, sps, sizeof(sps)), 0);
    RS_CHECK_EQ(packets[1].payload_len, sizeof(pps));
    RS_CHECK_EQ(std::memcmp(packets[1].payload, pps, sizeof(pps)), 0);
    RS_CHECK_EQ(packets[4].payload_len, sizeof(p_frame));
    RS_CHECK_EQ(std::memcmp(packets[4].payload, p_frame, sizeof(p_frame)), 0);
    uint8_t rebuilt[1300] = {};
    const uint32_t rebuilt_len = Reassemble(&packets[2], 2U, rebuilt, sizeof(rebuilt));
    RS_CHECK_EQ(rebuilt_len, sizeof(idr));
    RS_CHECK_EQ(std::memcmp(rebuilt, idr, sizeof(idr)), 0);
  }

  // Parameter set injection precedes the first slice packet.
  {
    uint8_t idr[100] = {};
    MakeNal(idr, 0x65, sizeof(idr));
    AuBuilder au;
    au.Nal(idr, sizeof(idr), true);
    rs::ClientCursor cursor;
    cursor.seq = 10U;
    rs::H264Packetizer::StartAu(cursor, au.len, 4321U, true);
    PacketCopy packets[4];
    uint32_t count = 0U;
    RS_CHECK(Collect(au.data, au.len, configured, cursor, 0x77U, packets, 4U, count));
    RS_CHECK_EQ(count, 3U);
    RS_CHECK_EQ(packets[0].payload_len, sizeof(sps));
    RS_CHECK_EQ(std::memcmp(packets[0].payload, sps, sizeof(sps)), 0);
    RS_CHECK(!packets[0].marker);
    RS_CHECK_EQ(packets[1].payload_len, sizeof(pps));
    RS_CHECK_EQ(std::memcmp(packets[1].payload, pps, sizeof(pps)), 0);
    RS_CHECK(!packets[1].marker);
    RS_CHECK_EQ(packets[2].payload_len, sizeof(idr));
    RS_CHECK(packets[2].marker);
    RS_CHECK_EQ(Seq(packets[0]), static_cast<uint16_t>(10U));
    RS_CHECK_EQ(Seq(packets[1]), static_cast<uint16_t>(11U));
    RS_CHECK_EQ(Seq(packets[2]), static_cast<uint16_t>(12U));
    RS_CHECK_EQ(Ts(packets[2]), 4321U);
  }

  // Injection without parameter sets fails instead of sending an empty SPS.
  {
    uint8_t idr[20] = {};
    MakeNal(idr, 0x65, sizeof(idr));
    AuBuilder au;
    au.Nal(idr, sizeof(idr), true);
    rs::ClientCursor cursor;
    rs::H264Packetizer::StartAu(cursor, au.len, 1U, true);
    const auto r = rs::H264Packetizer::Next(au.data, au.len, empty, cursor, 1U);
    RS_CHECK(!r.has_value());
    RS_CHECK_EQ(r.get_error(), rs::PacketizeError::kNotConfigured);
    RS_CHECK(!cursor.in_au);
  }

  // StartAu field contract and ResetCursor wait-for-IDR behaviour.
  {
    rs::ClientCursor cursor;
    cursor.seq = 0xFFFFU;
    rs::H264Packetizer::StartAu(cursor, 123U, 456U, true);
    RS_CHECK_EQ(cursor.nal_offset, 0U);
    RS_CHECK_EQ(cursor.frag_offset, 0U);
    RS_CHECK_EQ(cursor.au_len, 123U);
    RS_CHECK_EQ(cursor.ts, 456U);
    RS_CHECK(cursor.in_au);
    RS_CHECK_EQ(cursor.param_stage, static_cast<uint8_t>(0));
    RS_CHECK(!cursor.marker_sent);
    rs::H264Packetizer::StartAu(cursor, 123U, 456U, false);
    RS_CHECK_EQ(cursor.param_stage, static_cast<uint8_t>(2));
    rs::H264Packetizer::ResetCursor(cursor);
    RS_CHECK(!cursor.in_au);
    RS_CHECK(cursor.await_idr);
    RS_CHECK_EQ(cursor.frag_offset, 0U);
    RS_CHECK_EQ(cursor.param_stage, static_cast<uint8_t>(2));
    RS_CHECK_EQ(cursor.seq, static_cast<uint16_t>(0xFFFFU));  // Sequence is not reset.
  }

  // 16-bit sequence wrap is natural.
  {
    uint8_t nal[10] = {};
    MakeNal(nal, 0x41, sizeof(nal));
    AuBuilder au;
    au.Nal(nal, sizeof(nal), true);
    rs::ClientCursor cursor;
    cursor.seq = 0xFFFFU;
    rs::H264Packetizer::StartAu(cursor, au.len, 1U, false);
    const auto r = rs::H264Packetizer::Next(au.data, au.len, empty, cursor, 1U);
    RS_CHECK(r.has_value());
    RS_CHECK_EQ(static_cast<uint16_t>((static_cast<uint16_t>(r.value().header[2]) << 8U) |
                                      static_cast<uint16_t>(r.value().header[3])),
                static_cast<uint16_t>(0xFFFFU));
    RS_CHECK_EQ(cursor.seq, static_cast<uint16_t>(0U));
  }

  return 0;
}
