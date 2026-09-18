// Fairness Governor - stateful governor lifecycle tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

Result<std::unique_ptr<FairnessGovernor>> open_fixture(FairnessPolicy policy,
                                                       const std::string& store) {
  GovernorConfig config;
  config.policy = std::move(policy);
  config.epoch = config.policy.epoch;
  config.store_path = store;
  return FairnessGovernor::open(config);
}

}  // namespace

FG_TEST(lifecycle, in_memory_governor_evaluates_and_commits) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  FG_CHECK(governor->incarnation().valid());
  FG_CHECK_EQ(governor->accounting_generation(), 0u);
  FG_CHECK(!governor->recovery().store_present);

  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision =
      governor->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(decision.value().authority.governor, governor->incarnation());
  FG_CHECK_OK(governor->commit(decision.value()));
  FG_CHECK_EQ(governor->accounting_generation(), 1u);
  FG_CHECK_EQ(governor->last_committed_window().value(), 500u);

  const FairnessAccounting accounting = governor->accounting();
  FG_CHECK_EQ(accounting.subjects.size(), 2u);
  const SubjectAccounting* first = accounting.find(SubjectId::from_value(1));
  FG_CHECK(first != nullptr);
  if (first == nullptr) {
    return;
  }
  FG_CHECK_EQ(first->cumulative_deficit, 200u);
  FG_CHECK_EQ(first->unserved_streak, 3u);
  FG_CHECK_EQ(first->windows_observed, 1u);
  FG_CHECK_OK(governor->shutdown());
  FG_CHECK(governor->shutting_down());
}

FG_TEST(lifecycle, a_decision_cannot_be_committed_twice) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());
  FG_CHECK_OK(governor->commit(decision.value()));
  FG_CHECK_EQ(governor->commit(decision.value()).code(), StatusCode::StaleGeneration);
}

FG_TEST(lifecycle, a_decision_from_another_incarnation_is_rejected) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());
  FairnessDecision forged = decision.value();
  forged.authority.governor.ordinal += 1;
  FG_CHECK_EQ(governor->commit(forged).code(), StatusCode::StaleIncarnation);
}

FG_TEST(lifecycle, a_tampered_decision_is_rejected) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());
  FairnessDecision forged = decision.value();
  forged.correction.augment_units += 1;
  FG_CHECK_EQ(governor->commit(forged).code(), StatusCode::Conflict);

  forged = decision.value();
  forged.intents[0].units += 1;
  FG_CHECK_EQ(governor->commit(forged).code(), StatusCode::Conflict);

  forged = decision.value();
  forged.subjects.pop_back();
  FG_CHECK_EQ(governor->commit(forged).code(), StatusCode::Conflict);
}

FG_TEST(lifecycle, a_policy_change_invalidates_an_outstanding_decision) {
  FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());

  FairnessPolicy next = policy;
  next.generation = FairnessPolicyGeneration::from_value(2);
  next.subjects[1].share_weight = 5;
  FG_CHECK_OK(governor->install_policy(next));
  FG_CHECK_EQ(governor->policy_generation().value(), 2u);
  FG_CHECK_EQ(governor->commit(decision.value()).code(), StatusCode::PolicyGenerationMismatch);
}

FG_TEST(lifecycle, policy_installation_requires_an_advancing_generation) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  FG_CHECK_EQ(governor->install_policy(policy).code(), StatusCode::StaleGeneration);
  FairnessPolicy invalid = policy;
  invalid.generation = FairnessPolicyGeneration::from_value(2);
  invalid.subjects[0].share_weight = 0;
  FG_CHECK_EQ(governor->install_policy(invalid).code(), StatusCode::OutOfRange);
  FairnessPolicy other_id = policy;
  other_id.generation = FairnessPolicyGeneration::from_value(2);
  other_id.id = FairnessPolicyId::from_value(9);
  FG_CHECK_EQ(governor->install_policy(other_id).code(), StatusCode::Conflict);
}

FG_TEST(lifecycle, epoch_advance_invalidates_staged_evidence) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  FG_CHECK_EQ(governor->staged_snapshot_count(), 1u);
  FG_CHECK_EQ(governor->advance_epoch(FabricEpoch::from_value(1)).code(), StatusCode::StaleEpoch);
  FG_CHECK_OK(governor->advance_epoch(FabricEpoch::from_value(2)));
  FG_CHECK_EQ(governor->epoch().value(), 2u);
  FG_CHECK_EQ(governor->staged_snapshot_count(), 0u);
  FG_CHECK_EQ(governor->ingest_evidence(evidence).code(), StatusCode::StaleEpoch);
}

FG_TEST(lifecycle, ingestion_rejects_stale_and_contradictory_evidence) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());

  // The same identity with identical content is an idempotent duplicate.
  Result<IngestResult> duplicate = governor->ingest_evidence(evidence);
  FG_CHECK_OK(duplicate.status());
  FG_CHECK_EQ(duplicate.value().disposition, IngestResult::Disposition::DuplicateIgnored);

  // The same identity with different content is contradictory.
  EvidenceSnapshot changed = evidence;
  changed.observations[1].served_units = 900;
  FG_CHECK_EQ(governor->ingest_evidence(changed).code(), StatusCode::EvidenceContradictory);

  // A stale epoch is refused.
  EvidenceSnapshot stale = make_evidence(501, 2);
  observe(stale, 1, 1, 10);
  stale.id = EvidenceSnapshotId::from_value(11);
  FG_CHECK_EQ(governor->ingest_evidence(stale).code(), StatusCode::StaleEpoch);

  // A sequence that goes backwards is refused.
  EvidenceSnapshot backwards = make_evidence(501, 1, 12, 1);
  backwards.sequence = 0;
  observe(backwards, 1, 1, 10);
  FG_CHECK_EQ(governor->ingest_evidence(backwards).code(), StatusCode::SequenceViolation);
}

FG_TEST(lifecycle, a_committed_window_cannot_be_re_ingested) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());
  FG_CHECK_OK(governor->commit(decision.value()));
  EvidenceSnapshot replay = make_evidence(500, 1, 10, 2);
  observe(replay, 1, 1, 0, 4);
  observe(replay, 2, 1, 800);
  FG_CHECK_EQ(governor->ingest_evidence(replay).code(), StatusCode::StaleGeneration);
}

FG_TEST(lifecycle, shutdown_rejects_further_work) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  FG_CHECK_OK(governor->shutdown());
  FG_CHECK_OK(governor->shutdown());
  FG_CHECK_EQ(governor->ingest_evidence(evidence).code(), StatusCode::ShuttingDown);
  FG_CHECK_EQ(governor->evaluate(make_request(policy, evidence)).code(),
              StatusCode::ShuttingDown);
  FG_CHECK_EQ(governor->staged_snapshot_count(), 0u);
}

FG_TEST(lifecycle, opening_rejects_incoherent_configuration) {
  FairnessPolicy policy = starvation_policy();
  GovernorConfig config;
  config.policy = policy;
  config.epoch = FabricEpoch::from_value(2);
  FG_CHECK_EQ(FairnessGovernor::open(config).code(), StatusCode::Conflict);

  config.epoch = policy.epoch;
  config.require_durability = true;
  FG_CHECK_EQ(FairnessGovernor::open(config).code(), StatusCode::InvalidArgument);

  config.require_durability = false;
  config.limits.max_staged_snapshots = 0;
  FG_CHECK_EQ(FairnessGovernor::open(config).code(), StatusCode::InvalidArgument);

  config.limits.max_staged_snapshots = 8;
  policy.subjects[0].share_weight = 0;
  config.policy = policy;
  FG_CHECK_EQ(FairnessGovernor::open(config).code(), StatusCode::OutOfRange);
}
