// Fairness Governor - process incarnation identity generation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/core/entropy.hpp"

#include <chrono>
#include <cstdint>
#include <random>

#include "fairness_governor/persist/fsutil.hpp"

namespace fairness_governor {

BootId make_boot_id() noexcept {
  std::uint64_t value = 0;
  std::random_device device;
  value = (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
  if (value == 0) {
    value = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()) ^
            fsutil::process_token();
  }
  if (value == 0) {
    value = 0x5DEECE66DULL;
  }
  return BootId::from_value(value);
}

Incarnation make_incarnation() noexcept { return Incarnation{make_boot_id(), 1}; }

}  // namespace fairness_governor
