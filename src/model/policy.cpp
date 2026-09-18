// Fairness Governor - policy validation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/model/policy.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "fairness_governor/core/checked.hpp"

namespace fairness_governor {
namespace {

constexpr std::size_t kMaxLabelBytes = 128;

[[nodiscard]] PolicyValidation fail(StatusCode code, std::string detail) {
  PolicyValidation validation;
  validation.code = code;
  validation.detail = std::move(detail);
  return validation;
}

[[nodiscard]] bool weight_sum_fits(std::uint64_t& accumulator, std::uint32_t weight) noexcept {
  return checked_add(accumulator, static_cast<std::uint64_t>(weight), accumulator);
}

}  // namespace

PolicyValidation validate_policy(const FairnessPolicy& policy) {
  if (!policy.id.valid()) {
    return fail(StatusCode::PolicyInvalid, "policy id must be non-zero");
  }
  if (!policy.generation.valid()) {
    return fail(StatusCode::PolicyInvalid, "policy generation must be >= 1");
  }
  if (!policy.epoch.valid()) {
    return fail(StatusCode::PolicyInvalid, "policy epoch must be >= 1");
  }
  if (policy.groups.size() > kMaxGroups) {
    return fail(StatusCode::LimitExceeded, "group count exceeds kMaxGroups");
  }
  if (policy.subjects.empty()) {
    return fail(StatusCode::PolicyInvalid, "policy must declare at least one subject");
  }
  if (policy.subjects.size() > kMaxSubjects) {
    return fail(StatusCode::LimitExceeded, "subject count exceeds kMaxSubjects");
  }
  if (policy.fair_band_units > kMaxServiceUnits) {
    return fail(StatusCode::OutOfRange, "fair band exceeds kMaxServiceUnits");
  }
  if (policy.correction_threshold_units > kMaxServiceUnits) {
    return fail(StatusCode::OutOfRange, "correction threshold exceeds kMaxServiceUnits");
  }
  if (policy.correction_threshold_units < policy.fair_band_units) {
    return fail(StatusCode::PolicyInvalid,
                "correction threshold must be at least the fair band (hysteresis)");
  }
  if (policy.max_correction_units > kMaxServiceUnits * static_cast<std::uint64_t>(kMaxSubjects)) {
    return fail(StatusCode::OutOfRange, "max correction units exceeds the bounded domain");
  }
  if (policy.max_correction_bps > 100000u) {
    return fail(StatusCode::OutOfRange, "max correction basis points exceeds 100000");
  }
  if (policy.max_priority_modifier_bps > static_cast<std::uint32_t>(kMaxPriorityModifierBps)) {
    return fail(StatusCode::OutOfRange, "max priority modifier exceeds kMaxPriorityModifierBps");
  }
  if (policy.max_evidence_age_windows == 0) {
    return fail(StatusCode::PolicyInvalid, "max evidence age in windows must be >= 1");
  }
  if (policy.max_evidence_age_windows > 1000000u) {
    return fail(StatusCode::OutOfRange, "max evidence age in windows is implausible");
  }
  if (policy.max_carry_units > kMaxCarryUnits) {
    return fail(StatusCode::OutOfRange, "max carry units exceeds kMaxCarryUnits");
  }
  if (policy.provenance.source.size() > kMaxLabelBytes) {
    return fail(StatusCode::LimitExceeded, "policy provenance source is too long");
  }

  // --- Groups ---------------------------------------------------------------
  std::unordered_map<std::uint64_t, std::size_t> group_index;
  group_index.reserve(policy.groups.size() * 2 + 1);
  for (std::size_t i = 0; i < policy.groups.size(); ++i) {
    const FairnessGroup& group = policy.groups[i];
    if (!group.id.valid()) {
      return fail(StatusCode::PolicyInvalid, "group id must be non-zero");
    }
    if (!group.generation.valid()) {
      return fail(StatusCode::PolicyInvalid, "group generation must be >= 1");
    }
    if (!group_index.emplace(group.id.value(), i).second) {
      return fail(StatusCode::AlreadyExists, "duplicate group id");
    }
    if (group.share_weight == 0 || group.share_weight > kMaxShareWeight) {
      return fail(StatusCode::OutOfRange, "group share weight out of range");
    }
    if (group.guarantee_floor > kMaxServiceUnits) {
      return fail(StatusCode::OutOfRange, "group guarantee floor exceeds kMaxServiceUnits");
    }
    if (group.protected_obligation && group.obligation_rank == 0) {
      return fail(StatusCode::PolicyInvalid,
                  "a protected obligation requires a non-zero obligation rank");
    }
    if (group.label.size() > kMaxLabelBytes) {
      return fail(StatusCode::LimitExceeded, "group label is too long");
    }
  }

  for (const FairnessGroup& group : policy.groups) {
    if (!group.parent.valid()) {
      continue;
    }
    if (group_index.find(group.parent.value()) == group_index.end()) {
      return fail(StatusCode::NotFound, "group parent does not exist");
    }
    if (group.parent == group.id) {
      return fail(StatusCode::GroupCycle, "group cannot be its own parent");
    }
  }

  // Depth / cycle detection: walk to the root with a bounded step count.
  for (const FairnessGroup& group : policy.groups) {
    std::uint32_t depth = 0;
    FairnessGroupId cursor = group.id;
    for (;;) {
      ++depth;
      if (depth > kMaxGroupDepth) {
        return fail(StatusCode::GroupDepthExceeded, "group nesting exceeds kMaxGroupDepth");
      }
      const auto it = group_index.find(cursor.value());
      if (it == group_index.end()) {
        return fail(StatusCode::NotFound, "group parent chain references an unknown group");
      }
      const FairnessGroupId parent = policy.groups[it->second].parent;
      if (!parent.valid()) {
        break;
      }
      cursor = parent;
    }
  }

  // --- Subjects -------------------------------------------------------------
  std::unordered_set<std::uint64_t> subject_ids;
  subject_ids.reserve(policy.subjects.size() * 2 + 1);
  for (const Subject& subject : policy.subjects) {
    if (!subject.id.valid()) {
      return fail(StatusCode::PolicyInvalid, "subject id must be non-zero");
    }
    if (!subject.generation.valid()) {
      return fail(StatusCode::PolicyInvalid, "subject generation must be >= 1");
    }
    if (!subject_ids.insert(subject.id.value()).second) {
      return fail(StatusCode::AlreadyExists, "duplicate subject id");
    }
    if (!subject.group.valid()) {
      return fail(StatusCode::PolicyInvalid, "subject must reference a fairness group");
    }
    if (group_index.find(subject.group.value()) == group_index.end()) {
      return fail(StatusCode::NotFound, "subject references an unknown fairness group");
    }
    if (subject.share_weight == 0 || subject.share_weight > kMaxShareWeight) {
      return fail(StatusCode::OutOfRange, "subject share weight out of range");
    }
    if (subject.guarantee_floor > kMaxServiceUnits) {
      return fail(StatusCode::OutOfRange, "subject guarantee floor exceeds kMaxServiceUnits");
    }
    if (subject.starvation_windows > 1000000u || subject.guarantee_starvation_windows > 1000000u) {
      return fail(StatusCode::OutOfRange, "starvation thresholds are implausible");
    }
    if (!subject.priority.consistent()) {
      return fail(StatusCode::PolicyInvalid, "priority reference id/generation disagree");
    }
    if (!subject.qos.consistent()) {
      return fail(StatusCode::PolicyInvalid, "QoS reference id/generation disagree");
    }
    if (subject.protected_obligation && subject.obligation_rank == 0) {
      return fail(StatusCode::PolicyInvalid,
                  "a protected obligation requires a non-zero obligation rank");
    }
    const auto modifier = static_cast<std::uint32_t>(
        subject.priority_modifier_bps < 0 ? -static_cast<std::int64_t>(subject.priority_modifier_bps)
                                          : static_cast<std::int64_t>(subject.priority_modifier_bps));
    if (modifier > policy.max_priority_modifier_bps) {
      return fail(StatusCode::OutOfRange,
                  "subject priority modifier exceeds the policy bound");
    }
    if (modifier > static_cast<std::uint32_t>(kMaxPriorityModifierBps)) {
      return fail(StatusCode::OutOfRange, "subject priority modifier exceeds the hard bound");
    }
    if (subject.max_reduce_units > kMaxServiceUnits) {
      return fail(StatusCode::OutOfRange, "subject reduce cap exceeds kMaxServiceUnits");
    }
    if (subject.max_augment_units > kMaxServiceUnits) {
      return fail(StatusCode::OutOfRange, "subject augment cap exceeds kMaxServiceUnits");
    }
    if (subject.label.size() > kMaxLabelBytes) {
      return fail(StatusCode::LimitExceeded, "subject label is too long");
    }
  }

  // --- Weight sums fit the bounded domain -----------------------------------
  for (const FairnessGroup& group : policy.groups) {
    std::uint64_t sum = 0;
    for (const FairnessGroup& child : policy.groups) {
      if (child.parent == group.id && !weight_sum_fits(sum, child.share_weight)) {
        return fail(StatusCode::ArithmeticOverflow, "child group weight sum overflows");
      }
    }
    if (!group_subject_weight_sum(policy, group.id)) {
      return fail(StatusCode::PolicyInvalid, "a group has no governed weight");
    }
    sum = 0;
    for (const Subject& subject : policy.subjects) {
      if (subject.group == group.id && !weight_sum_fits(sum, subject.share_weight)) {
        return fail(StatusCode::ArithmeticOverflow, "subject weight sum overflows");
      }
    }
  }

  // --- Guarantee-floor coherence --------------------------------------------
  // A group that declares an absolute floor must be able to honour the floors of
  // its direct children. A policy that cannot is contradictory, not merely tight.
  for (const FairnessGroup& group : policy.groups) {
    if (group.guarantee_floor == 0) {
      continue;
    }
    std::uint64_t child_floors = 0;
    for (const FairnessGroup& child : policy.groups) {
      if (child.parent != group.id) {
        continue;
      }
      if (!checked_add(child_floors, child.guarantee_floor, child_floors)) {
        return fail(StatusCode::ArithmeticOverflow, "child floor sum overflows");
      }
    }
    for (const Subject& subject : policy.subjects) {
      if (subject.group != group.id) {
        continue;
      }
      if (!checked_add(child_floors, subject.guarantee_floor, child_floors)) {
        return fail(StatusCode::ArithmeticOverflow, "child floor sum overflows");
      }
    }
    if (child_floors > group.guarantee_floor) {
      return fail(StatusCode::PolicyOvercommit,
                  "group guarantee floor is below the sum of its children's floors");
    }
  }

  PolicyValidation ok;
  return ok;
}

std::vector<std::uint32_t> group_depths(const FairnessPolicy& policy) {
  std::vector<std::uint32_t> depths(policy.groups.size(), 0);
  std::unordered_map<std::uint64_t, std::size_t> index;
  index.reserve(policy.groups.size() * 2 + 1);
  for (std::size_t i = 0; i < policy.groups.size(); ++i) {
    index.emplace(policy.groups[i].id.value(), i);
  }
  for (std::size_t i = 0; i < policy.groups.size(); ++i) {
    std::uint32_t depth = 0;
    std::size_t cursor = i;
    for (;;) {
      ++depth;
      const FairnessGroupId parent = policy.groups[cursor].parent;
      if (!parent.valid()) {
        break;
      }
      const auto it = index.find(parent.value());
      if (it == index.end()) {
        break;
      }
      cursor = it->second;
      if (depth > kMaxGroupDepth + 1) {
        break;  // validated policies cannot reach this
      }
    }
    depths[i] = depth;
  }
  return depths;
}

std::uint64_t group_subject_weight_sum(const FairnessPolicy& policy,
                                       FairnessGroupId group) noexcept {
  std::uint64_t sum = 0;
  for (const Subject& subject : policy.subjects) {
    if (subject.group == group) {
      sum += subject.share_weight;
    }
  }
  return sum;
}

}  // namespace fairness_governor
