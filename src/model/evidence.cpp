// Fairness Governor - evidence validation and digesting.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/model/evidence.hpp"

#include <algorithm>
#include <vector>

#include "fairness_governor/core/bytes.hpp"
#include "fairness_governor/core/checked.hpp"

namespace fairness_governor {
namespace {

constexpr std::uint32_t kMaxStreakWindows = 1000000u;
constexpr std::size_t kMaxProducerNameBytes = 128;
constexpr std::size_t kMaxProvenanceSourceBytes = 128;

}  // namespace

Status validate_evidence(const EvidenceSnapshot& snapshot) {
  if (!snapshot.id.valid()) {
    return Status(StatusCode::InvalidArgument, "evidence snapshot id must be non-zero");
  }
  if (!snapshot.generation.valid()) {
    return Status(StatusCode::InvalidArgument, "evidence snapshot generation must be >= 1");
  }
  if (!snapshot.window.valid()) {
    return Status(StatusCode::InvalidArgument, "service window id must be non-zero");
  }
  if (!snapshot.window_generation.valid()) {
    return Status(StatusCode::InvalidArgument, "service window generation must be >= 1");
  }
  if (!snapshot.epoch.valid()) {
    return Status(StatusCode::InvalidArgument, "evidence epoch must be >= 1");
  }
  if (snapshot.observations.size() > kMaxEvidenceRecords) {
    return Status(StatusCode::LimitExceeded, "observation count exceeds kMaxEvidenceRecords");
  }
  if (snapshot.producer.name.size() > kMaxProducerNameBytes) {
    return Status(StatusCode::LimitExceeded, "producer name is too long");
  }
  if (snapshot.provenance.source.size() > kMaxProvenanceSourceBytes) {
    return Status(StatusCode::LimitExceeded, "provenance source is too long");
  }
  if (snapshot.captured_at_ns < 0) {
    return Status(StatusCode::InvalidArgument, "capture instant must not be negative");
  }

  std::uint64_t total_served = 0;
  std::vector<std::uint64_t> seen;
  seen.reserve(snapshot.observations.size());
  for (const SubjectObservation& observation : snapshot.observations) {
    if (!observation.id.valid()) {
      return Status(StatusCode::InvalidArgument, "observation subject id must be non-zero");
    }
    if (!observation.generation.valid()) {
      return Status(StatusCode::InvalidArgument, "observation generation must be >= 1");
    }
    if (observation.served_units > kMaxServiceUnits) {
      return Status(StatusCode::OutOfRange, "observed service exceeds kMaxServiceUnits");
    }
    if (observation.unserved_streak > kMaxStreakWindows ||
        observation.below_floor_streak > kMaxStreakWindows) {
      return Status(StatusCode::OutOfRange, "observation streak is implausible");
    }
    if (!observation.priority.consistent()) {
      return Status(StatusCode::InvalidArgument, "observation priority reference is incoherent");
    }
    if (!observation.qos.consistent()) {
      return Status(StatusCode::InvalidArgument, "observation QoS reference is incoherent");
    }
    if (observation.served_units > 0 && observation.unserved_streak != 0) {
      return Status(StatusCode::EvidenceContradictory,
                    "an observation cannot be served and unserved in the same window");
    }
    if (!checked_add(total_served, observation.served_units, total_served)) {
      return Status(StatusCode::ArithmeticOverflow, "total observed service overflows");
    }
    seen.push_back(observation.id.value());
  }

  std::sort(seen.begin(), seen.end());
  if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) {
    return Status(StatusCode::Duplicate, "evidence declares the same subject twice");
  }

  if (snapshot.declared_digest != 0) {
    const std::uint64_t computed = compute_observation_digest(snapshot);
    if (computed != snapshot.declared_digest) {
      return Status(StatusCode::EvidenceContradictory,
                    "declared observation digest does not match the observation set");
    }
  }
  return Status::success();
}

std::uint64_t compute_observation_digest(const EvidenceSnapshot& snapshot) {
  // Order-independent by construction: the per-observation digests are combined
  // with two commutative operators (addition and exclusive or), so a producer
  // may emit observations in any order and still declare the same digest.
  // Duplicate subjects are rejected before this point, so the combination has no
  // cancellation ambiguity.
  std::uint64_t sum = 0;
  std::uint64_t xored = 0;
  for (const SubjectObservation& observation : snapshot.observations) {
    std::uint8_t bytes[8 * 7];
    store_le<std::uint64_t>(bytes + 0, observation.id.value());
    store_le<std::uint64_t>(bytes + 8, observation.generation.value());
    store_le<std::uint64_t>(bytes + 16, observation.served_units);
    store_le<std::uint64_t>(bytes + 24, observation.unserved_streak);
    store_le<std::uint64_t>(bytes + 32, observation.below_floor_streak);
    store_le<std::uint64_t>(bytes + 40, observation.priority.id.value());
    store_le<std::uint64_t>(bytes + 48, observation.qos.id.value());
    const std::uint64_t digest = fnv1a64(bytes, sizeof(bytes));
    sum += digest;
    xored ^= digest;
  }
  return (sum * 0x9E3779B97F4A7C15ULL) ^ xored ^ kFnvOffsetBasis;
}

}  // namespace fairness_governor
