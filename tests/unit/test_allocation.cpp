// Fairness Governor - exact allocation tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <numeric>
#include <vector>

#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"

using namespace fairness_governor;
using namespace fgtest;
using namespace fgtest;

namespace {

std::uint64_t total(const std::vector<std::uint64_t>& values) {
  std::uint64_t sum = 0;
  for (const std::uint64_t value : values) {
    sum += value;
  }
  return sum;
}

}  // namespace

FG_TEST(allocation, weighted_share_is_proportional) {
  const std::vector<AllocationInput> children{{1, 1, 0, 0}, {2, 3, 0, 0}};
  Result<AllocationResult> result = allocate_whole(children, 800);
  FG_CHECK_OK(result.status());
  FG_CHECK_EQ(result.value().allocations[0], 200u);
  FG_CHECK_EQ(result.value().allocations[1], 600u);
  FG_CHECK(!result.value().overcommit);
}

FG_TEST(allocation, the_whole_is_always_distributed_exactly) {
  Rng rng(0xA110CULL);
  for (int iteration = 0; iteration < 3000; ++iteration) {
    const std::size_t count = 1 + rng.below(12);
    std::vector<AllocationInput> children;
    std::uint64_t whole = rng.below(1u << 22);
    for (std::size_t i = 0; i < count; ++i) {
      AllocationInput input;
      input.key = i + 1;
      input.weight = 1 + static_cast<std::uint32_t>(rng.below(1000));
      input.floor = rng.chance(30) ? rng.below(whole + 1) : 0;
      input.obligation_rank = rng.chance(20) ? static_cast<std::uint32_t>(rng.below(4)) : 0;
      children.push_back(input);
    }
    Result<AllocationResult> result = allocate_whole(children, whole);
    FG_CHECK_OK(result.status());
    FG_CHECK_EQ(total(result.value().allocations), whole);
    for (std::size_t i = 0; i < count; ++i) {
      if (!result.value().overcommit) {
        // Without overcommit every declared floor is honoured.
        FG_CHECK_MSG(result.value().allocations[i] >= children[i].floor,
                     "floor not honoured without overcommit");
      }
    }
  }
}

FG_TEST(allocation, empty_competitor_set_is_rejected) {
  const std::vector<AllocationInput> children;
  Result<AllocationResult> result = allocate_whole(children, 10);
  FG_CHECK_EQ(result.code(), StatusCode::InvalidArgument);
}

FG_TEST(allocation, zero_weight_is_rejected) {
  std::vector<AllocationInput> children{{1, 1, 0, 0}, {2, 0, 0, 0}};
  Result<AllocationResult> result = allocate_whole(children, 10);
  FG_CHECK_EQ(result.code(), StatusCode::OutOfRange);
}

FG_TEST(allocation, zero_whole_yields_zero_shares) {
  const std::vector<AllocationInput> children{{1, 1, 0, 0}, {2, 1, 0, 0}};
  Result<AllocationResult> result = allocate_whole(children, 0);
  FG_CHECK_OK(result.status());
  FG_CHECK_EQ(result.value().allocations[0], 0u);
  FG_CHECK_EQ(result.value().allocations[1], 0u);
}

FG_TEST(allocation, floors_are_funded_from_slack) {
  // Both children want the same weight, but the first has a floor above its share.
  const std::vector<AllocationInput> children{{1, 1, 400, 0}, {2, 3, 0, 0}};
  Result<AllocationResult> result = allocate_whole(children, 1000);
  FG_CHECK_OK(result.status());
  FG_CHECK(!result.value().overcommit);
  FG_CHECK_EQ(result.value().allocations[0], 400u);
  FG_CHECK_EQ(result.value().allocations[1], 600u);
  FG_CHECK_EQ(total(result.value().allocations), 1000u);
  FG_CHECK_EQ(result.value().floor_binding[0], 1u);
}

FG_TEST(allocation, unsatisfiable_floors_are_served_by_obligation_rank) {
  // Declared floors of 600 + 600 cannot both be met inside 800. The stronger
  // obligation is served first and the shortfall is reported.
  const std::vector<AllocationInput> children{{1, 1, 600, 0}, {2, 1, 600, 2}};
  Result<AllocationResult> result = allocate_whole(children, 800);
  FG_CHECK_OK(result.status());
  FG_CHECK(result.value().overcommit);
  FG_CHECK_EQ(total(result.value().allocations), 800u);
  FG_CHECK_EQ(result.value().allocations[1], 600u);
  FG_CHECK_EQ(result.value().allocations[0], 200u);
  FG_CHECK_EQ(result.value().overcommit_shortfall, 400u);
}

FG_TEST(allocation, rounding_residue_goes_to_the_largest_remainder) {
  const std::vector<AllocationInput> children{{1, 1, 0, 0}, {2, 1, 0, 0}, {3, 1, 0, 0}};
  Result<AllocationResult> result = allocate_whole(children, 10);
  FG_CHECK_OK(result.status());
  FG_CHECK_EQ(total(result.value().allocations), 10u);
  // 10/3 -> 3 each with residue 1; one unit goes to the lowest key.
  FG_CHECK_EQ(result.value().allocations[0], 4u);
  FG_CHECK_EQ(result.value().allocations[1], 3u);
  FG_CHECK_EQ(result.value().allocations[2], 3u);
}

FG_TEST(allocation, huge_weights_do_not_overflow) {
  const std::vector<AllocationInput> children{
      {1, kMaxShareWeight, 0, 0}, {2, kMaxShareWeight, 0, 0}};
  Result<AllocationResult> result = allocate_whole(children, kMaxServiceUnits);
  FG_CHECK_OK(result.status());
  FG_CHECK_EQ(total(result.value().allocations), kMaxServiceUnits);
  FG_CHECK_EQ(result.value().allocations[0], kMaxServiceUnits / 2);
}

FG_TEST(allocation, a_single_competitor_receives_everything) {
  const std::vector<AllocationInput> children{{1, 7, 0, 0}};
  Result<AllocationResult> result = allocate_whole(children, 12345);
  FG_CHECK_OK(result.status());
  FG_CHECK_EQ(result.value().allocations[0], 12345u);
}

FG_TEST(allocation, effective_weight_never_reaches_zero) {
  // Weights live in a 1e-4 scaled domain, so a raw weight of w is w * 10000.
  FG_CHECK_EQ(effective_share_weight(100, 0), 1000000u);
  FG_CHECK_EQ(effective_share_weight(100, 10000), 2000000u);
  FG_CHECK_EQ(effective_share_weight(100, -5000), 500000u);
  FG_CHECK_EQ(effective_share_weight(1, -9999), 1u);
  // A modifier at or below -100% cannot zero the weight: the smallest scale the
  // function will ever apply is 1/10000 of the nominal weight, and everything
  // below that collapses to the same floor monotonically.
  FG_CHECK_EQ(effective_share_weight(1, -10000), 1u);
  FG_CHECK_EQ(effective_share_weight(100, -9999), 100u);
  FG_CHECK_EQ(effective_share_weight(100, -10000), 100u);
  FG_CHECK_EQ(effective_share_weight(100, -100000), 100u);
  FG_CHECK(effective_share_weight(100, -100000) <= effective_share_weight(100, -5000));
  FG_CHECK(effective_share_weight(100, -5000) <= effective_share_weight(100, 0));
  FG_CHECK(effective_share_weight(100, 0) <= effective_share_weight(100, 5000));
  FG_CHECK_EQ(effective_share_weight(kMaxShareWeight, kMaxPriorityModifierBps),
              static_cast<std::uint64_t>(kMaxShareWeight) * 110000u);
}
