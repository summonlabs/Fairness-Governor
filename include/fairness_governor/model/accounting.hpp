// Fairness Governor - durable deficit/credit accounting.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The accounting is the only durable fairness state. It closes exactly:
//
//   cumulative_deficit - cumulative_surplus
//       == sum(window net) + discarded_deficit - discarded_surplus
//
// where net_i = entitlement_i - served_i and the discarded terms record carry
// that hit the configured bound. The property tests assert this identity after
// every randomized ingest, and again after a restart.
#ifndef FAIRNESS_GOVERNOR_MODEL_ACCOUNTING_HPP
#define FAIRNESS_GOVERNOR_MODEL_ACCOUNTING_HPP

#include <cstdint>
#include <vector>

#include "fairness_governor/core/ids.hpp"
#include "fairness_governor/core/limits.hpp"
#include "fairness_governor/model/refs.hpp"

namespace fairness_governor {

/// Durable per-subject accounting.
struct SubjectAccounting {
  SubjectId id{};
  SubjectGeneration generation{};

  /// Carried deficit (owed service) in units.
  std::uint64_t cumulative_deficit{0};
  /// Carried surplus (credit against future deficit) in units.
  std::uint64_t cumulative_surplus{0};
  /// Total discarded deficit due to the carry bound, for exact closure.
  std::uint64_t discarded_deficit{0};
  /// Total discarded surplus due to the carry bound, for exact closure.
  std::uint64_t discarded_surplus{0};

  /// Governor-maintained consecutive fully-unserved windows.
  std::uint32_t unserved_streak{0};
  /// Governor-maintained consecutive below-floor windows.
  std::uint32_t below_floor_streak{0};

  /// Windows observed since the subject entered the policy.
  std::uint64_t windows_observed{0};
  /// Total served units observed since the subject entered the policy.
  std::uint64_t total_served_units{0};

  /// Last window in which a corrective intent named this subject, and the
  /// generation of that intent. Used for cooldown.
  ServiceWindowId last_correction_window{};
  InterventionGeneration last_correction_generation{};

  friend bool operator==(const SubjectAccounting& a, const SubjectAccounting& b);
  friend bool operator!=(const SubjectAccounting& a, const SubjectAccounting& b) {
    return !(a == b);
  }
};

/// Durable accounting state for a whole policy population.
struct FairnessAccounting {
  FairnessPolicyId policy_id{};
  FairnessPolicyGeneration policy_generation{};
  FabricEpoch epoch{};

  /// Durable generation. Increases by exactly one per committed window.
  std::uint64_t generation{0};
  /// Durable generation of the last committed intervention authority.
  InterventionGeneration intervention_generation{};

  /// Last committed service window.
  ServiceWindowId last_window{};
  ServiceWindowGeneration last_window_generation{};

  /// Boot identity of the process that produced this state. Never used to grant
  /// liveness to a new process; only used to detect a foreign incarnation.
  BootId writer_boot{};

  std::vector<SubjectAccounting> subjects;

  [[nodiscard]] const SubjectAccounting* find(SubjectId wanted) const noexcept {
    for (const SubjectAccounting& entry : subjects) {
      if (entry.id == wanted) {
        return &entry;
      }
    }
    return nullptr;
  }

  [[nodiscard]] SubjectAccounting* find(SubjectId wanted) noexcept {
    for (SubjectAccounting& entry : subjects) {
      if (entry.id == wanted) {
        return &entry;
      }
    }
    return nullptr;
  }
};

/// Result of applying one window's net to a subject's carry.
struct CarryOutcome {
  std::uint64_t cumulative_deficit{0};
  std::uint64_t cumulative_surplus{0};
  std::uint64_t discarded_deficit{0};
  std::uint64_t discarded_surplus{0};
  /// Deficit recognized this window after credit was applied.
  std::uint64_t deficit_units{0};
  /// Surplus recognized this window after debit was applied.
  std::uint64_t surplus_units{0};
  /// Credit consumed from the carry by this window's deficit.
  std::uint64_t credit_applied{0};
  /// Debit consumed from the carry by this window's surplus.
  std::uint64_t debit_applied{0};
  /// True when the carry hit the configured bound and units were discarded.
  bool saturated{false};
};

/// Applies one window's net (entitlement - served) to a subject's carry.
///
/// The closure identity
///   cumulative_deficit - cumulative_surplus + discarded_deficit - discarded_surplus
/// increases by exactly `net` on every call, including when the carry saturates,
/// because saturation moves units between the live and discarded terms without
/// changing their signed combination. Returns false only on arithmetic overflow.
[[nodiscard]] bool apply_window_carry(const SubjectAccounting& current, std::int64_t net,
                                      std::uint64_t max_carry, CarryOutcome& out) noexcept;

/// Signed closure residual: cumulative_deficit - cumulative_surplus, plus the
/// discarded terms, must equal the observed sum of window nets.
[[nodiscard]] bool accounting_closes(const FairnessAccounting& accounting,
                                     std::int64_t expected_net_sum) noexcept;

/// Sum of all cumulative deficits, checked.
[[nodiscard]] bool accounting_deficit_total(const FairnessAccounting& accounting,
                                            std::uint64_t& out) noexcept;

/// Sum of all cumulative surpluses, checked.
[[nodiscard]] bool accounting_surplus_total(const FairnessAccounting& accounting,
                                            std::uint64_t& out) noexcept;

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_MODEL_ACCOUNTING_HPP
