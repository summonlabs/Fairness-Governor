// Fairness Governor - codec round-trip and rejection tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/persist/codec.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;
using namespace fgtest;

FG_TEST(codec, policy_round_trips) {
  FairnessPolicy policy = flat_policy(3);
  policy.provenance.revision = 4;
  policy.provenance.source = "operator-config";
  policy.provenance.source_digest = 0x1234;
  policy.provenance.captured_at_ns = -5;
  policy.subjects[0].priority.id = PriorityRef::from_value(3);
  policy.subjects[0].priority.generation = Generation<PriorityRefTag>::from_value(2);
  policy.subjects[1].priority_modifier_bps = -500;
  policy.groups[0].protected_obligation = true;
  policy.groups[0].obligation_rank = 1;
  const ByteBuffer encoded = codec::encode_policy(policy);
  FairnessPolicy decoded;
  FG_CHECK_OK(codec::decode_policy(encoded, decoded));
  FG_CHECK_EQ(codec::policy_digest(decoded), codec::policy_digest(policy));
  FG_CHECK_EQ(decoded.subjects.size(), policy.subjects.size());
  FG_CHECK_EQ(decoded.groups.size(), policy.groups.size());
  FG_CHECK_EQ(decoded.provenance.source, std::string("operator-config"));
  FG_CHECK_EQ(decoded.provenance.captured_at_ns, -5);
  FG_CHECK_EQ(decoded.subjects[1].priority_modifier_bps, -500);
  FG_CHECK(decoded.groups[0].protected_obligation);
}

FG_TEST(codec, policy_decoder_rejects_damage) {
  const FairnessPolicy policy = flat_policy(2);
  const ByteBuffer encoded = codec::encode_policy(policy);

  ByteBuffer truncated = encoded;
  truncated.resize(encoded.size() / 2);
  FairnessPolicy decoded;
  FG_CHECK(!codec::decode_policy(truncated, decoded).ok());

  ByteBuffer trailing = encoded;
  trailing.push_back(0);
  FG_CHECK_EQ(codec::decode_policy(trailing, decoded).code(), StatusCode::CorruptRecord);

  ByteBuffer wrong_version = encoded;
  wrong_version[0] = 99;
  FG_CHECK_EQ(codec::decode_policy(wrong_version, decoded).code(),
              StatusCode::UnsupportedFormat);

  ByteBuffer bad_count = encoded;
  bad_count[bad_count.size() - 1] = 1;
  FG_CHECK(!codec::decode_policy(bad_count, decoded).ok());
}

FG_TEST(codec, policy_decoder_rejects_a_structurally_invalid_policy) {
  FairnessPolicy policy = flat_policy(2);
  policy.subjects[0].share_weight = 0;
  // The encoder is faithful; the decoder re-validates and must reject.
  const ByteBuffer encoded = codec::encode_policy(policy);
  FairnessPolicy decoded;
  FG_CHECK_EQ(codec::decode_policy(encoded, decoded).code(), StatusCode::OutOfRange);
}

FG_TEST(codec, accounting_round_trips) {
  FairnessAccounting accounting;
  accounting.policy_id = FairnessPolicyId::from_value(1);
  accounting.policy_generation = FairnessPolicyGeneration::from_value(2);
  accounting.epoch = FabricEpoch::from_value(3);
  accounting.generation = 17;
  accounting.intervention_generation = InterventionGeneration::from_value(4);
  accounting.last_window = ServiceWindowId::from_value(500);
  accounting.last_window_generation = ServiceWindowGeneration::from_value(2);
  accounting.writer_boot = BootId::from_value(0xABCDEF);
  SubjectAccounting entry;
  entry.id = SubjectId::from_value(1);
  entry.generation = SubjectGeneration::from_value(1);
  entry.cumulative_deficit = 12;
  entry.cumulative_surplus = 7;
  entry.discarded_deficit = 3;
  entry.discarded_surplus = 1;
  entry.unserved_streak = 4;
  entry.below_floor_streak = 2;
  entry.windows_observed = 9;
  entry.total_served_units = 1234;
  entry.last_correction_window = ServiceWindowId::from_value(499);
  entry.last_correction_generation = InterventionGeneration::from_value(3);
  accounting.subjects.push_back(entry);

  const ByteBuffer encoded = codec::encode_accounting(accounting);
  FairnessAccounting decoded;
  FG_CHECK_OK(codec::decode_accounting(encoded, decoded));
  FG_CHECK_EQ(decoded.generation, accounting.generation);
  FG_CHECK_EQ(decoded.subjects.size(), 1u);
  FG_CHECK(decoded.subjects[0] == accounting.subjects[0]);
  FG_CHECK_EQ(decoded.writer_boot.value(), accounting.writer_boot.value());
}

FG_TEST(codec, accounting_decoder_rejects_duplicates_and_bounds) {
  FairnessAccounting accounting;
  accounting.policy_id = FairnessPolicyId::from_value(1);
  SubjectAccounting entry;
  entry.id = SubjectId::from_value(1);
  entry.generation = SubjectGeneration::from_value(1);
  accounting.subjects.push_back(entry);
  accounting.subjects.push_back(entry);
  const ByteBuffer encoded = codec::encode_accounting(accounting);
  FairnessAccounting decoded;
  FG_CHECK_EQ(codec::decode_accounting(encoded, decoded).code(), StatusCode::Duplicate);

  accounting.subjects.pop_back();
  accounting.subjects[0].cumulative_deficit = kMaxCarryUnits + 1;
  const ByteBuffer oversized = codec::encode_accounting(accounting);
  FG_CHECK_EQ(codec::decode_accounting(oversized, decoded).code(), StatusCode::OutOfRange);
}

FG_TEST(codec, evidence_round_trips) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  evidence.producer.name = "publisher-a";
  evidence.producer.instance = 3;
  evidence.provenance.source = "collector";
  evidence.provenance.revision = 2;
  evidence.provenance.captured_at_ns = 123456;
  observe(evidence, 1, 1, 100, 0, 2);
  evidence.observations[0].priority.id = PriorityRef::from_value(5);
  evidence.observations[0].priority.generation = Generation<PriorityRefTag>::from_value(1);

  const ByteBuffer encoded = codec::encode_evidence(evidence);
  EvidenceSnapshot decoded;
  FG_CHECK_OK(codec::decode_evidence(encoded, decoded));
  FG_CHECK_EQ(decoded.observations.size(), 1u);
  FG_CHECK(decoded.observations[0] == evidence.observations[0]);
  FG_CHECK_EQ(codec::evidence_digest(decoded), codec::evidence_digest(evidence));
}

FG_TEST(codec, evidence_decoder_rejects_invalid_content) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  observe(evidence, 1, 1, 100, 3);  // served and unserved at once
  const ByteBuffer encoded = codec::encode_evidence(evidence);
  EvidenceSnapshot decoded;
  FG_CHECK_EQ(codec::decode_evidence(encoded, decoded).code(), StatusCode::EvidenceContradictory);
}

FG_TEST(codec, digests_change_with_content) {
  FairnessPolicy policy = flat_policy(2);
  const std::uint64_t first = codec::policy_digest(policy);
  policy.subjects[0].share_weight = 2;
  FG_CHECK(codec::policy_digest(policy) != first);
  FG_CHECK_EQ(codec::policy_digest(policy), codec::policy_digest(policy));
}
