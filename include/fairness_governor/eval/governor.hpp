// Fairness Governor - the authoritative fairness evaluation engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Lifecycle:
//   open()          load durable policy + accounting, report recovery
//   install_policy  validate, fence by epoch, durable commit
//   ingest_evidence validate, fence by epoch/generation/sequence, stage
//   evaluate        pure: compute entitlement, outcomes, bounded correction
//   commit          durable boundary: apply accounting, advance generation
//
// evaluate() never mutates durable state and never emits intent that exceeds a
// policy bound. commit() refuses any decision whose authority vector no longer
// matches the live world, so a decision computed against a superseded policy,
// epoch, or accounting generation can never be committed.
#ifndef FAIRNESS_GOVERNOR_EVAL_GOVERNOR_HPP
#define FAIRNESS_GOVERNOR_EVAL_GOVERNOR_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fairness_governor/core/ids.hpp"
#include "fairness_governor/core/status.hpp"
#include "fairness_governor/model/accounting.hpp"
#include "fairness_governor/model/evidence.hpp"
#include "fairness_governor/model/outcome.hpp"
#include "fairness_governor/model/policy.hpp"

namespace fairness_governor {

class DurableStore;

/// Bounded ingestion policy for staged evidence.
struct IngestionLimits {
  /// Maximum distinct staged snapshots retained at once.
  std::uint32_t max_staged_snapshots{64};
  /// Maximum retained service windows in the recent-history ring.
  std::uint32_t max_history_windows{kMaxHistoryWindows};
  /// Maximum observations per staged snapshot.
  std::uint32_t max_observations_per_snapshot{kMaxEvidenceRecords};
};

/// Governor configuration.
struct GovernorConfig {
  FairnessPolicy policy{};
  /// Directory for durable state. Empty means purely in-memory (no durability).
  std::string store_path{};
  /// Fabric epoch this governor serves. Must match the policy epoch.
  FabricEpoch epoch{};
  /// Ingestion bounds.
  IngestionLimits limits{};
  /// When true, a commit requires an existing durable store.
  bool require_durability{false};
};

/// What recovery found. Durable state never restores liveness, freshness,
/// leases, or publisher authority; those fields exist to prove it.
struct RecoveryReport {
  bool store_present{false};
  bool policy_loaded{false};
  /// True when the configured policy was written durably during open because no
  /// committed policy existed yet, or because the configured one was newer.
  bool policy_committed{false};
  bool accounting_loaded{false};
  bool foreign_incarnation{false};
  bool unfinished_attempt_discarded{false};
  bool backup_recovered{false};
  bool corruption_detected{false};

  /// Always false by construction: evidence is dynamic and must be re-attested.
  bool evidence_authority_restored{false};
  /// Always false by construction: publisher authority must be re-handshaked.
  bool publisher_authority_restored{false};

  StatusCode policy_code{StatusCode::Ok};
  StatusCode accounting_code{StatusCode::Ok};
  std::string policy_detail;
  std::string accounting_detail;

  std::uint64_t loaded_accounting_generation{0};
  BootId previous_writer_boot{};
  FabricEpoch loaded_epoch{};
  std::string detail;
};

/// Result of ingesting one evidence snapshot.
struct IngestResult {
  enum class Disposition : std::uint8_t {
    Staged = 0,
    DuplicateIgnored = 1,
  };
  Disposition disposition{Disposition::Staged};
  EvidenceSnapshotId id{};
  EvidenceSnapshotGeneration generation{};
  ServiceWindowId window{};
  std::uint32_t observation_count{0};
};

/// The live authoritative identity of a governor, read atomically. Callers that
/// need several of these values must take them together: reading them one at a
/// time would let a concurrent policy installation or commit produce a mixture
/// of generations that never existed.
struct LiveAuthority {
  FairnessPolicyId policy_id{};
  FairnessPolicyGeneration policy_generation{};
  FabricEpoch epoch{};
  std::uint64_t accounting_generation{0};
  ServiceWindowId last_committed_window{};
  Incarnation governor{};
};

/// Exact request identity for an authoritative evaluation. Every element is
/// caller-supplied: the governor never guesses which evidence is current.
struct EvaluationRequest {
  FairnessPolicyId policy_id{};
  FairnessPolicyGeneration policy_generation{};
  FabricEpoch epoch{};
  EvidenceSnapshotId evidence_id{};
  EvidenceSnapshotGeneration evidence_generation{};
  ServiceWindowId window{};
  ServiceWindowGeneration window_generation{};
  /// Instant at which the caller wants eligibility judged. Never read from the
  /// system clock inside the engine.
  TimePointNs now_ns{0};
  std::uint64_t request_id{0};
  std::uint32_t attempt{0};
  /// When true, a subject missing from the snapshot yields UNKNOWN instead of
  /// STALE. Both are absence-of-authority outcomes; the caller chooses which.
  bool treat_missing_as_unknown{true};
  /// When true, an epoch mismatch is reported per subject as STALE rather than
  /// aborting the whole evaluation.
  bool allow_stale_result{true};
};

/// The governor. Thread-safe for concurrent ingest/evaluate/commit; every
/// public operation takes the state lock only for the duration of a state
/// transition, and never invokes user code or I/O while holding it.
class FairnessGovernor {
 public:
  FairnessGovernor(const FairnessGovernor&) = delete;
  FairnessGovernor& operator=(const FairnessGovernor&) = delete;
  ~FairnessGovernor();

  /// Opens a governor, recovering durable state from `config.store_path`.
  [[nodiscard]] static Result<std::unique_ptr<FairnessGovernor>> open(const GovernorConfig& config);

  /// Opens a purely in-memory governor (no durability). Useful for evaluation
  /// of hypothetical policies; commit() then only advances in-memory state.
  [[nodiscard]] static Result<std::unique_ptr<FairnessGovernor>> open_in_memory(
      const FairnessPolicy& policy);

  [[nodiscard]] const Incarnation& incarnation() const noexcept;
  [[nodiscard]] const RecoveryReport& recovery() const noexcept;
  [[nodiscard]] FabricEpoch epoch() const noexcept;
  [[nodiscard]] FairnessPolicyGeneration policy_generation() const noexcept;
  [[nodiscard]] std::uint64_t accounting_generation() const noexcept;
  [[nodiscard]] std::uint64_t intervention_generation() const noexcept;
  [[nodiscard]] ServiceWindowId last_committed_window() const noexcept;

  /// Returns a copy of the installed policy.
  [[nodiscard]] FairnessPolicy policy() const;

  /// Returns the live authoritative identity read atomically under one lock
  /// acquisition. Use this whenever more than one of these values is needed.
  [[nodiscard]] LiveAuthority authority() const;

  /// Returns a copy of the durable accounting state.
  [[nodiscard]] FairnessAccounting accounting() const;

  /// Installs a policy. Requires a strictly newer generation and an epoch at
  /// least as new as the current one; durably commits before returning.
  [[nodiscard]] Status install_policy(FairnessPolicy policy);

  /// Advances the fabric epoch. Advancing the epoch invalidates every staged
  /// snapshot and every decision computed in the previous epoch.
  [[nodiscard]] Status advance_epoch(FabricEpoch next_epoch);

  /// Validates and stages one evidence snapshot. Rejects stale epochs, stale
  /// windows, contradictory duplicates, and sequence violations.
  [[nodiscard]] Result<IngestResult> ingest_evidence(EvidenceSnapshot snapshot);

  /// Computes the authoritative fairness decision for `request`. Pure: no
  /// durable mutation, no intake, no scheduling.
  [[nodiscard]] Result<FairnessDecision> evaluate(const EvaluationRequest& request) const;

  /// The live authority vector for `request`'s window.
  [[nodiscard]] AuthorityVector live_authority(const EvaluationRequest& request) const;

  /// Durably commits the accounting effect of `decision`. Fails with
  /// AuthorityMismatch when the decision no longer matches live authority.
  [[nodiscard]] Status commit(const FairnessDecision& decision);

  /// Number of staged snapshots currently retained.
  [[nodiscard]] std::uint32_t staged_snapshot_count() const;

  /// Drops staged snapshots. Called after an epoch advance or a restart.
  void clear_staged();

  /// Stops accepting work, drops staged evidence, and releases the store.
  /// Idempotent. Never joins workers while holding the state lock.
  [[nodiscard]] Status shutdown();

  [[nodiscard]] bool shutting_down() const noexcept;

 private:
  struct Impl;
  explicit FairnessGovernor(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

// --- Pure evaluation entry point ---------------------------------------------
//
// The engine is exposed separately so that property and adversarial tests can
// drive it directly with an explicit accounting snapshot, without a store.

/// Everything the engine needs. All inputs are explicit; there are no defaults
/// that could silently pick "the latest" anything.
struct EvaluationContext {
  const FairnessPolicy* policy{nullptr};
  const FairnessAccounting* accounting{nullptr};
  const EvidenceSnapshot* evidence{nullptr};
  EvaluationRequest request{};
  FabricEpoch current_epoch{};
  /// Highest service window the governor has observed. Used for the age rule.
  ServiceWindowId current_window{};
  /// Highest committed service window, used to reject a replayed window.
  ServiceWindowId last_committed_window{};
};

/// Evaluates fairness exactly. Deterministic: identical context yields an
/// identical decision, including the content digest.
[[nodiscard]] Result<FairnessDecision> evaluate_fairness(const EvaluationContext& context);

// --- Exact allocation primitive ----------------------------------------------
//
// A "whole" is distributed among competing children. The algorithm is exact and
// conserving: the shares always sum to the whole, never above it and never below
// it. It proceeds in three steps, each of which is a pure integer function:
//
//   1. weighted share  share_i = floor(whole * weight_i / sum_weight)
//   2. floors          either reallocate from above-floor children (when the
//                      slack covers the shortfall) or, when the declared floors
//                      cannot all be met inside the whole, satisfy them in
//                      descending obligation-rank order and report overcommit
//   3. largest remainder  hand out the rounding residue, one unit at a time, in
//                      descending remainder order with identity as tie-break
//
// Because step 3 closes the sum exactly, every group's deficits and surpluses
// cancel exactly. That is what makes the deficit/credit accounting provable.

/// One competitor for a share of a whole.
struct AllocationInput {
  /// Stable tie-break key. Lower sorts first.
  std::uint64_t key{0};
  /// Relative share weight. Must be >= 1. Weights live in a 1e-4 scaled domain
  /// (a raw weight of w is supplied as w * 10000) so that a basis-point value
  /// modifier redistributes entitlement with no rounding loss.
  std::uint64_t weight{1};
  /// Absolute minimum units. 0 means no floor.
  std::uint64_t floor{0};
  /// Obligation strength; higher is stronger and is served first when the
  /// declared floors cannot all be met.
  std::uint32_t obligation_rank{0};
};

struct AllocationResult {
  std::vector<std::uint64_t> allocations;
  /// 1 where the declared floor raised the allocation above the plain weighted
  /// share, so the explanation can name the floor as the binding constraint.
  std::vector<std::uint8_t> floor_binding;
  /// True when the declared floors could not all be met inside the whole.
  bool overcommit{false};
  /// Units of demand that could not be met because of overcommit.
  std::uint64_t overcommit_shortfall{0};
};

/// Distributes `whole` exactly. Returns an error only for an empty competitor
/// set, a zero weight, or an arithmetic overflow of the weight sum.
[[nodiscard]] Result<AllocationResult> allocate_whole(const std::vector<AllocationInput>& children,
                                                      std::uint64_t whole);

/// The effective share weight after a policy-defined value modifier, in basis
/// points, expressed in the same 1e-4 scaled domain as an unmodified weight.
/// Never returns 0: a value modifier redistributes value but can never strip a
/// subject of all entitlement.
[[nodiscard]] std::uint64_t effective_share_weight(std::uint32_t weight,
                                                   std::int32_t modifier_bps) noexcept;

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_EVAL_GOVERNOR_HPP
