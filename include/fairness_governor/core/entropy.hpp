// Fairness Governor - process incarnation identities.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A boot identity is generated once per process and is never persisted, never
// restored, and never accepted from a peer as proof of anything except "this is
// a different process". It exists so that stale authority can be fenced.
#ifndef FAIRNESS_GOVERNOR_CORE_ENTROPY_HPP
#define FAIRNESS_GOVERNOR_CORE_ENTROPY_HPP

#include "fairness_governor/core/ids.hpp"

namespace fairness_governor {

/// Generates a fresh, non-zero boot identity for this process.
[[nodiscard]] BootId make_boot_id() noexcept;

/// Generates a fresh, valid incarnation for this process.
[[nodiscard]] Incarnation make_incarnation() noexcept;

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_CORE_ENTROPY_HPP
