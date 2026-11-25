// Compile-time and runtime cache-line alignment checks for hot-path storage.
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/config.hpp"
#include "rs/file_source.hpp"
#include "rs/frame_ingress.hpp"
#include "rs/rtsp_parser.hpp"
#include "rs/tx_stream.hpp"

#include <cstdint>

static_assert((rs::kCacheLineBytes & (rs::kCacheLineBytes - 1U)) == 0U, "cache line power of two");
static_assert(alignof(rs::FrameIngress) >= rs::kCacheLineBytes, "frame pool alignment");
static_assert(alignof(rs::TxStream) >= rs::kCacheLineBytes, "tx stream alignment");
static_assert(alignof(rs::RtspParser) >= rs::kCacheLineBytes, "parser alignment");
static_assert(alignof(rs::FileSource) >= rs::kCacheLineBytes, "file source alignment");
static_assert(sizeof(rs::FrameDesc) <= 32U, "FrameDesc size");
static_assert(alignof(rs::FrameDesc) <= 8U, "FrameDesc alignment");
static_assert((sizeof(rs::FrameDesc) % alignof(rs::FrameDesc)) == 0U, "FrameDesc tail padding");

int main() {
  rs::FrameIngress ingress;
  auto writer = ingress.Acquire(16U);
  RS_CHECK(writer.has_value());
  const uintptr_t address = reinterpret_cast<uintptr_t>(writer.value().au);
  RS_CHECK_EQ(address % rs::kCacheLineBytes, 0U);
  ingress.Abort(writer.value());
  ingress.ReleaseProducerSlot();
  std::printf("test_alignment: all checks passed\n");
  return 0;
}
