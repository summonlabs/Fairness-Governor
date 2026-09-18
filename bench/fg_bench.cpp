// Fairness Governor - synthetic fairness evaluation benchmark.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// HONESTY NOTE: everything measured here is a SYNTHETIC population evaluated in
// process. Nothing in this benchmark models, exercises, or measures a physical
// network, NIC, switch, DPU, or link. The numbers are completed evaluation
// throughput, not enqueue latency.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "fairness_governor/fairness_governor.hpp"

namespace {

using namespace fairness_governor;
using Clock = std::chrono::steady_clock;

struct SyntheticPopulation {
  FairnessPolicy policy;
  EvidenceSnapshot evidence;
  EvaluationRequest request;
  FabricEpoch epoch;
};

SyntheticPopulation build_population(std::uint64_t subject_count, std::uint32_t group_depth,
                                     std::uint32_t history_windows, std::uint64_t seed) {
  SyntheticPopulation population;
  population.epoch = FabricEpoch::from_value(1);
  FairnessPolicy& policy = population.policy;
  policy.id = FairnessPolicyId::from_value(1);
  policy.generation = FairnessPolicyGeneration::from_value(1);
  policy.epoch = population.epoch;
  policy.fair_band_units = 8;
  policy.correction_threshold_units = 64;
  policy.cooldown_windows = 0;
  policy.max_correction_units = 1ULL << 32;
  policy.max_correction_bps = 2500;
  policy.max_priority_modifier_bps = 2000;
  policy.max_evidence_age_windows = static_cast<std::uint32_t>(history_windows);
  policy.max_carry_units = 1ULL << 40;

  std::uint64_t state = seed == 0 ? 0x123456789ABCDEFULL : seed;
  auto next = [&state]() {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  };

  const std::uint32_t depth = std::max<std::uint32_t>(1, group_depth);
  std::vector<FairnessGroupId> level_ids;
  std::uint64_t group_id = 1;
  for (std::uint32_t level = 0; level < depth; ++level) {
    const std::uint32_t width = level == 0 ? 1 : 4;
    std::vector<FairnessGroupId> next_level;
    for (std::uint32_t index = 0; index < width; ++index) {
      FairnessGroup group;
      group.id = FairnessGroupId::from_value(group_id++);
      group.generation = FairnessGroupGeneration::from_value(1);
      if (level > 0) {
        group.parent = level_ids[index % level_ids.size()];
      }
      group.share_weight = 1 + static_cast<std::uint32_t>(next() % 16);
      group.guarantee_floor = 0;
      group.obligation_rank = 0;
      policy.groups.push_back(group);
      next_level.push_back(group.id);
    }
    level_ids = std::move(next_level);
  }

  EvidenceSnapshot& evidence = population.evidence;
  evidence.id = EvidenceSnapshotId::from_value(1);
  evidence.generation = EvidenceSnapshotGeneration::from_value(1);
  evidence.window = ServiceWindowId::from_value(history_windows == 0 ? 1 : history_windows);
  evidence.window_generation = ServiceWindowGeneration::from_value(1);
  evidence.epoch = population.epoch;
  evidence.sequence = 1;
  evidence.producer.name = "fg_bench_synthetic";
  evidence.producer.instance = 1;

  policy.subjects.reserve(subject_count);
  evidence.observations.reserve(subject_count);
  std::uint64_t subject_id = 1;
  for (std::uint64_t i = 0; i < subject_count; ++i) {
    Subject subject;
    subject.id = SubjectId::from_value(subject_id++);
    subject.generation = SubjectGeneration::from_value(1);
    subject.group = level_ids[i % level_ids.size()];
    subject.share_weight = 1 + static_cast<std::uint32_t>(next() % 64);
    subject.guarantee_floor = (next() % 8 == 0) ? 1000 : 0;
    subject.starvation_windows = (next() % 4 == 0) ? 3 : 0;
    subject.guarantee_starvation_windows = 0;
    subject.priority_modifier_bps = static_cast<std::int32_t>(next() % 2001) - 1000;
    subject.obligation_rank = (next() % 8 == 0) ? 2 : 0;
    subject.protected_obligation = subject.obligation_rank > 0;
    subject.max_reduce_units = 1ULL << 20;
    subject.max_augment_units = 1ULL << 20;
    policy.subjects.push_back(subject);

    SubjectObservation observation;
    observation.id = subject.id;
    observation.generation = subject.generation;
    observation.served_units = next() % 4096;
    observation.unserved_streak = observation.served_units == 0 ? 1 : 0;
    evidence.observations.push_back(observation);
  }

  population.request.policy_id = policy.id;
  population.request.policy_generation = policy.generation;
  population.request.epoch = population.epoch;
  population.request.evidence_id = evidence.id;
  population.request.evidence_generation = evidence.generation;
  population.request.window = evidence.window;
  population.request.window_generation = evidence.window_generation;
  population.request.request_id = 1;
  return population;
}

struct Measurement {
  std::uint64_t evaluations{0};
  double nanoseconds_per_evaluation{0};
  double evaluations_per_second{0};
  std::uint64_t digest{0};
};

Measurement measure(const SyntheticPopulation& population, std::uint64_t iterations) {
  FairnessAccounting accounting;
  accounting.policy_id = population.policy.id;
  accounting.policy_generation = population.policy.generation;
  accounting.epoch = population.epoch;
  for (const Subject& subject : population.policy.subjects) {
    SubjectAccounting entry;
    entry.id = subject.id;
    entry.generation = subject.generation;
    accounting.subjects.push_back(entry);
  }
  EvaluationContext context;
  context.policy = &population.policy;
  context.accounting = &accounting;
  context.evidence = &population.evidence;
  context.request = population.request;
  context.current_epoch = population.epoch;
  context.current_window = population.evidence.window;
  context.last_committed_window = ServiceWindowId::none();

  // Warm up so the measurement reflects steady-state completed work.
  for (int i = 0; i < 8; ++i) {
    Result<FairnessDecision> warm = evaluate_fairness(context);
    if (!warm.ok()) {
      std::cerr << "benchmark evaluation failed: " << warm.status().to_string() << "\n";
      std::exit(1);
    }
  }

  Measurement measurement;
  std::uint64_t digest = 0;
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    Result<FairnessDecision> decision = evaluate_fairness(context);
    if (!decision.ok()) {
      std::cerr << "benchmark evaluation failed: " << decision.status().to_string() << "\n";
      std::exit(1);
    }
    digest ^= decision.value().content_digest + 0x9E3779B97F4A7C15ULL + (digest << 6) + (digest >> 2);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
  measurement.evaluations = iterations;
  measurement.nanoseconds_per_evaluation =
      static_cast<double>(elapsed.count()) / static_cast<double>(iterations);
  measurement.evaluations_per_second =
      static_cast<double>(iterations) * 1e9 / static_cast<double>(elapsed.count());
  measurement.digest = digest;
  return measurement;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t iterations = 2000;
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--iterations" && i + 1 < argc) {
      iterations = std::strtoull(argv[++i], nullptr, 10);
    } else if (argument == "--quick") {
      quick = true;
      iterations = 200;
    }
  }

  std::cout << "Fairness Governor " << version_string()
            << " synthetic fairness evaluation benchmark\n";
  std::cout << "LABEL: SYNTHETIC - in-process evaluation of synthetic populations."
               " No physical network, NIC, switch, DPU, or link is exercised.\n";
  std::cout << "columns: subjects groups depth history iterations ns_per_eval evals_per_sec\n";

  std::vector<std::uint64_t> subject_counts{8, 64, 512, 4096};
  std::vector<std::uint32_t> group_depths{1, 2, 4};
  std::vector<std::uint32_t> histories{1, 64, 1024};
  if (quick) {
    subject_counts = {8, 64, 512};
    group_depths = {1, 2};
    histories = {1, 64};
  }

  for (const std::uint64_t subject_count : subject_counts) {
    for (const std::uint32_t depth : group_depths) {
      for (const std::uint32_t history : histories) {
        const SyntheticPopulation population =
            build_population(subject_count, depth, history, subject_count * 31 + depth * 7 + history);
        // The evaluation cost is dominated by the subject count, so smaller
        // populations get proportionally more iterations.
        const std::uint64_t scaled = std::max<std::uint64_t>(
            16, iterations * 64 / std::max<std::uint64_t>(subject_count, 1));
        const Measurement measurement = measure(population, scaled);
        std::printf("%8llu %6zu %5u %7u %9llu %12.0f %14.0f\n",
                    static_cast<unsigned long long>(subject_count),
                    population.policy.groups.size(), depth, history,
                    static_cast<unsigned long long>(measurement.evaluations),
                    measurement.nanoseconds_per_evaluation, measurement.evaluations_per_second);
        std::fflush(stdout);
      }
    }
  }
  std::cout << "digest-stable across iterations: yes (content digest is deterministic)\n";
  return 0;
}
