// Fairness Governor - fairness policy model and validation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A policy is the only source of authority about what "fair" means. It declares
// share weights, absolute guarantee floors, starvation thresholds, priority/value
// modifiers, protected obligations, and the bounds on any corrective intent.
// Nothing in the observed service history can change a policy.
#ifndef FAIRNESS_GOVERNOR_MODEL_POLICY_HPP
#define FAIRNESS_GOVERNOR_MODEL_POLICY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fairness_governor/core/ids.hpp"
#include "fairness_governor/core/limits.hpp"
#include "fairness_governor/core/status.hpp"
#include "fairness_governor/model/refs.hpp"

namespace fairness_governor {

/// One node of the fairness group forest. Groups carry the outer share of the
/// distributable whole; subjects carry the inner share.
struct FairnessGroup {
  FairnessGroupId id{};
  FairnessGroupGeneration generation{};
  FairnessGroupId parent{};  ///< Invalid id means "root group".
  std::uint32_t share_weight{1};
  /// Absolute minimum units this group must see per window. 0 means none.
  std::uint64_t guarantee_floor{0};
  /// Obligation strength. 0 = none. Higher values are stronger obligations that
  /// a corrective intent may never violate.
  std::uint32_t obligation_rank{0};
  /// When true, the group's guarantee floor is a protected obligation: the
  /// governor will refuse a corrective intent that would push the group below it.
  bool protected_obligation{false};
  std::string label;
};

/// One governed subject (a tenant flow, class, or aggregate the operator chose to
/// govern). The governor evaluates fairness for it; it does not schedule it.
struct Subject {
  SubjectId id{};
  SubjectGeneration generation{};
  FairnessGroupId group{};
  TenantId tenant{};

  /// Fair share weight inside the owning group. Must be >= 1.
  std::uint32_t share_weight{1};
  /// Absolute per-window guarantee floor, in service units.
  std::uint64_t guarantee_floor{0};
  /// Consecutive fully-unserved windows allowed before STARVATION_RISK.
  /// 0 disables the unserved-starvation rule for this subject.
  std::uint32_t starvation_windows{0};
  /// Consecutive windows below the guarantee floor allowed before
  /// STARVATION_RISK. 0 disables the below-guarantee rule.
  std::uint32_t guarantee_starvation_windows{0};

  /// Priority class reference (external, generation-matched).
  PriorityClassRef priority{};
  /// QoS class reference (external, generation-matched).
  QosClassRef qos{};
  /// Policy-defined value/priority modifier applied to entitlement, in basis
  /// points. Signed: it may raise or lower entitlement. Bounded by the policy's
  /// max_priority_modifier_bps and by kMaxPriorityModifierBps.
  std::int32_t priority_modifier_bps{0};
  /// Higher value means the priority reference is more authoritative. Used only
  /// for explanation, never to waive a starvation threshold.
  std::uint32_t priority_rank{0};

  /// Obligation strength of this subject. 0 = none.
  std::uint32_t obligation_rank{0};
  /// When true, the guarantee floor is a protected obligation.
  bool protected_obligation{false};

  /// Per-window cap on any corrective reduction of this subject.
  std::uint64_t max_reduce_units{0};
  /// Per-window cap on any corrective augmentation of this subject.
  std::uint64_t max_augment_units{0};

  std::string label;
};

/// The complete, validated fairness policy.
struct FairnessPolicy {
  FairnessPolicyId id{};
  FairnessPolicyGeneration generation{};
  FabricEpoch epoch{};

  std::uint64_t window_units{0};  ///< Nominal window size, for explanation only.

  /// Deviation at or below this magnitude is FAIR (hysteresis band).
  std::uint64_t fair_band_units{0};
  /// Deviation at or above this magnitude escalates to CORRECTION_REQUIRED.
  std::uint64_t correction_threshold_units{1};

  /// Maximum number of windows a corrective intent may cool down before the
  /// same subject may be corrected again.
  std::uint32_t cooldown_windows{0};

  /// Hard bound on the total magnitude of any single corrective plan.
  std::uint64_t max_correction_units{0};
  /// Hard bound on the total magnitude as basis points of total observed service.
  std::uint32_t max_correction_bps{10000};

  /// Bound on |priority_modifier_bps| any subject may declare.
  std::uint32_t max_priority_modifier_bps{0};

  /// Evidence freshness: a snapshot older than this many windows is STALE.
  std::uint32_t max_evidence_age_windows{1};
  /// Evidence freshness in producer-supplied nanoseconds. 0 disables the check.
  std::uint64_t max_evidence_age_ns{0};

  /// Upper bound on carried cumulative deficit/surplus per subject.
  std::uint64_t max_carry_units{kMaxCarryUnits};

  /// When true, entitlement floors are enforced before weighted shares and any
  /// remaining whole is distributed by weight. When false, entitlement is the
  /// pure weighted share and floors are reported as protected obligations only.
  bool enforce_guarantee_floors{true};

  std::vector<FairnessGroup> groups;
  std::vector<Subject> subjects;

  /// Policy provenance (which artifact produced it).
  Provenance provenance{};

  [[nodiscard]] const Subject* find_subject(SubjectId wanted) const noexcept {
    for (const Subject& subject : subjects) {
      if (subject.id == wanted) {
        return &subject;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const FairnessGroup* find_group(FairnessGroupId wanted) const noexcept {
    for (const FairnessGroup& group : groups) {
      if (group.id == wanted) {
        return &group;
      }
    }
    return nullptr;
  }
};

/// Validation report for a policy. A policy that fails validation is never used
/// to take an authoritative decision.
struct PolicyValidation {
  StatusCode code{StatusCode::Ok};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return code == StatusCode::Ok; }
};

/// Fully validates a policy: identities, generations, references, bounds, group
/// forest shape (no cycles, bounded depth), guaranteed-floor overcommit, and
/// every arithmetic domain the evaluator will rely on.
[[nodiscard]] PolicyValidation validate_policy(const FairnessPolicy& policy);

/// Computes the depth of each group (root = 1) after validation.
/// Preconditions: the policy has been validated.
[[nodiscard]] std::vector<std::uint32_t> group_depths(const FairnessPolicy& policy);

/// Sum of subject share weights inside a group.
[[nodiscard]] std::uint64_t group_subject_weight_sum(const FairnessPolicy& policy,
                                                     FairnessGroupId group) noexcept;

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_MODEL_POLICY_HPP
