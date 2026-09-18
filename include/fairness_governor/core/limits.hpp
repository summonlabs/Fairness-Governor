// Fairness Governor - bounded-domain limits.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every externally influenced magnitude is bounded here. The bounds are not
// cosmetic: they are what makes the authoritative arithmetic provably
// overflow-free. The evaluator rejects any input that exceeds them, so an
// accepted input can never produce a negative or wrapped intermediate.
#ifndef FAIRNESS_GOVERNOR_CORE_LIMITS_HPP
#define FAIRNESS_GOVERNOR_CORE_LIMITS_HPP

#include <cstdint>

namespace fairness_governor {

/// Largest service magnitude representable for a single subject in a window,
/// in service units. 2^40 units keeps a whole population's sum far inside
/// std::int64_t while still being far larger than any real service window.
inline constexpr std::uint64_t kMaxServiceUnits = (std::uint64_t{1} << 40);

/// Largest number of subjects in one policy.
inline constexpr std::uint32_t kMaxSubjects = 4096;

/// Largest number of fairness groups in one policy.
inline constexpr std::uint32_t kMaxGroups = 256;

/// Largest group nesting depth (a root group has depth 1).
inline constexpr std::uint32_t kMaxGroupDepth = 8;

/// Largest share weight for a subject or group.
inline constexpr std::uint32_t kMaxShareWeight = 0x7FFFFFFFu;

/// Largest cumulative deficit/surplus carried per subject.
inline constexpr std::uint64_t kMaxCarryUnits = (std::uint64_t{1} << 48);

/// Largest number of distinct corrective intents emitted in one decision:
/// one augmentation plus one reduction per governed subject, at most.
inline constexpr std::uint32_t kMaxCorrectiveIntents = 2 * 4096;

/// Largest number of evidence records inside one snapshot.
inline constexpr std::uint32_t kMaxEvidenceRecords = 4096;

/// Largest durable history depth (windows retained for explanation).
inline constexpr std::uint32_t kMaxHistoryWindows = 1024;

/// Largest encoded durable record we will ever allocate for.
inline constexpr std::uint64_t kMaxDurablePayloadBytes = (std::uint64_t{1} << 22);  // 4 MiB

/// Largest frame payload accepted from a peer before the frame is rejected.
inline constexpr std::uint32_t kMaxFramePayloadBytes = (std::uint32_t{1} << 20) - 1;  // 1 MiB - 1

/// Largest rendered explanation, in bytes.
inline constexpr std::uint64_t kMaxExplanationBytes = (std::uint64_t{1} << 20);  // 1 MiB

/// Dimensionless fixed-point denominator used for basis-point modifiers.
inline constexpr std::int64_t kBasisPointScale = 10000;

/// Largest magnitude of a policy-defined priority/value modifier, in basis points.
inline constexpr std::int32_t kMaxPriorityModifierBps = 100000;  // +/- 1000%

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_CORE_LIMITS_HPP
