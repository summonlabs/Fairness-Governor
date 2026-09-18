// Fairness Governor - policy validation tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/model/policy.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(policy, valid_policy_is_accepted) {
  const FairnessPolicy policy = flat_policy(3);
  FG_CHECK(validate_policy(policy).ok());
}

FG_TEST(policy, identity_and_generation_are_mandatory) {
  FairnessPolicy policy = flat_policy(1);
  policy.id = FairnessPolicyId::none();
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::PolicyInvalid);
  policy = flat_policy(1);
  policy.generation = FairnessPolicyGeneration{};
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::PolicyInvalid);
  policy = flat_policy(1);
  policy.epoch = FabricEpoch{};
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::PolicyInvalid);
}

FG_TEST(policy, duplicate_identities_are_rejected) {
  FairnessPolicy policy = flat_policy(2);
  policy.subjects[1].id = policy.subjects[0].id;
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::AlreadyExists);
  policy = flat_policy(1);
  FairnessGroup duplicate = policy.groups[0];
  policy.groups.push_back(duplicate);
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::AlreadyExists);
}

FG_TEST(policy, zero_weight_is_rejected) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].share_weight = 0;
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::OutOfRange);
  policy = flat_policy(1);
  policy.groups[0].share_weight = 0;
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::OutOfRange);
}

FG_TEST(policy, unknown_group_reference_is_rejected) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].group = FairnessGroupId::from_value(99);
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::NotFound);
}

FG_TEST(policy, group_cycle_is_rejected) {
  FairnessPolicy policy = flat_policy(1);
  FairnessGroup second;
  second.id = FairnessGroupId::from_value(2);
  second.generation = FairnessGroupGeneration::from_value(1);
  second.parent = FairnessGroupId::from_value(1);
  policy.groups[0].parent = second.id;
  policy.groups.push_back(second);
  const PolicyValidation validation = validate_policy(policy);
  FG_CHECK(validation.code == StatusCode::GroupCycle ||
           validation.code == StatusCode::GroupDepthExceeded);
}

FG_TEST(policy, group_depth_is_bounded) {
  FairnessPolicy policy = flat_policy(1);
  policy.groups.clear();
  std::uint64_t previous = 0;
  for (std::uint32_t level = 0; level < kMaxGroupDepth + 2; ++level) {
    FairnessGroup group;
    group.id = FairnessGroupId::from_value(level + 1);
    group.generation = FairnessGroupGeneration::from_value(1);
    group.parent = FairnessGroupId::from_value(previous);
    policy.groups.push_back(group);
    previous = group.id.value();
  }
  policy.subjects[0].group = FairnessGroupId::from_value(previous);
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::GroupDepthExceeded);
}

FG_TEST(policy, hysteresis_must_not_exceed_the_correction_threshold) {
  FairnessPolicy policy = flat_policy(1);
  policy.fair_band_units = 100;
  policy.correction_threshold_units = 50;
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::PolicyInvalid);
}

FG_TEST(policy, modifier_beyond_the_policy_bound_is_rejected) {
  FairnessPolicy policy = flat_policy(1);
  policy.max_priority_modifier_bps = 100;
  policy.subjects[0].priority_modifier_bps = 101;
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::OutOfRange);
  policy.subjects[0].priority_modifier_bps = -101;
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::OutOfRange);
  policy.subjects[0].priority_modifier_bps = -100;
  FG_CHECK(validate_policy(policy).ok());
}

FG_TEST(policy, protected_obligation_requires_a_rank) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].protected_obligation = true;
  policy.subjects[0].obligation_rank = 0;
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::PolicyInvalid);
}

FG_TEST(policy, child_floors_may_not_exceed_a_group_floor) {
  FairnessPolicy policy = flat_policy(2);
  policy.groups[0].guarantee_floor = 100;
  policy.subjects[0].guarantee_floor = 40;
  policy.subjects[1].guarantee_floor = 40;
  FG_CHECK(validate_policy(policy).ok());
  policy.subjects[1].guarantee_floor = 61;
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::PolicyOvercommit);
}

FG_TEST(policy, incoherent_external_references_are_rejected) {
  FairnessPolicy policy = flat_policy(1);
  policy.subjects[0].priority.id = PriorityRef::from_value(5);
  policy.subjects[0].priority.generation = Generation<PriorityRefTag>{};
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::PolicyInvalid);
  policy.subjects[0].priority.generation = Generation<PriorityRefTag>::from_value(1);
  FG_CHECK(validate_policy(policy).ok());
}

FG_TEST(policy, group_depths_are_computed) {
  FairnessPolicy policy = flat_policy(1);
  FairnessGroup second;
  second.id = FairnessGroupId::from_value(2);
  second.generation = FairnessGroupGeneration::from_value(1);
  second.parent = FairnessGroupId::from_value(1);
  policy.groups.push_back(second);
  const std::vector<std::uint32_t> depths = group_depths(policy);
  FG_CHECK_EQ(depths.size(), 2u);
  FG_CHECK_EQ(depths[0], 1u);
  FG_CHECK_EQ(depths[1], 2u);
}
