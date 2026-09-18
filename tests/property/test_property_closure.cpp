// Fairness Governor - multi-window accounting closure properties.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <memory>
#include <vector>

#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

struct RunningTotals {
  std::uint64_t deficit = 0;
  std::uint64_t surplus = 0;
  std::uint64_t discarded_deficit = 0;
  std::uint64_t discarded_surplus = 0;
};

}  // namespace

FG_TEST(property_closure, durable_accounting_closes_over_many_windows) {
  Rng rng(0xC105EULL);
  for (int run = 0; run < 60; ++run) {
    FairnessPolicy policy = flat_policy(1 + static_cast<std::uint32_t>(rng.below(8)));
    policy.fair_band_units = rng.below(32);
    policy.correction_threshold_units = policy.fair_band_units + 1;
    policy.max_correction_units = rng.below(1ULL << 20);
    policy.max_correction_bps = static_cast<std::uint32_t>(rng.below(10001));
    policy.max_carry_units = 1 + rng.below(1ULL << 14);
    for (Subject& subject : policy.subjects) {
      subject.share_weight = 1 + static_cast<std::uint32_t>(rng.below(1000));
      subject.guarantee_floor = rng.chance(30) ? rng.below(500) : 0;
      subject.max_augment_units = rng.below(1ULL << 14);
      subject.max_reduce_units = rng.below(1ULL << 14);
      subject.starvation_windows = rng.chance(30) ? 1 + static_cast<std::uint32_t>(rng.below(3)) : 0;
    }
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open_in_memory(policy);
    FG_CHECK_OK(opened.status());
    std::unique_ptr<FairnessGovernor>& governor = opened.value();

    std::int64_t expected_net_sum = 0;
    std::uint64_t expected_served = 0;
    std::vector<std::uint32_t> expected_unserved(policy.subjects.size(), 0);
    std::vector<std::uint32_t> expected_below(policy.subjects.size(), 0);

    const std::uint32_t windows = 1 + static_cast<std::uint32_t>(rng.below(40));
    for (std::uint32_t window = 0; window < windows; ++window) {
      EvidenceSnapshot evidence = make_evidence(500 + window, 1, 10, window + 1);
      evidence.sequence = window + 1;
      for (std::size_t i = 0; i < policy.subjects.size(); ++i) {
        const std::uint64_t served = rng.chance(25) ? 0 : rng.below(1ULL << 14);
        observe(evidence, i + 1, 1, served);
        expected_served += served;
        if (served == 0) {
          expected_unserved[i] += 1;
        } else {
          expected_unserved[i] = 0;
        }
        if (policy.subjects[i].guarantee_floor > 0 &&
            served < policy.subjects[i].guarantee_floor) {
          expected_below[i] += 1;
        } else {
          expected_below[i] = 0;
        }
      }
      Result<IngestResult> ingested = governor->ingest_evidence(evidence);
      FG_CHECK_OK(ingested.status());
      Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
      FG_CHECK_OK(decision.status());
      for (const SubjectFairnessState& state : decision.value().subjects) {
        expected_net_sum += static_cast<std::int64_t>(state.entitlement_units) -
                            static_cast<std::int64_t>(state.served_units);
      }
      FG_CHECK_OK(governor->commit(decision.value()));
    }

    const FairnessAccounting accounting = governor->accounting();
    FG_CHECK_EQ(accounting.generation, windows);
    FG_CHECK_EQ(accounting.last_window.value(), 500u + windows - 1);
    FG_CHECK(accounting_closes(accounting, expected_net_sum));

    std::uint64_t served_total = 0;
    for (std::size_t i = 0; i < policy.subjects.size(); ++i) {
      const SubjectAccounting* entry = accounting.find(policy.subjects[i].id);
      FG_CHECK(entry != nullptr);
      if (entry == nullptr) {
        return;
      }
      FG_CHECK_EQ(entry->windows_observed, windows);
      served_total += entry->total_served_units;
      FG_CHECK_EQ(entry->unserved_streak, expected_unserved[i]);
      FG_CHECK_EQ(entry->below_floor_streak, expected_below[i]);
      FG_CHECK(entry->cumulative_deficit <= policy.max_carry_units);
      FG_CHECK(entry->cumulative_surplus <= policy.max_carry_units);
    }
    FG_CHECK_EQ(served_total, expected_served);
    FG_CHECK_OK(governor->shutdown());
  }
}

FG_TEST(property_closure, credit_and_debt_cancel_over_a_symmetric_history) {
  FairnessPolicy policy = flat_policy(2);
  policy.max_carry_units = 1ULL << 40;
  policy.fair_band_units = 0;
  policy.correction_threshold_units = 1;
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  // Alternate who over-serves so that each subject's credit is later consumed.
  for (std::uint32_t window = 0; window < 20; ++window) {
    EvidenceSnapshot evidence = make_evidence(500 + window, 1, 10, window + 1);
    evidence.sequence = window + 1;
    const bool first_over = (window % 2) == 0;
    observe(evidence, 1, 1, first_over ? 1000 : 0);
    observe(evidence, 2, 1, first_over ? 0 : 1000);
    FG_CHECK_OK(governor->ingest_evidence(evidence).status());
    Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
    FG_CHECK_OK(decision.status());
    FG_CHECK_OK(governor->commit(decision.value()));
  }
  const FairnessAccounting accounting = governor->accounting();
  const SubjectAccounting* first = accounting.find(SubjectId::from_value(1));
  const SubjectAccounting* second = accounting.find(SubjectId::from_value(2));
  FG_CHECK(first != nullptr && second != nullptr);
  if (first == nullptr || second == nullptr) {
    return;
  }
  // Equal weights and a symmetric history leave both subjects with no net debt.
  FG_CHECK_EQ(first->cumulative_deficit, first->cumulative_surplus);
  FG_CHECK_EQ(second->cumulative_deficit, second->cumulative_surplus);
  FG_CHECK_EQ(accounting.generation, 20u);
  FG_CHECK_OK(governor->shutdown());
}

FG_TEST(property_closure, saturating_carry_still_closes) {
  // Entitlement is a share of the service actually delivered, so a deficit only
  // accrues when another subject is being served. That is a deliberate
  // separation: corrective intent redistributes service, it never creates it.
  FairnessPolicy policy = flat_policy(2);
  policy.max_carry_units = 100;
  policy.fair_band_units = 0;
  policy.correction_threshold_units = 1;
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  std::int64_t net_sum = 0;
  for (std::uint32_t window = 0; window < 30; ++window) {
    EvidenceSnapshot evidence = make_evidence(500 + window, 1, 10, window + 1);
    evidence.sequence = window + 1;
    observe(evidence, 1, 1, 0);
    observe(evidence, 2, 1, 2000);
    FG_CHECK_OK(governor->ingest_evidence(evidence).status());
    Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
    FG_CHECK_OK(decision.status());
    for (const SubjectFairnessState& state : decision.value().subjects) {
      net_sum += static_cast<std::int64_t>(state.entitlement_units) -
                 static_cast<std::int64_t>(state.served_units);
    }
    FG_CHECK_OK(governor->commit(decision.value()));
  }
  const FairnessAccounting accounting = governor->accounting();
  FG_CHECK(accounting_closes(accounting, net_sum));
  const SubjectAccounting* entry = accounting.find(SubjectId::from_value(1));
  FG_CHECK(entry != nullptr);
  if (entry == nullptr) {
    return;
  }
  FG_CHECK_EQ(entry->cumulative_deficit, 100u);
  FG_CHECK(entry->discarded_deficit > 0u);
  FG_CHECK_OK(governor->shutdown());
}

FG_TEST(property_closure, a_window_with_no_service_still_advances_the_streak) {
  FairnessPolicy policy = flat_policy(2);
  policy.subjects[0].starvation_windows = 3;
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  for (std::uint32_t window = 0; window < 3; ++window) {
    EvidenceSnapshot evidence = make_evidence(500 + window, 1, 10, window + 1);
    evidence.sequence = window + 1;
    observe(evidence, 1, 1, 0);
    observe(evidence, 2, 1, 0);
    FG_CHECK_OK(governor->ingest_evidence(evidence).status());
    Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
    FG_CHECK_OK(decision.status());
    if (window < 2) {
      FG_CHECK(find_state(decision.value(), 1)->outcome != Outcome::StarvationRisk);
    } else {
      FG_CHECK_EQ(find_state(decision.value(), 1)->outcome, Outcome::StarvationRisk);
    }
    FG_CHECK_OK(governor->commit(decision.value()));
  }
  FG_CHECK_OK(governor->shutdown());
}
