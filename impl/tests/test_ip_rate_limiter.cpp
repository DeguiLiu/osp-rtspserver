// Per-IP accept/request token bucket behavior.
// SPDX-License-Identifier: MIT
#include "check.hpp"

#include "rs/ip_rate_limiter.hpp"

int main() {
  rs::IpRateLimiter limiter;
  const uint32_t ip = 0x0100007FU;
  uint32_t allowed = 0U;
  while (limiter.AllowAccept(ip, 1000000000ULL)) {
    ++allowed;
  }
  RS_CHECK_EQ(allowed, rs::kIpAcceptBurst);

  // ~1 s later: rate/s tokens are available again.
  const uint64_t next = 1000000000ULL + rs::kNsPerSec;
  uint32_t refilled = 0U;
  while (limiter.AllowAccept(ip, next)) {
    ++refilled;
  }
  RS_CHECK_EQ(refilled, rs::kIpAcceptRatePerSec);

  uint32_t requests = 0U;
  while (limiter.AllowRequest(ip, next)) {
    ++requests;
  }
  RS_CHECK_EQ(requests, rs::kIpRequestBurst);

  // A different peer gets its own bucket.
  RS_CHECK(limiter.AllowAccept(0x0200007FU, next));
  std::printf("test_ip_rate_limiter: all checks passed\n");
  return 0;
}
