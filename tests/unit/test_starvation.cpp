// Fairness Governor - starvation semantics tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(starvation, threshold_boundary_is_exact) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].starvation_windows = 3;
  for (std::uint32_t streak = 0; streak <= 5; ++streak) {
    EvidenceSnapshot evidence = make_evidence(10, 1);
    observe(evidence, 1, 1, 0, streak);
    Result<FairnessDecision> decision =
        run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
    FG_CHECK_OK(decision.status());
    const SubjectFairnessState* state = find_state(decision.value(), 1);
    if (streak >= 3) {
      FG_CHECK_EQ(state->outcome, Outcome::StarvationRisk);
      FG_CHECK_EQ(state->reason, ReasonCode::StarvationUnserved);
    } else {
      FG_CHECK(state->outcome != Outcome::StarvationRisk);
    }
  }
}

FG_TEST(starvation, the_higher_of_durable_and_reported_streak_wins) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].starvation_windows = 3;
  policy.subjects[0].guarantee_floor = 100;
  FairnessAccounting accounting;
  accounting.policy_id = policy.id;
  accounting.policy_generation = policy.generation;
  accounting.epoch = policy.epoch;
  SubjectAccounting entry;
  entry.id = policy.subjects[0].id;
  entry.generation = policy.subjects[0].generation;
  entry.unserved_streak = 5;
  accounting.subjects.push_back(entry);

  // A producer claiming a reset streak cannot clear the durable counter.
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0, 1);
  EvaluationContext context;
  context.policy = &policy;
  context.accounting = &accounting;
  context.evidence = &evidence;
  context.request = make_request(policy, evidence);
  context.current_epoch = policy.epoch;
  context.current_window = evidence.window;
  Result<FairnessDecision> decision = evaluate_fairness(context);
  FG_CHECK_OK(decision.status());
  const SubjectFairnessState* state = find_state(decision.value(), 1);
  FG_CHECK_EQ(state->unserved_streak, 6u);
  FG_CHECK_EQ(state->outcome, Outcome::StarvationRisk);
}

FG_TEST(starvation, a_served_window_resets_the_streak) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].starvation_windows = 2;
  FairnessAccounting accounting;
  accounting.policy_id = policy.id;
  SubjectAccounting entry;
  entry.id = policy.subjects[0].id;
  entry.generation = policy.subjects[0].generation;
  entry.unserved_streak = 5;
  accounting.subjects.push_back(entry);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 50);
  EvaluationContext context;
  context.policy = &policy;
  context.accounting = &accounting;
  context.evidence = &evidence;
  context.request = make_request(policy, evidence);
  context.current_epoch = policy.epoch;
  context.current_window = evidence.window;
  Result<FairnessDecision> decision = evaluate_fairness(context);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->unserved_streak, 0u);
  FG_CHECK_EQ(find_state(decision.value(), 1)->outcome, Outcome::Fair);
}

FG_TEST(starvation, below_floor_threshold_is_independent) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].guarantee_floor = 500;
  policy.subjects[0].guarantee_starvation_windows = 3;
  for (std::uint32_t streak = 0; streak <= 4; ++streak) {
    EvidenceSnapshot evidence = make_evidence(10, 1);
    observe(evidence, 1, 1, 100, 0, streak);
    Result<FairnessDecision> decision =
        run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
    FG_CHECK_OK(decision.status());
    const SubjectFairnessState* state = find_state(decision.value(), 1);
    if (streak >= 3) {
      FG_CHECK_EQ(state->outcome, Outcome::StarvationRisk);
      FG_CHECK_EQ(state->reason, ReasonCode::StarvationBelowFloor);
    } else {
      FG_CHECK(state->outcome != Outcome::StarvationRisk);
    }
  }
}

FG_TEST(starvation, a_zero_threshold_disables_the_rule) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].starvation_windows = 0;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0, 1000);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK(find_state(decision.value(), 1)->outcome != Outcome::StarvationRisk);
}

FG_TEST(starvation, long_starvation_windows_are_counted_without_wrapping) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].starvation_windows = 1000000;
  FairnessAccounting accounting;
  accounting.policy_id = policy.id;
  SubjectAccounting entry;
  entry.id = policy.subjects[0].id;
  entry.generation = policy.subjects[0].generation;
  entry.unserved_streak = 999999;
  accounting.subjects.push_back(entry);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0);
  EvaluationContext context;
  context.policy = &policy;
  context.accounting = &accounting;
  context.evidence = &evidence;
  context.request = make_request(policy, evidence);
  context.current_epoch = policy.epoch;
  context.current_window = evidence.window;
  Result<FairnessDecision> decision = evaluate_fairness(context);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->unserved_streak, 1000000u);
  FG_CHECK_EQ(find_state(decision.value(), 1)->outcome, Outcome::StarvationRisk);
}

FG_TEST(starvation, starvation_is_reported_even_with_no_correction_available) {
  FairnessPolicy policy = flat_policy(2);
  policy.subjects[0].starvation_windows = 1;
  policy.subjects[0].max_augment_units = 0;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0, 5);
  observe(evidence, 2, 1, 500);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  // No budget was authorized, so the outcome stays STARVATION_RISK rather than
  // being silently downgraded.
  FG_CHECK_EQ(find_state(decision.value(), 1)->outcome, Outcome::StarvationRisk);
  FG_CHECK_EQ(decision.value().correction.authorized_budget_units, 0u);
}
