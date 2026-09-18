// Fairness Governor - persistence and restart correctness.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <memory>
#include <string>

#include "fairness_governor/persist/fsutil.hpp"
#include "fairness_governor/persist/store.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

GovernorConfig durable_config(const FairnessPolicy& policy, const std::string& store) {
  GovernorConfig config;
  config.policy = policy;
  config.epoch = policy.epoch;
  config.store_path = store;
  config.require_durability = true;
  return config;
}

/// Runs one service window and commits it, returning the decision.
Result<FairnessDecision> run_window(FairnessGovernor& governor, const FairnessPolicy& policy,
                                    std::uint32_t window, std::uint64_t served_first,
                                    std::uint64_t served_second) {
  EvidenceSnapshot evidence = make_evidence(500 + window, 1, 10, window + 1);
  evidence.sequence = window + 1;
  observe(evidence, 1, 1, served_first);
  observe(evidence, 2, 1, served_second);
  const Status ingested = governor.ingest_evidence(evidence).status();
  if (!ingested.ok()) {
    return ingested;
  }
  Result<FairnessDecision> decision = governor.evaluate(make_request(policy, evidence));
  if (!decision.ok()) {
    return decision.status();
  }
  const Status committed = governor.commit(decision.value());
  if (!committed.ok()) {
    return committed;
  }
  return decision;
}

}  // namespace

FG_TEST(restart, accounting_survives_a_restart_and_keeps_closing) {
  FairnessPolicy policy = flat_policy(2);
  policy.subjects[0].starvation_windows = 2;
  const std::string directory = make_temp_directory("restart-basic");

  std::int64_t net_sum = 0;
  std::uint64_t deficit_after_first = 0;
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    std::unique_ptr<FairnessGovernor>& governor = opened.value();
    FG_CHECK(governor->recovery().store_present);
    // Nothing was durable yet, so the configured policy was committed during
    // open rather than loaded from disk.
    FG_CHECK(governor->recovery().policy_committed);
    FG_CHECK(!governor->recovery().policy_loaded);
    FG_CHECK(!governor->recovery().accounting_loaded);
    for (std::uint32_t window = 0; window < 5; ++window) {
      Result<FairnessDecision> decision = run_window(*governor, policy, window, 0, 1000);
      FG_CHECK_OK(decision.status());
      for (const SubjectFairnessState& state : decision.value().subjects) {
        net_sum += static_cast<std::int64_t>(state.entitlement_units) -
                   static_cast<std::int64_t>(state.served_units);
      }
    }
    deficit_after_first = governor->accounting().find(SubjectId::from_value(1))->cumulative_deficit;
    FG_CHECK(deficit_after_first > 0);
    FG_CHECK_OK(governor->shutdown());
  }

  {
    Result<std::unique_ptr<FairnessGovernor>> reopened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(reopened.status());
    std::unique_ptr<FairnessGovernor>& governor = reopened.value();
    const RecoveryReport& report = governor->recovery();
    FG_CHECK(report.accounting_loaded);
    FG_CHECK(report.foreign_incarnation);
    FG_CHECK_EQ(report.loaded_accounting_generation, 5u);
    const FairnessAccounting restored = governor->accounting();
    FG_CHECK(accounting_closes(restored, net_sum));
    const SubjectAccounting* first = restored.find(SubjectId::from_value(1));
    FG_CHECK(first != nullptr);
    if (first == nullptr) {
      return;
    }
    FG_CHECK_EQ(first->cumulative_deficit, deficit_after_first);
    FG_CHECK_EQ(first->windows_observed, 5u);
    FG_CHECK_EQ(first->unserved_streak, 5u);
    FG_CHECK_EQ(governor->last_committed_window().value(), 504u);

    // The restart fence: the same window cannot be replayed.
    EvidenceSnapshot replay = make_evidence(504, 1, 10, 6);
    observe(replay, 1, 1, 0);
    observe(replay, 2, 1, 1000);
    FG_CHECK_EQ(governor->ingest_evidence(replay).code(), StatusCode::StaleGeneration);

    // Serving continues and the closure still holds.
    for (std::uint32_t window = 5; window < 9; ++window) {
      Result<FairnessDecision> decision = run_window(*governor, policy, window, 100, 900);
      FG_CHECK_OK(decision.status());
      for (const SubjectFairnessState& state : decision.value().subjects) {
        net_sum += static_cast<std::int64_t>(state.entitlement_units) -
                   static_cast<std::int64_t>(state.served_units);
      }
    }
    FG_CHECK(accounting_closes(governor->accounting(), net_sum));
    FG_CHECK_EQ(governor->accounting_generation(), 9u);
    FG_CHECK_OK(governor->shutdown());
  }
  remove_directory(directory);
}

FG_TEST(restart, a_policy_installed_before_the_restart_is_restored) {
  FairnessPolicy policy = flat_policy(2);
  const std::string directory = make_temp_directory("restart-policy");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    FairnessPolicy next = policy;
    next.generation = FairnessPolicyGeneration::from_value(4);
    next.subjects[0].share_weight = 7;
    FG_CHECK_OK(opened.value()->install_policy(next));
    FG_CHECK_OK(opened.value()->shutdown());
  }
  Result<std::unique_ptr<FairnessGovernor>> reopened =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(reopened.status());
  FG_CHECK_EQ(reopened.value()->policy_generation().value(), 4u);
  FG_CHECK_EQ(reopened.value()->policy().subjects[0].share_weight, 7u);
  FG_CHECK(reopened.value()->recovery().policy_loaded);
  FG_CHECK_OK(reopened.value()->shutdown());
  remove_directory(directory);
}

FG_TEST(restart, accounting_for_removed_subjects_is_dropped_not_misapplied) {
  FairnessPolicy policy = flat_policy(3);
  const std::string directory = make_temp_directory("restart-migration");
  const SubjectId removed = policy.subjects[2].id;
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    std::unique_ptr<FairnessGovernor>& governor = opened.value();
    // Observe every governed subject so that each one accrues durable accounting.
    EvidenceSnapshot evidence = make_evidence(500, 1, 10, 1);
    observe(evidence, 1, 1, 100);
    observe(evidence, 2, 1, 100);
    observe(evidence, 3, 1, 100);
    FG_CHECK_OK(governor->ingest_evidence(evidence).status());
    Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
    FG_CHECK_OK(decision.status());
    FG_CHECK_OK(governor->commit(decision.value()));
    FG_CHECK(governor->accounting().find(removed) != nullptr);
    FairnessPolicy shrunk = policy;
    shrunk.generation = FairnessPolicyGeneration::from_value(2);
    shrunk.subjects.pop_back();
    FG_CHECK_OK(governor->install_policy(shrunk));
    FG_CHECK_OK(governor->shutdown());
  }
  Result<std::unique_ptr<FairnessGovernor>> reopened =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(reopened.status());
  // The reopened policy is generation 2, which no longer governs the subject.
  FG_CHECK_EQ(reopened.value()->policy_generation().value(), 2u);
  FG_CHECK(reopened.value()->accounting().find(removed) == nullptr);
  FG_CHECK_OK(reopened.value()->shutdown());
  remove_directory(directory);
}

FG_TEST(restart, repeated_restarts_do_not_grow_or_corrupt_state) {
  FairnessPolicy policy = flat_policy(2);
  const std::string directory = make_temp_directory("restart-loop");
  std::int64_t net_sum = 0;
  std::uint32_t committed = 0;
  for (int cycle = 0; cycle < 6; ++cycle) {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    std::unique_ptr<FairnessGovernor>& governor = opened.value();
    for (std::uint32_t window = 0; window < 3; ++window) {
      Result<FairnessDecision> decision =
          run_window(*governor, policy, committed, (window * 37) % 900, 800 - (window * 11));
      FG_CHECK_OK(decision.status());
      for (const SubjectFairnessState& state : decision.value().subjects) {
        net_sum += static_cast<std::int64_t>(state.entitlement_units) -
                   static_cast<std::int64_t>(state.served_units);
      }
      ++committed;
    }
    const FairnessAccounting accounting = governor->accounting();
    FG_CHECK(accounting_closes(accounting, net_sum));
    FG_CHECK_EQ(accounting.subjects.size(), 2u);
    FG_CHECK_OK(governor->shutdown());
  }
  FG_CHECK_EQ(committed, 18u);
  remove_directory(directory);
}

FG_TEST(restart, a_foreign_incarnation_does_not_inherit_authority) {
  FairnessPolicy policy = flat_policy(2);
  const std::string directory = make_temp_directory("restart-authority");
  Result<std::unique_ptr<FairnessGovernor>> first =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(first.status());
  Result<FairnessDecision> decision = run_window(*first.value(), policy, 0, 10, 20);
  FG_CHECK_OK(decision.status());
  const AuthorityVector stale = first.value()->live_authority(make_request(
      policy, make_evidence(501, 1, 10, 2)));
  FG_CHECK_OK(first.value()->shutdown());

  Result<std::unique_ptr<FairnessGovernor>> second =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(second.status());
  const AuthorityVector live = second.value()->live_authority(make_request(
      policy, make_evidence(501, 1, 10, 2)));
  // The durable generation is deliberately continuous across a restart; what a
  // restart must never restore is the previous process's authority.
  FG_CHECK(stale.governor != live.governor);
  FG_CHECK(stale.accounting_generation == live.accounting_generation);
  FG_CHECK(!decision.value().is_authoritative_for(live));
  FG_CHECK(second.value()->recovery().foreign_incarnation);
  FG_CHECK(!second.value()->recovery().evidence_authority_restored);
  FG_CHECK_OK(second.value()->shutdown());
  remove_directory(directory);
}
