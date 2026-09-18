// Fairness Governor - versioned, integrity-checked, crash-safe durable store.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// On-disk record layout (little-endian, fixed 64-byte header):
//
//   0  magic            4   "FGRC"
//   4  format_version   2
//   6  record_kind      2
//   8  flags            4
//   12 reserved         4   must be zero
//   16 generation       8
//   24 writer_boot      8
//   32 writer_ordinal   8
//   40 payload_length   8
//   48 payload_checksum 8
//   56 header_checksum  8   over bytes [0,56) with this field treated as zero
//   64 payload...
//
// Commit protocol (never acknowledge before the state is durable):
//   validate -> plan -> write .tmp + flush -> write .jnl + flush
//   -> snapshot main to .prev -> atomic replace main <- .tmp -> drop .jnl -> drop .prev
//
// Recovery distinguishes: committed state, an unfinished attempt (a .tmp whose
// journal never completed), a torn replace (recovered from .prev), and
// corruption (checksum/format failure, which is reported and never repaired
// silently).
#ifndef FAIRNESS_GOVERNOR_PERSIST_STORE_HPP
#define FAIRNESS_GOVERNOR_PERSIST_STORE_HPP

#include <cstdint>
#include <string>

#include "fairness_governor/core/bytes.hpp"
#include "fairness_governor/core/ids.hpp"
#include "fairness_governor/core/status.hpp"

namespace fairness_governor {

enum class RecordKind : std::uint16_t {
  Policy = 1,
  Accounting = 2,
};

[[nodiscard]] constexpr const char* record_kind_name(RecordKind kind) noexcept {
  return kind == RecordKind::Policy ? "policy" : "accounting";
}

/// Fault injection used exclusively by failure-injection tests. Every flag
/// models a real crash or media fault; nothing here is reachable in production
/// configuration because the default value injects nothing.
struct StoreFaultInjection {
  bool fail_before_temp_write{false};
  bool fail_after_temp_write{false};
  bool fail_after_journal_write{false};
  /// Simulates a replace that is not atomic: main is left half-written.
  bool torn_replace{false};
  bool fail_after_replace{false};
  bool corrupt_payload{false};
  bool truncate_temp{false};
  bool oversize_payload{false};

  [[nodiscard]] bool any() const noexcept {
    return fail_before_temp_write || fail_after_temp_write || fail_after_journal_write ||
           torn_replace || fail_after_replace || corrupt_payload || truncate_temp ||
           oversize_payload;
  }
};

/// Result of loading one record file.
struct LoadedRecord {
  bool present{false};
  RecordKind kind{RecordKind::Policy};
  std::uint64_t generation{0};
  BootId writer_boot{};
  std::uint64_t writer_ordinal{0};
  ByteBuffer payload;
  StatusCode code{StatusCode::Ok};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return code == StatusCode::Ok; }
};

/// Notes produced by recovery, surfaced through the governor's RecoveryReport.
struct RecoveryNotes {
  bool journal_cleared{false};
  bool backup_recovered{false};
  bool unfinished_attempt_discarded{false};
  bool corruption_detected{false};
  bool temp_discarded{false};
};

/// A directory holding crash-safe record files with atomic generation advance.
class DurableStore {
 public:
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;
  DurableStore(DurableStore&&) noexcept = default;
  DurableStore& operator=(DurableStore&&) noexcept = default;

  [[nodiscard]] static Result<DurableStore> open(std::string directory);

  [[nodiscard]] const std::string& directory() const noexcept { return directory_; }

  /// Runs recovery for one record kind and reports what it found. Idempotent.
  [[nodiscard]] Status recover(RecordKind kind, RecoveryNotes& notes) const;

  /// Loads a committed record. Never adopts a temporary file.
  [[nodiscard]] LoadedRecord load(RecordKind kind) const;

  /// Commits `payload` as generation `next_generation`. Fails with
  /// CommitRejected unless the durable generation is exactly
  /// `expected_generation` (0 means "no record exists").
  [[nodiscard]] Status commit(RecordKind kind, std::uint64_t expected_generation,
                              std::uint64_t next_generation, const ByteBuffer& payload,
                              const Incarnation& writer);

  /// Fault injection hook for failure-injection tests only.
  void set_fault_injection(StoreFaultInjection injection) noexcept { faults_ = injection; }
  [[nodiscard]] const StoreFaultInjection& fault_injection() const noexcept { return faults_; }

  [[nodiscard]] std::string record_path(RecordKind kind) const;

 private:
  explicit DurableStore(std::string directory) : directory_(std::move(directory)) {}

  std::string directory_;
  StoreFaultInjection faults_{};
};

/// Encodes a record (header + payload) for durability.
[[nodiscard]] ByteBuffer encode_record(RecordKind kind, std::uint64_t generation,
                                       const Incarnation& writer, const ByteBuffer& payload);

/// Decodes and validates a record. Rejects truncation, trailing bytes, a bad
/// header checksum, a bad payload checksum, a wrong kind, and a wrong version.
[[nodiscard]] Status decode_record(const ByteBuffer& bytes, RecordKind expected_kind,
                                   std::uint64_t& generation_out, BootId& writer_boot_out,
                                   std::uint64_t& writer_ordinal_out, ByteBuffer& payload_out);

/// Size of the fixed record header.
inline constexpr std::uint64_t kRecordHeaderSize = 64;

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_PERSIST_STORE_HPP
