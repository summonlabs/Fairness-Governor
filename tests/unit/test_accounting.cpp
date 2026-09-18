// Fairness Governor - deficit/credit closure tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/model/accounting.hpp"
#include "test_harness.hpp"

using namespace fairness_governor;
using namespace fgtest;
using namespace fgtest;

FG_TEST(accounting, carry_closes_exactly_over_random_nets) {
  Rng rng(0xACCDULL);
  for (int iteration = 0; iteration < 2000; ++iteration) {
    SubjectAccounting entry;
    entry.id = SubjectId::from_value(1);
    entry.generation = SubjectGeneration::from_value(1);
    const std::uint64_t max_carry = 1 + rng.below(5000);
    std::int64_t net_sum = 0;
    for (int window = 0; window < 40; ++window) {
      const std::int64_t net = static_cast<std::int64_t>(rng.below(12000)) - 6000;
      CarryOutcome outcome;
      FG_CHECK(apply_window_carry(entry, net, max_carry, outcome));
      entry.cumulative_deficit = outcome.cumulative_deficit;
      entry.cumulative_surplus = outcome.cumulative_surplus;
      entry.discarded_deficit = outcome.discarded_deficit;
      entry.discarded_surplus = outcome.discarded_surplus;
      net_sum += net;
      // Closure: the signed carry combination moves by exactly the window net.
      FairnessAccounting accounting;
      accounting.policy_id = FairnessPolicyId::from_value(1);
      accounting.subjects.push_back(entry);
      FG_CHECK(accounting_closes(accounting, net_sum));
      FG_CHECK(entry.cumulative_deficit <= max_carry);
      FG_CHECK(entry.cumulative_surplus <= max_carry);
    }
  }
}

FG_TEST(accounting, credit_is_consumed_before_deficit_is_recorded) {
  SubjectAccounting entry;
  entry.id = SubjectId::from_value(1);
  entry.generation = SubjectGeneration::from_value(1);
  CarryOutcome first;
  FG_CHECK(apply_window_carry(entry, -100, 1000, first));
  FG_CHECK_EQ(first.cumulative_surplus, 100u);
  FG_CHECK_EQ(first.surplus_units, 100u);
  entry.cumulative_surplus = first.cumulative_surplus;
  FG_CHECK_EQ(entry.cumulative_deficit, 0u);

  CarryOutcome second;
  FG_CHECK(apply_window_carry(entry, 60, 1000, second));
  FG_CHECK_EQ(second.credit_applied, 60u);
  FG_CHECK_EQ(second.cumulative_surplus, 40u);
  FG_CHECK_EQ(second.cumulative_deficit, 0u);
  FG_CHECK_EQ(second.deficit_units, 0u);
  entry.cumulative_surplus = second.cumulative_surplus;

  CarryOutcome third;
  FG_CHECK(apply_window_carry(entry, 100, 1000, third));
  FG_CHECK_EQ(third.credit_applied, 40u);
  FG_CHECK_EQ(third.deficit_units, 60u);
  FG_CHECK_EQ(third.cumulative_deficit, 60u);
}

FG_TEST(accounting, saturation_is_reported_and_preserves_closure) {
  SubjectAccounting entry;
  entry.id = SubjectId::from_value(1);
  entry.generation = SubjectGeneration::from_value(1);
  CarryOutcome first;
  FG_CHECK(apply_window_carry(entry, 500, 100, first));
  FG_CHECK(first.saturated);
  FG_CHECK_EQ(first.cumulative_deficit, 100u);
  FG_CHECK_EQ(first.discarded_deficit, 400u);

  entry.cumulative_deficit = first.cumulative_deficit;
  entry.discarded_deficit = first.discarded_deficit;
  CarryOutcome second;
  FG_CHECK(apply_window_carry(entry, 50, 100, second));
  FG_CHECK_EQ(second.cumulative_deficit, 100u);
  FG_CHECK_EQ(second.discarded_deficit, 450u);
}

FG_TEST(accounting, totals_are_summed_with_overflow_detection) {
  FairnessAccounting accounting;
  SubjectAccounting first;
  first.id = SubjectId::from_value(1);
  first.cumulative_deficit = 10;
  SubjectAccounting second;
  second.id = SubjectId::from_value(2);
  second.cumulative_deficit = 25;
  second.cumulative_surplus = 5;
  accounting.subjects.push_back(first);
  accounting.subjects.push_back(second);
  std::uint64_t total = 0;
  FG_CHECK(accounting_deficit_total(accounting, total));
  FG_CHECK_EQ(total, 35u);
  FG_CHECK(accounting_surplus_total(accounting, total));
  FG_CHECK_EQ(total, 5u);
}

FG_TEST(accounting, equality_covers_every_field) {
  SubjectAccounting first;
  first.id = SubjectId::from_value(1);
  first.generation = SubjectGeneration::from_value(1);
  SubjectAccounting second = first;
  FG_CHECK(first == second);
  second.windows_observed = 1;
  FG_CHECK(first != second);
  second = first;
  second.last_correction_window = ServiceWindowId::from_value(3);
  FG_CHECK(first != second);
}
