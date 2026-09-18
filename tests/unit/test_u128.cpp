// Fairness Governor - exact 128-bit arithmetic tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <limits>

#include "fairness_governor/core/u128.hpp"
#include "test_harness.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

/// Independent reference product: repeated 128-bit doubling and addition.
/// Deliberately a different algorithm from the limb expansion under test. The
/// true product of two 64-bit values always fits in 128 bits, so the modular
/// arithmetic below is exact.
U128 reference_add(U128 a, U128 b) {
  const std::uint64_t low = a.lo + b.lo;
  const std::uint64_t carry = low < a.lo ? 1ULL : 0ULL;
  return U128{low, a.hi + b.hi + carry};
}

U128 reference_mul(std::uint64_t a, std::uint64_t b) {
  U128 result{0, 0};
  U128 addend{a, 0};
  for (int bit = 0; bit < 64; ++bit) {
    if (((b >> bit) & 1ULL) != 0) {
      result = reference_add(result, addend);
    }
    addend = reference_add(addend, addend);
  }
  return result;
}

}  // namespace

FG_TEST(u128, mul_known_vectors) {
  FG_CHECK_EQ(mul_wide(0, 0).lo, 0u);
  FG_CHECK_EQ(mul_wide(1, 1).lo, 1u);
  const U128 big = mul_wide(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL);
  // (2^64-1)^2 = 2^128 - 2^65 + 1 -> hi = 2^64 - 2, lo = 1.
  FG_CHECK_EQ(big.hi, 0xFFFFFFFFFFFFFFFEULL);
  FG_CHECK_EQ(big.lo, 1u);
  const U128 two_pow = mul_wide(1ULL << 63, 4);
  FG_CHECK_EQ(two_pow.hi, 2u);
  FG_CHECK_EQ(two_pow.lo, 0u);
}

FG_TEST(u128, mul_matches_reference_on_seeded_inputs) {
  Rng rng(0xC0FFEEULL);
  for (int i = 0; i < 4000; ++i) {
    std::uint64_t a = rng.next();
    std::uint64_t b = rng.next();
    if (i % 4 == 1) {
      a &= 0xFFFFFFFFULL;
    }
    if (i % 4 == 2) {
      b &= 0xFFFFULL;
    }
    if (i % 4 == 3) {
      a = 1ULL << (rng.below(64));
      b = 1ULL << (rng.below(64));
    }
    const U128 expected = reference_mul(a, b);
    const U128 actual = mul_wide(a, b);
    FG_CHECK_EQ(actual.lo, expected.lo);
    FG_CHECK_EQ(actual.hi, expected.hi);
  }
}

FG_TEST(u128, div_reconstructs_exactly) {
  Rng rng(0xBADC0DEULL);
  for (int i = 0; i < 4000; ++i) {
    const std::uint64_t low = rng.next();
    const std::uint64_t high = i % 3 == 0 ? 0 : (rng.next() >> (rng.below(64)));
    U128 numerator{low, high};
    std::uint64_t denominator = rng.next();
    if (denominator == 0) {
      denominator = 1;
    }
    if (i % 5 == 0) {
      denominator = 1ULL << rng.below(64);
    }
    const Div128Result divided = div_wide(numerator, denominator);
    FG_CHECK(!divided.divide_by_zero);
    FG_CHECK(divided.remainder < denominator);
    // quotient * denominator + remainder == numerator, exactly, in 128 bits.
    const U128 low_product = mul_wide(divided.quotient.lo, denominator);
    const std::uint64_t high_product = divided.quotient.hi * denominator;
    const U128 product{low_product.lo, low_product.hi + high_product};
    const std::uint64_t reconstructed_low = product.lo + divided.remainder;
    const std::uint64_t carry = reconstructed_low < product.lo ? 1ULL : 0ULL;
    FG_CHECK_EQ(reconstructed_low, numerator.lo);
    FG_CHECK_EQ(product.hi + carry, numerator.hi);
  }
}

FG_TEST(u128, div_by_zero_is_reported) {
  const Div128Result result = div_wide(U128{5, 0}, 0);
  FG_CHECK(result.divide_by_zero);
  const MulDivResult muldiv = mul_div(5, 5, 0);
  FG_CHECK(muldiv.divide_by_zero);
  FG_CHECK(!muldiv.ok());
}

FG_TEST(u128, mul_div_exactness_and_overflow) {
  FG_CHECK_EQ(mul_div(100, 3, 4).value, 75u);
  FG_CHECK_EQ(mul_div(100, 3, 4).remainder, 0u);
  FG_CHECK(mul_div_is_exact(100, 3, 4));
  FG_CHECK(!mul_div_is_exact(100, 1, 3));
  FG_CHECK_EQ(mul_div(10, 10, 3).value, 33u);
  FG_CHECK_EQ(mul_div(10, 10, 3).remainder, 1u);

  // A product that does not fit in 64 bits must be reported, not truncated.
  const MulDivResult overflow = mul_div(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 1);
  FG_CHECK(overflow.overflow);
  FG_CHECK(!overflow.ok());

  // The same product divided by a large denominator fits again.
  const MulDivResult fits = mul_div(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
                                    0xFFFFFFFFFFFFFFFFULL);
  FG_CHECK(fits.ok());
  FG_CHECK_EQ(fits.value, 0xFFFFFFFFFFFFFFFFULL);
}

FG_TEST(u128, mul_div_is_monotone_in_the_whole) {
  std::uint64_t previous = 0;
  for (std::uint64_t whole = 0; whole < 5000; ++whole) {
    const MulDivResult result = mul_div(whole, 7, 13);
    FG_CHECK(result.ok());
    FG_CHECK(result.value >= previous);
    previous = result.value;
  }
}

FG_TEST(u128, mul_div_never_exceeds_the_mathematical_quotient) {
  Rng rng(0x5EEDULL);
  for (int i = 0; i < 2000; ++i) {
    const std::uint64_t whole = rng.below(1u << 20);
    const std::uint64_t weight = 1 + rng.below(1u << 16);
    const std::uint64_t total = weight + rng.below(1u << 16);
    const MulDivResult result = mul_div(whole, weight, total);
    FG_CHECK(result.ok());
    FG_CHECK(result.value <= whole);
  }
}
