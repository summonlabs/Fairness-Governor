// Fairness Governor - dynamic service evidence.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Evidence is dynamic and untrusted-for-liveness: it describes what a producer
// observed in one service window. Evidence never carries entitlement authority
// and is never restored as live after a restart — it must be re-attested.
#ifndef FAIRNESS_GOVERNOR_MODEL_EVIDENCE_HPP
#define FAIRNESS_GOVERNOR_MODEL_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fairness_governor/core/ids.hpp"
#include "fairness_governor/core/limits.hpp"
#include "fairness_governor/core/status.hpp"
#include "fairness_governor/model/refs.hpp"

namespace fairness_governor {

/// Observed service for one subject inside one service window.
struct SubjectObservation {
  SubjectId id{};
  SubjectGeneration generation{};
  /// Served units attributed to this subject in this window.
  std::uint64_t served_units{0};
  /// Consecutive fully-unserved windows ending at this window, as counted by
  /// the producer. The governor also keeps its own durable counter and uses the
  /// larger of the two, so a producer cannot reset a starvation streak.
  std::uint32_t unserved_streak{0};
  /// Consecutive windows below the guarantee floor ending at this window.
  std::uint32_t below_floor_streak{0};
  /// Priority class reference observed for this window.
  PriorityClassRef priority{};
  QosClassRef qos{};

  friend bool operator==(const SubjectObservation& a, const SubjectObservation& b) {
    return a.id == b.id && a.generation == b.generation && a.served_units == b.served_units &&
           a.unserved_streak == b.unserved_streak && a.below_floor_streak == b.below_floor_streak &&
           a.priority == b.priority && a.qos == b.qos;
  }
};

/// One complete, immutable service-window report.
struct EvidenceSnapshot {
  EvidenceSnapshotId id{};
  EvidenceSnapshotGeneration generation{};
  ServiceWindowId window{};
  ServiceWindowGeneration window_generation{};
  FabricEpoch epoch{};

  /// Producer-supplied capture instant, used for the freshness rule.
  TimePointNs captured_at_ns{0};
  /// Monotonic sequence assigned by the producer.
  SequenceNumber sequence{0};

  ProducerIdentity producer{};
  Provenance provenance{};

  /// Declared digest of the observation set. When non-zero, the governor
  /// recomputes it and rejects a mismatch as contradictory evidence.
  std::uint64_t declared_digest{0};

  std::vector<SubjectObservation> observations;

  [[nodiscard]] const SubjectObservation* find(SubjectId subject) const noexcept {
    for (const SubjectObservation& observation : observations) {
      if (observation.id == subject) {
        return &observation;
      }
    }
    return nullptr;
  }
};

/// Structurally validates a snapshot: sizes, bounds, duplicate subjects,
/// contradictory priority references, and arithmetic domain.
[[nodiscard]] Status validate_evidence(const EvidenceSnapshot& snapshot);

/// Deterministic digest of the observation set, order-independent.
[[nodiscard]] std::uint64_t compute_observation_digest(const EvidenceSnapshot& snapshot);

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_MODEL_EVIDENCE_HPP
