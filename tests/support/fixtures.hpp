// Fairness Governor - shared test fixtures.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef FG_TEST_FIXTURES_HPP
#define FG_TEST_FIXTURES_HPP

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fairness_governor/fairness_governor.hpp"
#include "test_harness.hpp"

namespace fgtest {

using namespace fairness_governor;

/// A policy with the requested number of flat subjects under one root group.
[[nodiscard]] inline FairnessPolicy flat_policy(std::uint32_t subject_count,
                                                std::uint64_t epoch_value = 1) {
  FairnessPolicy policy;
  policy.id = FairnessPolicyId::from_value(1);
  policy.generation = FairnessPolicyGeneration::from_value(1);
  policy.epoch = FabricEpoch::from_value(epoch_value);
  policy.window_units = 1000;
  policy.fair_band_units = 8;
  policy.correction_threshold_units = 64;
  policy.cooldown_windows = 0;
  policy.max_correction_units = 1ULL << 30;
  policy.max_correction_bps = 10000;
  policy.max_priority_modifier_bps = 1000;
  policy.max_evidence_age_windows = 4;
  policy.max_evidence_age_ns = 0;
  policy.max_carry_units = 1ULL << 40;

  FairnessGroup group;
  group.id = FairnessGroupId::from_value(1);
  group.generation = FairnessGroupGeneration::from_value(1);
  group.share_weight = 1;
  policy.groups.push_back(group);

  for (std::uint32_t i = 0; i < subject_count; ++i) {
    Subject subject;
    subject.id = SubjectId::from_value(i + 1);
    subject.generation = SubjectGeneration::from_value(1);
    subject.group = group.id;
    subject.share_weight = 1;
    subject.max_reduce_units = 1ULL << 30;
    subject.max_augment_units = 1ULL << 30;
    policy.subjects.push_back(subject);
  }
  return policy;
}

/// A policy with one group, two subjects, and the classic starvation setup.
[[nodiscard]] inline FairnessPolicy starvation_policy() {
  FairnessPolicy policy = flat_policy(2);
  policy.subjects[0].guarantee_floor = 200;
  policy.subjects[0].starvation_windows = 3;
  policy.subjects[0].protected_obligation = true;
  policy.subjects[0].obligation_rank = 1;
  policy.subjects[1].share_weight = 3;
  policy.subjects[1].starvation_windows = 2;
  return policy;
}

[[nodiscard]] inline EvidenceSnapshot make_evidence(std::uint64_t window_value,
                                                    std::uint64_t epoch_value,
                                                    std::uint64_t snapshot_id = 10,
                                                    std::uint64_t snapshot_generation = 1) {
  EvidenceSnapshot evidence;
  evidence.id = EvidenceSnapshotId::from_value(snapshot_id);
  evidence.generation = EvidenceSnapshotGeneration::from_value(snapshot_generation);
  evidence.window = ServiceWindowId::from_value(window_value);
  evidence.window_generation = ServiceWindowGeneration::from_value(1);
  evidence.epoch = FabricEpoch::from_value(epoch_value);
  evidence.sequence = 1;
  evidence.producer.name = "fixture";
  evidence.producer.instance = 1;
  evidence.producer.version = 1;
  return evidence;
}

inline void observe(EvidenceSnapshot& evidence, std::uint64_t subject, std::uint64_t generation,
                    std::uint64_t served, std::uint32_t unserved = 0,
                    std::uint32_t below = 0) {
  SubjectObservation observation;
  observation.id = SubjectId::from_value(subject);
  observation.generation = SubjectGeneration::from_value(generation);
  observation.served_units = served;
  observation.unserved_streak = unserved;
  observation.below_floor_streak = below;
  evidence.observations.push_back(observation);
}

[[nodiscard]] inline EvaluationRequest make_request(const FairnessPolicy& policy,
                                                    const EvidenceSnapshot& evidence) {
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
  request.attempt = 0;
  return request;
}

/// Runs the pure engine with a fresh, empty accounting snapshot.
[[nodiscard]] inline Result<FairnessDecision> run_engine(const FairnessPolicy& policy,
                                                         const EvidenceSnapshot& evidence,
                                                         const EvaluationRequest& request,
                                                         ServiceWindowId current_window) {
  FairnessAccounting accounting;
  accounting.policy_id = policy.id;
  accounting.policy_generation = policy.generation;
  accounting.epoch = policy.epoch;
  EvaluationContext context;
  context.policy = &policy;
  context.accounting = &accounting;
  context.evidence = &evidence;
  context.request = request;
  context.current_epoch = policy.epoch;
  context.current_window = current_window;
  context.last_committed_window = ServiceWindowId::none();
  return evaluate_fairness(context);
}

[[nodiscard]] inline const SubjectFairnessState* find_state(const FairnessDecision& decision,
                                                            std::uint64_t subject) {
  for (const SubjectFairnessState& state : decision.subjects) {
    if (state.id == SubjectId::from_value(subject)) {
      return &state;
    }
  }
  return nullptr;
}

/// The canonical two-subject scenario used across several suites.
[[nodiscard]] inline std::string canonical_scenario() {
  return
      "version 1\n"
      "epoch 7\n"
      "policy 1 1 window_units=1000 fair_band=8 correction_threshold=64"
      " max_correction=100000 max_correction_bps=5000 max_modifier_bps=1000 max_age_windows=2\n"
      "group 1 1 weight=1\n"
      "subject 1 1 group=1 weight=1 floor=200 starvation=3 rank=1 protected=1"
      " reduce_cap=1000 augment_cap=1000 label=tenant-a\n"
      "subject 2 1 group=1 weight=3 starvation=2 reduce_cap=1000 augment_cap=1000"
      " label=tenant-b\n"
      "evidence 10 1 window=500 window_gen=1 epoch=7 sequence=1 producer=demo\n"
      "obs 1 1 served=0 unserved=3\n"
      "obs 2 1 served=800\n"
      "request policy=1:1 evidence=10:1 window=500:1 epoch=7\n";
}

/// Path of a built tool, resolved from the test environment.
[[nodiscard]] inline std::string tool_path(const std::string& name) {
  return env_or("FG_TOOL_DIR", "tools") + "/" + name + env_or("FG_EXE_SUFFIX", "");
}

/// Writes `text` to `path`, creating parent directories as needed.
[[nodiscard]] inline bool write_text(const std::string& path, const std::string& text) {
  return static_cast<bool>(std::ofstream(path, std::ios::binary) << text);
}

/// Waits until the child's output contains `marker`, returning the full output.
[[nodiscard]] inline std::string wait_for_marker(ChildProcess& child, const std::string& marker,
                                                 int max_millis) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_millis);
  for (;;) {
    const std::string output = child.drain_output();
    if (output.find(marker) != std::string::npos) {
      return output;
    }
    if (!child.running()) {
      return child.drain_output();
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return child.drain_output();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/// Extracts the value of `key` from a "key=value" token in `text`.
[[nodiscard]] inline std::string field_value(const std::string& text, const std::string& key) {
  const std::string needle = key + "=";
  std::size_t position = text.find(needle);
  if (position == std::string::npos) {
    return {};
  }
  position += needle.size();
  std::size_t end = position;
  while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\r') {
    ++end;
  }
  return text.substr(position, end - position);
}

}  // namespace fgtest

#endif  // FG_TEST_FIXTURES_HPP
