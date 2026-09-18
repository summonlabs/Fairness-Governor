// Fairness Governor - priority/value modifier interaction tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(priority, modifier_redistributes_without_creating_units) {
  FairnessPolicy policy = flat_policy(2);
  policy.max_priority_modifier_bps = 5000;
  policy.subjects[0].priority_modifier_bps = 1000;   // +10%
  policy.subjects[1].priority_modifier_bps = -1000;  // -10%
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 500);
  observe(evidence, 2, 1, 500);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  std::int64_t delta_sum = 0;
  std::uint64_t entitlement_sum = 0;
  for (const SubjectFairnessState& state : decision.value().subjects) {
    delta_sum += state.priority_delta_units;
    entitlement_sum += state.entitlement_units;
  }
  FG_CHECK_EQ(entitlement_sum, 1000u);
  FG_CHECK_EQ(delta_sum, 0);
  // +-10% on equal weights is exactly 550/450 in the scaled weight domain.
  FG_CHECK_EQ(find_state(decision.value(), 1)->entitlement_units, 550u);
  FG_CHECK_EQ(find_state(decision.value(), 2)->entitlement_units, 450u);
  FG_CHECK_EQ(find_state(decision.value(), 1)->base_entitlement_units, 500u);
  FG_CHECK_EQ(find_state(decision.value(), 2)->base_entitlement_units, 500u);
  FG_CHECK_EQ(find_state(decision.value(), 1)->priority_delta_units, 50);
  FG_CHECK_EQ(find_state(decision.value(), 2)->priority_delta_units, -50);
}

FG_TEST(priority, base_entitlement_excludes_the_modifier) {
  FairnessPolicy policy = flat_policy(2);
  policy.max_priority_modifier_bps = 5000;
  policy.subjects[0].priority_modifier_bps = 2000;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 500);
  observe(evidence, 2, 1, 500);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  const SubjectFairnessState* boosted = find_state(decision.value(), 1);
  const SubjectFairnessState* plain = find_state(decision.value(), 2);
  FG_CHECK_EQ(boosted->base_entitlement_units, 500u);
  FG_CHECK_EQ(plain->base_entitlement_units, 500u);
  FG_CHECK_EQ(boosted->entitlement_units + plain->entitlement_units, 1000u);
}

FG_TEST(priority, priority_never_waives_a_starvation_threshold) {
  // The lowest-value subject still starves at its explicit threshold.
  FairnessPolicy policy = flat_policy(2);
  policy.max_priority_modifier_bps = 10000;
  policy.subjects[0].priority_modifier_bps = -10000;
  policy.subjects[0].starvation_windows = 2;
  policy.subjects[0].priority_rank = 0;
  policy.subjects[1].priority_modifier_bps = 10000;
  policy.subjects[1].priority_rank = 10;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0, 4);
  observe(evidence, 2, 1, 1000);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->outcome, Outcome::StarvationRisk);
}

FG_TEST(priority, a_modifier_cannot_zero_the_weight_and_starvation_still_governs) {
  FairnessPolicy policy = flat_policy(2);
  policy.max_priority_modifier_bps = 10000;
  policy.subjects[0].priority_modifier_bps = -10000;
  policy.subjects[0].starvation_windows = 2;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 1000);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  // The modifier scales the weight down to the smallest representable value but
  // never to zero, and a fully devalued subject still starves at its threshold.
  FG_CHECK(effective_share_weight(policy.subjects[0].share_weight, -10000) >= 1u);
  FG_CHECK_EQ(find_state(decision.value(), 1)->outcome, Outcome::StarvationRisk);
  // The 1:10000 weight ratio leaves the valued subject holding exactly the whole
  // observed service, so by policy it is neither over- nor under-served.
  FG_CHECK_EQ(find_state(decision.value(), 2)->entitlement_units, 1000u);
  FG_CHECK_EQ(find_state(decision.value(), 2)->outcome, Outcome::Fair);
}

FG_TEST(priority, a_modifier_beyond_the_policy_bound_is_clipped_and_reported) {
  FairnessPolicy policy = flat_policy(2);
  policy.max_priority_modifier_bps = 100000;
  policy.subjects[0].priority_modifier_bps = 50000;
  // Downgrade the policy bound after construction to model an unvalidated policy
  // reaching the engine directly.
  policy.max_priority_modifier_bps = 1000;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 500);
  observe(evidence, 2, 1, 500);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  const SubjectFairnessState* state = find_state(decision.value(), 1);
  FG_CHECK(state->modifier_clipped);
  FG_CHECK_EQ(state->applied_modifier_bps, 1000);
}

FG_TEST(priority, a_priority_reference_change_invalidates_the_evidence) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].priority.id = PriorityRef::from_value(9);
  policy.subjects[0].priority.generation = Generation<PriorityRefTag>::from_value(2);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  evidence.observations[0].priority.id = PriorityRef::from_value(9);
  evidence.observations[0].priority.generation = Generation<PriorityRefTag>::from_value(1);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->outcome, Outcome::Stale);
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::PriorityReferenceMismatch);
}

FG_TEST(priority, a_matching_priority_reference_is_authoritative) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].priority.id = PriorityRef::from_value(9);
  policy.subjects[0].priority.generation = Generation<PriorityRefTag>::from_value(2);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  evidence.observations[0].priority = policy.subjects[0].priority;
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->outcome, Outcome::Fair);
}
