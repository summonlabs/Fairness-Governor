// Fairness Governor - strongly typed identities, generations, epochs, incarnations.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef FAIRNESS_GOVERNOR_CORE_IDS_HPP
#define FAIRNESS_GOVERNOR_CORE_IDS_HPP

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace fairness_governor {

/// Strongly typed 64-bit identity. Value 0 is reserved to mean "absent/none"
/// and is never a legal identity for an entity that must exist.
template <class Tag>
class Id {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;

  constexpr Id() noexcept = default;
  constexpr explicit Id(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Id from_value(std::uint64_t value) noexcept { return Id(value); }
  [[nodiscard]] static constexpr Id none() noexcept { return Id(0); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
  friend constexpr std::strong_ordering operator<=>(Id a, Id b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  std::uint64_t value_{0};
};

/// Strongly typed monotonic generation counter bound to an identity tag.
/// Generations start at 1; 0 means "no generation has been published yet".
template <class Tag>
class Generation {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;

  constexpr Generation() noexcept = default;
  constexpr explicit Generation(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Generation from_value(std::uint64_t value) noexcept {
    return Generation(value);
  }
  [[nodiscard]] static constexpr Generation initial() noexcept { return Generation(1); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

  /// Advances the generation. Saturates rather than wrapping: a wrapped
  /// generation would silently make stale work look fresh.
  [[nodiscard]] constexpr Generation next() const noexcept {
    if (value_ == UINT64_MAX) {
      return *this;
    }
    return Generation(value_ + 1);
  }

  friend constexpr bool operator==(Generation a, Generation b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr std::strong_ordering operator<=>(Generation a, Generation b) noexcept {
    return a.value_ <=> b.value_;
  }

 private:
  std::uint64_t value_{0};
};

// --- Identity tags -----------------------------------------------------------

struct FairnessGroupIdTag;
struct SubjectIdTag;
struct FairnessPolicyIdTag;
struct ServiceWindowIdTag;
struct EvidenceSnapshotIdTag;
struct InterventionIdTag;
struct PriorityRefTag;
struct QosRefTag;
struct FabricEpochTag;
struct PublisherIdTag;
struct BootIdTag;
struct TenantIdTag;

using FairnessGroupId = Id<FairnessGroupIdTag>;
using SubjectId = Id<SubjectIdTag>;
using FairnessPolicyId = Id<FairnessPolicyIdTag>;
using ServiceWindowId = Id<ServiceWindowIdTag>;
using EvidenceSnapshotId = Id<EvidenceSnapshotIdTag>;
using InterventionId = Id<InterventionIdTag>;
using PriorityRef = Id<PriorityRefTag>;
using QosRef = Id<QosRefTag>;
using PublisherId = Id<PublisherIdTag>;
using TenantId = Id<TenantIdTag>;

using FairnessGroupGeneration = Generation<FairnessGroupIdTag>;
using SubjectGeneration = Generation<SubjectIdTag>;
using FairnessPolicyGeneration = Generation<FairnessPolicyIdTag>;
using ServiceWindowGeneration = Generation<ServiceWindowIdTag>;
using EvidenceSnapshotGeneration = Generation<EvidenceSnapshotIdTag>;
using InterventionGeneration = Generation<InterventionIdTag>;

/// Fabric epoch: the fencing domain for authoritative fairness decisions.
/// A decision taken in epoch E is void once the fabric advances past E.
using FabricEpoch = Generation<FabricEpochTag>;

/// Boot identity of a process incarnation. Random 64-bit, generated once per
/// process start; must never be restored from durable state.
using BootId = Id<BootIdTag>;

/// A process incarnation: the pair (boot, ordinal). Two governors in the same
/// boot are distinguished by ordinal; a restarted process can never reuse the
/// previous boot id.
struct Incarnation {
  BootId boot{};
  std::uint64_t ordinal{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return boot.valid() && ordinal != 0; }
  friend constexpr bool operator==(const Incarnation& a, const Incarnation& b) noexcept {
    return a.boot == b.boot && a.ordinal == b.ordinal;
  }
  friend constexpr bool operator!=(const Incarnation& a, const Incarnation& b) noexcept {
    return !(a == b);
  }
};

/// Sequence number inside a publisher session. Strictly increasing per publisher.
using SequenceNumber = std::uint64_t;

/// Wall-clock-ish instant supplied by the caller, in nanoseconds. The governor
/// never reads the system clock for authoritative decisions: time is an input,
/// which is what makes canonical input yield canonical output.
using TimePointNs = std::int64_t;

/// Deterministic 64-bit non-cryptographic digest (FNV-1a) used for integrity
/// checks of durable records and frames. Documented as an integrity check
/// against corruption, not as a cryptographic authentication mechanism.
[[nodiscard]] constexpr std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size,
                                              std::uint64_t seed = 1469598103934665603ULL) noexcept {
  std::uint64_t hash = seed;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<std::uint64_t>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;

[[nodiscard]] constexpr std::uint64_t fnv1a64_continue(std::uint64_t hash,
                                                       const std::uint8_t* data,
                                                       std::size_t size) noexcept {
  return fnv1a64(data, size, hash);
}

/// Stable, allocation-free textual rendering of an unsigned magnitude.
[[nodiscard]] inline std::string to_decimal(std::uint64_t value) {
  char buffer[21];
  std::size_t index = sizeof(buffer);
  if (value == 0) {
    return std::string("0");
  }
  while (value != 0 && index > 0) {
    buffer[--index] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
  return std::string(buffer + index, sizeof(buffer) - index);
}

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_CORE_IDS_HPP
