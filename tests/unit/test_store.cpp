// Fairness Governor - durable record store tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/persist/store.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;
using namespace fgtest;

namespace {

Incarnation writer() { return Incarnation{BootId::from_value(0x1234), 1}; }

ByteBuffer payload_of(std::uint8_t seed, std::size_t size) {
  ByteBuffer buffer(size);
  for (std::size_t i = 0; i < size; ++i) {
    buffer[i] = static_cast<std::uint8_t>(seed + i);
  }
  return buffer;
}

}  // namespace

FG_TEST(store, record_round_trips) {
  const ByteBuffer payload = payload_of(7, 100);
  const ByteBuffer record = encode_record(RecordKind::Policy, 5, writer(), payload);
  std::uint64_t generation = 0;
  BootId boot;
  std::uint64_t ordinal = 0;
  ByteBuffer decoded;
  FG_CHECK_OK(decode_record(record, RecordKind::Policy, generation, boot, ordinal, decoded));
  FG_CHECK_EQ(generation, 5u);
  FG_CHECK_EQ(boot.value(), 0x1234u);
  FG_CHECK_EQ(ordinal, 1u);
  FG_CHECK_EQ(decoded.size(), payload.size());
  FG_CHECK(decoded == payload);
}

FG_TEST(store, record_rejects_every_structural_damage) {
  const ByteBuffer payload = payload_of(1, 32);
  const ByteBuffer record = encode_record(RecordKind::Policy, 1, writer(), payload);
  std::uint64_t generation = 0;
  BootId boot;
  std::uint64_t ordinal = 0;
  ByteBuffer decoded;

  ByteBuffer bad_magic = record;
  bad_magic[0] ^= 0xFF;
  FG_CHECK_EQ(decode_record(bad_magic, RecordKind::Policy, generation, boot, ordinal, decoded).code(),
              StatusCode::CorruptRecord);

  ByteBuffer bad_version = record;
  bad_version[4] = 9;
  FG_CHECK_EQ(
      decode_record(bad_version, RecordKind::Policy, generation, boot, ordinal, decoded).code(),
      StatusCode::UnsupportedFormat);

  ByteBuffer bad_kind = record;
  bad_kind[6] = 2;
  FG_CHECK_EQ(decode_record(bad_kind, RecordKind::Policy, generation, boot, ordinal, decoded).code(),
              StatusCode::CorruptRecord);

  ByteBuffer bad_reserved = record;
  bad_reserved[12] = 1;
  FG_CHECK_EQ(
      decode_record(bad_reserved, RecordKind::Policy, generation, boot, ordinal, decoded).code(),
      StatusCode::CorruptRecord);

  ByteBuffer truncated = record;
  truncated.resize(kRecordHeaderSize - 1);
  FG_CHECK_EQ(decode_record(truncated, RecordKind::Policy, generation, boot, ordinal, decoded).code(),
              StatusCode::Truncated);

  ByteBuffer short_payload = record;
  short_payload.pop_back();
  FG_CHECK_EQ(
      decode_record(short_payload, RecordKind::Policy, generation, boot, ordinal, decoded).code(),
      StatusCode::Truncated);

  ByteBuffer corrupt_payload = record;
  corrupt_payload[kRecordHeaderSize] ^= 0xFF;
  FG_CHECK_EQ(
      decode_record(corrupt_payload, RecordKind::Policy, generation, boot, ordinal, decoded).code(),
      StatusCode::ChecksumMismatch);

  ByteBuffer corrupt_header = record;
  corrupt_header[16] ^= 0xFF;
  FG_CHECK_EQ(
      decode_record(corrupt_header, RecordKind::Policy, generation, boot, ordinal, decoded).code(),
      StatusCode::ChecksumMismatch);

  ByteBuffer trailing = record;
  trailing.push_back(0);
  FG_CHECK_EQ(decode_record(trailing, RecordKind::Policy, generation, boot, ordinal, decoded).code(),
              StatusCode::Truncated);
}

FG_TEST(store, commit_and_load_round_trip) {
  const std::string directory = make_temp_directory("store-round-trip");
  FG_CHECK(!directory.empty());
  Result<DurableStore> opened = DurableStore::open(directory);
  FG_CHECK_OK(opened.status());
  DurableStore store = opened.take();
  FG_CHECK(!store.load(RecordKind::Policy).present);
  FG_CHECK_OK(store.commit(RecordKind::Policy, 0, 1, payload_of(3, 40), writer()));
  const LoadedRecord loaded = store.load(RecordKind::Policy);
  FG_CHECK(loaded.present);
  FG_CHECK(loaded.ok());
  FG_CHECK_EQ(loaded.generation, 1u);
  FG_CHECK(loaded.payload == payload_of(3, 40));
  remove_directory(directory);
}

FG_TEST(store, commit_is_compare_and_swap_on_the_generation) {
  const std::string directory = make_temp_directory("store-cas");
  Result<DurableStore> opened = DurableStore::open(directory);
  FG_CHECK_OK(opened.status());
  DurableStore store = opened.take();
  FG_CHECK_OK(store.commit(RecordKind::Accounting, 0, 1, payload_of(1, 16), writer()));
  FG_CHECK_EQ(store.commit(RecordKind::Accounting, 0, 1, payload_of(2, 16), writer()).code(),
              StatusCode::CommitRejected);
  FG_CHECK_OK(store.commit(RecordKind::Accounting, 1, 2, payload_of(2, 16), writer()));
  FG_CHECK_EQ(store.load(RecordKind::Accounting).generation, 2u);
  FG_CHECK_EQ(store.commit(RecordKind::Accounting, 2, 2, payload_of(3, 16), writer()).code(),
              StatusCode::CommitRejected);
  remove_directory(directory);
}

FG_TEST(store, commit_requires_a_live_incarnation) {
  const std::string directory = make_temp_directory("store-incarnation");
  Result<DurableStore> opened = DurableStore::open(directory);
  FG_CHECK_OK(opened.status());
  DurableStore store = opened.take();
  Incarnation invalid;
  FG_CHECK_EQ(store.commit(RecordKind::Policy, 0, 1, payload_of(1, 8), invalid).code(),
              StatusCode::InvalidArgument);
  remove_directory(directory);
}

FG_TEST(store, recovery_clears_a_journal_after_a_completed_replace) {
  const std::string directory = make_temp_directory("store-journal-done");
  Result<DurableStore> opened = DurableStore::open(directory);
  FG_CHECK_OK(opened.status());
  DurableStore store = opened.take();
  FG_CHECK_OK(store.commit(RecordKind::Policy, 0, 1, payload_of(1, 16), writer()));
  // Simulate a crash between the replace and the journal cleanup.
  FG_CHECK_OK(fsutil::write_file(store.record_path(RecordKind::Policy) + ".jnl",
                                 payload_of(9, 8)));
  RecoveryNotes notes;
  FG_CHECK_OK(store.recover(RecordKind::Policy, notes));
  FG_CHECK(notes.journal_cleared);
  FG_CHECK(!notes.corruption_detected);
  FG_CHECK(store.load(RecordKind::Policy).ok());
  remove_directory(directory);
}

FG_TEST(store, recovery_discards_an_abandoned_temporary) {
  const std::string directory = make_temp_directory("store-temp");
  Result<DurableStore> opened = DurableStore::open(directory);
  FG_CHECK_OK(opened.status());
  DurableStore store = opened.take();
  FG_CHECK_OK(store.commit(RecordKind::Policy, 0, 1, payload_of(1, 16), writer()));
  // A temporary file with no journal is an attempt that never reached its
  // durable boundary and must never be adopted.
  FG_CHECK_OK(fsutil::write_file(store.record_path(RecordKind::Policy) + ".tmp",
                                 encode_record(RecordKind::Policy, 2, writer(), payload_of(2, 16))));
  RecoveryNotes notes;
  FG_CHECK_OK(store.recover(RecordKind::Policy, notes));
  FG_CHECK(notes.temp_discarded);
  FG_CHECK_EQ(store.load(RecordKind::Policy).generation, 1u);
  FG_CHECK(!fsutil::exists(store.record_path(RecordKind::Policy) + ".tmp"));
  remove_directory(directory);
}

FG_TEST(store, recovery_restores_from_the_backup_after_a_torn_replace) {
  const std::string directory = make_temp_directory("store-torn");
  Result<DurableStore> opened = DurableStore::open(directory);
  FG_CHECK_OK(opened.status());
  DurableStore store = opened.take();
  FG_CHECK_OK(store.commit(RecordKind::Accounting, 0, 1, payload_of(1, 32), writer()));

  StoreFaultInjection faults;
  faults.torn_replace = true;
  store.set_fault_injection(faults);
  FG_CHECK(!store.commit(RecordKind::Accounting, 1, 2, payload_of(2, 32), writer()).ok());
  store.set_fault_injection(StoreFaultInjection{});

  RecoveryNotes notes;
  FG_CHECK_OK(store.recover(RecordKind::Accounting, notes));
  FG_CHECK(notes.backup_recovered);
  const LoadedRecord loaded = store.load(RecordKind::Accounting);
  FG_CHECK(loaded.ok());
  FG_CHECK_EQ(loaded.generation, 1u);
  FG_CHECK(loaded.payload == payload_of(1, 32));
  remove_directory(directory);
}

FG_TEST(store, an_unusable_record_is_never_silently_repaired) {
  const std::string directory = make_temp_directory("store-corrupt");
  Result<DurableStore> opened = DurableStore::open(directory);
  FG_CHECK_OK(opened.status());
  DurableStore store = opened.take();
  const std::string path = store.record_path(RecordKind::Policy);
  // A full-size header followed by bytes that are not a valid record.
  FG_CHECK_OK(fsutil::write_file(path, payload_of(1, 96)));
  const LoadedRecord loaded = store.load(RecordKind::Policy);
  FG_CHECK(loaded.present);
  FG_CHECK(!loaded.ok());
  FG_CHECK_EQ(loaded.code, StatusCode::CorruptRecord);
  // A commit against an unusable record is refused rather than overwriting it.
  FG_CHECK_EQ(store.commit(RecordKind::Policy, 0, 1, payload_of(2, 16), writer()).code(),
              StatusCode::CommitRejected);
  remove_directory(directory);
}

FG_TEST(store, injected_failures_leave_the_previous_record_intact) {
  const std::string directory = make_temp_directory("store-injection");
  Result<DurableStore> opened = DurableStore::open(directory);
  FG_CHECK_OK(opened.status());
  DurableStore store = opened.take();
  FG_CHECK_OK(store.commit(RecordKind::Policy, 0, 1, payload_of(1, 24), writer()));

  const char* const stages[] = {"before", "after-temp", "after-journal", "after-replace"};
  std::uint64_t generation = 1;
  for (const char* stage : stages) {
    StoreFaultInjection faults;
    const std::string name = stage;
    faults.fail_before_temp_write = name == "before";
    faults.fail_after_temp_write = name == "after-temp";
    faults.fail_after_journal_write = name == "after-journal";
    faults.fail_after_replace = name == "after-replace";
    store.set_fault_injection(faults);
    const Status status =
        store.commit(RecordKind::Policy, generation, generation + 1, payload_of(2, 24), writer());
    FG_CHECK(!status.ok());
    store.set_fault_injection(StoreFaultInjection{});
    RecoveryNotes notes;
    FG_CHECK_OK(store.recover(RecordKind::Policy, notes));
    const LoadedRecord loaded = store.load(RecordKind::Policy);
    FG_CHECK(loaded.ok());
    // Every injected failure leaves a valid record. Only the completed-replace
    // case has actually crossed the durable boundary.
    if (name == "after-replace") {
      generation += 1;
      FG_CHECK_EQ(loaded.generation, generation);
    } else {
      FG_CHECK_EQ(loaded.generation, generation);
      FG_CHECK_OK(store.commit(RecordKind::Policy, generation, generation + 1,
                               payload_of(2, 24), writer()));
      generation += 1;
    }
  }
  remove_directory(directory);
}

FG_TEST(store, oversized_payload_is_refused) {
  const std::string directory = make_temp_directory("store-oversize");
  Result<DurableStore> opened = DurableStore::open(directory);
  FG_CHECK_OK(opened.status());
  DurableStore store = opened.take();
  ByteBuffer huge(kMaxDurablePayloadBytes + 1, 0);
  FG_CHECK_EQ(store.commit(RecordKind::Policy, 0, 1, huge, writer()).code(),
              StatusCode::OversizedPayload);
  remove_directory(directory);
}
