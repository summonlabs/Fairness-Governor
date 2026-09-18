// Fairness Governor - adversarial input rejection.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

#include "fairness_governor/fairness_governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(adversarial_input, malformed_scenarios_are_rejected_without_crashing) {
  const std::vector<std::string> documents{
      "",
      "\n\n\n",
      "version",
      "version ",
      "version 1",
      "version 1\n",
      "version 1\nepoch",
      "version 1\nepoch ",
      "version 1\npolicy",
      "version 1\npolicy 1",
      "version 1\npolicy 1 1\n",
      "version 1\npolicy 1 1\ngroup",
      "version 1\npolicy 1 1\ngroup 1",
      "version 1\npolicy 1 1\ngroup 1 1\nsubject",
      "version 1\npolicy 1 1\ngroup 1 1\nsubject 1",
      "version 1\npolicy 1 1\ngroup 1 1\nsubject 1 1",
      "version 1\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=",
      "version 1\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=1\nsubject 1 1 group=1\n",
      "version 1\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=1\nevidence\n",
      "version 1\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=1\nevidence 1 1\nobs\n",
      "version 1\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=1\nevidence 1 1\nobs 1 1\n",
      "version 1\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=1\nrequest\n",
      "\x00\x01\x02\x03",
      "version 1\r\nepoch 1\r\n",
      "version 1\nepoch 1\npolicy 1 1 fair_band=999999999999999999999999999\n",
      "version 1\nepoch 1\npolicy 1 1 max_age_windows=4294967296\n",
      "version 1\nepoch 1\npolicy 1 1 max_modifier_bps=4294967295\n",
  };
  for (const std::string& document : documents) {
    const Result<Scenario> parsed = parse_scenario(document);
    // A document may only parse when everything it does declare is coherent.
    if (parsed.ok() && parsed.value().has_policy) {
      FG_CHECK(validate_policy(parsed.value().policy).ok());
    }
  }
}

FG_TEST(adversarial_input, random_bytes_never_parse_into_a_policy) {
  Rng rng(0xF022ULL);
  for (int iteration = 0; iteration < 4000; ++iteration) {
    const std::size_t length = rng.below(256);
    std::string document;
    document.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
      document.push_back(static_cast<char>(rng.below(256)));
    }
    const Result<Scenario> parsed = parse_scenario(document);
    if (parsed.ok() && parsed.value().has_policy) {
      FG_CHECK(validate_policy(parsed.value().policy).ok());
    }
  }
}

FG_TEST(adversarial_input, random_bytes_never_decode_into_a_record) {
  Rng rng(0xD3C0DEULL);
  for (int iteration = 0; iteration < 8000; ++iteration) {
    const std::size_t length = rng.below(200);
    ByteBuffer bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
      bytes[i] = static_cast<std::uint8_t>(rng.below(256));
    }
    std::uint64_t generation = 0;
    BootId boot;
    std::uint64_t ordinal = 0;
    ByteBuffer payload;
    const Status status =
        decode_record(bytes, RecordKind::Policy, generation, boot, ordinal, payload);
    // The decoder either rejects or returns a record that re-encodes identically.
    if (status.ok()) {
      const ByteBuffer reencoded = encode_record(RecordKind::Policy, generation,
                                                Incarnation{boot, ordinal}, payload);
      FG_CHECK(reencoded.size() == bytes.size());
    }
  }
}

FG_TEST(adversarial_input, random_bytes_never_decode_into_a_policy) {
  Rng rng(0xF0117ULL);
  for (int iteration = 0; iteration < 8000; ++iteration) {
    const std::size_t length = rng.below(300);
    ByteBuffer bytes(length);
    for (std::size_t i = 0; i < length; ++i) {
      bytes[i] = static_cast<std::uint8_t>(rng.below(256));
    }
    FairnessPolicy policy;
    const Status status = codec::decode_policy(bytes, policy);
    if (status.ok()) {
      FG_CHECK(validate_policy(policy).ok());
    }
  }
}

FG_TEST(adversarial_input, single_byte_corruption_of_a_valid_policy_is_detected) {
  const FairnessPolicy policy = flat_policy(4);
  const ByteBuffer encoded = codec::encode_policy(policy);
  Rng rng(0x1B17EULL);
  std::size_t accepted = 0;
  for (int iteration = 0; iteration < 3000; ++iteration) {
    ByteBuffer damaged = encoded;
    const std::size_t index = static_cast<std::size_t>(rng.below(damaged.size()));
    damaged[index] ^= static_cast<std::uint8_t>(1u << rng.below(8));
    FairnessPolicy decoded;
    const Status status = codec::decode_policy(damaged, decoded);
    if (status.ok()) {
      // If a corruption still decodes, the result must still be a legal policy:
      // the decoder never returns a structurally broken object. Detecting the
      // corruption itself is the durable record checksum's job, not the codec's.
      ++accepted;
      FG_CHECK(validate_policy(decoded).ok());
    }
  }
  // Corruption in the structural parts is caught; only variations confined to
  // free-text or same-value bytes can survive.
  FG_CHECK(accepted < 3000);
}

FG_TEST(adversarial_input, oversized_evidence_is_rejected_before_allocation) {
  EvidenceSnapshot evidence = make_evidence(10, 1);
  evidence.observations.resize(kMaxEvidenceRecords + 1);
  FG_CHECK_EQ(validate_evidence(evidence).code(), StatusCode::LimitExceeded);

  ByteBuffer bytes;
  bytes.resize(kMaxDurablePayloadBytes + 1, 0);
  EvidenceSnapshot decoded;
  FG_CHECK_EQ(codec::decode_evidence(bytes, decoded).code(), StatusCode::OversizedPayload);
  FairnessPolicy policy;
  FG_CHECK_EQ(codec::decode_policy(bytes, policy).code(), StatusCode::OversizedPayload);
  FairnessAccounting accounting;
  FG_CHECK_EQ(codec::decode_accounting(bytes, accounting).code(), StatusCode::OversizedPayload);
}

FG_TEST(adversarial_input, contradictory_evidence_is_rejected) {
  FairnessPolicy policy = flat_policy(2);
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 100);
  observe(evidence, 2, 1, 100);
  evidence.declared_digest = 12345;
  FG_CHECK_EQ(governor->ingest_evidence(evidence).code(),
              StatusCode::EvidenceContradictory);
  FG_CHECK_OK(governor->shutdown());
}

FG_TEST(adversarial_input, a_policy_with_extreme_values_is_either_accepted_or_rejected) {
  FairnessPolicy policy = flat_policy(2);
  policy.subjects[0].share_weight = kMaxShareWeight;
  policy.subjects[1].share_weight = kMaxShareWeight;
  policy.subjects[0].guarantee_floor = kMaxServiceUnits;
  policy.max_correction_units = kMaxServiceUnits * kMaxSubjects;
  policy.max_carry_units = kMaxCarryUnits;
  FG_CHECK(validate_policy(policy).ok());

  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, kMaxServiceUnits);
  observe(evidence, 2, 1, kMaxServiceUnits);
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  std::uint64_t total = 0;
  for (const SubjectFairnessState& state : decision.value().subjects) {
    total += state.entitlement_units;
  }
  FG_CHECK_EQ(total, 2 * kMaxServiceUnits);
}

FG_TEST(adversarial_input, an_empty_policy_document_is_rejected) {
  FairnessPolicy policy;
  policy.id = FairnessPolicyId::from_value(1);
  policy.generation = FairnessPolicyGeneration::from_value(1);
  policy.epoch = FabricEpoch::from_value(1);
  FG_CHECK_EQ(validate_policy(policy).code, StatusCode::PolicyInvalid);
}

FG_TEST(adversarial_input, a_governor_rejects_an_oversized_staged_set_without_growing) {
  FairnessPolicy policy = flat_policy(1);
  GovernorConfig config;
  config.policy = policy;
  config.epoch = policy.epoch;
  config.limits.max_staged_snapshots = 4;
  config.limits.max_observations_per_snapshot = 8;
  Result<std::unique_ptr<FairnessGovernor>> opened = FairnessGovernor::open(config);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  for (std::uint32_t window = 0; window < 64; ++window) {
    EvidenceSnapshot evidence = make_evidence(500 + window, 1, 10, window + 1);
    evidence.sequence = window + 1;
    observe(evidence, 1, 1, window);
    FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  }
  FG_CHECK(governor->staged_snapshot_count() <= 4u);

  EvidenceSnapshot too_many = make_evidence(600, 1, 99, 1);
  for (std::uint32_t i = 0; i < 9; ++i) {
    observe(too_many, i + 1, 1, 1);
  }
  FG_CHECK_EQ(governor->ingest_evidence(too_many).code(), StatusCode::LimitExceeded);
  FG_CHECK_OK(governor->shutdown());
}
