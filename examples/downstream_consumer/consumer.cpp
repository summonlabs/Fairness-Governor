// Fairness Governor - independent downstream consumer.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This program only uses the public, installed interface. It exists to prove that
// a third party can consume the library through find_package, link it, and get
// the documented behaviour without touching the source tree.
#include <cstdio>
#include <memory>

#include "fairness_governor/fairness_governor.hpp"

int main() {
  using namespace fairness_governor;

  FairnessPolicy policy;
  policy.id = FairnessPolicyId::from_value(1);
  policy.generation = FairnessPolicyGeneration::from_value(1);
  policy.epoch = FabricEpoch::from_value(1);
  policy.fair_band_units = 8;
  policy.correction_threshold_units = 64;
  policy.max_correction_units = 100000;
  policy.max_correction_bps = 10000;
  policy.max_priority_modifier_bps = 1000;
  policy.max_evidence_age_windows = 4;

  FairnessGroup group;
  group.id = FairnessGroupId::from_value(1);
  group.generation = FairnessGroupGeneration::from_value(1);
  policy.groups.push_back(group);

  for (std::uint64_t i = 1; i <= 2; ++i) {
    Subject subject;
    subject.id = SubjectId::from_value(i);
    subject.generation = SubjectGeneration::from_value(1);
    subject.group = group.id;
    subject.share_weight = 1;
    subject.max_augment_units = 10000;
    subject.max_reduce_units = 10000;
    policy.subjects.push_back(subject);
  }
  policy.subjects[0].starvation_windows = 2;

  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  if (!opened.ok()) {
    std::printf("open failed: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  EvidenceSnapshot evidence;
  evidence.id = EvidenceSnapshotId::from_value(7);
  evidence.generation = EvidenceSnapshotGeneration::from_value(1);
  evidence.window = ServiceWindowId::from_value(100);
  evidence.window_generation = ServiceWindowGeneration::from_value(1);
  evidence.epoch = policy.epoch;
  evidence.sequence = 1;
  SubjectObservation first;
  first.id = SubjectId::from_value(1);
  first.generation = SubjectGeneration::from_value(1);
  first.served_units = 0;
  first.unserved_streak = 3;
  SubjectObservation second;
  second.id = SubjectId::from_value(2);
  second.generation = SubjectGeneration::from_value(1);
  second.served_units = 1000;
  evidence.observations.push_back(first);
  evidence.observations.push_back(second);

  if (!governor->ingest_evidence(evidence).ok()) {
    std::printf("ingest failed\n");
    return 1;
  }

  EvaluationRequest request;
  request.policy_id = policy.id;
  request.policy_generation = policy.generation;
  request.epoch = policy.epoch;
  request.evidence_id = evidence.id;
  request.evidence_generation = evidence.generation;
  request.window = evidence.window;
  request.window_generation = evidence.window_generation;

  Result<FairnessDecision> decision = governor->evaluate(request);
  if (!decision.ok()) {
    std::printf("evaluate failed: %s\n", decision.status().to_string().c_str());
    return 1;
  }
  if (decision.value().outcome != Outcome::StarvationRisk) {
    std::printf("unexpected outcome: %s\n", std::string(to_string(decision.value().outcome)).c_str());
    return 1;
  }
  if (decision.value().correction.augment_units != decision.value().correction.reduce_units) {
    std::printf("corrective plan does not conserve units\n");
    return 1;
  }
  if (!governor->commit(decision.value()).ok()) {
    std::printf("commit failed\n");
    return 1;
  }

  std::printf("consumer ok: %s\n", summarize(decision.value()).c_str());
  std::printf("library %s %s\n", std::string(library_name()).c_str(),
              std::string(version_string()).c_str());
  (void)governor->shutdown();
  return 0;
}
