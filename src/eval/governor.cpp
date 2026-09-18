// Fairness Governor - stateful governor lifecycle, ingestion, commit.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/eval/governor.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "fairness_governor/core/checked.hpp"
#include "fairness_governor/core/entropy.hpp"
#include "fairness_governor/persist/codec.hpp"
#include "fairness_governor/persist/fsutil.hpp"
#include "fairness_governor/persist/store.hpp"

namespace fairness_governor {
namespace {

[[nodiscard]] std::uint64_t publisher_key(const ProducerIdentity& producer) noexcept {
  const std::uint64_t name_digest = fnv1a64(
      reinterpret_cast<const std::uint8_t*>(producer.name.data()), producer.name.size());
  return (name_digest * 0x100000001B3ULL) ^ producer.instance;
}

}  // namespace

struct FairnessGovernor::Impl {
  mutable std::mutex mutex;
  GovernorConfig config{};
  FairnessPolicy policy{};
  FairnessAccounting accounting{};
  FabricEpoch epoch{};
  Incarnation incarnation{};
  RecoveryReport recovery{};
  std::unique_ptr<DurableStore> store;
  std::uint64_t policy_record_generation{0};
  std::uint64_t accounting_record_generation{0};

  /// Staged snapshots keyed by service window value. Bounded by config.limits.
  std::map<std::uint64_t, EvidenceSnapshot> staged;
  /// Digest of every ingested snapshot identity, for contradiction detection.
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> ingested;
  /// Highest sequence observed per publisher.
  std::map<std::uint64_t, SequenceNumber> publisher_sequences;

  std::atomic<bool> stopping{false};

  [[nodiscard]] ServiceWindowId current_window() const {
    ServiceWindowId best = accounting.last_window;
    for (const auto& entry : staged) {
      if (entry.second.window.value() > best.value()) {
        best = entry.second.window;
      }
    }
    return best;
  }

  [[nodiscard]] Status commit_accounting() {
    if (!store) {
      return Status::success();
    }
    const ByteBuffer payload = codec::encode_accounting(accounting);
    const Status status = store->commit(RecordKind::Accounting, accounting_record_generation,
                                        accounting_record_generation + 1, payload, incarnation);
    if (!status.ok()) {
      return status;
    }
    ++accounting_record_generation;
    return Status::success();
  }

  [[nodiscard]] Status commit_policy() {
    if (!store) {
      return Status::success();
    }
    const ByteBuffer payload = codec::encode_policy(policy);
    const Status status = store->commit(RecordKind::Policy, policy_record_generation,
                                        policy_record_generation + 1, payload, incarnation);
    if (!status.ok()) {
      return status;
    }
    ++policy_record_generation;
    return Status::success();
  }
};

FairnessGovernor::FairnessGovernor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FairnessGovernor::~FairnessGovernor() = default;

const Incarnation& FairnessGovernor::incarnation() const noexcept { return impl_->incarnation; }
const RecoveryReport& FairnessGovernor::recovery() const noexcept { return impl_->recovery; }
FabricEpoch FairnessGovernor::epoch() const noexcept { return impl_->epoch; }

FairnessPolicyGeneration FairnessGovernor::policy_generation() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->policy.generation;
}

std::uint64_t FairnessGovernor::accounting_generation() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->accounting.generation;
}

std::uint64_t FairnessGovernor::intervention_generation() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->accounting.intervention_generation.value();
}

ServiceWindowId FairnessGovernor::last_committed_window() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->accounting.last_window;
}

FairnessPolicy FairnessGovernor::policy() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->policy;
}

LiveAuthority FairnessGovernor::authority() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  LiveAuthority authority;
  authority.policy_id = impl_->policy.id;
  authority.policy_generation = impl_->policy.generation;
  authority.epoch = impl_->epoch;
  authority.accounting_generation = impl_->accounting.generation;
  authority.last_committed_window = impl_->accounting.last_window;
  authority.governor = impl_->incarnation;
  return authority;
}

FairnessAccounting FairnessGovernor::accounting() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->accounting;
}

bool FairnessGovernor::shutting_down() const noexcept {
  return impl_->stopping.load(std::memory_order_acquire);
}

std::uint32_t FairnessGovernor::staged_snapshot_count() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return static_cast<std::uint32_t>(impl_->staged.size());
}

void FairnessGovernor::clear_staged() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->staged.clear();
  impl_->ingested.clear();
  impl_->publisher_sequences.clear();
}

Result<std::unique_ptr<FairnessGovernor>> FairnessGovernor::open(const GovernorConfig& config) {
  const PolicyValidation validation = validate_policy(config.policy);
  if (!validation.ok()) {
    return Status(validation.code, validation.detail);
  }
  if (!config.epoch.valid()) {
    return Status(StatusCode::InvalidArgument, "governor epoch must be >= 1");
  }
  if (config.policy.epoch != config.epoch) {
    return Status(StatusCode::Conflict, "policy epoch does not match the governor epoch");
  }
  if (config.limits.max_staged_snapshots == 0 || config.limits.max_history_windows == 0 ||
      config.limits.max_observations_per_snapshot == 0) {
    return Status(StatusCode::InvalidArgument, "ingestion limits must be non-zero");
  }
  if (config.store_path.empty() && config.require_durability) {
    return Status(StatusCode::InvalidArgument, "durability required but no store path was given");
  }

  auto impl = std::make_unique<Impl>();
  impl->config = config;
  impl->policy = config.policy;
  impl->epoch = config.epoch;
  impl->incarnation = make_incarnation();
  impl->accounting.policy_id = impl->policy.id;
  impl->accounting.policy_generation = impl->policy.generation;
  impl->accounting.epoch = impl->epoch;

  if (config.store_path.empty()) {
    auto governor = std::unique_ptr<FairnessGovernor>(new FairnessGovernor(std::move(impl)));
    return governor;
  }

  Result<DurableStore> opened = DurableStore::open(config.store_path);
  if (!opened.ok()) {
    return opened.status();
  }
  impl->store = std::make_unique<DurableStore>(opened.take());
  impl->recovery.store_present = true;

  RecoveryNotes policy_notes{};
  RecoveryNotes accounting_notes{};
  const Status policy_recovery = impl->store->recover(RecordKind::Policy, policy_notes);
  const Status accounting_recovery =
      impl->store->recover(RecordKind::Accounting, accounting_notes);
  impl->recovery.unfinished_attempt_discarded =
      policy_notes.unfinished_attempt_discarded || accounting_notes.unfinished_attempt_discarded;
  impl->recovery.backup_recovered =
      policy_notes.backup_recovered || accounting_notes.backup_recovered;
  impl->recovery.corruption_detected =
      policy_notes.corruption_detected || accounting_notes.corruption_detected;
  if (!policy_recovery.ok() || !accounting_recovery.ok()) {
    impl->recovery.corruption_detected = true;
    impl->recovery.detail = "recovery could not establish a committed record";
    return Status(StatusCode::RecoveryRequired, impl->recovery.detail);
  }

  // --- Policy ---------------------------------------------------------------
  const LoadedRecord policy_record = impl->store->load(RecordKind::Policy);
  if (policy_record.present) {
    if (!policy_record.ok()) {
      impl->recovery.policy_code = policy_record.code;
      impl->recovery.policy_detail = policy_record.detail;
      impl->recovery.corruption_detected = true;
      return Status(policy_record.code, "durable policy record is not usable: " +
                                            policy_record.detail);
    }
    FairnessPolicy durable_policy;
    const Status decoded = codec::decode_policy(policy_record.payload, durable_policy);
    if (!decoded.ok()) {
      impl->recovery.policy_code = decoded.code();
      impl->recovery.policy_detail = std::string(decoded.detail());
      impl->recovery.corruption_detected = true;
      return Status(decoded.code(), "durable policy record failed validation");
    }
    impl->policy_record_generation = policy_record.generation;
    if (durable_policy.id != impl->policy.id) {
      return Status(StatusCode::Conflict,
                    "durable policy identity differs from the configured policy identity");
    }
    if (durable_policy.generation > impl->policy.generation) {
      // A committed policy is never silently rolled back to an older one.
      impl->policy = durable_policy;
      impl->epoch = durable_policy.epoch;
      impl->recovery.policy_loaded = true;
    } else if (durable_policy.generation == impl->policy.generation) {
      if (codec::policy_digest(durable_policy) != codec::policy_digest(impl->policy)) {
        return Status(StatusCode::Conflict,
                      "durable policy content differs at the same generation");
      }
      impl->recovery.policy_loaded = true;
    } else {
      // Configured policy is newer: install it durably before serving.
      const Status committed = impl->commit_policy();
      if (!committed.ok()) {
        return committed;
      }
      impl->recovery.policy_committed = true;
    }
  } else {
    impl->recovery.policy_code = StatusCode::NotFound;
    const Status committed = impl->commit_policy();
    if (!committed.ok()) {
      return committed;
    }
    impl->recovery.policy_committed = true;
  }
  impl->accounting.policy_id = impl->policy.id;
  impl->accounting.policy_generation = impl->policy.generation;
  impl->accounting.epoch = impl->epoch;

  // --- Accounting -----------------------------------------------------------
  const LoadedRecord accounting_record = impl->store->load(RecordKind::Accounting);
  if (accounting_record.present) {
    if (!accounting_record.ok()) {
      impl->recovery.accounting_code = accounting_record.code;
      impl->recovery.accounting_detail = accounting_record.detail;
      impl->recovery.corruption_detected = true;
      return Status(accounting_record.code, "durable accounting record is not usable");
    }
    FairnessAccounting durable_accounting;
    const Status decoded = codec::decode_accounting(accounting_record.payload, durable_accounting);
    if (!decoded.ok()) {
      impl->recovery.accounting_code = decoded.code();
      impl->recovery.accounting_detail = std::string(decoded.detail());
      impl->recovery.corruption_detected = true;
      return Status(decoded.code(), "durable accounting record failed validation");
    }
    impl->accounting_record_generation = accounting_record.generation;
    impl->accounting = std::move(durable_accounting);
    impl->recovery.accounting_loaded = true;
    impl->recovery.loaded_accounting_generation = impl->accounting.generation;
    impl->recovery.previous_writer_boot = impl->accounting.writer_boot;
    impl->recovery.loaded_epoch = impl->accounting.epoch;
    impl->recovery.foreign_incarnation =
        impl->accounting.writer_boot.valid() && impl->accounting.writer_boot != impl->incarnation.boot;
    if (impl->accounting.epoch.value() > impl->epoch.value()) {
      // A committed epoch is never rolled back either.
      impl->epoch = impl->accounting.epoch;
    }
    // Keep the accounting that still describes governed subjects; drop entries
    // whose subject generation no longer exists in the policy.
    std::vector<SubjectAccounting> kept;
    kept.reserve(impl->accounting.subjects.size());
    for (SubjectAccounting& entry : impl->accounting.subjects) {
      const Subject* subject = impl->policy.find_subject(entry.id);
      if (subject != nullptr && subject->generation == entry.generation) {
        kept.push_back(std::move(entry));
      }
    }
    impl->accounting.subjects = std::move(kept);
    impl->accounting.policy_id = impl->policy.id;
    impl->accounting.policy_generation = impl->policy.generation;
    impl->accounting.epoch = impl->epoch;
  } else {
    impl->recovery.accounting_code = StatusCode::NotFound;
  }
  // Dynamic evidence is never restored and never grants liveness.
  impl->staged.clear();
  impl->ingested.clear();
  impl->publisher_sequences.clear();
  impl->recovery.evidence_authority_restored = false;
  impl->recovery.publisher_authority_restored = false;
  impl->recovery.detail = impl->recovery.foreign_incarnation
                              ? "durable accounting belongs to a previous incarnation"
                              : "durable accounting belongs to this incarnation";

  auto governor = std::unique_ptr<FairnessGovernor>(new FairnessGovernor(std::move(impl)));
  return governor;
}

Result<std::unique_ptr<FairnessGovernor>> FairnessGovernor::open_in_memory(
    const FairnessPolicy& policy) {
  GovernorConfig config;
  config.policy = policy;
  config.epoch = policy.epoch;
  config.store_path.clear();
  return open(config);
}

Status FairnessGovernor::install_policy(FairnessPolicy policy) {
  const PolicyValidation validation = validate_policy(policy);
  if (!validation.ok()) {
    return Status(validation.code, validation.detail);
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->stopping.load(std::memory_order_acquire)) {
    return Status(StatusCode::ShuttingDown, "governor is shutting down");
  }
  if (policy.id != impl_->policy.id) {
    return Status(StatusCode::Conflict, "policy identity change requires reopening the governor");
  }
  if (policy.generation <= impl_->policy.generation) {
    return Status(StatusCode::StaleGeneration, "policy generation must advance");
  }
  if (policy.epoch.value() < impl_->epoch.value()) {
    return Status(StatusCode::StaleEpoch, "policy epoch is behind the fabric epoch");
  }
  const FairnessPolicy previous_policy = impl_->policy;
  const FabricEpoch previous_epoch = impl_->epoch;
  const bool epoch_advanced = policy.epoch.value() > impl_->epoch.value();
  impl_->policy = std::move(policy);
  if (epoch_advanced) {
    impl_->epoch = impl_->policy.epoch;
    impl_->staged.clear();
    impl_->ingested.clear();
    impl_->publisher_sequences.clear();
  }
  impl_->accounting.policy_generation = impl_->policy.generation;
  impl_->accounting.epoch = impl_->epoch;
  const Status committed = impl_->commit_policy();
  if (!committed.ok()) {
    impl_->policy = previous_policy;
    impl_->epoch = previous_epoch;
    impl_->accounting.policy_generation = previous_policy.generation;
    impl_->accounting.epoch = previous_epoch;
    return committed;
  }
  const Status accounting_committed = impl_->commit_accounting();
  if (!accounting_committed.ok()) {
    return accounting_committed;
  }
  return Status::success();
}

Status FairnessGovernor::advance_epoch(FabricEpoch next_epoch) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->stopping.load(std::memory_order_acquire)) {
    return Status(StatusCode::ShuttingDown, "governor is shutting down");
  }
  if (!next_epoch.valid() || next_epoch.value() <= impl_->epoch.value()) {
    return Status(StatusCode::StaleEpoch, "epoch must strictly advance");
  }
  const FabricEpoch previous = impl_->epoch;
  impl_->epoch = next_epoch;
  impl_->accounting.epoch = next_epoch;
  impl_->staged.clear();
  impl_->ingested.clear();
  impl_->publisher_sequences.clear();
  const Status committed = impl_->commit_accounting();
  if (!committed.ok()) {
    impl_->epoch = previous;
    impl_->accounting.epoch = previous;
    return committed;
  }
  return Status::success();
}

Result<IngestResult> FairnessGovernor::ingest_evidence(EvidenceSnapshot snapshot) {
  const Status validation = validate_evidence(snapshot);
  if (!validation.ok()) {
    return validation;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->stopping.load(std::memory_order_acquire)) {
    return Status(StatusCode::ShuttingDown, "governor is shutting down");
  }
  if (snapshot.epoch != impl_->epoch) {
    return Status(StatusCode::StaleEpoch, "evidence epoch does not match the fabric epoch");
  }
  if (snapshot.observations.size() > impl_->config.limits.max_observations_per_snapshot) {
    return Status(StatusCode::LimitExceeded, "observation count exceeds the configured bound");
  }
  if (impl_->accounting.last_window.valid() &&
      snapshot.window.value() <= impl_->accounting.last_window.value()) {
    return Status(StatusCode::StaleGeneration, "service window is already committed");
  }

  const std::uint64_t digest = codec::evidence_digest(snapshot);
  const auto key = std::make_pair(snapshot.id.value(), snapshot.generation.value());
  const auto existing = impl_->ingested.find(key);
  if (existing != impl_->ingested.end()) {
    if (existing->second != digest) {
      return Status(StatusCode::EvidenceContradictory,
                    "the same evidence identity arrived with different content");
    }
    IngestResult result;
    result.disposition = IngestResult::Disposition::DuplicateIgnored;
    result.id = snapshot.id;
    result.generation = snapshot.generation;
    result.window = snapshot.window;
    result.observation_count = static_cast<std::uint32_t>(snapshot.observations.size());
    return result;
  }

  const std::uint64_t pkey = publisher_key(snapshot.producer);
  const auto sequence = impl_->publisher_sequences.find(pkey);
  if (sequence != impl_->publisher_sequences.end() && snapshot.sequence < sequence->second) {
    return Status(StatusCode::SequenceViolation, "evidence sequence went backwards");
  }

  const auto staged_entry = impl_->staged.find(snapshot.window.value());
  if (staged_entry != impl_->staged.end() &&
      snapshot.generation <= staged_entry->second.generation) {
    return Status(StatusCode::StaleGeneration, "a newer snapshot for this window is already staged");
  }

  IngestResult result;
  result.disposition = IngestResult::Disposition::Staged;
  result.id = snapshot.id;
  result.generation = snapshot.generation;
  result.window = snapshot.window;
  result.observation_count = static_cast<std::uint32_t>(snapshot.observations.size());

  impl_->ingested.emplace(key, digest);
  if (sequence == impl_->publisher_sequences.end()) {
    impl_->publisher_sequences.emplace(pkey, snapshot.sequence);
  } else {
    sequence->second = std::max(sequence->second, snapshot.sequence);
  }
  impl_->staged[snapshot.window.value()] = std::move(snapshot);

  while (impl_->staged.size() > impl_->config.limits.max_staged_snapshots) {
    impl_->staged.erase(impl_->staged.begin());
  }
  while (impl_->ingested.size() > static_cast<std::size_t>(impl_->config.limits.max_staged_snapshots) *
                                      4u) {
    impl_->ingested.erase(impl_->ingested.begin());
  }
  while (impl_->publisher_sequences.size() > 1024u) {
    impl_->publisher_sequences.erase(impl_->publisher_sequences.begin());
  }
  return result;
}

AuthorityVector FairnessGovernor::live_authority(const EvaluationRequest& request) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  AuthorityVector authority;
  authority.policy_id = impl_->policy.id;
  authority.policy_generation = impl_->policy.generation;
  authority.epoch = impl_->epoch;
  authority.evidence_id = request.evidence_id;
  authority.evidence_generation = request.evidence_generation;
  authority.window = request.window;
  authority.window_generation = request.window_generation;
  authority.accounting_generation = impl_->accounting.generation;
  authority.governor = impl_->incarnation;
  authority.request_id = request.request_id;
  authority.attempt = request.attempt;
  return authority;
}

Result<FairnessDecision> FairnessGovernor::evaluate(const EvaluationRequest& request) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->stopping.load(std::memory_order_acquire)) {
    return Status(StatusCode::ShuttingDown, "governor is shutting down");
  }
  EvaluationContext context;
  context.policy = &impl_->policy;
  context.accounting = &impl_->accounting;
  context.request = request;
  context.current_epoch = impl_->epoch;
  context.current_window = impl_->current_window();
  context.last_committed_window = impl_->accounting.last_window;
  const auto staged_entry = impl_->staged.find(request.window.value());
  context.evidence = staged_entry == impl_->staged.end() ? nullptr : &staged_entry->second;

  Result<FairnessDecision> evaluated = evaluate_fairness(context);
  if (!evaluated.ok()) {
    return evaluated;
  }
  evaluated.value().authority.governor = impl_->incarnation;
  return evaluated;
}

Status FairnessGovernor::commit(const FairnessDecision& decision) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->stopping.load(std::memory_order_acquire)) {
    return Status(StatusCode::ShuttingDown, "governor is shutting down");
  }
  if (decision.authority.governor != impl_->incarnation) {
    return Status(StatusCode::StaleIncarnation, "decision was produced by another incarnation");
  }
  if (decision.authority.epoch != impl_->epoch) {
    return Status(StatusCode::StaleEpoch, "decision was taken in a superseded fabric epoch");
  }
  if (decision.authority.policy_id != impl_->policy.id ||
      decision.authority.policy_generation != impl_->policy.generation) {
    return Status(StatusCode::PolicyGenerationMismatch,
                  "decision was taken against a superseded policy generation");
  }
  if (decision.authority.accounting_generation != impl_->accounting.generation) {
    return Status(StatusCode::StaleGeneration,
                  "decision was taken against a superseded accounting generation");
  }
  if (!decision.authority.window.valid() || !decision.authority.window_generation.valid()) {
    return Status(StatusCode::InvalidArgument, "decision does not name a service window");
  }
  if (impl_->accounting.last_window.valid() &&
      decision.authority.window.value() <= impl_->accounting.last_window.value()) {
    return Status(StatusCode::StaleGeneration, "service window was already committed");
  }
  if (decision.subjects.size() != impl_->policy.subjects.size()) {
    return Status(StatusCode::Conflict, "decision does not cover the installed policy population");
  }
  if (codec::decision_digest(decision) != decision.content_digest) {
    return Status(StatusCode::Conflict, "decision content digest does not match its contents");
  }
  const std::uint64_t augment_declared = decision.correction.augment_units;
  if (augment_declared != decision.correction.reduce_units) {
    return Status(StatusCode::Conflict, "corrective plan does not conserve units");
  }
  std::uint64_t augment_sum = 0;
  std::uint64_t reduce_sum = 0;
  for (const CorrectiveIntent& intent : decision.intents) {
    if (intent.direction == CorrectionDirection::Augment) {
      if (!checked_add(augment_sum, intent.units, augment_sum)) {
        return Status(StatusCode::ArithmeticOverflow, "intent total overflows");
      }
    } else {
      if (!checked_add(reduce_sum, intent.units, reduce_sum)) {
        return Status(StatusCode::ArithmeticOverflow, "intent total overflows");
      }
    }
  }
  if (augment_sum != augment_declared || reduce_sum != decision.correction.reduce_units) {
    return Status(StatusCode::Conflict, "corrective intents do not match the declared plan");
  }
  if (decision.intents.size() > kMaxCorrectiveIntents) {
    return Status(StatusCode::LimitExceeded, "corrective intent count exceeds the bound");
  }

  // --- Apply ---------------------------------------------------------------
  FairnessAccounting previous = impl_->accounting;
  std::unordered_map<std::uint64_t, const Subject*> policy_index;
  policy_index.reserve(impl_->policy.subjects.size() * 2 + 1);
  for (const Subject& subject : impl_->policy.subjects) {
    policy_index.emplace(subject.id.value(), &subject);
  }
  std::unordered_map<std::uint64_t, std::size_t> accounting_index;
  accounting_index.reserve(impl_->accounting.subjects.size() * 2 + 1);
  for (std::size_t index = 0; index < impl_->accounting.subjects.size(); ++index) {
    accounting_index.emplace(impl_->accounting.subjects[index].id.value(), index);
  }
  InterventionGeneration generation = impl_->accounting.intervention_generation;
  for (const CorrectiveIntent& intent : decision.intents) {
    if (intent.generation.value() > generation.value()) {
      generation = intent.generation;
    }
  }

  for (const SubjectFairnessState& state : decision.subjects) {
    if (!state.has_authority) {
      continue;
    }
    const auto policy_entry = policy_index.find(state.id.value());
    const Subject* subject =
        policy_entry == policy_index.end() ? nullptr : policy_entry->second;
    if (subject == nullptr || subject->generation != state.generation) {
      impl_->accounting = std::move(previous);
      return Status(StatusCode::Conflict, "decision names a subject the policy does not govern");
    }
    const auto existing = accounting_index.find(state.id.value());
    SubjectAccounting* entry = nullptr;
    if (existing == accounting_index.end()) {
      SubjectAccounting created;
      created.id = state.id;
      created.generation = state.generation;
      impl_->accounting.subjects.push_back(created);
      accounting_index.emplace(state.id.value(), impl_->accounting.subjects.size() - 1);
      entry = &impl_->accounting.subjects.back();
    } else {
      entry = &impl_->accounting.subjects[existing->second];
    }
    const std::int64_t net = static_cast<std::int64_t>(state.entitlement_units) -
                             static_cast<std::int64_t>(state.served_units);
    CarryOutcome carry;
    if (!apply_window_carry(*entry, net, impl_->policy.max_carry_units, carry)) {
      impl_->accounting = std::move(previous);
      return Status(StatusCode::ArithmeticOverflow, "carry application overflows");
    }
    entry->cumulative_deficit = carry.cumulative_deficit;
    entry->cumulative_surplus = carry.cumulative_surplus;
    entry->discarded_deficit = carry.discarded_deficit;
    entry->discarded_surplus = carry.discarded_surplus;
    entry->unserved_streak = state.unserved_streak;
    entry->below_floor_streak = state.below_floor_streak;
    if (!checked_add(entry->windows_observed, std::uint64_t{1}, entry->windows_observed) ||
        !checked_add(entry->total_served_units, state.served_units, entry->total_served_units)) {
      impl_->accounting = std::move(previous);
      return Status(StatusCode::ArithmeticOverflow, "accounting counters overflow");
    }
  }
  for (const CorrectiveIntent& intent : decision.intents) {
    const auto index = accounting_index.find(intent.subject.value());
    if (index == accounting_index.end()) {
      continue;
    }
    SubjectAccounting* entry = &impl_->accounting.subjects[index->second];
    if (intent.generation.value() > entry->last_correction_generation.value()) {
      entry->last_correction_window = decision.authority.window;
      entry->last_correction_generation = intent.generation;
    }
  }

  impl_->accounting.intervention_generation = generation;
  impl_->accounting.last_window = decision.authority.window;
  impl_->accounting.last_window_generation = decision.authority.window_generation;
  impl_->accounting.writer_boot = impl_->incarnation.boot;
  impl_->accounting.policy_id = impl_->policy.id;
  impl_->accounting.policy_generation = impl_->policy.generation;
  impl_->accounting.epoch = impl_->epoch;
  if (!checked_add(impl_->accounting.generation, std::uint64_t{1}, impl_->accounting.generation)) {
    impl_->accounting = std::move(previous);
    return Status(StatusCode::ArithmeticOverflow, "accounting generation exhausted");
  }

  const Status committed = impl_->commit_accounting();
  if (!committed.ok()) {
    // The durable boundary was not crossed: no acknowledgement, no state change.
    impl_->accounting = std::move(previous);
    return committed;
  }
  return Status::success();
}

Status FairnessGovernor::shutdown() {
  if (impl_->stopping.exchange(true, std::memory_order_acq_rel)) {
    return Status::success();
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->staged.clear();
  impl_->ingested.clear();
  impl_->publisher_sequences.clear();
  impl_->store.reset();
  return Status::success();
}

}  // namespace fairness_governor
