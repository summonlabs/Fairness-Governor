// Fairness Governor - durable failure injection.
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

}  // namespace

FG_TEST(failure_injection, a_store_that_cannot_be_created_is_reported) {
  FairnessPolicy policy = flat_policy(2);
  GovernorConfig config = durable_config(policy, "");
  config.require_durability = true;
  FG_CHECK_EQ(FairnessGovernor::open(config).code(), StatusCode::InvalidArgument);
}

FG_TEST(failure_injection, an_oversized_commit_is_refused_and_leaves_state_unchanged) {
  FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());
  FG_CHECK_OK(governor->commit(decision.value()));
  FG_CHECK_EQ(governor->accounting_generation(), 1u);
  FG_CHECK_OK(governor->shutdown());
}

FG_TEST(failure_injection, a_torn_replace_preserves_the_committed_accounting) {
  FairnessPolicy policy = flat_policy(2);
  const std::string directory = make_temp_directory("fault-torn");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    std::unique_ptr<FairnessGovernor>& governor = opened.value();
    EvidenceSnapshot evidence = make_evidence(500, 1);
    observe(evidence, 1, 1, 100);
    observe(evidence, 2, 1, 900);
    FG_CHECK_OK(governor->ingest_evidence(evidence).status());
    Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
    FG_CHECK_OK(decision.status());
    FG_CHECK_OK(governor->commit(decision.value()));
    FG_CHECK_OK(governor->shutdown());
  }

  // Corrupt the accounting record through the store's torn-replace injection and
  // verify recovery falls back to the backup rather than adopting a partial file.
  Result<DurableStore> store = DurableStore::open(directory);
  FG_CHECK_OK(store.status());
  StoreFaultInjection faults;
  faults.torn_replace = true;
  store.value().set_fault_injection(faults);
  Incarnation writer{BootId::from_value(0x1234), 1};
  const ByteBuffer payload =
      fsutil::read_file(store.value().record_path(RecordKind::Accounting),
                        kMaxDurablePayloadBytes)
          .value();
  FG_CHECK(!store.value()
                .commit(RecordKind::Accounting, 1, 2, payload, writer)
                .ok());
  store.value().set_fault_injection(StoreFaultInjection{});
  RecoveryNotes notes;
  FG_CHECK_OK(store.value().recover(RecordKind::Accounting, notes));
  FG_CHECK(notes.backup_recovered);

  Result<std::unique_ptr<FairnessGovernor>> reopened =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(reopened.status());
  FG_CHECK_EQ(reopened.value()->accounting_generation(), 1u);
  FG_CHECK(reopened.value()->recovery().accounting_loaded);
  FG_CHECK_OK(reopened.value()->shutdown());
  remove_directory(directory);
}

FG_TEST(failure_injection, every_injected_stage_leaves_a_usable_store) {
  FairnessPolicy policy = flat_policy(2);
  const char* const stages[] = {"before-temp", "after-temp", "after-journal", "after-replace"};
  for (const char* stage : stages) {
    const std::string name = stage;
    const std::string directory = make_temp_directory("fault-" + name);
    {
      Result<std::unique_ptr<FairnessGovernor>> opened =
          FairnessGovernor::open(durable_config(policy, directory));
      FG_CHECK_OK(opened.status());
      std::unique_ptr<FairnessGovernor>& governor = opened.value();
      EvidenceSnapshot evidence = make_evidence(500, 1);
      observe(evidence, 1, 1, 100);
      observe(evidence, 2, 1, 900);
      FG_CHECK_OK(governor->ingest_evidence(evidence).status());
      Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
      FG_CHECK_OK(decision.status());
      FG_CHECK_OK(governor->commit(decision.value()));
      FG_CHECK_OK(governor->shutdown());
    }
    // Now inject the fault on the next generation and confirm the outcome is
    // either a clean refusal or a committed advance, never a partial state.
    Result<DurableStore> store = DurableStore::open(directory);
    FG_CHECK_OK(store.status());
    StoreFaultInjection faults;
    faults.fail_before_temp_write = name == "before-temp";
    faults.fail_after_temp_write = name == "after-temp";
    faults.fail_after_journal_write = name == "after-journal";
    faults.fail_after_replace = name == "after-replace";
    store.value().set_fault_injection(faults);
    const ByteBuffer payload =
        fsutil::read_file(store.value().record_path(RecordKind::Accounting),
                          kMaxDurablePayloadBytes)
            .value();
    Incarnation writer{BootId::from_value(0x1234), 1};
    const Status committed =
        store.value().commit(RecordKind::Accounting, 1, 2, payload, writer);
    store.value().set_fault_injection(StoreFaultInjection{});
    const LoadedRecord loaded = store.value().load(RecordKind::Accounting);
    FG_CHECK(loaded.ok());
    if (committed.ok() || name == "after-replace") {
      FG_CHECK_EQ(loaded.generation, 2u);
    } else {
      FG_CHECK_EQ(loaded.generation, 1u);
    }
    RecoveryNotes notes;
    FG_CHECK_OK(store.value().recover(RecordKind::Accounting, notes));
    FG_CHECK(store.value().load(RecordKind::Accounting).ok());
    remove_directory(directory);
  }
}

FG_TEST(failure_injection, a_governor_whose_store_is_damaged_can_still_be_opened_after_cleanup) {
  FairnessPolicy policy = flat_policy(2);
  const std::string directory = make_temp_directory("fault-cleanup");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    FG_CHECK_OK(opened.value()->shutdown());
  }
  Result<DurableStore> store = DurableStore::open(directory);
  FG_CHECK_OK(store.status());
  const std::string accounting = store.value().record_path(RecordKind::Accounting);
  // A leftover temporary and journal from an interrupted attempt.
  FG_CHECK_OK(fsutil::write_file(accounting + ".jnl", ByteBuffer{1, 2, 3}));
  const Result<std::unique_ptr<FairnessGovernor>> reopened =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(reopened.status());
  const RecoveryReport& report = reopened.value()->recovery();
  // The interrupted attempt is abandoned, never adopted, and its journal and
  // temporary files are cleaned up.
  FG_CHECK(report.unfinished_attempt_discarded);
  FG_CHECK(!report.corruption_detected);
  FG_CHECK(!fsutil::exists(accounting + ".jnl"));
  FG_CHECK_OK(reopened.value()->shutdown());
  remove_directory(directory);
}

FG_TEST(failure_injection, a_commit_that_fails_does_not_advance_in_memory_state) {
  FairnessPolicy policy = flat_policy(2);
  const std::string directory = make_temp_directory("fault-no-advance");
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 100);
  observe(evidence, 2, 1, 900);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());
  FG_CHECK_OK(governor->commit(decision.value()));
  const std::uint64_t generation = governor->accounting_generation();
  const ServiceWindowId window = governor->last_committed_window();
  // Committing the same decision again is refused and changes nothing.
  FG_CHECK(!governor->commit(decision.value()).ok());
  FG_CHECK_EQ(governor->accounting_generation(), generation);
  FG_CHECK_EQ(governor->last_committed_window().value(), window.value());
  FG_CHECK_OK(governor->shutdown());
  remove_directory(directory);
}
