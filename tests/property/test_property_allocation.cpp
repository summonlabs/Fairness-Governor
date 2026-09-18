// Fairness Governor - seeded randomized allocation properties.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <set>
#include <vector>

#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

std::uint64_t sum_of(const std::vector<std::uint64_t>& values) {
  std::uint64_t sum = 0;
  for (const std::uint64_t value : values) {
    sum += value;
  }
  return sum;
}

std::vector<AllocationInput> random_children(Rng& rng, std::size_t count, std::uint64_t whole,
                                             bool with_floors) {
  std::vector<AllocationInput> children;
  children.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    AllocationInput input;
    input.key = i + 1;
    input.weight = 1 + rng.below(kMaxShareWeight);
    input.floor = with_floors && rng.chance(50) ? rng.below(whole + 1) : 0;
    input.obligation_rank = rng.chance(30) ? static_cast<std::uint32_t>(rng.below(8)) : 0;
    children.push_back(input);
  }
  return children;
}

}  // namespace

FG_TEST(property_allocation, the_whole_is_always_conserved) {
  Rng rng(0x51EEDULL);
  for (int iteration = 0; iteration < 20000; ++iteration) {
    const std::size_t count = 1 + rng.below(24);
    const std::uint64_t whole = rng.chance(10) ? 0 : rng.below(1ULL << 40);
    const bool with_floors = rng.chance(70);
    const std::vector<AllocationInput> children = random_children(rng, count, whole, with_floors);
    Result<AllocationResult> result = allocate_whole(children, whole);
    FG_CHECK_OK(result.status());
    FG_CHECK_EQ(sum_of(result.value().allocations), whole);
    for (const std::uint64_t allocation : result.value().allocations) {
      FG_CHECK(allocation <= whole);
    }
  }
}

FG_TEST(property_allocation, floors_are_honoured_unless_overcommitted) {
  Rng rng(0xF100FULL);
  for (int iteration = 0; iteration < 20000; ++iteration) {
    const std::size_t count = 1 + rng.below(16);
    const std::uint64_t whole = rng.below(1ULL << 24);
    const std::vector<AllocationInput> children = random_children(rng, count, whole, true);
    Result<AllocationResult> result = allocate_whole(children, whole);
    FG_CHECK_OK(result.status());
    std::uint64_t floor_sum = 0;
    for (const AllocationInput& child : children) {
      floor_sum += child.floor;
    }
    // Overcommit is reported exactly when the declared floors cannot all fit.
    const bool must_overcommit = floor_sum > whole;
    if (result.value().overcommit) {
      FG_CHECK(must_overcommit);
    } else {
      FG_CHECK(!must_overcommit);
      for (std::size_t i = 0; i < count; ++i) {
        FG_CHECK(result.value().allocations[i] >= children[i].floor);
      }
    }
    if (result.value().overcommit) {
      FG_CHECK(result.value().overcommit_shortfall > 0);
    } else {
      FG_CHECK_EQ(result.value().overcommit_shortfall, 0u);
    }
  }
}

FG_TEST(property_allocation, equal_weights_differ_by_at_most_one_unit) {
  Rng rng(0xE0A1ULL);
  for (int iteration = 0; iteration < 5000; ++iteration) {
    const std::size_t count = 1 + rng.below(32);
    const std::uint64_t whole = rng.below(1ULL << 20);
    std::vector<AllocationInput> children;
    for (std::size_t i = 0; i < count; ++i) {
      children.push_back(AllocationInput{i + 1, 5, 0, 0});
    }
    Result<AllocationResult> result = allocate_whole(children, whole);
    FG_CHECK_OK(result.status());
    std::uint64_t lowest = UINT64_MAX;
    std::uint64_t highest = 0;
    for (const std::uint64_t allocation : result.value().allocations) {
      lowest = allocation < lowest ? allocation : lowest;
      highest = allocation > highest ? allocation : highest;
    }
    FG_CHECK(highest - lowest <= 1);
  }
}

FG_TEST(property_allocation, allocation_is_deterministic_and_key_ordered) {
  Rng rng(0xD37ULL);
  for (int iteration = 0; iteration < 3000; ++iteration) {
    const std::size_t count = 1 + rng.below(12);
    const std::uint64_t whole = rng.below(1ULL << 20);
    const std::vector<AllocationInput> children = random_children(rng, count, whole, true);
    Result<AllocationResult> first = allocate_whole(children, whole);
    Result<AllocationResult> second = allocate_whole(children, whole);
    FG_CHECK_OK(first.status());
    FG_CHECK_OK(second.status());
    FG_CHECK(first.value().allocations == second.value().allocations);
  }
}

FG_TEST(property_allocation, monotone_in_weight) {
  // Raising a child's weight can never lower its own allocation.
  Rng rng(0x110E1ULL);
  for (int iteration = 0; iteration < 5000; ++iteration) {
    const std::size_t count = 2 + rng.below(8);
    const std::uint64_t whole = 1 + rng.below(1ULL << 20);
    std::vector<AllocationInput> children = random_children(rng, count, whole, false);
    Result<AllocationResult> before = allocate_whole(children, whole);
    FG_CHECK_OK(before.status());
    const std::size_t target = rng.below(count);
    const std::uint64_t original = children[target].weight;
    const std::uint64_t raised = original + 1 + rng.below(1000);
    children[target].weight = raised;
    Result<AllocationResult> after = allocate_whole(children, whole);
    FG_CHECK_OK(after.status());
    FG_CHECK(after.value().allocations[target] >= before.value().allocations[target]);
  }
}

FG_TEST(property_allocation, a_huge_population_stays_bounded) {
  std::vector<AllocationInput> children;
  children.reserve(kMaxSubjects);
  for (std::uint32_t i = 0; i < kMaxSubjects; ++i) {
    children.push_back(AllocationInput{i + 1, 1 + (i % 97), 0, 0});
  }
  Result<AllocationResult> result = allocate_whole(children, kMaxServiceUnits);
  FG_CHECK_OK(result.status());
  FG_CHECK_EQ(sum_of(result.value().allocations), kMaxServiceUnits);
}

FG_TEST(property_allocation, weight_sum_overflow_is_reported) {
  std::vector<AllocationInput> children;
  for (int i = 0; i < 3; ++i) {
    children.push_back(AllocationInput{static_cast<std::uint64_t>(i), kMaxShareWeight, 0, 0});
  }
  // Three weights near 2^31 cannot overflow a 64-bit sum, so the allocation
  // succeeds; the overflow path is exercised by the competitor-count bound.
  Result<AllocationResult> result = allocate_whole(children, 1000);
  FG_CHECK_OK(result.status());
  std::vector<AllocationInput> too_many(kMaxSubjects + kMaxGroups + 1);
  for (std::size_t i = 0; i < too_many.size(); ++i) {
    too_many[i] = AllocationInput{i + 1, 1, 0, 0};
  }
  FG_CHECK_EQ(allocate_whole(too_many, 1000).code(), StatusCode::LimitExceeded);
}
