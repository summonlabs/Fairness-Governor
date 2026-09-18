// Fairness Governor - checked arithmetic tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <limits>

#include "fairness_governor/core/checked.hpp"
#include "test_harness.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(checked, add_detects_overflow) {
  std::uint64_t out = 0;
  FG_CHECK(checked_add<std::uint64_t>(1, 2, out));
  FG_CHECK_EQ(out, 3u);
  FG_CHECK(!checked_add(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1}, out));
  std::uint32_t narrow_out = 0;
  FG_CHECK(!checked_add(std::numeric_limits<std::uint32_t>::max(), std::uint32_t{1}, narrow_out));
}

FG_TEST(checked, mul_detects_overflow) {
  std::uint64_t out = 0;
  FG_CHECK(checked_mul(std::uint64_t{6}, std::uint64_t{7}, out));
  FG_CHECK_EQ(out, 42u);
  FG_CHECK(!checked_mul(std::numeric_limits<std::uint64_t>::max(), std::uint64_t{2}, out));
  FG_CHECK(checked_mul(std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max(), out));
  FG_CHECK_EQ(out, 0u);
}

FG_TEST(checked, sub_detects_underflow) {
  std::uint64_t out = 0;
  FG_CHECK(checked_sub(std::uint64_t{5}, std::uint64_t{3}, out));
  FG_CHECK_EQ(out, 2u);
  FG_CHECK(!checked_sub(std::uint64_t{3}, std::uint64_t{5}, out));
}

FG_TEST(checked, signed_add_mul_are_exact) {
  std::int64_t out = 0;
  FG_CHECK(checked_add_signed(-5, 3, out));
  FG_CHECK_EQ(out, -2);
  FG_CHECK(!checked_add_signed(std::numeric_limits<std::int64_t>::max(), 1, out));
  FG_CHECK(!checked_add_signed(std::numeric_limits<std::int64_t>::min(), -1, out));
  FG_CHECK(checked_mul_signed(-6, 7, out));
  FG_CHECK_EQ(out, -42);
  FG_CHECK(!checked_mul_signed(std::numeric_limits<std::int64_t>::max(), 2, out));
  FG_CHECK(!checked_mul_signed(std::numeric_limits<std::int64_t>::min(), -1, out));
}

FG_TEST(checked, narrow_rejects_lossy_conversions) {
  std::uint32_t out = 0;
  FG_CHECK(narrow<std::uint32_t>(std::uint64_t{7}, out));
  FG_CHECK_EQ(out, 7u);
  FG_CHECK(!narrow<std::uint32_t>(std::uint64_t{1} << 40, out));
  FG_CHECK(!narrow<std::uint32_t>(std::int64_t{-1}, out));
  std::uint8_t byte = 0;
  FG_CHECK(narrow<std::uint8_t>(255, byte));
  FG_CHECK(!narrow<std::uint8_t>(256, byte));
}

FG_TEST(checked, magnitude_handles_the_signed_minimum) {
  FG_CHECK_EQ(magnitude(0), 0u);
  FG_CHECK_EQ(magnitude(-1), 1u);
  FG_CHECK_EQ(magnitude(std::numeric_limits<std::int64_t>::min()),
              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ULL);
}

FG_TEST(checked, saturating_add_saturates) {
  FG_CHECK_EQ(saturating_add(1, 2), 3u);
  FG_CHECK_EQ(saturating_add(std::numeric_limits<std::uint64_t>::max(), 1),
              std::numeric_limits<std::uint64_t>::max());
}
