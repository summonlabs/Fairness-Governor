// Fairness Governor - evidence validation tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/model/evidence.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(evidence, valid_snapshot_is_accepted) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  observe(evidence, 2, 1, 200);
  FG_CHECK_OK(validate_evidence(evidence));
}

FG_TEST(evidence, identity_fields_are_mandatory) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  evidence.id = EvidenceSnapshotId::none();
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::InvalidArgument);
  evidence = make_evidence(10, 1);
  evidence.generation = EvidenceSnapshotGeneration{};
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::InvalidArgument);
  evidence = make_evidence(10, 1);
  evidence.window = ServiceWindowId::none();
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::InvalidArgument);
  evidence = make_evidence(10, 1);
  evidence.window_generation = ServiceWindowGeneration{};
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::InvalidArgument);
  evidence = make_evidence(10, 1);
  evidence.epoch = FabricEpoch{};
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::InvalidArgument);
  evidence = make_evidence(10, 1);
  evidence.captured_at_ns = -1;
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::InvalidArgument);
}

FG_TEST(evidence, duplicate_subjects_are_rejected) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  observe(evidence, 1, 1, 200);
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::Duplicate);
}

FG_TEST(evidence, served_and_unserved_in_the_same_window_is_contradictory) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100, 2);
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::EvidenceContradictory);
}

FG_TEST(evidence, oversized_service_is_rejected) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, kMaxServiceUnits + 1);
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::OutOfRange);
}

FG_TEST(evidence, a_maximal_population_stays_inside_the_bounded_domain) {
  // The record bound and the per-subject bound together keep the population
  // total at most kMaxEvidenceRecords * kMaxServiceUnits = 2^52, so the checked
  // accumulation in the validator can never wrap for an accepted snapshot.
  EvidenceSnapshot evidence = make_evidence(10, 1);
  for (std::uint64_t i = 0; i < kMaxEvidenceRecords; ++i) {
    observe(evidence, i + 1, 1, kMaxServiceUnits);
  }
  FG_CHECK_OK(validate_evidence(evidence));
  FG_CHECK_EQ(kMaxEvidenceRecords * kMaxServiceUnits, (std::uint64_t{1} << 52));
}

FG_TEST(evidence, declared_digest_must_match) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100);
  evidence.declared_digest = compute_observation_digest(evidence);
  FG_CHECK_OK(validate_evidence(evidence));
  evidence.declared_digest ^= 1;
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::EvidenceContradictory);
}

FG_TEST(evidence, observation_digest_is_order_independent) {
  EvidenceSnapshot first = make_evidence(10, 1);
  observe(first, 1, 1, 100);
  observe(first, 2, 1, 200);
  EvidenceSnapshot second = make_evidence(10, 1);
  observe(second, 2, 1, 200);
  observe(second, 1, 1, 100);
  FG_CHECK_EQ(compute_observation_digest(first), compute_observation_digest(second));
  second.observations[0].served_units = 201;
  FG_CHECK(compute_observation_digest(first) != compute_observation_digest(second));
}

FG_TEST(evidence, observation_lookup_finds_the_subject) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 4, 1, 100);
  FG_CHECK(evidence.find(SubjectId::from_value(4)) != nullptr);
  FG_CHECK(evidence.find(SubjectId::from_value(5)) == nullptr);
}

FG_TEST(evidence, too_many_observations_are_rejected) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  evidence.observations.resize(kMaxEvidenceRecords + 1);
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::LimitExceeded);
}
