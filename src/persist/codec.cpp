// Fairness Governor - strict bounded codecs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/persist/codec.hpp"

#include <string>
#include <unordered_set>
#include <vector>

#include "fairness_governor/core/checked.hpp"
#include "fairness_governor/version.hpp"

namespace fairness_governor::codec {
namespace {

constexpr std::uint32_t kMaxEncodedStringBytes = 256;
constexpr std::uint32_t kMaxEncodedRecords = 65536;

void write_provenance(ByteWriter& writer, const Provenance& provenance) {
  writer.u64(provenance.revision);
  writer.blob(provenance.source);
  writer.u64(provenance.source_digest);
  writer.i64(provenance.captured_at_ns);
}

[[nodiscard]] bool read_provenance(ByteReader& reader, Provenance& out) {
  return reader.u64(out.revision) && reader.blob(out.source, kMaxEncodedStringBytes) &&
         reader.u64(out.source_digest) && reader.i64(out.captured_at_ns);
}

void write_priority(ByteWriter& writer, const PriorityClassRef& ref) {
  writer.u64(ref.id.value());
  writer.u64(ref.generation.value());
}

[[nodiscard]] bool read_priority(ByteReader& reader, PriorityClassRef& out) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  if (!reader.u64(id) || !reader.u64(generation)) {
    return false;
  }
  out.id = PriorityRef::from_value(id);
  out.generation = Generation<PriorityRefTag>::from_value(generation);
  return true;
}

void write_qos(ByteWriter& writer, const QosClassRef& ref) {
  writer.u64(ref.id.value());
  writer.u64(ref.generation.value());
}

[[nodiscard]] bool read_qos(ByteReader& reader, QosClassRef& out) {
  std::uint64_t id = 0;
  std::uint64_t generation = 0;
  if (!reader.u64(id) || !reader.u64(generation)) {
    return false;
  }
  out.id = QosRef::from_value(id);
  out.generation = Generation<QosRefTag>::from_value(generation);
  return true;
}

[[nodiscard]] std::uint64_t digest_bytes(const ByteBuffer& bytes) {
  return checksum64(bytes.data(), bytes.size());
}

}  // namespace

// --- Policy ------------------------------------------------------------------

ByteBuffer encode_policy(const FairnessPolicy& policy) {
  ByteBuffer buffer;
  ByteWriter writer(buffer);
  writer.u16(kFormatVersion);
  writer.u64(policy.id.value());
  writer.u64(policy.generation.value());
  writer.u64(policy.epoch.value());
  writer.u64(policy.window_units);
  writer.u64(policy.fair_band_units);
  writer.u64(policy.correction_threshold_units);
  writer.u32(policy.cooldown_windows);
  writer.u64(policy.max_correction_units);
  writer.u32(policy.max_correction_bps);
  writer.u32(policy.max_priority_modifier_bps);
  writer.u32(policy.max_evidence_age_windows);
  writer.u64(policy.max_evidence_age_ns);
  writer.u64(policy.max_carry_units);
  writer.u8(policy.enforce_guarantee_floors ? 1 : 0);
  write_provenance(writer, policy.provenance);
  writer.u32(static_cast<std::uint32_t>(policy.groups.size()));
  for (const FairnessGroup& group : policy.groups) {
    writer.u64(group.id.value());
    writer.u64(group.generation.value());
    writer.u64(group.parent.value());
    writer.u32(group.share_weight);
    writer.u64(group.guarantee_floor);
    writer.u32(group.obligation_rank);
    writer.u8(group.protected_obligation ? 1 : 0);
    writer.blob(group.label);
  }
  writer.u32(static_cast<std::uint32_t>(policy.subjects.size()));
  for (const Subject& subject : policy.subjects) {
    writer.u64(subject.id.value());
    writer.u64(subject.generation.value());
    writer.u64(subject.group.value());
    writer.u64(subject.tenant.value());
    writer.u32(subject.share_weight);
    writer.u64(subject.guarantee_floor);
    writer.u32(subject.starvation_windows);
    writer.u32(subject.guarantee_starvation_windows);
    write_priority(writer, subject.priority);
    write_qos(writer, subject.qos);
    writer.u32(static_cast<std::uint32_t>(subject.priority_modifier_bps));
    writer.u32(subject.priority_rank);
    writer.u32(subject.obligation_rank);
    writer.u8(subject.protected_obligation ? 1 : 0);
    writer.u64(subject.max_reduce_units);
    writer.u64(subject.max_augment_units);
    writer.blob(subject.label);
  }
  return buffer;
}

Status decode_policy(const ByteBuffer& bytes, FairnessPolicy& out) {
  if (bytes.size() > kMaxDurablePayloadBytes) {
    return Status(StatusCode::OversizedPayload, "policy payload exceeds the durable bound");
  }
  ByteReader reader(bytes.data(), bytes.size());
  std::uint16_t version = 0;
  FairnessPolicy policy;
  std::uint8_t flag = 0;
  std::uint32_t group_count = 0;
  std::uint32_t subject_count = 0;
  if (!reader.u16(version)) {
    return Status(StatusCode::Truncated, "policy header truncated");
  }
  if (version != kFormatVersion) {
    return Status(StatusCode::UnsupportedFormat, "unsupported policy format version");
  }
  std::uint64_t raw = 0;
  bool ok = reader.u64(raw);
  policy.id = FairnessPolicyId::from_value(raw);
  ok = ok && reader.u64(raw);
  policy.generation = FairnessPolicyGeneration::from_value(raw);
  ok = ok && reader.u64(raw);
  policy.epoch = FabricEpoch::from_value(raw);
  ok = ok && reader.u64(policy.window_units) && reader.u64(policy.fair_band_units) &&
       reader.u64(policy.correction_threshold_units) && reader.u32(policy.cooldown_windows) &&
       reader.u64(policy.max_correction_units) && reader.u32(policy.max_correction_bps) &&
       reader.u32(policy.max_priority_modifier_bps) && reader.u32(policy.max_evidence_age_windows) &&
       reader.u64(policy.max_evidence_age_ns) && reader.u64(policy.max_carry_units) &&
       reader.u8(flag);
  if (!ok) {
    return Status(StatusCode::Truncated, "policy fields truncated");
  }
  policy.enforce_guarantee_floors = flag != 0;
  if (!read_provenance(reader, policy.provenance)) {
    return Status(StatusCode::Truncated, "policy provenance truncated");
  }
  if (!reader.u32(group_count) || group_count > kMaxGroups) {
    return Status(group_count > kMaxGroups ? StatusCode::LimitExceeded : StatusCode::Truncated,
                  "policy group count invalid");
  }
  policy.groups.reserve(group_count);
  for (std::uint32_t i = 0; i < group_count; ++i) {
    FairnessGroup group;
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint64_t parent = 0;
    std::uint8_t protected_flag = 0;
    if (!reader.u64(id) || !reader.u64(generation) || !reader.u64(parent) ||
        !reader.u32(group.share_weight) || !reader.u64(group.guarantee_floor) ||
        !reader.u32(group.obligation_rank) || !reader.u8(protected_flag) ||
        !reader.blob(group.label, kMaxEncodedStringBytes)) {
      return Status(StatusCode::Truncated, "policy group record truncated");
    }
    group.id = FairnessGroupId::from_value(id);
    group.generation = FairnessGroupGeneration::from_value(generation);
    group.parent = FairnessGroupId::from_value(parent);
    group.protected_obligation = protected_flag != 0;
    policy.groups.push_back(std::move(group));
  }
  if (!reader.u32(subject_count) || subject_count > kMaxSubjects) {
    return Status(subject_count > kMaxSubjects ? StatusCode::LimitExceeded : StatusCode::Truncated,
                  "policy subject count invalid");
  }
  policy.subjects.reserve(subject_count);
  for (std::uint32_t i = 0; i < subject_count; ++i) {
    Subject subject;
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint64_t group = 0;
    std::uint64_t tenant = 0;
    std::uint32_t modifier = 0;
    std::uint8_t protected_flag = 0;
    if (!reader.u64(id) || !reader.u64(generation) || !reader.u64(group) || !reader.u64(tenant) ||
        !reader.u32(subject.share_weight) || !reader.u64(subject.guarantee_floor) ||
        !reader.u32(subject.starvation_windows) ||
        !reader.u32(subject.guarantee_starvation_windows) || !read_priority(reader, subject.priority) ||
        !read_qos(reader, subject.qos) || !reader.u32(modifier) ||
        !reader.u32(subject.priority_rank) || !reader.u32(subject.obligation_rank) ||
        !reader.u8(protected_flag) || !reader.u64(subject.max_reduce_units) ||
        !reader.u64(subject.max_augment_units) || !reader.blob(subject.label, kMaxEncodedStringBytes)) {
      return Status(StatusCode::Truncated, "policy subject record truncated");
    }
    subject.id = SubjectId::from_value(id);
    subject.generation = SubjectGeneration::from_value(generation);
    subject.group = FairnessGroupId::from_value(group);
    subject.tenant = TenantId::from_value(tenant);
    subject.priority_modifier_bps = static_cast<std::int32_t>(modifier);
    subject.protected_obligation = protected_flag != 0;
    policy.subjects.push_back(std::move(subject));
  }
  if (!reader.empty()) {
    return Status(StatusCode::CorruptRecord, "policy record has trailing bytes");
  }
  const PolicyValidation validation = validate_policy(policy);
  if (!validation.ok()) {
    return Status(validation.code, validation.detail);
  }
  out = std::move(policy);
  return Status::success();
}

// --- Accounting --------------------------------------------------------------

ByteBuffer encode_accounting(const FairnessAccounting& accounting) {
  ByteBuffer buffer;
  ByteWriter writer(buffer);
  writer.u16(kFormatVersion);
  writer.u64(accounting.policy_id.value());
  writer.u64(accounting.policy_generation.value());
  writer.u64(accounting.epoch.value());
  writer.u64(accounting.generation);
  writer.u64(accounting.intervention_generation.value());
  writer.u64(accounting.last_window.value());
  writer.u64(accounting.last_window_generation.value());
  writer.u64(accounting.writer_boot.value());
  writer.u32(static_cast<std::uint32_t>(accounting.subjects.size()));
  for (const SubjectAccounting& entry : accounting.subjects) {
    writer.u64(entry.id.value());
    writer.u64(entry.generation.value());
    writer.u64(entry.cumulative_deficit);
    writer.u64(entry.cumulative_surplus);
    writer.u64(entry.discarded_deficit);
    writer.u64(entry.discarded_surplus);
    writer.u32(entry.unserved_streak);
    writer.u32(entry.below_floor_streak);
    writer.u64(entry.windows_observed);
    writer.u64(entry.total_served_units);
    writer.u64(entry.last_correction_window.value());
    writer.u64(entry.last_correction_generation.value());
  }
  return buffer;
}

Status decode_accounting(const ByteBuffer& bytes, FairnessAccounting& out) {
  if (bytes.size() > kMaxDurablePayloadBytes) {
    return Status(StatusCode::OversizedPayload, "accounting payload exceeds the durable bound");
  }
  ByteReader reader(bytes.data(), bytes.size());
  std::uint16_t version = 0;
  FairnessAccounting accounting;
  std::uint32_t count = 0;
  std::uint64_t raw = 0;
  if (!reader.u16(version)) {
    return Status(StatusCode::Truncated, "accounting header truncated");
  }
  if (version != kFormatVersion) {
    return Status(StatusCode::UnsupportedFormat, "unsupported accounting format version");
  }
  bool ok = reader.u64(raw);
  accounting.policy_id = FairnessPolicyId::from_value(raw);
  ok = ok && reader.u64(raw);
  accounting.policy_generation = FairnessPolicyGeneration::from_value(raw);
  ok = ok && reader.u64(raw);
  accounting.epoch = FabricEpoch::from_value(raw);
  ok = ok && reader.u64(accounting.generation) && reader.u64(raw);
  accounting.intervention_generation = InterventionGeneration::from_value(raw);
  ok = ok && reader.u64(raw);
  accounting.last_window = ServiceWindowId::from_value(raw);
  ok = ok && reader.u64(raw);
  accounting.last_window_generation = ServiceWindowGeneration::from_value(raw);
  ok = ok && reader.u64(raw);
  accounting.writer_boot = BootId::from_value(raw);
  ok = ok && reader.u32(count);
  if (!ok) {
    return Status(StatusCode::Truncated, "accounting fields truncated");
  }
  if (count > kMaxEncodedRecords) {
    return Status(StatusCode::LimitExceeded, "accounting subject count exceeds the bound");
  }
  accounting.subjects.reserve(count);
  std::unordered_set<std::uint64_t> seen;
  seen.reserve(count * 2 + 1);
  for (std::uint32_t i = 0; i < count; ++i) {
    SubjectAccounting entry;
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    std::uint64_t window = 0;
    std::uint64_t intervention = 0;
    if (!reader.u64(id) || !reader.u64(generation) || !reader.u64(entry.cumulative_deficit) ||
        !reader.u64(entry.cumulative_surplus) || !reader.u64(entry.discarded_deficit) ||
        !reader.u64(entry.discarded_surplus) || !reader.u32(entry.unserved_streak) ||
        !reader.u32(entry.below_floor_streak) || !reader.u64(entry.windows_observed) ||
        !reader.u64(entry.total_served_units) || !reader.u64(window) || !reader.u64(intervention)) {
      return Status(StatusCode::Truncated, "accounting subject record truncated");
    }
    if (entry.cumulative_deficit > kMaxCarryUnits || entry.cumulative_surplus > kMaxCarryUnits) {
      return Status(StatusCode::OutOfRange, "accounting carry exceeds the durable bound");
    }
    if (entry.unserved_streak > 1000000u || entry.below_floor_streak > 1000000u) {
      return Status(StatusCode::OutOfRange, "accounting streak exceeds the bound");
    }
    if (!seen.insert(id).second) {
      return Status(StatusCode::Duplicate, "accounting declares the same subject twice");
    }
    entry.id = SubjectId::from_value(id);
    entry.generation = SubjectGeneration::from_value(generation);
    entry.last_correction_window = ServiceWindowId::from_value(window);
    entry.last_correction_generation = InterventionGeneration::from_value(intervention);
    accounting.subjects.push_back(std::move(entry));
  }
  if (!reader.empty()) {
    return Status(StatusCode::CorruptRecord, "accounting record has trailing bytes");
  }
  out = std::move(accounting);
  return Status::success();
}

// --- Evidence ----------------------------------------------------------------

ByteBuffer encode_evidence(const EvidenceSnapshot& snapshot) {
  ByteBuffer buffer;
  ByteWriter writer(buffer);
  writer.u16(kFormatVersion);
  writer.u64(snapshot.id.value());
  writer.u64(snapshot.generation.value());
  writer.u64(snapshot.window.value());
  writer.u64(snapshot.window_generation.value());
  writer.u64(snapshot.epoch.value());
  writer.i64(snapshot.captured_at_ns);
  writer.u64(snapshot.sequence);
  writer.blob(snapshot.producer.name);
  writer.u64(snapshot.producer.instance);
  writer.u64(snapshot.producer.version);
  write_provenance(writer, snapshot.provenance);
  writer.u64(snapshot.declared_digest);
  writer.u32(static_cast<std::uint32_t>(snapshot.observations.size()));
  for (const SubjectObservation& observation : snapshot.observations) {
    writer.u64(observation.id.value());
    writer.u64(observation.generation.value());
    writer.u64(observation.served_units);
    writer.u32(observation.unserved_streak);
    writer.u32(observation.below_floor_streak);
    write_priority(writer, observation.priority);
    write_qos(writer, observation.qos);
  }
  return buffer;
}

Status decode_evidence(const ByteBuffer& bytes, EvidenceSnapshot& out) {
  if (bytes.size() > kMaxDurablePayloadBytes) {
    return Status(StatusCode::OversizedPayload, "evidence payload exceeds the bound");
  }
  ByteReader reader(bytes.data(), bytes.size());
  std::uint16_t version = 0;
  EvidenceSnapshot snapshot;
  std::uint32_t count = 0;
  std::uint64_t raw = 0;
  if (!reader.u16(version)) {
    return Status(StatusCode::Truncated, "evidence header truncated");
  }
  if (version != kFormatVersion) {
    return Status(StatusCode::UnsupportedFormat, "unsupported evidence format version");
  }
  bool ok = reader.u64(raw);
  snapshot.id = EvidenceSnapshotId::from_value(raw);
  ok = ok && reader.u64(raw);
  snapshot.generation = EvidenceSnapshotGeneration::from_value(raw);
  ok = ok && reader.u64(raw);
  snapshot.window = ServiceWindowId::from_value(raw);
  ok = ok && reader.u64(raw);
  snapshot.window_generation = ServiceWindowGeneration::from_value(raw);
  ok = ok && reader.u64(raw);
  snapshot.epoch = FabricEpoch::from_value(raw);
  ok = ok && reader.i64(snapshot.captured_at_ns) && reader.u64(snapshot.sequence) &&
       reader.blob(snapshot.producer.name, kMaxEncodedStringBytes) &&
       reader.u64(snapshot.producer.instance) && reader.u64(snapshot.producer.version) &&
       read_provenance(reader, snapshot.provenance) && reader.u64(snapshot.declared_digest) &&
       reader.u32(count);
  if (!ok) {
    return Status(StatusCode::Truncated, "evidence fields truncated");
  }
  if (count > kMaxEvidenceRecords) {
    return Status(StatusCode::LimitExceeded, "evidence observation count exceeds the bound");
  }
  snapshot.observations.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    SubjectObservation observation;
    std::uint64_t id = 0;
    std::uint64_t generation = 0;
    if (!reader.u64(id) || !reader.u64(generation) || !reader.u64(observation.served_units) ||
        !reader.u32(observation.unserved_streak) || !reader.u32(observation.below_floor_streak) ||
        !read_priority(reader, observation.priority) || !read_qos(reader, observation.qos)) {
      return Status(StatusCode::Truncated, "evidence observation truncated");
    }
    observation.id = SubjectId::from_value(id);
    observation.generation = SubjectGeneration::from_value(generation);
    snapshot.observations.push_back(std::move(observation));
  }
  if (!reader.empty()) {
    return Status(StatusCode::CorruptRecord, "evidence record has trailing bytes");
  }
  const Status validation = validate_evidence(snapshot);
  if (!validation.ok()) {
    return validation;
  }
  out = std::move(snapshot);
  return Status::success();
}

// --- Digests -----------------------------------------------------------------

std::uint64_t policy_digest(const FairnessPolicy& policy) {
  const ByteBuffer bytes = encode_policy(policy);
  return digest_bytes(bytes);
}

std::uint64_t evidence_digest(const EvidenceSnapshot& snapshot) {
  const ByteBuffer bytes = encode_evidence(snapshot);
  return digest_bytes(bytes);
}

std::uint64_t decision_digest(const FairnessDecision& decision) {
  ByteBuffer buffer;
  ByteWriter writer(buffer);
  writer.u64(decision.authority.policy_id.value());
  writer.u64(decision.authority.policy_generation.value());
  writer.u64(decision.authority.epoch.value());
  writer.u64(decision.authority.evidence_id.value());
  writer.u64(decision.authority.evidence_generation.value());
  writer.u64(decision.authority.window.value());
  writer.u64(decision.authority.window_generation.value());
  writer.u64(decision.authority.accounting_generation);
  writer.u64(decision.authority.request_id);
  writer.u32(decision.authority.attempt);
  writer.u8(static_cast<std::uint8_t>(decision.outcome));
  for (const SubjectFairnessState& state : decision.subjects) {
    writer.u64(state.id.value());
    writer.u64(state.generation.value());
    writer.u64(state.group.value());
    writer.u8(static_cast<std::uint8_t>(state.outcome));
    writer.u16(static_cast<std::uint16_t>(state.reason));
    writer.u8(state.has_authority ? 1 : 0);
    writer.u64(state.entitlement_units);
    writer.u64(state.base_entitlement_units);
    writer.u64(state.group_entitlement_units);
    writer.u64(state.served_units);
    writer.i64(state.deviation_units);
    writer.u64(state.deficit_units);
    writer.u64(state.surplus_units);
    writer.u64(state.cumulative_deficit_after);
    writer.u64(state.cumulative_surplus_after);
    writer.u32(state.unserved_streak);
    writer.u32(state.below_floor_streak);
    writer.u64(state.proposed_augment_units);
    writer.u64(state.proposed_reduce_units);
    writer.u64(state.unsatisfied_demand_units);
    writer.i64(state.priority_delta_units);
    writer.u32(static_cast<std::uint32_t>(state.applied_modifier_bps));
  }
  for (const GroupFairnessState& state : decision.groups) {
    writer.u64(state.id.value());
    writer.u64(state.generation.value());
    writer.u64(state.entitlement_units);
    writer.u64(state.served_units);
    writer.i64(state.deviation_units);
    writer.u8(state.withheld_correction ? 1 : 0);
  }
  for (const CorrectiveIntent& intent : decision.intents) {
    writer.u64(intent.id.value());
    writer.u64(intent.generation.value());
    writer.u8(static_cast<std::uint8_t>(intent.direction));
    writer.u64(intent.subject.value());
    writer.u64(intent.units);
    writer.u8(static_cast<std::uint8_t>(intent.bound));
    writer.u16(static_cast<std::uint16_t>(intent.reason));
    writer.u8(intent.from_starvation ? 1 : 0);
  }
  writer.u64(decision.correction.reducible_pool_units);
  writer.u64(decision.correction.demand_units);
  writer.u64(decision.correction.authorized_budget_units);
  writer.u64(decision.correction.augment_units);
  writer.u64(decision.correction.reduce_units);
  writer.u64(decision.correction.withheld_units);
  writer.u64(decision.correction.unsatisfied_demand_units);
  writer.u8(decision.correction.protected_obligation_blocked ? 1 : 0);
  writer.u8(decision.correction.authority_missing ? 1 : 0);
  return digest_bytes(buffer);
}

}  // namespace fairness_governor::codec
