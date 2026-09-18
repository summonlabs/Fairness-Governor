// Fairness Governor - stale evidence and authority rejection tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

Result<FairnessDecision> evaluate_with(const FairnessPolicy& policy,
                                       const EvidenceSnapshot& evidence,
                                       EvaluationRequest request,
                                       ServiceWindowId current_window,
                                       ServiceWindowId last_committed = ServiceWindowId::none()) {
  FairnessAccounting accounting;
  accounting.policy_id = policy.id;
  accounting.policy_generation = policy.generation;
  accounting.epoch = policy.epoch;
  EvaluationContext context;
  context.policy = &policy;
  context.accounting = &accounting;
  context.evidence = &evidence;
  context.request = request;
  context.current_epoch = policy.epoch;
  context.current_window = current_window;
  context.last_committed_window = last_committed;
  return evaluate_fairness(context);
}

}  // namespace

FG_TEST(stale, missing_evidence_yields_unknown_not_fair) {
  const FairnessPolicy policy = flat_policy(2);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  EvaluationRequest request = make_request(policy, evidence);
  EvaluationContext context;
  context.policy = &policy;
  context.accounting = nullptr;
  FairnessAccounting accounting;
  context.accounting = &accounting;
  context.evidence = nullptr;
  context.request = request;
  context.current_epoch = policy.epoch;
  Result<FairnessDecision> decision = evaluate_fairness(context);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(decision.value().outcome, Outcome::Unknown);
  FG_CHECK_EQ(decision.value().counts.unknown, 2u);
  FG_CHECK_EQ(decision.value().correction.augment_units, 0u);
  FG_CHECK_EQ(decision.value().intents.size(), 0u);
}

FG_TEST(stale, epoch_mismatch_is_rejected) {
  const FairnessPolicy policy = flat_policy(1);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  evidence.epoch = FabricEpoch::from_value(9);
  EvaluationRequest request = make_request(policy, evidence);
  Result<FairnessDecision> decision = evaluate_with(policy, evidence, request, evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(decision.value().outcome, Outcome::Stale);
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::EvidenceStaleEpoch);
}

FG_TEST(stale, snapshot_generation_mismatch_is_rejected) {
  const FairnessPolicy policy = flat_policy(1);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  EvaluationRequest request = make_request(policy, evidence);
  request.evidence_generation = EvidenceSnapshotGeneration::from_value(2);
  Result<FairnessDecision> decision = evaluate_with(policy, evidence, request, evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::EvidenceStaleGeneration);
}

FG_TEST(stale, policy_generation_mismatch_is_rejected) {
  FairnessPolicy policy = flat_policy(1);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  EvaluationRequest request = make_request(policy, evidence);
  request.policy_generation = FairnessPolicyGeneration::from_value(2);
  Result<FairnessDecision> decision = evaluate_with(policy, evidence, request, evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::PolicyGenerationMismatch);
}

FG_TEST(stale, window_mismatch_is_rejected) {
  const FairnessPolicy policy = flat_policy(1);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  EvaluationRequest request = make_request(policy, evidence);
  request.window = ServiceWindowId::from_value(11);
  Result<FairnessDecision> decision = evaluate_with(policy, evidence, request, evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::EvidenceStaleWindow);
}

FG_TEST(stale, an_old_window_beyond_the_age_bound_is_rejected) {
  FairnessPolicy policy = flat_policy(1);
  policy.max_evidence_age_windows = 2;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  Result<FairnessDecision> decision =
      evaluate_with(policy, evidence, make_request(policy, evidence),
                    ServiceWindowId::from_value(13));
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::EvidenceStaleAge);

  Result<FairnessDecision> fresh =
      evaluate_with(policy, evidence, make_request(policy, evidence),
                    ServiceWindowId::from_value(12));
  FG_CHECK_OK(fresh.status());
  FG_CHECK_EQ(fresh.value().outcome, Outcome::Fair);
}

FG_TEST(stale, an_unattested_future_window_is_rejected) {
  const FairnessPolicy policy = flat_policy(1);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  Result<FairnessDecision> decision =
      evaluate_with(policy, evidence, make_request(policy, evidence),
                    ServiceWindowId::from_value(9));
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::EvidenceStaleWindow);
}

FG_TEST(stale, a_replayed_window_is_rejected) {
  const FairnessPolicy policy = flat_policy(1);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  Result<FairnessDecision> decision =
      evaluate_with(policy, evidence, make_request(policy, evidence), evidence.window,
                    ServiceWindowId::from_value(10));
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::EvidenceStaleWindow);
}

FG_TEST(stale, future_capture_instant_is_rejected) {
  FairnessPolicy policy = flat_policy(1);
  policy.max_evidence_age_ns = 1000;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  evidence.captured_at_ns = 5000;
  observe(evidence, 1, 1, 100);
  EvaluationRequest request = make_request(policy, evidence);
  request.now_ns = 1000;
  Result<FairnessDecision> decision = evaluate_with(policy, evidence, request, evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::EvidenceStaleAge);
}

FG_TEST(stale, aged_capture_instant_is_rejected) {
  FairnessPolicy policy = flat_policy(1);
  policy.max_evidence_age_ns = 1000;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  evidence.captured_at_ns = 1000;
  observe(evidence, 1, 1, 100);
  EvaluationRequest request = make_request(policy, evidence);
  request.now_ns = 1000 + 1001;
  Result<FairnessDecision> decision = evaluate_with(policy, evidence, request, evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::EvidenceStaleAge);
  request.now_ns = 2000;
  Result<FairnessDecision> exact = evaluate_with(policy, evidence, request, evidence.window);
  FG_CHECK_OK(exact.status());
  FG_CHECK_EQ(exact.value().outcome, Outcome::Fair);
}

FG_TEST(stale, missing_subject_becomes_unknown_or_stale) {
  const FairnessPolicy policy = flat_policy(2);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  EvaluationRequest request = make_request(policy, evidence);
  Result<FairnessDecision> unknown = evaluate_with(policy, evidence, request, evidence.window);
  FG_CHECK_OK(unknown.status());
  FG_CHECK_EQ(find_state(unknown.value(), 2)->outcome, Outcome::Unknown);

  request.treat_missing_as_unknown = false;
  Result<FairnessDecision> stale = evaluate_with(policy, evidence, request, evidence.window);
  FG_CHECK_OK(stale.status());
  FG_CHECK_EQ(find_state(stale.value(), 2)->outcome, Outcome::Stale);
}

FG_TEST(stale, subject_generation_mismatch_is_rejected) {
  const FairnessPolicy policy = flat_policy(1);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 2, 100);
  Result<FairnessDecision> decision =
      evaluate_with(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::SubjectGenerationMismatch);
}

FG_TEST(stale, stale_authority_never_plans_a_correction) {
  const FairnessPolicy policy = starvation_policy();
  EvidenceSnapshot evidence = make_evidence(500, 9);
  observe(evidence, 1, 1, 0, 10);
  observe(evidence, 2, 1, 800);
  Result<FairnessDecision> decision =
      evaluate_with(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(decision.value().outcome, Outcome::Stale);
  FG_CHECK_EQ(decision.value().intents.size(), 0u);
  FG_CHECK_EQ(decision.value().correction.authorized_budget_units, 0u);
  FG_CHECK(decision.value().correction.authority_missing);
}
