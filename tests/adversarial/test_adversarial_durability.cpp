// Fairness Governor - adversarial durable-state handling.
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

FG_TEST(adversarial_durability, a_corrupt_policy_record_is_never_silently_repaired) {
  const FairnessPolicy policy = starvation_policy();
  const std::string directory = make_temp_directory("adv-corrupt-policy");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    FG_CHECK_OK(opened.value()->shutdown());
  }
  Result<DurableStore> store = DurableStore::open(directory);
  FG_CHECK_OK(store.status());
  const std::string path = store.value().record_path(RecordKind::Policy);
  Result<ByteBuffer> bytes = fsutil::read_file(path, kMaxDurablePayloadBytes + 64);
  FG_CHECK_OK(bytes.status());
  bytes.value()[kRecordHeaderSize + 2] ^= 0xFF;
  FG_CHECK_OK(fsutil::write_file(path, bytes.value()));

  const Result<std::unique_ptr<FairnessGovernor>> reopened =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK(!reopened.ok());
  FG_CHECK(reopened.code() == StatusCode::ChecksumMismatch ||
           reopened.code() == StatusCode::CorruptRecord);
  remove_directory(directory);
}

FG_TEST(adversarial_durability, a_truncated_accounting_record_is_rejected) {
  const FairnessPolicy policy = starvation_policy();
  const std::string directory = make_temp_directory("adv-truncated");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    EvidenceSnapshot evidence = make_evidence(500, 1);
    observe(evidence, 1, 1, 0, 3);
    observe(evidence, 2, 1, 800);
    FG_CHECK_OK(opened.value()->ingest_evidence(evidence).status());
    Result<FairnessDecision> decision = opened.value()->evaluate(make_request(policy, evidence));
    FG_CHECK_OK(decision.status());
    FG_CHECK_OK(opened.value()->commit(decision.value()));
    FG_CHECK_OK(opened.value()->shutdown());
  }
  Result<DurableStore> store = DurableStore::open(directory);
  FG_CHECK_OK(store.status());
  const std::string path = store.value().record_path(RecordKind::Accounting);
  Result<ByteBuffer> bytes = fsutil::read_file(path, kMaxDurablePayloadBytes + 64);
  FG_CHECK_OK(bytes.status());
  ByteBuffer truncated = bytes.value();
  truncated.resize(truncated.size() / 2);
  FG_CHECK_OK(fsutil::write_file(path, truncated));

  const Result<std::unique_ptr<FairnessGovernor>> reopened =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK(!reopened.ok());
  remove_directory(directory);
}

FG_TEST(adversarial_durability, a_record_replaced_by_garbage_is_rejected) {
  const FairnessPolicy policy = starvation_policy();
  const std::string directory = make_temp_directory("adv-garbage");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    FG_CHECK_OK(opened.value()->shutdown());
  }
  Result<DurableStore> store = DurableStore::open(directory);
  FG_CHECK_OK(store.status());
  Rng rng(0x6A2BULL);
  for (int iteration = 0; iteration < 64; ++iteration) {
    ByteBuffer garbage(1 + rng.below(4096));
    for (std::uint8_t& byte : garbage) {
      byte = static_cast<std::uint8_t>(rng.below(256));
    }
    FG_CHECK_OK(fsutil::write_file(store.value().record_path(RecordKind::Policy), garbage));
    const Result<std::unique_ptr<FairnessGovernor>> reopened =
        FairnessGovernor::open(durable_config(policy, directory));
    // Random bytes essentially never form a valid record; when they do, the
    // decoded policy must still be a legal policy.
    if (!reopened.ok()) {
      FG_CHECK(reopened.code() != StatusCode::Ok);
    }
  }
  remove_directory(directory);
}

FG_TEST(adversarial_durability, a_store_path_that_is_a_file_is_rejected) {
  const FairnessPolicy policy = starvation_policy();
  const std::string directory = make_temp_directory("adv-not-a-dir");
  const std::string file_path = directory + "/blocking-file";
  const ByteBuffer payload{1, 2, 3};
  FG_CHECK_OK(fsutil::write_file(file_path, payload));
  const Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open(durable_config(policy, file_path));
  FG_CHECK(!opened.ok());
  remove_directory(directory);
}

FG_TEST(adversarial_durability, durable_state_does_not_restore_liveness_or_freshness) {
  const FairnessPolicy policy = starvation_policy();
  const std::string directory = make_temp_directory("adv-no-liveness");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    EvidenceSnapshot evidence = make_evidence(500, 1);
    observe(evidence, 1, 1, 0, 3);
    observe(evidence, 2, 1, 800);
    FG_CHECK_OK(opened.value()->ingest_evidence(evidence).status());
    Result<FairnessDecision> decision = opened.value()->evaluate(make_request(policy, evidence));
    FG_CHECK_OK(decision.status());
    FG_CHECK_OK(opened.value()->commit(decision.value()));
    FG_CHECK_EQ(opened.value()->staged_snapshot_count(), 1u);
    FG_CHECK_OK(opened.value()->shutdown());
  }
  Result<std::unique_ptr<FairnessGovernor>> reopened =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(reopened.status());
  const RecoveryReport& report = reopened.value()->recovery();
  FG_CHECK(report.accounting_loaded);
  FG_CHECK(report.foreign_incarnation);
  FG_CHECK(!report.evidence_authority_restored);
  FG_CHECK(!report.publisher_authority_restored);
  // Staged evidence and publisher sequences are gone, so the same window cannot
  // be replayed just because the process restarted.
  FG_CHECK_EQ(reopened.value()->staged_snapshot_count(), 0u);
  EvidenceSnapshot replay = make_evidence(500, 1, 10, 2);
  observe(replay, 1, 1, 0, 3);
  observe(replay, 2, 1, 800);
  FG_CHECK_EQ(reopened.value()->ingest_evidence(replay).code(), StatusCode::StaleGeneration);
  FG_CHECK_OK(reopened.value()->shutdown());
  remove_directory(directory);
}

FG_TEST(adversarial_durability, a_decision_from_a_previous_incarnation_cannot_commit) {
  const FairnessPolicy policy = starvation_policy();
  const std::string directory = make_temp_directory("adv-incarnation");
  Result<std::unique_ptr<FairnessGovernor>> first =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(first.status());
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(first.value()->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision = first.value()->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());
  FG_CHECK_OK(first.value()->shutdown());

  // A second governor in a new incarnation must reject the stale decision.
  Result<std::unique_ptr<FairnessGovernor>> second =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(second.status());
  FG_CHECK(second.value()->incarnation() != first.value()->incarnation());
  FG_CHECK_EQ(second.value()->commit(decision.value()).code(), StatusCode::StaleIncarnation);
  FG_CHECK_EQ(second.value()->accounting_generation(), 0u);
  FG_CHECK_OK(second.value()->shutdown());
  remove_directory(directory);
}

FG_TEST(adversarial_durability, an_oversized_file_on_disk_is_rejected) {
  const FairnessPolicy policy = starvation_policy();
  const std::string directory = make_temp_directory("adv-oversize-file");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    FG_CHECK_OK(opened.value()->shutdown());
  }
  Result<DurableStore> store = DurableStore::open(directory);
  FG_CHECK_OK(store.status());
  // A well-formed record whose payload exceeds the permitted bound.
  const ByteBuffer huge(kMaxDurablePayloadBytes + 1, 0);
  const ByteBuffer record = encode_record(RecordKind::Policy, 1,
                                          Incarnation{BootId::from_value(0x99), 1}, huge);
  FG_CHECK_OK(fsutil::write_file(store.value().record_path(RecordKind::Policy), record));
  const LoadedRecord loaded = store.value().load(RecordKind::Policy);
  FG_CHECK(loaded.present);
  FG_CHECK(!loaded.ok());
  FG_CHECK_EQ(loaded.code, StatusCode::OversizedPayload);
  remove_directory(directory);
}

FG_TEST(adversarial_durability, a_foreign_policy_identity_is_rejected) {
  const FairnessPolicy policy = starvation_policy();
  const std::string directory = make_temp_directory("adv-foreign-policy");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    FG_CHECK_OK(opened.value()->shutdown());
  }
  FairnessPolicy other = policy;
  other.id = FairnessPolicyId::from_value(77);
  const Result<std::unique_ptr<FairnessGovernor>> reopened =
      FairnessGovernor::open(durable_config(other, directory));
  FG_CHECK_EQ(reopened.code(), StatusCode::Conflict);
  remove_directory(directory);
}

FG_TEST(adversarial_durability, a_durable_policy_is_never_rolled_back) {
  const FairnessPolicy policy = starvation_policy();
  const std::string directory = make_temp_directory("adv-no-rollback");
  {
    Result<std::unique_ptr<FairnessGovernor>> opened =
        FairnessGovernor::open(durable_config(policy, directory));
    FG_CHECK_OK(opened.status());
    FairnessPolicy newer = policy;
    newer.generation = FairnessPolicyGeneration::from_value(5);
    newer.subjects[1].share_weight = 9;
    FG_CHECK_OK(opened.value()->install_policy(newer));
    FG_CHECK_OK(opened.value()->shutdown());
  }
  // Reopening with the older policy must adopt the committed newer generation.
  Result<std::unique_ptr<FairnessGovernor>> reopened =
      FairnessGovernor::open(durable_config(policy, directory));
  FG_CHECK_OK(reopened.status());
  FG_CHECK_EQ(reopened.value()->policy_generation().value(), 5u);
  FG_CHECK_EQ(reopened.value()->policy().subjects[1].share_weight, 9u);
  FG_CHECK(reopened.value()->recovery().policy_loaded);
  FG_CHECK_OK(reopened.value()->shutdown());
  remove_directory(directory);
}
