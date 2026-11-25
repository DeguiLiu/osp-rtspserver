// Fixed-table per-IP token buckets for accept and request admission.
// No allocation; entries are reclaimed by oldest-last-use when full.
// SPDX-License-Identifier: MIT
#ifndef RS_IP_RATE_LIMITER_HPP_
#define RS_IP_RATE_LIMITER_HPP_

#include "rs/config.hpp"

#include <cstdint>

namespace rs {

class IpRateLimiter final {
 public:
  [[nodiscard]] bool AllowAccept(uint32_t ip, uint64_t now_ns) noexcept {
    return Allow(ip, now_ns, true);
  }
  [[nodiscard]] bool AllowRequest(uint32_t ip, uint64_t now_ns) noexcept {
    return Allow(ip, now_ns, false);
  }

 private:
  struct Entry {
    uint32_t ip = 0U;
    uint64_t accept_last_ns = 0U;
    uint64_t request_last_ns = 0U;
    uint32_t accept_tokens = 0U;
    uint32_t request_tokens = 0U;
    bool used = false;
  };

  static void Refill(uint32_t& tokens, uint64_t& last_ns, uint64_t now_ns, uint32_t burst,
                     uint32_t rate_per_sec) noexcept {
    if (last_ns == 0U) {
      tokens = burst;
      last_ns = now_ns;
      return;
    }
    if (now_ns > last_ns) {
      const uint64_t add = ((now_ns - last_ns) * rate_per_sec) / kNsPerSec;
      const uint64_t next = static_cast<uint64_t>(tokens) + add;
      tokens = (next > burst) ? burst : static_cast<uint32_t>(next);
      last_ns = now_ns;
    }
  }

  [[nodiscard]] Entry& Find(uint32_t ip, uint64_t now_ns) noexcept {
    Entry* oldest = &entries_[0];
    for (uint32_t i = 0U; i < kIpRateTableSize; ++i) {
      if (entries_[i].used && entries_[i].ip == ip) {
        return entries_[i];
      }
      if (!entries_[i].used) {
        entries_[i] = Entry{};
        entries_[i].ip = ip;
        entries_[i].used = true;
        return entries_[i];
      }
      const uint64_t age = (entries_[i].accept_last_ns > entries_[i].request_last_ns)
                               ? entries_[i].accept_last_ns
                               : entries_[i].request_last_ns;
      const uint64_t oldest_age = (oldest->accept_last_ns > oldest->request_last_ns)
                                      ? oldest->accept_last_ns
                                      : oldest->request_last_ns;
      if (age < oldest_age) {
        oldest = &entries_[i];
      }
    }
    static_cast<void>(now_ns);
    *oldest = Entry{};
    oldest->ip = ip;
    oldest->used = true;
    return *oldest;
  }

  [[nodiscard]] bool Allow(uint32_t ip, uint64_t now_ns, bool accept) noexcept {
    Entry& e = Find(ip, now_ns);
    if (accept) {
      Refill(e.accept_tokens, e.accept_last_ns, now_ns, kIpAcceptBurst, kIpAcceptRatePerSec);
      if (e.accept_tokens == 0U) {
        return false;
      }
      --e.accept_tokens;
      return true;
    }
    Refill(e.request_tokens, e.request_last_ns, now_ns, kIpRequestBurst, kIpRequestRatePerSec);
    if (e.request_tokens == 0U) {
      return false;
    }
    --e.request_tokens;
    return true;
  }

  Entry entries_[kIpRateTableSize];
};

}  // namespace rs

#endif  // RS_IP_RATE_LIMITER_HPP_
