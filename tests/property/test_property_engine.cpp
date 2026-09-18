// Fairness Governor - seeded randomized engine invariants.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <set>
#include <vector>

#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

struct RandomPopulation {
  FairnessPolicy policy;
  EvidenceSnapshot evidence;
  EvaluationRequest request;
};

RandomPopulation random_population(Rng& rng, std::uint32_t subject_count) {
  RandomPopulation population;
  FairnessPolicy& policy = population.policy;
  policy.id = FairnessPolicyId::from_value(1);
  policy.generation = FairnessPolicyGeneration::from_value(1);
  policy.epoch = FabricEpoch::from_value(1);
  policy.fair_band_units = rng.below(64);
  policy.correction_threshold_units = policy.fair_band_units + rng.below(4096);
  policy.cooldown_windows = static_cast<std::uint32_t>(rng.below(4));
  policy.max_correction_units = rng.below(1ULL << 24);
  policy.max_correction_bps = static_cast<std::uint32_t>(rng.below(10001));
  policy.max_priority_modifier_bps = static_cast<std::uint32_t>(rng.below(10001));
  policy.max_evidence_age_windows = 1 + static_cast<std::uint32_t>(rng.below(4));
  policy.max_carry_units = 1 + rng.below(1ULL << 20);

  const std::uint32_t group_count = 1 + static_cast<std::uint32_t>(rng.below(4));
  for (std::uint32_t g = 0; g < group_count; ++g) {
    FairnessGroup group;
    group.id = FairnessGroupId::from_value(g + 1);
    group.generation = FairnessGroupGeneration::from_value(1);
    if (g > 0 && rng.chance(50)) {
      group.parent = FairnessGroupId::from_value(1 + rng.below(g));
    }
    group.share_weight = 1 + static_cast<std::uint32_t>(rng.below(20));
    // Group floors stay zero so that a generated policy is structurally valid
    // by construction and every iteration exercises the engine.
    group.guarantee_floor = 0;
    group.protected_obligation = rng.chance(30);
    group.obligation_rank = group.protected_obligation ? 1 + static_cast<std::uint32_t>(rng.below(4)) : 0;
    policy.groups.push_back(group);
  }

  EvidenceSnapshot& evidence = population.evidence;
  evidence.id = EvidenceSnapshotId::from_value(1);
  evidence.generation = EvidenceSnapshotGeneration::from_value(1);
  evidence.window = ServiceWindowId::from_value(100 + rng.below(50));
  evidence.window_generation = ServiceWindowGeneration::from_value(1);
  evidence.epoch = policy.epoch;
  evidence.sequence = 1;
  evidence.producer.name = "property";

  for (std::uint32_t i = 0; i < subject_count; ++i) {
    Subject subject;
    subject.id = SubjectId::from_value(i + 1);
    subject.generation = SubjectGeneration::from_value(1);
    // Round-robin so that every declared group governs at least one subject:
    // a policy with an empty group is structurally invalid by design.
    subject.group = FairnessGroupId::from_value(1 + (i % group_count));
    subject.share_weight = 1 + static_cast<std::uint32_t>(rng.below(kMaxShareWeight));
    subject.guarantee_floor = rng.chance(25) ? rng.below(2000) : 0;
    subject.starvation_windows = rng.chance(40) ? 1 + static_cast<std::uint32_t>(rng.below(5)) : 0;
    subject.guarantee_starvation_windows =
        subject.guarantee_floor > 0 && rng.chance(30)
            ? 1 + static_cast<std::uint32_t>(rng.below(4))
            : 0;
    subject.priority_modifier_bps = static_cast<std::int32_t>(rng.below(policy.max_priority_modifier_bps + 1)) -
                                    static_cast<std::int32_t>(rng.below(policy.max_priority_modifier_bps + 1));
    subject.priority_rank = static_cast<std::uint32_t>(rng.below(8));
    subject.obligation_rank = rng.chance(30) ? 1 + static_cast<std::uint32_t>(rng.below(4)) : 0;
    subject.protected_obligation = subject.obligation_rank > 0 && rng.chance(70);
    subject.max_reduce_units = rng.chance(10) ? 0 : rng.below(1ULL << 16);
    subject.max_augment_units = rng.chance(10) ? 0 : rng.below(1ULL << 16);
    policy.subjects.push_back(subject);

    SubjectObservation observation;
    observation.id = subject.id;
    observation.generation = subject.generation;
    observation.served_units = rng.chance(20) ? 0 : rng.below(1ULL << 16);
    observation.unserved_streak =
        observation.served_units == 0 ? static_cast<std::uint32_t>(rng.below(8)) : 0;
    observation.below_floor_streak =
        subject.guarantee_floor > 0 && observation.served_units < subject.guarantee_floor
            ? static_cast<std::uint32_t>(rng.below(6))
            : 0;
    evidence.observations.push_back(observation);
  }

  const PolicyValidation validation = validate_policy(policy);
  if (!validation.ok()) {
    // The generated policy was structurally invalid; the caller re-rolls.
    population.policy.subjects.clear();
  }
  population.request = make_request(policy, evidence);
  return population;
}

}  // namespace

FG_TEST(property_engine, randomized_populations_hold_every_invariant) {
  Rng rng(0x9E3779B9ULL);
  std::uint32_t evaluated = 0;
  for (int iteration = 0; iteration < 4000; ++iteration) {
    // At least four subjects, so every one of the at-most-four groups is
    // guaranteed to govern somebody: a policy with an empty group is invalid.
    const std::uint32_t count = 4 + static_cast<std::uint32_t>(rng.below(16));
    RandomPopulation population = random_population(rng, count);
    if (population.policy.subjects.empty()) {
      continue;
    }
    Result<FairnessDecision> decision =
        run_engine(population.policy, population.evidence, population.request,
                   population.evidence.window);
    FG_CHECK_OK(decision.status());
    const FairnessDecision& value = decision.value();
    ++evaluated;

    FG_CHECK_EQ(value.subjects.size(), population.policy.subjects.size());

    std::uint64_t entitlement_sum = 0;
    std::uint64_t served_sum = 0;
    std::uint64_t deficit_sum = 0;
    std::uint64_t surplus_sum = 0;
    for (const SubjectFairnessState& state : value.subjects) {
      FG_CHECK(state.entitlement_units <= kMaxServiceUnits);
      FG_CHECK(state.served_units <= kMaxServiceUnits);
      FG_CHECK(state.cumulative_deficit_after <= population.policy.max_carry_units);
      FG_CHECK(state.cumulative_surplus_after <= population.policy.max_carry_units);
      FG_CHECK(state.deviation_units == static_cast<std::int64_t>(state.served_units) -
                                            static_cast<std::int64_t>(state.entitlement_units));
      if (state.outcome == Outcome::CorrectionRequired) {
        FG_CHECK(state.proposed_augment_units > 0);
      }
      if (state.outcome == Outcome::BlockedByStrongerObligation) {
        // A blocked subject is short of what it needs; it may still have been
        // granted a partial correction that the obligation did not withhold.
        FG_CHECK(state.unsatisfied_demand_units > 0);
      }
      if (state.has_authority) {
        entitlement_sum += state.entitlement_units;
        served_sum += state.served_units;
        deficit_sum += state.deficit_units;
        surplus_sum += state.surplus_units;
        FG_CHECK(state.outcome != Outcome::Unknown);
        FG_CHECK(state.outcome != Outcome::Stale);
      } else {
        FG_CHECK(state.outcome == Outcome::Unknown || state.outcome == Outcome::Stale);
        FG_CHECK_EQ(state.proposed_augment_units, 0u);
        FG_CHECK_EQ(state.proposed_reduce_units, 0u);
      }
    }
    // Authoritative entitlements are an exact redistribution of observed service.
    FG_CHECK_EQ(entitlement_sum, served_sum);
    // Everything owed by one subject is owed to it by another.
    FG_CHECK_EQ(deficit_sum, surplus_sum);

    FG_CHECK_EQ(value.correction.augment_units, value.correction.reduce_units);
    FG_CHECK(value.correction.augment_units <= population.policy.max_correction_units);
    FG_CHECK(value.intents.size() <= kMaxCorrectiveIntents);

    // Bounded correction: a protected subject is never reduced below its floor.
    for (const SubjectFairnessState& state : value.subjects) {
      const Subject* subject = population.policy.find_subject(state.id);
      FG_CHECK(subject != nullptr);
      if (subject->protected_obligation && state.served_units > subject->guarantee_floor) {
        FG_CHECK(state.proposed_reduce_units <= state.served_units - subject->guarantee_floor);
      }
      FG_CHECK(state.proposed_reduce_units <= subject->max_reduce_units);
      FG_CHECK(state.proposed_augment_units <= subject->max_augment_units);
    }
    // Starvation is never silently downgraded to a lesser outcome.
    for (const SubjectFairnessState& state : value.subjects) {
      const bool starving =
          (state.starvation_limit_windows > 0 &&
           state.unserved_streak >= state.starvation_limit_windows) ||
          (state.guarantee_starvation_limit_windows > 0 &&
           state.below_floor_streak >= state.guarantee_starvation_limit_windows);
      if (starving && state.has_authority) {
        FG_CHECK(state.outcome == Outcome::StarvationRisk ||
                 state.outcome == Outcome::BlockedByStrongerObligation);
      }
    }
    // A stale or unknown population never plans anything.
    if (value.counts.stale > 0 || value.counts.unknown > 0) {
      FG_CHECK_EQ(value.intents.size(), 0u);
    }
  }
  // Every generated policy is valid by construction, so all iterations evaluate.
  FG_CHECK_EQ(evaluated, 4000u);
}

FG_TEST(property_engine, extreme_magnitudes_do_not_overflow) {
  Rng rng(0xF00DF00DULL);
  for (int iteration = 0; iteration < 200; ++iteration) {
    FairnessPolicy policy = flat_policy(4);
    policy.max_correction_units = 1ULL << 40;
    policy.max_carry_units = kMaxCarryUnits;
    for (Subject& subject : policy.subjects) {
      subject.share_weight = rng.chance(50) ? kMaxShareWeight : 1;
      subject.guarantee_floor = rng.chance(50) ? kMaxServiceUnits : 0;
      subject.max_augment_units = kMaxServiceUnits;
      subject.max_reduce_units = kMaxServiceUnits;
    }
    EvidenceSnapshot evidence = make_evidence(500, 1);
    for (std::uint64_t i = 0; i < 4; ++i) {
      observe(evidence, i + 1, 1, rng.chance(50) ? kMaxServiceUnits : 0);
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
