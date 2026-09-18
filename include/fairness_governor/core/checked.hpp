// Fairness Governor - checked integer arithmetic for externally influenced sizes.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef FAIRNESS_GOVERNOR_CORE_CHECKED_HPP
#define FAIRNESS_GOVERNOR_CORE_CHECKED_HPP

#include <cstdint>
#include <limits>
#include <type_traits>

namespace fairness_governor {

/// Checked unsigned addition: returns false and leaves `out` untouched on overflow.
template <class T>
[[nodiscard]] constexpr bool checked_add(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_add is defined for unsigned types");
  if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
    return false;
  }
  out = static_cast<T>(a + b);
  return true;
}

/// Checked unsigned multiplication.
template <class T>
[[nodiscard]] constexpr bool checked_mul(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_mul is defined for unsigned types");
  if (a != 0 && b > static_cast<T>(std::numeric_limits<T>::max() / a)) {
    return false;
  }
  out = static_cast<T>(a * b);
  return true;
}

/// Checked unsigned subtraction; returns false when it would go negative.
template <class T>
[[nodiscard]] constexpr bool checked_sub(T a, T b, T& out) noexcept {
  static_assert(std::is_unsigned_v<T>, "checked_sub is defined for unsigned types");
  if (b > a) {
    return false;
  }
  out = static_cast<T>(a - b);
  return true;
}

/// Checked signed addition.
[[nodiscard]] constexpr bool checked_add_signed(std::int64_t a, std::int64_t b,
                                                std::int64_t& out) noexcept {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return false;
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return false;
  }
  out = a + b;
  return true;
}

/// Checked signed subtraction.
[[nodiscard]] constexpr bool checked_sub_signed(std::int64_t a, std::int64_t b,
                                                std::int64_t& out) noexcept {
  if (b == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  return checked_add_signed(a, -b, out);
}

/// Checked signed multiplication (exact, no wrapping).
[[nodiscard]] constexpr bool checked_mul_signed(std::int64_t a, std::int64_t b,
                                                std::int64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  if (a == -1 && b == kMin) {
    return false;
  }
  if (b == -1 && a == kMin) {
    return false;
  }
  if (a > 0) {
    if (b > 0) {
      if (a > kMax / b) {
        return false;
      }
    } else {
      if (b < kMin / a) {
        return false;
      }
    }
  } else {
    if (b > 0) {
      if (a < kMin / b) {
        return false;
      }
    } else {
      if (a != 0 && b < kMax / a) {
        return false;
      }
    }
  }
  out = a * b;
  return true;
}

/// Checked unsigned narrowing cast.
template <class To, class From>
[[nodiscard]] constexpr bool narrow(From value, To& out) noexcept {
  static_assert(std::is_integral_v<From> && std::is_integral_v<To>);
  if constexpr (std::is_signed_v<From>) {
    if (value < 0) {
      return false;
    }
  }
  const auto unsigned_value = static_cast<std::uint64_t>(value);
  if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<To>::max())) {
    return false;
  }
  out = static_cast<To>(unsigned_value);
  return true;
}

/// Absolute value of a signed 64-bit integer as an unsigned magnitude.
[[nodiscard]] constexpr std::uint64_t magnitude(std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1ULL;
}

/// Saturating unsigned add used only where saturation is explicitly reported.
[[nodiscard]] constexpr std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  std::uint64_t out = 0;
  if (!checked_add(a, b, out)) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return out;
}

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_CORE_CHECKED_HPP
