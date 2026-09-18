// Fairness Governor - status and result types.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef FAIRNESS_GOVERNOR_CORE_STATUS_HPP
#define FAIRNESS_GOVERNOR_CORE_STATUS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace fairness_governor {

/// Every rejection reason the governor can produce. Codes are stable and are
/// part of the explanation surface, so operators can act on them.
enum class StatusCode : std::uint16_t {
  Ok = 0,

  // Structural / lifecycle
  InvalidArgument = 1,
  OutOfRange = 2,
  NotFound = 3,
  AlreadyExists = 4,
  Duplicate = 5,
  Conflict = 6,
  NotInitialized = 7,
  Unsupported = 8,
  Internal = 9,

  // Authority / generation / epoch
  StaleGeneration = 20,
  StaleEpoch = 21,
  StaleIncarnation = 22,
  StaleEvidence = 23,
  EvidenceMissing = 24,
  EvidenceContradictory = 25,
  AuthorityMismatch = 26,
  IncarnationMismatch = 27,

  // Policy validation
  PolicyInvalid = 40,
  PolicyGenerationMismatch = 41,
  PolicyOvercommit = 42,
  GroupCycle = 43,
  GroupDepthExceeded = 44,
  SubjectGenerationMismatch = 45,

  // Arithmetic
  ArithmeticOverflow = 60,
  ArithmeticUnderflow = 61,
  DivideByZero = 62,
  LimitExceeded = 63,

  // Durable state
  IoError = 80,
  CorruptRecord = 81,
  UnsupportedFormat = 82,
  Truncated = 83,
  ChecksumMismatch = 84,
  OversizedPayload = 85,
  CommitRejected = 86,
  RecoveryRequired = 87,

  // Transport
  TransportError = 100,
  FrameInvalid = 101,
  FrameTooLarge = 102,
  HandshakeRejected = 103,
  SequenceViolation = 104,
  LinkClosed = 105,
  LinkBusy = 106,
  /// A peer stopped sending inside a message and its idle budget was spent.
  IdleTimeout = 107,

  // Lifecycle
  ShuttingDown = 120,
  Cancelled = 121,
  Busy = 122,
};

[[nodiscard]] constexpr std::string_view to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "OK";
    case StatusCode::InvalidArgument: return "INVALID_ARGUMENT";
    case StatusCode::OutOfRange: return "OUT_OF_RANGE";
    case StatusCode::NotFound: return "NOT_FOUND";
    case StatusCode::AlreadyExists: return "ALREADY_EXISTS";
    case StatusCode::Duplicate: return "DUPLICATE";
    case StatusCode::Conflict: return "CONFLICT";
    case StatusCode::NotInitialized: return "NOT_INITIALIZED";
    case StatusCode::Unsupported: return "UNSUPPORTED";
    case StatusCode::Internal: return "INTERNAL";
    case StatusCode::StaleGeneration: return "STALE_GENERATION";
    case StatusCode::StaleEpoch: return "STALE_EPOCH";
    case StatusCode::StaleIncarnation: return "STALE_INCARNATION";
    case StatusCode::StaleEvidence: return "STALE_EVIDENCE";
    case StatusCode::EvidenceMissing: return "EVIDENCE_MISSING";
    case StatusCode::EvidenceContradictory: return "EVIDENCE_CONTRADICTORY";
    case StatusCode::AuthorityMismatch: return "AUTHORITY_MISMATCH";
    case StatusCode::IncarnationMismatch: return "INCARNATION_MISMATCH";
    case StatusCode::PolicyInvalid: return "POLICY_INVALID";
    case StatusCode::PolicyGenerationMismatch: return "POLICY_GENERATION_MISMATCH";
    case StatusCode::PolicyOvercommit: return "POLICY_OVERCOMMIT";
    case StatusCode::GroupCycle: return "GROUP_CYCLE";
    case StatusCode::GroupDepthExceeded: return "GROUP_DEPTH_EXCEEDED";
    case StatusCode::SubjectGenerationMismatch: return "SUBJECT_GENERATION_MISMATCH";
    case StatusCode::ArithmeticOverflow: return "ARITHMETIC_OVERFLOW";
    case StatusCode::ArithmeticUnderflow: return "ARITHMETIC_UNDERFLOW";
    case StatusCode::DivideByZero: return "DIVIDE_BY_ZERO";
    case StatusCode::LimitExceeded: return "LIMIT_EXCEEDED";
    case StatusCode::IoError: return "IO_ERROR";
    case StatusCode::CorruptRecord: return "CORRUPT_RECORD";
    case StatusCode::UnsupportedFormat: return "UNSUPPORTED_FORMAT";
    case StatusCode::Truncated: return "TRUNCATED";
    case StatusCode::ChecksumMismatch: return "CHECKSUM_MISMATCH";
    case StatusCode::OversizedPayload: return "OVERSIZED_PAYLOAD";
    case StatusCode::CommitRejected: return "COMMIT_REJECTED";
    case StatusCode::RecoveryRequired: return "RECOVERY_REQUIRED";
    case StatusCode::TransportError: return "TRANSPORT_ERROR";
    case StatusCode::FrameInvalid: return "FRAME_INVALID";
    case StatusCode::FrameTooLarge: return "FRAME_TOO_LARGE";
    case StatusCode::HandshakeRejected: return "HANDSHAKE_REJECTED";
    case StatusCode::SequenceViolation: return "SEQUENCE_VIOLATION";
    case StatusCode::LinkClosed: return "LINK_CLOSED";
    case StatusCode::LinkBusy: return "LINK_BUSY";
    case StatusCode::IdleTimeout: return "IDLE_TIMEOUT";
    case StatusCode::ShuttingDown: return "SHUTTING_DOWN";
    case StatusCode::Cancelled: return "CANCELLED";
    case StatusCode::Busy: return "BUSY";
  }
  return "UNKNOWN_STATUS";
}

/// A status value with a human-readable detail string. Never carries authority:
/// authority always lives in the explicit generation/epoch fields of the
/// structures that own it.
class Status {
 public:
  Status() noexcept = default;

  /// The detail text is OWNED. An earlier revision stored a view into the
  /// caller's temporary, which made every formatted failure message dangle; the
  /// status carries its own storage so a detail string can never outlive its
  /// bytes.
  Status(StatusCode code, std::string detail = {}) : code_(code), detail_(std::move(detail)) {}

  /// Named `success` rather than `ok` so the factory cannot collide with the
  /// `ok()` predicate of the same class.
  [[nodiscard]] static Status success() noexcept { return Status(StatusCode::Ok); }
  [[nodiscard]] static Status failure(StatusCode code, std::string detail) {
    return Status(code, std::move(detail));
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] std::string_view detail() const noexcept { return detail_; }
  [[nodiscard]] const std::string& detail_text() const noexcept { return detail_; }

  [[nodiscard]] std::string to_string() const {
    // Qualified: the member name would otherwise hide the namespace-scope
    // rendering overload of the same name.
    std::string out(::fairness_governor::to_string(code_));
    if (!detail_.empty()) {
      out += ": ";
      out += detail_;
    }
    return out;
  }

 private:
  StatusCode code_{StatusCode::Ok};
  std::string detail_{};
};

/// Result carrier: either a value or a Status.
template <class T>
class Result {
 private:
  using StatusType = ::fairness_governor::Status;

 public:
  using value_type = T;
  using status_type = StatusType;

  // Not noexcept: a value or a detail string may allocate.
  Result(T value) : storage_(std::move(value)) {}             // NOLINT(google-explicit-constructor)
  Result(StatusType failure) : storage_(std::move(failure)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  [[nodiscard]] const StatusType& status() const noexcept {
    static const StatusType kOk{};
    return ok() ? kOk : std::get<StatusType>(storage_);
  }
  [[nodiscard]] StatusCode code() const noexcept { return status().code(); }

  [[nodiscard]] T& value() noexcept { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const noexcept { return std::get<T>(storage_); }
  [[nodiscard]] T&& take() noexcept { return std::move(std::get<T>(storage_)); }

 private:
  std::variant<T, StatusType> storage_;
};

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_CORE_STATUS_HPP
