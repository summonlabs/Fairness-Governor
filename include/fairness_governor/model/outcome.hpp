// Fairness Governor - fairness outcomes, per-subject state, corrective intent.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef FAIRNESS_GOVERNOR_MODEL_OUTCOME_HPP
#define FAIRNESS_GOVERNOR_MODEL_OUTCOME_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fairness_governor/core/ids.hpp"
#include "fairness_governor/core/limits.hpp"
#include "fairness_governor/core/status.hpp"
#include "fairness_governor/model/refs.hpp"

namespace fairness_governor {

/// The authoritative fairness outcome vocabulary. Nothing outside this set is
/// ever returned, and UNKNOWN/STALE are never upgraded to a positive outcome.
enum class Outcome : std::uint8_t {
  Fair = 0,
  OverServed = 1,
  UnderServed = 2,
  StarvationRisk = 3,
  CorrectionRequired = 4,
  BlockedByStrongerObligation = 5,
  Unknown = 6,
  Stale = 7,
};

[[nodiscard]] constexpr std::string_view to_string(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::Fair: return "FAIR";
    case Outcome::OverServed: return "OVER_SERVED";
    case Outcome::UnderServed: return "UNDER_SERVED";
    case Outcome::StarvationRisk: return "STARVATION_RISK";
    case Outcome::CorrectionRequired: return "CORRECTION_REQUIRED";
    case Outcome::BlockedByStrongerObligation: return "BLOCKED_BY_STRONGER_OBLIGATION";
    case Outcome::Unknown: return "UNKNOWN";
    case Outcome::Stale: return "STALE";
  }
  return "UNKNOWN";
}

/// Severity ordering used to aggregate a population outcome. UNKNOWN and STALE
/// are absence-of-authority states and dominate every positive outcome.
[[nodiscard]] constexpr std::uint32_t outcome_rank(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::Fair: return 0;
    case Outcome::OverServed: return 1;
    case Outcome::UnderServed: return 2;
    case Outcome::StarvationRisk: return 3;
    case Outcome::CorrectionRequired: return 4;
    case Outcome::BlockedByStrongerObligation: return 5;
    case Outcome::Unknown: return 6;
    case Outcome::Stale: return 7;
  }
  return 6;
}

/// Why a subject is not simply FAIR. Stable codes for operators.
enum class ReasonCode : std::uint16_t {
  None = 0,
  WithinFairBand = 1,
  WeightedShareDeficit = 2,
  WeightedShareSurplus = 3,
  GuaranteeFloorDeficit = 4,
  GuaranteeFloorSurplus = 5,
  StarvationUnserved = 6,
  StarvationBelowFloor = 7,
  EvidenceAbsent = 8,
  EvidenceStaleGeneration = 9,
  EvidenceStaleWindow = 10,
  EvidenceStaleEpoch = 11,
  EvidenceStaleAge = 12,
  SubjectGenerationMismatch = 13,
  PolicyGenerationMismatch = 14,
  PriorityReferenceMismatch = 15,
  CorrectiveBudgetExhausted = 16,
  CorrectiveCooldown = 17,
  ProtectedObligationHeld = 18,
  SubjectCapReached = 19,
  PolicyBoundReached = 20,
  NoCorrectionAvailable = 21,
  OvercommitDetected = 22,
};

[[nodiscard]] constexpr std::string_view to_string(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::None: return "NONE";
    case ReasonCode::WithinFairBand: return "WITHIN_FAIR_BAND";
    case ReasonCode::WeightedShareDeficit: return "WEIGHTED_SHARE_DEFICIT";
    case ReasonCode::WeightedShareSurplus: return "WEIGHTED_SHARE_SURPLUS";
    case ReasonCode::GuaranteeFloorDeficit: return "GUARANTEE_FLOOR_DEFICIT";
    case ReasonCode::GuaranteeFloorSurplus: return "GUARANTEE_FLOOR_SURPLUS";
    case ReasonCode::StarvationUnserved: return "STARVATION_UNSERVED";
    case ReasonCode::StarvationBelowFloor: return "STARVATION_BELOW_FLOOR";
    case ReasonCode::EvidenceAbsent: return "EVIDENCE_ABSENT";
    case ReasonCode::EvidenceStaleGeneration: return "EVIDENCE_STALE_GENERATION";
    case ReasonCode::EvidenceStaleWindow: return "EVIDENCE_STALE_WINDOW";
    case ReasonCode::EvidenceStaleEpoch: return "EVIDENCE_STALE_EPOCH";
    case ReasonCode::EvidenceStaleAge: return "EVIDENCE_STALE_AGE";
    case ReasonCode::SubjectGenerationMismatch: return "SUBJECT_GENERATION_MISMATCH";
    case ReasonCode::PolicyGenerationMismatch: return "POLICY_GENERATION_MISMATCH";
    case ReasonCode::PriorityReferenceMismatch: return "PRIORITY_REFERENCE_MISMATCH";
    case ReasonCode::CorrectiveBudgetExhausted: return "CORRECTIVE_BUDGET_EXHAUSTED";
    case ReasonCode::CorrectiveCooldown: return "CORRECTIVE_COOLDOWN";
    case ReasonCode::ProtectedObligationHeld: return "PROTECTED_OBLIGATION_HELD";
    case ReasonCode::SubjectCapReached: return "SUBJECT_CAP_REACHED";
    case ReasonCode::PolicyBoundReached: return "POLICY_BOUND_REACHED";
    case ReasonCode::NoCorrectionAvailable: return "NO_CORRECTION_AVAILABLE";
    case ReasonCode::OvercommitDetected: return "OVERCOMMIT_DETECTED";
  }
  return "NONE";
}

/// Which policy bound limited a corrective intent.
enum class CorrectionBound : std::uint8_t {
  None = 0,
  SubjectAugmentCap = 1,
  SubjectReduceCap = 2,
  PolicyTotalBudget = 3,
  PolicyBasisPoints = 4,
  ProtectedObligation = 5,
  Cooldown = 6,
  BudgetExhausted = 7,
  DemandSatisfied = 8,
  AuthorityMissing = 9,
};

[[nodiscard]] constexpr std::string_view to_string(CorrectionBound bound) noexcept {
  switch (bound) {
    case CorrectionBound::None: return "NONE";
    case CorrectionBound::SubjectAugmentCap: return "SUBJECT_AUGMENT_CAP";
    case CorrectionBound::SubjectReduceCap: return "SUBJECT_REDUCE_CAP";
    case CorrectionBound::PolicyTotalBudget: return "POLICY_TOTAL_BUDGET";
    case CorrectionBound::PolicyBasisPoints: return "POLICY_BASIS_POINTS";
    case CorrectionBound::ProtectedObligation: return "PROTECTED_OBLIGATION";
    case CorrectionBound::Cooldown: return "COOLDOWN";
    case CorrectionBound::BudgetExhausted: return "BUDGET_EXHAUSTED";
    case CorrectionBound::DemandSatisfied: return "DEMAND_SATISFIED";
    case CorrectionBound::AuthorityMissing: return "AUTHORITY_MISSING";
  }
  return "NONE";
}

/// Direction of a corrective fairness intent. The governor emits intent only;
/// enforcement belongs to another system.
enum class CorrectionDirection : std::uint8_t {
  Augment = 0,
  Reduce = 1,
};

[[nodiscard]] constexpr std::string_view to_string(CorrectionDirection direction) noexcept {
  return direction == CorrectionDirection::Augment ? "AUGMENT" : "REDUCE";
}

/// The authority vector: exactly which generations, epoch and incarnation
/// justified a decision. A decision is authoritative only while every element
/// still matches the live world.
struct AuthorityVector {
  FairnessPolicyId policy_id{};
  FairnessPolicyGeneration policy_generation{};
  FabricEpoch epoch{};
  EvidenceSnapshotId evidence_id{};
  EvidenceSnapshotGeneration evidence_generation{};
  ServiceWindowId window{};
  ServiceWindowGeneration window_generation{};
  /// Durable accounting generation the decision was computed against.
  std::uint64_t accounting_generation{0};
  /// Process incarnation that produced the decision.
  Incarnation governor{};
  /// Caller-supplied attempt identity, for de-duplicating retried requests.
  std::uint64_t request_id{0};
  std::uint32_t attempt{0};

  friend bool operator==(const AuthorityVector& a, const AuthorityVector& b) {
    return a.policy_id == b.policy_id && a.policy_generation == b.policy_generation &&
           a.epoch == b.epoch && a.evidence_id == b.evidence_id &&
           a.evidence_generation == b.evidence_generation && a.window == b.window &&
           a.window_generation == b.window_generation &&
           a.accounting_generation == b.accounting_generation && a.governor == b.governor &&
           a.request_id == b.request_id && a.attempt == b.attempt;
  }
};

/// Per-subject authoritative fairness state and explanation.
struct SubjectFairnessState {
  SubjectId id{};
  SubjectGeneration generation{};
  FairnessGroupId group{};
  Outcome outcome{Outcome::Unknown};
  ReasonCode reason{ReasonCode::None};

  bool has_authority{false};
  bool is_protected_obligation{false};
  std::uint32_t obligation_rank{0};

  /// Entitlement actually assigned to the owning group in this window.
  std::uint64_t group_entitlement_units{0};
  /// Authoritative entitlement for this subject in this window.
  std::uint64_t entitlement_units{0};
  /// Entitlement before the priority/value modifier, for explanation.
  std::uint64_t base_entitlement_units{0};
  /// Entitlement floor contributed by the guarantee, for explanation.
  std::uint64_t guarantee_floor_units{0};
  /// Units added (or removed) by the priority/value modifier, signed.
  std::int64_t priority_delta_units{0};
  /// The priority/value modifier actually applied, in basis points.
  std::int32_t applied_modifier_bps{0};
  /// Whether the applied modifier was clipped to the policy bound.
  bool modifier_clipped{false};

  std::uint64_t served_units{0};
  /// served - entitlement, signed.
  std::int64_t deviation_units{0};
  std::uint64_t deficit_units{0};
  std::uint64_t surplus_units{0};

  /// Carry after this window is applied (only meaningful for a committed window).
  std::uint64_t cumulative_deficit_after{0};
  std::uint64_t cumulative_surplus_after{0};

  std::uint32_t unserved_streak{0};
  std::uint32_t below_floor_streak{0};
  std::uint32_t starvation_limit_windows{0};
  std::uint32_t guarantee_starvation_limit_windows{0};

  /// Units of corrective augmentation proposed for this subject.
  std::uint64_t proposed_augment_units{0};
  /// Units of corrective reduction proposed for this subject.
  std::uint64_t proposed_reduce_units{0};
  /// Unmet demand after all bounds were applied.
  std::uint64_t unsatisfied_demand_units{0};

  /// The priority class the policy expects, and the one evidence reported.
  PriorityClassRef expected_priority{};
  PriorityClassRef observed_priority{};

  std::string label;
};

/// Aggregate state of one fairness group, for explanation.
struct GroupFairnessState {
  FairnessGroupId id{};
  FairnessGroupGeneration generation{};
  FairnessGroupId parent{};
  std::uint32_t depth{0};
  std::uint64_t entitlement_units{0};
  std::uint64_t served_units{0};
  std::int64_t deviation_units{0};
  bool is_protected_obligation{false};
  std::uint32_t obligation_rank{0};
  std::uint64_t guarantee_floor_units{0};
  /// True when this group's protected obligation withheld reducible units.
  bool withheld_correction{false};
  std::string label;
};

/// One bounded corrective fairness intent. This is intent, not enforcement:
/// the governor never allocates bandwidth, schedules, or shapes traffic.
struct CorrectiveIntent {
  InterventionId id{};
  InterventionGeneration generation{};
  CorrectionDirection direction{CorrectionDirection::Augment};
  SubjectId subject{};
  SubjectGeneration subject_generation{};
  FairnessGroupId group{};
  std::uint64_t units{0};
  CorrectionBound bound{CorrectionBound::None};
  ReasonCode reason{ReasonCode::None};
  bool from_starvation{false};
  bool from_protected_obligation{false};
  std::uint32_t obligation_rank{0};
};

/// Summary of the bounded corrective plan.
struct CorrectionSummary {
  /// Reducible units the plan could draw from before any policy cap.
  std::uint64_t reducible_pool_units{0};
  /// Demand from under-served subjects before any policy cap.
  std::uint64_t demand_units{0};
  /// Budget actually authorized by the policy caps.
  std::uint64_t authorized_budget_units{0};
  std::uint64_t augment_units{0};
  std::uint64_t reduce_units{0};
  /// Reducible units withheld solely because a protected obligation covers them.
  std::uint64_t withheld_units{0};
  /// Demand that could not be met after all bounds.
  std::uint64_t unsatisfied_demand_units{0};
  std::uint32_t cooldown_suppressed{0};
  bool bounded_by_policy_total{false};
  bool bounded_by_policy_bps{false};
  bool protected_obligation_blocked{false};
  bool authority_missing{false};
};

/// Bounded diagnostic notes attached to a decision.
struct DiagnosticNote {
  StatusCode code{StatusCode::Ok};
  ReasonCode reason{ReasonCode::None};
  SubjectId subject{};
  std::string detail;
};

/// Count of subjects per outcome.
struct OutcomeCounts {
  std::uint32_t fair{0};
  std::uint32_t over_served{0};
  std::uint32_t under_served{0};
  std::uint32_t starvation_risk{0};
  std::uint32_t correction_required{0};
  std::uint32_t blocked_by_stronger_obligation{0};
  std::uint32_t unknown{0};
  std::uint32_t stale{0};

  [[nodiscard]] std::uint32_t total() const noexcept {
    return fair + over_served + under_served + starvation_risk + correction_required +
           blocked_by_stronger_obligation + unknown + stale;
  }
};

/// The authoritative fairness decision.
struct FairnessDecision {
  AuthorityVector authority{};
  Outcome outcome{Outcome::Unknown};
  OutcomeCounts counts{};
  std::vector<SubjectFairnessState> subjects;
  std::vector<GroupFairnessState> groups;
  std::vector<CorrectiveIntent> intents;
  CorrectionSummary correction{};
  std::vector<DiagnosticNote> notes{};

  /// Deterministic digest of the decision content (excludes the incarnation so
  /// that two governors computing the same decision agree on the digest).
  std::uint64_t content_digest{0};

  /// True when this decision still matches the supplied live authority.
  [[nodiscard]] bool is_authoritative_for(const AuthorityVector& live) const noexcept {
    return authority.policy_id == live.policy_id &&
           authority.policy_generation == live.policy_generation && authority.epoch == live.epoch &&
           authority.evidence_id == live.evidence_id &&
           authority.evidence_generation == live.evidence_generation &&
           authority.window == live.window &&
           authority.window_generation == live.window_generation &&
           authority.accounting_generation == live.accounting_generation;
  }
};

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_MODEL_OUTCOME_HPP
