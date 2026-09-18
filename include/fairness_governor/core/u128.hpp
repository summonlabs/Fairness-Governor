// Fairness Governor - portable exact 128-bit multiply/divide primitives.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The authoritative fairness arithmetic is exact integer arithmetic. Entitlement
// is a weighted share of a whole, which requires a 128-bit intermediate product
// that no 64-bit type can hold. Rather than depend on a compiler intrinsic or on
// __int128 (absent on MSVC), the primitives below are implemented portably from
// 32-bit limbs and bitwise restoring division. They are constexpr, deterministic,
// and exhaustively cross-checked by the property tests.
#ifndef FAIRNESS_GOVERNOR_CORE_U128_HPP
#define FAIRNESS_GOVERNOR_CORE_U128_HPP

#include <cstdint>

namespace fairness_governor {

/// Unsigned 128-bit value as two 64-bit halves.
struct U128 {
  std::uint64_t lo{0};
  std::uint64_t hi{0};

  [[nodiscard]] constexpr bool is_zero() const noexcept { return lo == 0 && hi == 0; }
  [[nodiscard]] constexpr bool fits_u64() const noexcept { return hi == 0; }

  friend constexpr bool operator==(const U128& a, const U128& b) noexcept {
    return a.lo == b.lo && a.hi == b.hi;
  }
  friend constexpr bool operator!=(const U128& a, const U128& b) noexcept { return !(a == b); }
};

/// Exact 64x64 -> 128 unsigned product (Hacker's Delight style limb expansion).
[[nodiscard]] constexpr U128 mul_wide(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a_lo = a & 0xFFFFFFFFULL;
  const std::uint64_t a_hi = a >> 32;
  const std::uint64_t b_lo = b & 0xFFFFFFFFULL;
  const std::uint64_t b_hi = b >> 32;

  const std::uint64_t p0 = a_lo * b_lo;
  const std::uint64_t p1 = a_lo * b_hi;
  const std::uint64_t p2 = a_hi * b_lo;
  const std::uint64_t p3 = a_hi * b_hi;

  const std::uint64_t mid = p1 + (p0 >> 32);
  const std::uint64_t mid2 = p2 + (mid & 0xFFFFFFFFULL);
  const std::uint64_t carry = (mid2 >> 32) + (mid >> 32);

  U128 result;
  result.lo = (mid2 << 32) | (p0 & 0xFFFFFFFFULL);
  result.hi = p3 + carry;
  return result;
}

/// Result of a 128/64 division.
struct Div128Result {
  U128 quotient{};
  std::uint64_t remainder{0};
  bool divide_by_zero{false};
};

/// Exact 128/64 division with an exact remainder, by binary restoring division.
/// 128 iterations of constant work: deterministic and branch-predictable, and
/// small enough that the benchmark measures real evaluation cost.
[[nodiscard]] constexpr Div128Result div_wide(U128 numerator, std::uint64_t denominator) noexcept {
  Div128Result result;
  if (denominator == 0) {
    result.divide_by_zero = true;
    return result;
  }
  U128 quotient{};
  std::uint64_t remainder = 0;
  for (int bit = 127; bit >= 0; --bit) {
    const bool incoming = (bit >= 64) ? (((numerator.hi) >> (bit - 64)) & 1ULL) != 0
                                      : (((numerator.lo) >> bit) & 1ULL) != 0;
    const bool carry_out = (remainder >> 63) != 0;
    remainder = (remainder << 1) | (incoming ? 1ULL : 0ULL);
    if (carry_out || remainder >= denominator) {
      // Unsigned wrap-around is well defined and yields exactly
      // (2^64 + remainder) - denominator when carry_out is set.
      remainder -= denominator;
      if (bit >= 64) {
        quotient.hi |= (1ULL << (bit - 64));
      } else {
        quotient.lo |= (1ULL << bit);
      }
    }
  }
  result.quotient = quotient;
  result.remainder = remainder;
  return result;
}

/// Outcome of an exact `value * multiplier / divisor` evaluation.
struct MulDivResult {
  std::uint64_t value{0};
  std::uint64_t remainder{0};
  bool overflow{false};
  bool divide_by_zero{false};

  [[nodiscard]] constexpr bool ok() const noexcept { return !overflow && !divide_by_zero; }
};

/// Exact floor((value * multiplier) / divisor) with overflow detection.
[[nodiscard]] constexpr MulDivResult mul_div(std::uint64_t value, std::uint64_t multiplier,
                                             std::uint64_t divisor) noexcept {
  MulDivResult result;
  if (divisor == 0) {
    result.divide_by_zero = true;
    return result;
  }
  const U128 product = mul_wide(value, multiplier);
  const Div128Result divided = div_wide(product, divisor);
  if (!divided.quotient.fits_u64()) {
    result.overflow = true;
    return result;
  }
  result.value = divided.quotient.lo;
  result.remainder = divided.remainder;
  return result;
}

/// True when `value * multiplier` is exactly divisible by `divisor`.
[[nodiscard]] constexpr bool mul_div_is_exact(std::uint64_t value, std::uint64_t multiplier,
                                              std::uint64_t divisor) noexcept {
  const MulDivResult r = mul_div(value, multiplier, divisor);
  return r.ok() && r.remainder == 0;
}

/// Signed exact floor-division of `value` by `divisor` (truncation toward
/// negative infinity is not used; the governor uses truncation toward zero and
/// documents it, because entitlement rounding must never invent units).
[[nodiscard]] constexpr std::int64_t div_trunc(std::int64_t value, std::int64_t divisor) noexcept {
  if (divisor == 0) {
    return 0;
  }
  return value / divisor;
}

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_CORE_U128_HPP
