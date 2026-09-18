// Fairness Governor - crash-safe, integrity-checked durable store.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/persist/store.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "fairness_governor/core/checked.hpp"
#include "fairness_governor/core/limits.hpp"
#include "fairness_governor/persist/fsutil.hpp"
#include "fairness_governor/version.hpp"

namespace fairness_governor {
namespace {

constexpr std::uint32_t kRecordMagic = 0x43524746u;  // 'F','G','R','C'
constexpr std::uint16_t kHeaderChecksumOffset = 56;
constexpr std::uint16_t kPayloadChecksumOffset = 48;

[[nodiscard]] std::string companion_path(const std::string& base, const char* suffix) {
  return base + suffix;
}

}  // namespace

ByteBuffer encode_record(RecordKind kind, std::uint64_t generation, const Incarnation& writer,
                         const ByteBuffer& payload) {
  ByteBuffer record(kRecordHeaderSize + payload.size(), 0);
  store_le<std::uint32_t>(record.data() + 0, kRecordMagic);
  store_le<std::uint16_t>(record.data() + 4, kFormatVersion);
  store_le<std::uint16_t>(record.data() + 6, static_cast<std::uint16_t>(kind));
  store_le<std::uint32_t>(record.data() + 8, 0);   // flags
  store_le<std::uint32_t>(record.data() + 12, 0);  // reserved
  store_le<std::uint64_t>(record.data() + 16, generation);
  store_le<std::uint64_t>(record.data() + 24, writer.boot.value());
  store_le<std::uint64_t>(record.data() + 32, writer.ordinal);
  store_le<std::uint64_t>(record.data() + 40, static_cast<std::uint64_t>(payload.size()));
  store_le<std::uint64_t>(record.data() + kPayloadChecksumOffset,
                          checksum64(payload.data(), payload.size()));
  store_le<std::uint64_t>(record.data() + kHeaderChecksumOffset, 0);
  store_le<std::uint64_t>(record.data() + kHeaderChecksumOffset,
                          checksum64(record.data(), kRecordHeaderSize));
  if (!payload.empty()) {
    std::copy(payload.begin(), payload.end(), record.begin() + kRecordHeaderSize);
  }
  return record;
}

Status decode_record(const ByteBuffer& bytes, RecordKind expected_kind,
                     std::uint64_t& generation_out, BootId& writer_boot_out,
                     std::uint64_t& writer_ordinal_out, ByteBuffer& payload_out) {
  if (bytes.size() < kRecordHeaderSize) {
    return Status(StatusCode::Truncated, "record is shorter than its header");
  }
  if (bytes.size() > kRecordHeaderSize + kMaxDurablePayloadBytes) {
    return Status(StatusCode::OversizedPayload, "record exceeds the durable bound");
  }
  const std::uint8_t* header = bytes.data();
  if (load_le<std::uint32_t>(header + 0) != kRecordMagic) {
    return Status(StatusCode::CorruptRecord, "record magic mismatch");
  }
  const std::uint16_t version = load_le<std::uint16_t>(header + 4);
  if (version != kFormatVersion) {
    return Status(StatusCode::UnsupportedFormat, "unsupported record format version");
  }
  const std::uint16_t kind = load_le<std::uint16_t>(header + 6);
  if (kind != static_cast<std::uint16_t>(expected_kind)) {
    return Status(StatusCode::CorruptRecord, "record kind mismatch");
  }
  if (load_le<std::uint32_t>(header + 8) != 0 || load_le<std::uint32_t>(header + 12) != 0) {
    return Status(StatusCode::CorruptRecord, "record reserved fields must be zero");
  }
  const std::uint64_t header_checksum = load_le<std::uint64_t>(header + kHeaderChecksumOffset);
  ByteBuffer header_copy(header, header + kRecordHeaderSize);
  store_le<std::uint64_t>(header_copy.data() + kHeaderChecksumOffset, 0);
  if (checksum64(header_copy.data(), header_copy.size()) != header_checksum) {
    return Status(StatusCode::ChecksumMismatch, "record header checksum mismatch");
  }
  const std::uint64_t payload_length = load_le<std::uint64_t>(header + 40);
  if (payload_length > kMaxDurablePayloadBytes) {
    return Status(StatusCode::OversizedPayload, "record payload length exceeds the bound");
  }
  if (bytes.size() != kRecordHeaderSize + payload_length) {
    return Status(StatusCode::Truncated, "record payload length does not match the file size");
  }
  const std::uint64_t payload_checksum = load_le<std::uint64_t>(header + kPayloadChecksumOffset);
  if (checksum64(bytes.data() + kRecordHeaderSize, static_cast<std::size_t>(payload_length)) !=
      payload_checksum) {
    return Status(StatusCode::ChecksumMismatch, "record payload checksum mismatch");
  }
  generation_out = load_le<std::uint64_t>(header + 16);
  writer_boot_out = BootId::from_value(load_le<std::uint64_t>(header + 24));
  writer_ordinal_out = load_le<std::uint64_t>(header + 32);
  payload_out.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kRecordHeaderSize), bytes.end());
  return Status::success();
}

Result<DurableStore> DurableStore::open(std::string directory) {
  const Status created = fsutil::ensure_directory(directory);
  if (!created.ok()) {
    return created;
  }
  return DurableStore(std::move(directory));
}

std::string DurableStore::record_path(RecordKind kind) const {
  return directory_ + "/fairness_" + record_kind_name(kind) + ".rec";
}

Status DurableStore::recover(RecordKind kind, RecoveryNotes& notes) const {
  const std::string base = record_path(kind);
  const std::string temp = companion_path(base, ".tmp");
  const std::string journal = companion_path(base, ".jnl");
  const std::string backup = companion_path(base, ".prev");

  const bool journal_present = fsutil::exists(journal);
  const bool backup_present = fsutil::exists(backup);
  const bool temp_present = fsutil::exists(temp);

  if (journal_present) {
    notes.journal_cleared = true;
    const LoadedRecord main_record = load(kind);
    if (main_record.present && main_record.ok()) {
      // The replace completed before the crash; the committed record stands.
      fsutil::remove_file(journal);
      fsutil::remove_file(backup);
      fsutil::remove_file(temp);
      return Status::success();
    }
    if (backup_present) {
      const Status restored = fsutil::replace_file(backup, base);
      fsutil::remove_file(journal);
      fsutil::remove_file(temp);
      if (!restored.ok()) {
        notes.corruption_detected = true;
        return restored;
      }
      notes.backup_recovered = true;
      const LoadedRecord restored_record = load(kind);
      if (!restored_record.ok()) {
        notes.corruption_detected = true;
        return Status(restored_record.code, "backup recovery produced an invalid record");
      }
      return Status::success();
    }
    // Nothing usable to fall back to: the attempt is abandoned and reported.
    notes.unfinished_attempt_discarded = true;
    notes.corruption_detected = main_record.present;
    fsutil::remove_file(journal);
    fsutil::remove_file(temp);
    return Status::success();
  }

  if (temp_present) {
    // A temporary file with no journal is an abandoned attempt. It is never
    // adopted as committed state.
    notes.temp_discarded = true;
    notes.unfinished_attempt_discarded = true;
    fsutil::remove_file(temp);
  }
  if (backup_present) {
    fsutil::remove_file(backup);
  }
  return Status::success();
}

LoadedRecord DurableStore::load(RecordKind kind) const {
  LoadedRecord record;
  record.kind = kind;
  const std::string base = record_path(kind);
  if (!fsutil::exists(base)) {
    record.present = false;
    record.code = StatusCode::NotFound;
    record.detail = "no committed record";
    return record;
  }
  record.present = true;
  Result<ByteBuffer> bytes =
      fsutil::read_file(base, kRecordHeaderSize + kMaxDurablePayloadBytes);
  if (!bytes.ok()) {
    record.code = bytes.code();
    record.detail = std::string(bytes.status().detail());
    return record;
  }
  Status decoded = decode_record(bytes.value(), kind, record.generation, record.writer_boot,
                                 record.writer_ordinal, record.payload);
  if (!decoded.ok()) {
    record.code = decoded.code();
    record.detail = std::string(decoded.detail());
    return record;
  }
  record.code = StatusCode::Ok;
  record.detail.clear();
  return record;
}

Status DurableStore::commit(RecordKind kind, std::uint64_t expected_generation,
                            std::uint64_t next_generation, const ByteBuffer& payload,
                            const Incarnation& writer) {
  if (faults_.oversize_payload) {
    return Status(StatusCode::OversizedPayload, "injected oversized payload");
  }
  if (payload.size() > kMaxDurablePayloadBytes) {
    return Status(StatusCode::OversizedPayload, "payload exceeds the durable bound");
  }
  if (!writer.boot.valid() || writer.ordinal == 0) {
    return Status(StatusCode::InvalidArgument, "durable commit requires a live incarnation");
  }
  if (next_generation <= expected_generation) {
    return Status(StatusCode::CommitRejected, "next generation must advance");
  }

  const std::string base = record_path(kind);
  const std::string temp = companion_path(base, ".tmp");
  const std::string journal = companion_path(base, ".jnl");
  const std::string backup = companion_path(base, ".prev");

  const bool main_present = fsutil::exists(base);
  if (main_present) {
    const LoadedRecord current = load(kind);
    if (!current.ok()) {
      return Status(StatusCode::CommitRejected,
                    "existing durable record is not valid; recovery is required");
    }
    if (current.generation != expected_generation) {
      return Status(StatusCode::CommitRejected, "durable generation advanced under this commit");
    }
  } else if (expected_generation != 0) {
    return Status(StatusCode::CommitRejected, "no durable record exists for this generation");
  }

  ByteBuffer encoded = encode_record(kind, next_generation, writer, payload);
  if (faults_.fail_before_temp_write) {
    return Status(StatusCode::IoError, "injected failure before the temporary write");
  }
  if (faults_.truncate_temp && encoded.size() > kRecordHeaderSize) {
    encoded.resize(kRecordHeaderSize + 1);
  }
  if (faults_.corrupt_payload && encoded.size() > kRecordHeaderSize) {
    encoded[kRecordHeaderSize] ^= 0xFFu;
  }
  Status written = fsutil::write_file(temp, encoded);
  if (!written.ok()) {
    fsutil::remove_file(temp);
    return written;
  }
  if (faults_.fail_after_temp_write) {
    return Status(StatusCode::IoError, "injected failure after the temporary write");
  }

  {
    ByteBuffer journal_bytes;
    ByteWriter writer_state(journal_bytes);
    writer_state.u64(next_generation);
    writer_state.u64(checksum64(payload.data(), payload.size()));
    writer_state.u64(writer.boot.value());
    writer_state.u64(writer.ordinal);
    const Status journalled = fsutil::write_file(journal, journal_bytes);
    if (!journalled.ok()) {
      fsutil::remove_file(temp);
      return journalled;
    }
  }
  if (faults_.fail_after_journal_write) {
    return Status(StatusCode::IoError, "injected failure after the journal write");
  }

  if (main_present) {
    const Status snapshotted = fsutil::copy_file(base, backup, kRecordHeaderSize + kMaxDurablePayloadBytes);
    if (!snapshotted.ok()) {
      fsutil::remove_file(journal);
      fsutil::remove_file(temp);
      return snapshotted;
    }
  }

  if (faults_.torn_replace) {
    // Models a replace that is not atomic: half of the new record lands in the
    // committed path. Recovery must fall back to the backup.
    ByteBuffer torn = encoded;
    if (torn.size() > kRecordHeaderSize + 1) {
      torn.resize(kRecordHeaderSize + 1);
    }
    const Status partial = fsutil::write_file(base, torn);
    if (!partial.ok()) {
      return partial;
    }
    fsutil::remove_file(temp);
    return Status(StatusCode::IoError, "injected torn replace");
  }

  const Status replaced = fsutil::replace_file(temp, base);
  if (!replaced.ok()) {
    fsutil::remove_file(journal);
    fsutil::remove_file(temp);
    return replaced;
  }
  fsutil::sync_directory(base);
  if (faults_.fail_after_replace) {
    // The record is durable; the caller learns it through the error and the
    // recovery notes, which is exactly the ambiguous-outcome case.
    return Status(StatusCode::IoError, "injected failure after the replace");
  }

  fsutil::remove_file(journal);
  fsutil::remove_file(backup);
  return Status::success();
}

}  // namespace fairness_governor
