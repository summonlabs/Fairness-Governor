// Fairness Governor - external references and provenance.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The governor references priority/QoS classes owned by other systems; it never
// creates, mutates, or enforces them. A reference is only usable when it is
// generation-matched, so a re-created class cannot be silently reused.
#ifndef FAIRNESS_GOVERNOR_MODEL_REFS_HPP
#define FAIRNESS_GOVERNOR_MODEL_REFS_HPP

#include <cstdint>
#include <string>

#include "fairness_governor/core/ids.hpp"

namespace fairness_governor {

/// Where a policy element came from: an operator-visible configuration artifact.
struct Provenance {
  /// Monotonic revision of the source artifact.
  std::uint64_t revision{0};
  /// Identity of the source (for example a config bundle name). Not authority.
  std::string source;
  /// Digest of the exact source bytes this element was derived from. Empty is
  /// allowed for locally constructed policies and is reported as unbound.
  std::uint64_t source_digest{0};
  /// Capture instant supplied by the producer.
  TimePointNs captured_at_ns{0};

  [[nodiscard]] bool bound() const noexcept { return source_digest != 0 && revision != 0; }
};

/// A priority class owned by an external system, referenced by generation.
/// PriorityRefTag identity 0 with generation 0 means "no priority class".
struct PriorityClassRef {
  PriorityRef id{};
  Generation<PriorityRefTag> generation{};

  [[nodiscard]] bool present() const noexcept { return id.valid(); }
  [[nodiscard]] bool consistent() const noexcept { return id.valid() == generation.valid(); }
  friend bool operator==(const PriorityClassRef& a, const PriorityClassRef& b) {
    return a.id == b.id && a.generation == b.generation;
  }
};

/// A QoS class owned by an external system, referenced by generation. The
/// governor reads it to explain value; it never creates or enforces it.
struct QosClassRef {
  QosRef id{};
  Generation<QosRefTag> generation{};

  [[nodiscard]] bool present() const noexcept { return id.valid(); }
  [[nodiscard]] bool consistent() const noexcept { return id.valid() == generation.valid(); }
  friend bool operator==(const QosClassRef& a, const QosClassRef& b) {
    return a.id == b.id && a.generation == b.generation;
  }
};

/// Identity of the software that produced an evidence snapshot.
struct ProducerIdentity {
  std::string name;
  std::uint64_t instance{0};
  std::uint64_t version{0};
};

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_MODEL_REFS_HPP
