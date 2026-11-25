// Deterministic H.264 / H.265 Annex B test stream generator.
// Writes <out> and <out>.expect; the expected file has one line per access
// unit: "<au_index> <is_idr> <au_bytes> <fnv1a64_hex>". The H.265 variant adds
// a leading "# codec=H265" line; the H.264 output is byte-identical to the
// phase-1 generator so existing regression hashes stay valid.
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vector>

namespace {

constexpr uint64_t kSeed = UINT64_C(0x9E3779B97F4A7C15);
constexpr uint32_t kDefaultAus = 250U;
constexpr uint32_t kDefaultGop = 25U;
constexpr uint32_t kDefaultFps = 25U;
constexpr uint32_t kMaxAus = 100000U;
constexpr uint32_t kStartCodeBytes = 4U;

enum class Codec : uint8_t { kH264 = 0U, kH265 = 1U };

// Baseline profile 0x42, constraint flags 0x00, level 1F (Main level).
constexpr uint8_t kSps[] = {0x67, 0x42, 0x00, 0x1F, 0xE9, 0x02, 0x83, 0xF0, 0x0A, 0x0F, 0xA8, 0x0F, 0x10, 0x00,
                            0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x03, 0x03, 0xC8, 0xF1, 0x83, 0x19, 0x60};
constexpr uint8_t kPps[] = {0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0};
constexpr uint8_t kAud[] = {0x09, 0xF0};

// H.265 access unit delimiter: 2-byte NAL header, nal_unit_type 35.
constexpr uint8_t kHevcAud[] = {0x46, 0x01};
// VPS/SPS/PPS captured from libx265 (320x240, keyint 25, repeat-headers=1).
// Real parameter sets keep `ffprobe` able to report hevc/320x240 for the
// synthetic file even though the slice payloads are random bytes.
constexpr uint8_t kHevcVps[] = {0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00,
                                0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x3C, 0x95, 0x98, 0x09};
constexpr uint8_t kHevcSps[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00,
                                0x00, 0x03, 0x00, 0x3C, 0xA0, 0x0A, 0x08, 0x0F, 0x16, 0x59, 0x59, 0xA4, 0x93, 0x2B,
                                0xC0, 0x5A, 0x02, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x03, 0x00, 0x32, 0x10};
constexpr uint8_t kHevcPps[] = {0x44, 0x01, 0xC1, 0x72, 0xB4, 0x62, 0x40};

// HEVC NAL headers (F=0, layer 0, temporal_id_plus1=1).
constexpr uint8_t kHevcIdrHeader = 0x26U;   // IDR_W_RADL (19)
constexpr uint8_t kHevcTrailHeader = 0x02U; // TRAIL_R (1)

class Rng {
 public:
  explicit Rng(uint64_t seed) noexcept : state_(seed) {}

  uint64_t Next() noexcept {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return state_;
  }

  uint32_t Range(uint32_t low, uint32_t high) noexcept {
    const uint32_t span = high - low + 1U;
    return low + static_cast<uint32_t>(Next() % span);
  }

 private:
  uint64_t state_;
};

uint64_t Fnv1a64(const uint8_t* data, size_t len) noexcept {
  uint64_t hash = UINT64_C(0xCBF29CE484222325);
  for (size_t i = 0U; i < len; ++i) {
    hash ^= data[i];
    hash *= UINT64_C(0x100000001B3);
  }
  return hash;
}

void AppendNal(std::vector<uint8_t>& au, const uint8_t* nal, size_t len) {
  static const uint8_t kStartCode[kStartCodeBytes] = {0x00, 0x00, 0x00, 0x01};
  au.insert(au.end(), kStartCode, kStartCode + kStartCodeBytes);
  au.insert(au.end(), nal, nal + len);
}

// Fills a synthetic slice with bytes in [0x20, 0xFE]; avoiding 0x00 keeps the
// Annex B start-code scan unambiguous without emulation-prevention bytes.
void AppendSlice(std::vector<uint8_t>& au, uint8_t nal_header, uint32_t payload_bytes, Rng& rng) {
  std::vector<uint8_t> nal;
  nal.reserve(static_cast<size_t>(payload_bytes) + 1U);
  nal.push_back(nal_header);
  for (uint32_t i = 0U; i < payload_bytes; ++i) {
    nal.push_back(static_cast<uint8_t>(0x20U + (rng.Next() % 0xDFU)));
  }
  AppendNal(au, nal.data(), nal.size());
}

// H.265 carries a 2-byte NAL header; the payload uses the same 0x20..0xFE range
// so a start-code scan stays unambiguous.
void AppendHevcSlice(std::vector<uint8_t>& au, uint8_t nal_type_header, uint32_t payload_bytes, Rng& rng) {
  std::vector<uint8_t> nal;
  nal.reserve(static_cast<size_t>(payload_bytes) + 2U);
  nal.push_back(nal_type_header);
  nal.push_back(0x01U);
  for (uint32_t i = 0U; i < payload_bytes; ++i) {
    nal.push_back(static_cast<uint8_t>(0x20U + (rng.Next() % 0xDFU)));
  }
  AppendNal(au, nal.data(), nal.size());
}

bool ParseU32(const char* text, uint32_t& out) noexcept {
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  const bool ok = (nullptr != end) && (0 == *end) && (value <= 0xFFFFFFFFUL);
  if (ok) {
    out = static_cast<uint32_t>(value);
  }
  return ok;
}

void Usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s <out.h264|out.h265> [--aus N] [--gop G] [--fps F] [--codec h264|h265]\n",
               argv0);
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t aus = kDefaultAus;
  uint32_t gop = kDefaultGop;
  uint32_t fps = kDefaultFps;
  Codec codec = Codec::kH264;
  const char* out_path = nullptr;
  bool args_ok = (argc >= 2);
  for (int i = 1; i < argc && args_ok; ++i) {
    if (0 == std::strcmp(argv[i], "--aus") && i + 1 < argc) {
      args_ok = ParseU32(argv[++i], aus) && (aus > 0U) && (aus <= kMaxAus);
    } else if (0 == std::strcmp(argv[i], "--gop") && i + 1 < argc) {
      args_ok = ParseU32(argv[++i], gop) && (gop > 0U) && (gop <= aus);
    } else if (0 == std::strcmp(argv[i], "--fps") && i + 1 < argc) {
      args_ok = ParseU32(argv[++i], fps) && (fps > 0U) && (fps <= 1000U);
    } else if (0 == std::strcmp(argv[i], "--codec") && i + 1 < argc) {
      const char* value = argv[++i];
      if (0 == std::strcmp(value, "h264")) {
        codec = Codec::kH264;
      } else if (0 == std::strcmp(value, "h265")) {
        codec = Codec::kH265;
      } else {
        args_ok = false;
      }
    } else if (nullptr == out_path && argv[i][0] != '-') {
      out_path = argv[i];
    } else {
      args_ok = false;
    }
  }
  if (!args_ok || nullptr == out_path) {
    Usage(argv[0]);
    return 2;
  }

  char expect_path[4096];
  const int path_len = std::snprintf(expect_path, sizeof(expect_path), "%s.expect", out_path);
  if (path_len < 0 || static_cast<size_t>(path_len) >= sizeof(expect_path)) {
    std::fprintf(stderr, "make_test_stream: output path too long\n");
    return 2;
  }
  std::FILE* stream_file = std::fopen(out_path, "wb");
  std::FILE* expect_file = (nullptr != stream_file) ? std::fopen(expect_path, "wb") : nullptr;
  if (nullptr == stream_file || nullptr == expect_file) {
    std::fprintf(stderr, "make_test_stream: cannot open %s or %s\n", out_path, expect_path);
    if (nullptr != stream_file) {
      std::fclose(stream_file);
    }
    return 2;
  }

  Rng rng(kSeed);
  uint64_t total_bytes = 0U;
  bool write_ok = true;
  if (Codec::kH265 == codec) {
    write_ok = std::fprintf(expect_file, "# codec=H265\n") > 0;
  }
  for (uint32_t au_index = 0U; au_index < aus && write_ok; ++au_index) {
    const bool is_idr = (0U == (au_index % gop));
    std::vector<uint8_t> au;
    au.reserve(8192U);
    if (Codec::kH265 == codec) {
      // H.265: AUD, then VPS/SPS/PPS before every IRAP AU, one slice.
      AppendNal(au, kHevcAud, sizeof(kHevcAud));
      if (is_idr) {
        AppendNal(au, kHevcVps, sizeof(kHevcVps));
        AppendNal(au, kHevcSps, sizeof(kHevcSps));
        AppendNal(au, kHevcPps, sizeof(kHevcPps));
        AppendHevcSlice(au, kHevcIdrHeader, rng.Range(3000U, 8000U), rng);
      } else {
        // Same mix as H.264: plain TRAIL_R, >1200-byte NAL (forces FU) and
        // multi-NAL access units.
        const bool large = (5U == (au_index % 17U));
        AppendHevcSlice(au, kHevcTrailHeader, rng.Range(large ? 1400U : 400U, large ? 1900U : 1500U), rng);
        if (3U == (au_index % 7U)) {
          AppendHevcSlice(au, kHevcTrailHeader, rng.Range(300U, 600U), rng);
        }
      }
    } else {
      AppendNal(au, kAud, sizeof(kAud));
      if (is_idr) {
        AppendNal(au, kSps, sizeof(kSps));
        AppendNal(au, kPps, sizeof(kPps));
        AppendSlice(au, 0x65U, rng.Range(3000U, 8000U), rng);
      } else {
        // Mix plain slices, FU-A sized slices (>1200 bytes) and multi-NAL AUs.
        const bool large = (5U == (au_index % 17U));
        AppendSlice(au, 0x41U, rng.Range(large ? 1400U : 400U, large ? 1900U : 1500U), rng);
        if (3U == (au_index % 7U)) {
          AppendSlice(au, 0x41U, rng.Range(300U, 600U), rng);
        }
      }
    }
    const uint64_t hash = Fnv1a64(au.data(), au.size());
    write_ok = (std::fwrite(au.data(), 1U, au.size(), stream_file) == au.size()) &&
               (std::fprintf(expect_file, "%u %u %zu %016llx\n", au_index, is_idr ? 1U : 0U, au.size(),
                             static_cast<unsigned long long>(hash)) > 0);
    total_bytes += au.size();
  }
  const bool close_ok = (0 == std::fclose(stream_file)) && (0 == std::fclose(expect_file));
  if (!write_ok || !close_ok) {
    std::fprintf(stderr, "make_test_stream: write failed\n");
    return 2;
  }
  std::printf("make_test_stream: %s, %u AUs, %llu bytes, gop=%u fps=%u (~%.1f s)\n",
              (Codec::kH265 == codec) ? "H265" : "H264", aus,
              static_cast<unsigned long long>(total_bytes), gop, fps,
              static_cast<double>(aus) / static_cast<double>(fps));
  return 0;
}
