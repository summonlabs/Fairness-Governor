// Fairness Governor - minimal end-to-end example.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Builds a two-tenant policy, feeds one service window of evidence, evaluates
// fairness, explains it, and durably commits the accounting.
#include <iostream>
#include <memory>
#include <string>

#include "fairness_governor/fairness_governor.hpp"

int main(int argc, char** argv) {
  using namespace fairness_governor;
  const std::string store_path = argc > 1 ? argv[1] : std::string();

  FairnessPolicy policy;
  policy.id = FairnessPolicyId::from_value(1);
  policy.generation = FairnessPolicyGeneration::from_value(1);
  policy.epoch = FabricEpoch::from_value(1);
  policy.fair_band_units = 4;
  policy.correction_threshold_units = 32;
  policy.max_correction_units = 100000;
  policy.max_correction_bps = 5000;
  policy.max_priority_modifier_bps = 1000;
  policy.max_evidence_age_windows = 2;

  FairnessGroup group;
  group.id = FairnessGroupId::from_value(1);
  group.generation = FairnessGroupGeneration::from_value(1);
  group.share_weight = 1;
  policy.groups.push_back(group);

  Subject tenant_a;
  tenant_a.id = SubjectId::from_value(1);
  tenant_a.generation = SubjectGeneration::from_value(1);
  tenant_a.group = group.id;
  tenant_a.share_weight = 1;
  tenant_a.guarantee_floor = 200;
  tenant_a.starvation_windows = 3;
  tenant_a.protected_obligation = true;
  tenant_a.obligation_rank = 1;
  tenant_a.max_reduce_units = 1000;
  tenant_a.max_augment_units = 1000;
  tenant_a.label = "tenant-a";

  Subject tenant_b;
  tenant_b.id = SubjectId::from_value(2);
  tenant_b.generation = SubjectGeneration::from_value(1);
  tenant_b.group = group.id;
  tenant_b.share_weight = 3;
  tenant_b.starvation_windows = 2;
  tenant_b.max_reduce_units = 1000;
  tenant_b.max_augment_units = 1000;
  tenant_b.label = "tenant-b";
  policy.subjects.push_back(tenant_a);
  policy.subjects.push_back(tenant_b);

  GovernorConfig config;
  config.policy = policy;
  config.epoch = policy.epoch;
  config.store_path = store_path;
  Result<std::unique_ptr<FairnessGovernor>> opened = FairnessGovernor::open(config);
  if (!opened.ok()) {
    std::cerr << "open failed: " << opened.status().to_string() << "\n";
    return 1;
  }
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  EvidenceSnapshot evidence;
  evidence.id = EvidenceSnapshotId::from_value(10);
  evidence.generation = EvidenceSnapshotGeneration::from_value(1);
  evidence.window = ServiceWindowId::from_value(500);
  evidence.window_generation = ServiceWindowGeneration::from_value(1);
  evidence.epoch = policy.epoch;
  evidence.sequence = 1;
  evidence.producer.name = "example";
  evidence.producer.instance = 1;
  SubjectObservation first;
  first.id = tenant_a.id;
  first.generation = tenant_a.generation;
  first.served_units = 0;
  first.unserved_streak = 3;
  SubjectObservation second;
  second.id = tenant_b.id;
  second.generation = tenant_b.generation;
  second.served_units = 800;
  evidence.observations.push_back(first);
  evidence.observations.push_back(second);

  Result<IngestResult> ingested = governor->ingest_evidence(evidence);
  if (!ingested.ok()) {
    std::cerr << "ingest failed: " << ingested.status().to_string() << "\n";
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
  request.now_ns = evidence.captured_at_ns;
  request.request_id = 1;

  Result<FairnessDecision> decision = governor->evaluate(request);
  if (!decision.ok()) {
    std::cerr << "evaluate failed: " << decision.status().to_string() << "\n";
    return 1;
  }
  std::cout << explain(decision.value());
  const Status committed = governor->commit(decision.value());
  if (!committed.ok()) {
    std::cerr << "commit failed: " << committed.to_string() << "\n";
    return 1;
  }
  std::cout << "committed accounting generation " << governor->accounting_generation() << "\n";
  (void)governor->shutdown();
  return 0;
}
