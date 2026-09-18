// Fairness Governor - canonical input yields canonical outcome.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <memory>
#include <set>
#include <vector>

#include "fairness_governor/eval/explain.hpp"
#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

struct CanonicalCase {
  FairnessPolicy policy;
  EvidenceSnapshot evidence;
  EvaluationRequest request;
};

CanonicalCase random_case(Rng& rng) {
  CanonicalCase test_case;
  FairnessPolicy& policy = test_case.policy;
  policy.id = FairnessPolicyId::from_value(1);
  policy.generation = FairnessPolicyGeneration::from_value(1);
  policy.epoch = FabricEpoch::from_value(3);
  policy.fair_band_units = rng.below(20);
  policy.correction_threshold_units = policy.fair_band_units + rng.below(200);
  policy.max_correction_units = rng.below(1ULL << 16);
  policy.max_correction_bps = static_cast<std::uint32_t>(rng.below(10001));
  policy.max_priority_modifier_bps = static_cast<std::uint32_t>(rng.below(5000));
  policy.max_carry_units = 1 + rng.below(1ULL << 12);

  FairnessGroup group;
  group.id = FairnessGroupId::from_value(1);
  group.generation = FairnessGroupGeneration::from_value(1);
  group.share_weight = 1 + static_cast<std::uint32_t>(rng.below(8));
  policy.groups.push_back(group);

  EvidenceSnapshot& evidence = test_case.evidence;
  evidence.id = EvidenceSnapshotId::from_value(5);
  evidence.generation = EvidenceSnapshotGeneration::from_value(2);
  evidence.window = ServiceWindowId::from_value(700);
  evidence.window_generation = ServiceWindowGeneration::from_value(1);
  evidence.epoch = policy.epoch;
  evidence.sequence = 1;

  const std::uint32_t count = 1 + static_cast<std::uint32_t>(rng.below(12));
  for (std::uint32_t i = 0; i < count; ++i) {
    Subject subject;
    subject.id = SubjectId::from_value(i + 1);
    subject.generation = SubjectGeneration::from_value(1);
    subject.group = group.id;
    subject.share_weight = 1 + static_cast<std::uint32_t>(rng.below(500));
    subject.guarantee_floor = rng.chance(30) ? rng.below(300) : 0;
    subject.starvation_windows = rng.chance(30) ? 1 + static_cast<std::uint32_t>(rng.below(3)) : 0;
    subject.priority_modifier_bps =
        static_cast<std::int32_t>(rng.below(policy.max_priority_modifier_bps + 1)) -
        static_cast<std::int32_t>(rng.below(policy.max_priority_modifier_bps + 1));
    subject.max_augment_units = rng.below(4096);
    subject.max_reduce_units = rng.below(4096);
    policy.subjects.push_back(subject);
    observe(evidence, i + 1, 1, rng.chance(20) ? 0 : rng.below(5000));
  }
  test_case.request = make_request(policy, evidence);
  return test_case;
}

}  // namespace

FG_TEST(property_determinism, identical_input_yields_identical_decision) {
  Rng rng(0xDE7EULL);
  for (int iteration = 0; iteration < 2000; ++iteration) {
    CanonicalCase test_case = random_case(rng);
    Result<FairnessDecision> first =
        run_engine(test_case.policy, test_case.evidence, test_case.request,
                   test_case.evidence.window);
    Result<FairnessDecision> second =
        run_engine(test_case.policy, test_case.evidence, test_case.request,
                   test_case.evidence.window);
    FG_CHECK_OK(first.status());
    FG_CHECK_OK(second.status());
    FG_CHECK_EQ(first.value().content_digest, second.value().content_digest);
    FG_CHECK_EQ(first.value().subjects.size(), second.value().subjects.size());
    for (std::size_t i = 0; i < first.value().subjects.size(); ++i) {
      FG_CHECK_EQ(first.value().subjects[i].entitlement_units,
                  second.value().subjects[i].entitlement_units);
      FG_CHECK_EQ(first.value().subjects[i].outcome, second.value().subjects[i].outcome);
      FG_CHECK_EQ(first.value().subjects[i].reason, second.value().subjects[i].reason);
    }
    FG_CHECK_EQ(explain(first.value()), explain(second.value()));
  }
}

FG_TEST(property_determinism, two_governors_agree_on_the_digest) {
  Rng rng(0x1C0DEULL);
  for (int iteration = 0; iteration < 300; ++iteration) {
    CanonicalCase test_case = random_case(rng);
    Result<std::unique_ptr<FairnessGovernor>> first =
        FairnessGovernor::open_in_memory(test_case.policy);
    Result<std::unique_ptr<FairnessGovernor>> second =
        FairnessGovernor::open_in_memory(test_case.policy);
    FG_CHECK_OK(first.status());
    FG_CHECK_OK(second.status());
    // Two different incarnations: the digest deliberately excludes the
    // incarnation so that independent governors can agree on content.
    FG_CHECK(first.value()->incarnation() != second.value()->incarnation());
    FG_CHECK_OK(first.value()->ingest_evidence(test_case.evidence).status());
    FG_CHECK_OK(second.value()->ingest_evidence(test_case.evidence).status());
    Result<FairnessDecision> first_decision = first.value()->evaluate(test_case.request);
    Result<FairnessDecision> second_decision = second.value()->evaluate(test_case.request);
    FG_CHECK_OK(first_decision.status());
    FG_CHECK_OK(second_decision.status());
    FG_CHECK_EQ(first_decision.value().content_digest, second_decision.value().content_digest);
    FG_CHECK_OK(first.value()->shutdown());
    FG_CHECK_OK(second.value()->shutdown());
  }
}

FG_TEST(property_determinism, observation_order_does_not_change_the_decision) {
  Rng rng(0x0DDE1ULL);
  for (int iteration = 0; iteration < 500; ++iteration) {
    CanonicalCase test_case = random_case(rng);
    EvidenceSnapshot shuffled = test_case.evidence;
    for (std::size_t i = shuffled.observations.size(); i > 1; --i) {
      const std::size_t j = static_cast<std::size_t>(rng.below(i));
      std::swap(shuffled.observations[i - 1], shuffled.observations[j]);
    }
    Result<FairnessDecision> first =
        run_engine(test_case.policy, test_case.evidence, test_case.request,
                   test_case.evidence.window);
    Result<FairnessDecision> second =
        run_engine(test_case.policy, shuffled, test_case.request, test_case.evidence.window);
    FG_CHECK_OK(first.status());
    FG_CHECK_OK(second.status());
    FG_CHECK_EQ(first.value().content_digest, second.value().content_digest);
  }
}

FG_TEST(property_determinism, distinct_inputs_produce_distinct_digests) {
  Rng rng(0xD1571C7ULL);
  std::set<std::uint64_t> digests;
  std::size_t attempts = 0;
  while (digests.size() < 200 && attempts < 2000) {
    ++attempts;
    CanonicalCase test_case = random_case(rng);
    Result<FairnessDecision> decision =
        run_engine(test_case.policy, test_case.evidence, test_case.request,
                   test_case.evidence.window);
    FG_CHECK_OK(decision.status());
    digests.insert(decision.value().content_digest);
  }
  // Collisions are possible in principle, but 200 distinct populations should
  // not collide in a 64-bit digest.
  FG_CHECK(digests.size() > 190);
}
