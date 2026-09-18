// Fairness Governor - basic engine outcome tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(engine, proportional_service_is_fair) {
  const FairnessPolicy policy = flat_policy(2);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 500);
  observe(evidence, 2, 1, 500);
  const EvaluationRequest request = make_request(policy, evidence);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, request, evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(decision.value().outcome, Outcome::Fair);
  FG_CHECK_EQ(decision.value().counts.fair, 2u);
  FG_CHECK_EQ(decision.value().correction.augment_units, 0u);
  FG_CHECK_EQ(decision.value().correction.reduce_units, 0u);
}

FG_TEST(engine, entitlement_tracks_share_weight_not_equality) {
  // Fairness is not equality: with weights 1 and 3 the equal split is unfair,
  // and the proportional split is fair.
  FairnessPolicy policy = flat_policy(2);
  policy.subjects[1].share_weight = 3;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 500);
  observe(evidence, 2, 1, 500);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  const SubjectFairnessState* first = find_state(decision.value(), 1);
  const SubjectFairnessState* second = find_state(decision.value(), 2);
  FG_CHECK(first != nullptr && second != nullptr);
  if (first == nullptr || second == nullptr) {
    return;
  }
  FG_CHECK_EQ(first->entitlement_units, 250u);
  FG_CHECK_EQ(second->entitlement_units, 750u);
  // Equal absolute service is unfair because the entitlements differ: the
  // heavy-weight subject is under-served and the light one is over-served.
  FG_CHECK_EQ(first->outcome, Outcome::OverServed);
  FG_CHECK_EQ(second->outcome, Outcome::CorrectionRequired);
}

FG_TEST(engine, entitlement_always_sums_to_observed_service) {
  Rng rng(0xE171ULL);
  for (int iteration = 0; iteration < 300; ++iteration) {
    const std::uint32_t count = 1 + static_cast<std::uint32_t>(rng.below(10));
    FairnessPolicy policy = flat_policy(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      policy.subjects[i].share_weight = 1 + static_cast<std::uint32_t>(rng.below(50));
    }
    EvidenceSnapshot evidence = make_evidence(10, 1);
    for (std::uint32_t i = 0; i < count; ++i) {
      observe(evidence, i + 1, 1, rng.below(10000));
    }
    Result<FairnessDecision> decision =
        run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
    FG_CHECK_OK(decision.status());
    std::uint64_t entitlement_sum = 0;
    std::uint64_t served_sum = 0;
    for (const SubjectFairnessState& state : decision.value().subjects) {
      entitlement_sum += state.entitlement_units;
      served_sum += state.served_units;
    }
    FG_CHECK_EQ(entitlement_sum, served_sum);
  }
}

FG_TEST(engine, deviation_signs_follow_service_minus_entitlement) {
  const FairnessPolicy policy = flat_policy(2);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 900);
  observe(evidence, 2, 1, 100);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  const SubjectFairnessState* first = find_state(decision.value(), 1);
  const SubjectFairnessState* second = find_state(decision.value(), 2);
  FG_CHECK(first != nullptr && second != nullptr);
  if (first == nullptr || second == nullptr) {
    return;
  }
  FG_CHECK_EQ(first->deviation_units, 400);
  FG_CHECK_EQ(second->deviation_units, -400);
  FG_CHECK_EQ(first->surplus_units, 400u);
  FG_CHECK_EQ(second->deficit_units, 400u);
}

FG_TEST(engine, group_entitlement_is_the_parent_allocation) {
  FairnessPolicy policy = flat_policy(2);
  FairnessGroup nested;
  nested.id = FairnessGroupId::from_value(2);
  nested.generation = FairnessGroupGeneration::from_value(1);
  nested.parent = FairnessGroupId::from_value(1);
  policy.groups.push_back(nested);
  policy.subjects[1].group = nested.id;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 400);
  observe(evidence, 2, 1, 600);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(decision.value().groups.size(), 2u);
  // The root owns the whole; the nested group and the ungrouped subject split it
  // by weight, so the nested group owns half and the subject inside it owns all
  // of that half.
  FG_CHECK_EQ(find_state(decision.value(), 2)->group_entitlement_units, 500u);
  std::uint64_t group_total = 0;
  for (const GroupFairnessState& group : decision.value().groups) {
    group_total += group.entitlement_units;
  }
  FG_CHECK_EQ(group_total, 1000u + 500u);
}

FG_TEST(engine, zero_service_is_fair_without_a_starvation_threshold) {
  const FairnessPolicy policy = flat_policy(2);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0);
  observe(evidence, 2, 1, 0);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  // Nothing was served, so every entitlement is zero and the deviation is zero.
  FG_CHECK_EQ(decision.value().outcome, Outcome::Fair);
  // But the unserved streak still advances in the durable projection.
  FG_CHECK_EQ(find_state(decision.value(), 1)->unserved_streak, 1u);
}

FG_TEST(engine, evidence_for_unrelated_subjects_is_noted_and_ignored) {
  const FairnessPolicy policy = flat_policy(1);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  observe(evidence, 99, 1, 100);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(decision.value().subjects.size(), 1u);
  FG_CHECK(!decision.value().notes.empty());
}

FG_TEST(engine, group_cycle_is_reported_by_the_engine_when_unvalidated) {
  FairnessPolicy policy = flat_policy(1);
  FairnessGroup second;
  second.id = FairnessGroupId::from_value(2);
  second.generation = FairnessGroupGeneration::from_value(1);
  second.parent = FairnessGroupId::from_value(1);
  policy.groups[0].parent = second.id;
  policy.groups.push_back(second);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 10);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK(!decision.ok());
  FG_CHECK(decision.code() == StatusCode::GroupDepthExceeded ||
           decision.code() == StatusCode::PolicyInvalid);
}

FG_TEST(engine, null_context_is_rejected) {
  EvaluationContext context;
  Result<FairnessDecision> decision = evaluate_fairness(context);
  FG_CHECK_EQ(decision.code(), StatusCode::InvalidArgument);
}
