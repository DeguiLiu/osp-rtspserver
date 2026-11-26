// Minimal assertion helpers for host unit tests (no framework, no exceptions).
// SPDX-License-Identifier: MIT
#ifndef RS_TESTS_CHECK_HPP_
#define RS_TESTS_CHECK_HPP_

#include <cstdio>
#include <cstdlib>

#define RS_CHECK(cond)                                                              \
  do {                                                                              \
    if ((cond) == false) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                                     \
    }                                                                               \
  } while (0)

#define RS_CHECK_EQ(a, b)                                                                     \
  do {                                                                                        \
    const auto rs_a_ = (a);                                                                   \
    const auto rs_b_ = (b);                                                                   \
    if ((rs_a_ == rs_b_) == false) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s != %s\n", __FILE__, __LINE__, #a, #b); \
      return 1;                                                                               \
    }                                                                                         \
  } while (0)

#define RS_REQUIRE(cond)                                                              \
  do {                                                                                \
    if ((cond) == false) {                                                            \
      std::fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #cond); \
      std::abort();                                                                   \
    }                                                                                 \
  } while (0)

#endif  // RS_TESTS_CHECK_HPP_
