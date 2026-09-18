// Fairness Governor - bounded corrective intent tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

std::uint64_t intent_total(const FairnessDecision& decision, CorrectionDirection direction) {
  std::uint64_t total = 0;
  for (const CorrectiveIntent& intent : decision.intents) {
    if (intent.direction == direction) {
      total += intent.units;
    }
  }
  return total;
}

}  // namespace

FG_TEST(correction, plan_conserves_units) {
  Rng rng(0xC0FFEE11ULL);
  for (int iteration = 0; iteration < 400; ++iteration) {
    const std::uint32_t count = 2 + static_cast<std::uint32_t>(rng.below(6));
    FairnessPolicy policy = flat_policy(count);
    policy.fair_band_units = rng.below(50);
    policy.correction_threshold_units = policy.fair_band_units + rng.below(200);
    policy.max_correction_units = rng.below(100000);
    policy.max_correction_bps = static_cast<std::uint32_t>(rng.below(10001));
    for (std::uint32_t i = 0; i < count; ++i) {
      policy.subjects[i].max_augment_units = rng.below(5000);
      policy.subjects[i].max_reduce_units = rng.below(5000);
      policy.subjects[i].guarantee_floor = rng.chance(20) ? rng.below(2000) : 0;
      policy.subjects[i].protected_obligation = policy.subjects[i].guarantee_floor > 0;
      policy.subjects[i].obligation_rank = policy.subjects[i].protected_obligation ? 1u : 0u;
    }
    EvidenceSnapshot evidence = make_evidence(10, 1);
    for (std::uint32_t i = 0; i < count; ++i) {
      observe(evidence, i + 1, 1, rng.below(8000));
    }
    Result<FairnessDecision> decision =
        run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
    FG_CHECK_OK(decision.status());
    const FairnessDecision& value = decision.value();
    // The two sides of a corrective plan always balance: the governor proposes a
    // redistribution, never a creation of service.
    FG_CHECK_EQ(value.correction.augment_units, value.correction.reduce_units);
    FG_CHECK_EQ(intent_total(value, CorrectionDirection::Augment),
                value.correction.augment_units);
    FG_CHECK_EQ(intent_total(value, CorrectionDirection::Reduce),
                value.correction.reduce_units);
    FG_CHECK(value.correction.augment_units <= policy.max_correction_units);
    std::uint64_t served_total = 0;
    for (const SubjectFairnessState& state : value.subjects) {
      served_total += state.served_units;
    }
    FG_CHECK(value.correction.augment_units <=
             served_total * policy.max_correction_bps / 10000);
  }
}

FG_TEST(correction, subject_caps_are_respected) {
  FairnessPolicy policy = flat_policy(2);
  policy.subjects[0].max_augment_units = 5;
  policy.subjects[1].max_reduce_units = 1000;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0);
  observe(evidence, 2, 1, 1000);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  const SubjectFairnessState* starved = find_state(decision.value(), 1);
  FG_CHECK_EQ(starved->entitlement_units, 500u);
  FG_CHECK_EQ(starved->proposed_augment_units, 5u);
  FG_CHECK_EQ(starved->unsatisfied_demand_units, 495u);
  FG_CHECK_EQ(decision.value().correction.unsatisfied_demand_units, 495u);
}

FG_TEST(correction, protected_obligation_withholds_reducible_units) {
  FairnessPolicy policy = flat_policy(2);
  // Subject 2 over-serves but is protected at a floor of 400.
  policy.subjects[1].guarantee_floor = 400;
  policy.subjects[1].protected_obligation = true;
  policy.subjects[1].obligation_rank = 3;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0);
  observe(evidence, 2, 1, 1000);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  const FairnessDecision& value = decision.value();
  // Subject 2 may only give up everything above its protected floor.
  FG_CHECK_EQ(value.correction.withheld_units, 0u);
  FG_CHECK_EQ(value.correction.reduce_units, 500u);
  FG_CHECK_EQ(find_state(value, 1)->outcome, Outcome::CorrectionRequired);
}

FG_TEST(correction, a_protected_source_blocks_the_correction_it_cannot_fund) {
  // Four subjects, deliberately over-committed floors so that one protected
  // subject ends up entitled to less than its own protected floor. Its surplus
  // then exceeds the headroom the obligation leaves, so part of it is withheld
  // and the strongest remaining demand cannot be funded.
  FairnessPolicy policy = flat_policy(4);
  policy.subjects[0].guarantee_floor = 1800;  // the strongest obligation
  policy.subjects[0].obligation_rank = 9;
  policy.subjects[0].protected_obligation = true;
  policy.subjects[0].max_augment_units = 100000;
  policy.subjects[1].guarantee_floor = 1500;
  policy.subjects[1].obligation_rank = 5;
  policy.subjects[1].protected_obligation = true;
  policy.subjects[1].max_reduce_units = 10000;
  policy.subjects[2].starvation_windows = 1;

  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0);
  observe(evidence, 2, 1, 2000);
  observe(evidence, 3, 1, 0, 3);
  observe(evidence, 4, 1, 0);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  const FairnessDecision& value = decision.value();

  const SubjectFairnessState* strongest = find_state(value, 1);
  const SubjectFairnessState* protected_source = find_state(value, 2);
  FG_CHECK(strongest != nullptr && protected_source != nullptr);
  if (strongest == nullptr || protected_source == nullptr) {
    return;
  }
  FG_CHECK_EQ(strongest->entitlement_units, 1800u);
  FG_CHECK_EQ(strongest->deficit_units, 1800u);
  FG_CHECK_EQ(protected_source->entitlement_units, 200u);
  FG_CHECK_EQ(protected_source->served_units, 2000u);
  // served - floor = 500 of reducible headroom against an 1800 unit surplus, so
  // 1300 units of the surplus are held back by the protected obligation.
  FG_CHECK_EQ(value.correction.withheld_units, 1300u);
  FG_CHECK_EQ(value.correction.reducible_pool_units, 500u);
  FG_CHECK_EQ(value.correction.authorized_budget_units, 500u);
  FG_CHECK_EQ(value.correction.augment_units, 500u);
  FG_CHECK_EQ(value.correction.reduce_units, 500u);
  FG_CHECK_EQ(value.correction.unsatisfied_demand_units, 1300u);
  FG_CHECK(value.correction.protected_obligation_blocked);
  // The partial correction is still proposed, but the outcome records that a
  // stronger obligation is what prevents the subject from being made whole.
  FG_CHECK_EQ(strongest->proposed_augment_units, 500u);
  FG_CHECK_EQ(strongest->unsatisfied_demand_units, 1300u);
  FG_CHECK_EQ(strongest->outcome, Outcome::BlockedByStrongerObligation);
  FG_CHECK_EQ(strongest->reason, ReasonCode::ProtectedObligationHeld);
  FG_CHECK_EQ(protected_source->outcome, Outcome::OverServed);
}

FG_TEST(correction, policy_total_budget_caps_the_plan) {
  FairnessPolicy policy = flat_policy(2);
  policy.max_correction_units = 100;
  policy.max_correction_bps = 10000;
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0);
  observe(evidence, 2, 1, 1000);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(decision.value().correction.augment_units, 100u);
  FG_CHECK(decision.value().correction.bounded_by_policy_total);
}

FG_TEST(correction, policy_basis_point_budget_caps_the_plan) {
  FairnessPolicy policy = flat_policy(2);
  policy.max_correction_units = 1u << 30;
  policy.max_correction_bps = 1000;  // 10%
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 0);
  observe(evidence, 2, 1, 1000);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(decision.value().correction.augment_units, 100u);
  FG_CHECK(decision.value().correction.bounded_by_policy_bps);
}

FG_TEST(correction, cooldown_suppresses_a_non_starvation_correction) {
  FairnessPolicy policy = flat_policy(2);
  policy.cooldown_windows = 5;
  FairnessAccounting accounting;
  accounting.policy_id = policy.id;
  accounting.epoch = policy.epoch;
  accounting.generation = 3;
  SubjectAccounting entry;
  entry.id = policy.subjects[0].id;
  entry.generation = policy.subjects[0].generation;
  entry.last_correction_window = ServiceWindowId::from_value(499);
  entry.last_correction_generation = InterventionGeneration::from_value(1);
  accounting.subjects.push_back(entry);

  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0);
  observe(evidence, 2, 1, 1000);
  EvaluationContext context;
  context.policy = &policy;
  context.accounting = &accounting;
  context.evidence = &evidence;
  context.request = make_request(policy, evidence);
  context.current_epoch = policy.epoch;
  context.current_window = evidence.window;
  Result<FairnessDecision> decision = evaluate_fairness(context);
  FG_CHECK_OK(decision.status());
  FG_CHECK_EQ(find_state(decision.value(), 1)->proposed_augment_units, 0u);
  FG_CHECK_EQ(find_state(decision.value(), 1)->reason, ReasonCode::CorrectiveCooldown);
  FG_CHECK_EQ(decision.value().correction.augment_units, 0u);
}

FG_TEST(correction, cooldown_never_suppresses_starvation_prevention) {
  FairnessPolicy policy = flat_policy(2);
  policy.cooldown_windows = 5;
  policy.subjects[0].starvation_windows = 1;
  FairnessAccounting accounting;
  accounting.policy_id = policy.id;
  accounting.epoch = policy.epoch;
  SubjectAccounting entry;
  entry.id = policy.subjects[0].id;
  entry.generation = policy.subjects[0].generation;
  entry.last_correction_window = ServiceWindowId::from_value(499);
  entry.last_correction_generation = InterventionGeneration::from_value(1);
  accounting.subjects.push_back(entry);

  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 4);
  observe(evidence, 2, 1, 1000);
  EvaluationContext context;
  context.policy = &policy;
  context.accounting = &accounting;
  context.evidence = &evidence;
  context.request = make_request(policy, evidence);
  context.current_epoch = policy.epoch;
  context.current_window = evidence.window;
  Result<FairnessDecision> decision = evaluate_fairness(context);
  FG_CHECK_OK(decision.status());
  FG_CHECK(find_state(decision.value(), 1)->proposed_augment_units > 0u);
  FG_CHECK_EQ(find_state(decision.value(), 1)->outcome, Outcome::StarvationRisk);
}

FG_TEST(correction, intention_identities_are_deterministic) {
  const FairnessPolicy policy = starvation_policy();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  const EvaluationRequest request = make_request(policy, evidence);
  Result<FairnessDecision> first = run_engine(policy, evidence, request, evidence.window);
  Result<FairnessDecision> second = run_engine(policy, evidence, request, evidence.window);
  FG_CHECK_OK(first.status());
  FG_CHECK_OK(second.status());
  FG_CHECK_EQ(first.value().intents.size(), second.value().intents.size());
  for (std::size_t i = 0; i < first.value().intents.size(); ++i) {
    FG_CHECK_EQ(first.value().intents[i].id.value(), second.value().intents[i].id.value());
    FG_CHECK_EQ(first.value().intents[i].generation.value(),
                second.value().intents[i].generation.value());
  }
  FG_CHECK_EQ(first.value().content_digest, second.value().content_digest);
}
