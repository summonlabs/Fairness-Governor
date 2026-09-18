// Fairness Governor - version and build identity.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef FAIRNESS_GOVERNOR_VERSION_HPP
#define FAIRNESS_GOVERNOR_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace fairness_governor {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// Durable/on-wire format version. Independent from the product version so that
/// a format can be frozen while the product keeps moving.
inline constexpr std::uint16_t kFormatVersion = 1;

/// The ABI/API compatibility level of the public headers.
inline constexpr std::uint32_t kAbiVersion = 1;

[[nodiscard]] constexpr std::string_view version_string() noexcept {
  return "1.0.0";
}

[[nodiscard]] constexpr std::string_view library_name() noexcept {
  return "Fairness Governor";
}

[[nodiscard]] constexpr std::string_view vendor_name() noexcept {
  return "Summon Software Labs";
}

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_VERSION_HPP
