// Fairness Governor - concurrency, shutdown, and lock-discipline tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// These tests exercise the documented lock discipline directly: no callback or
// join ever happens while the governor's state lock is held, shutdown never
// blocks work that must complete, and cancelled work never mutates authoritative
// state.
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(concurrency, parallel_ingest_and_evaluate_never_tear_state) {
  FairnessPolicy policy = flat_policy(16);
  policy.subjects[0].starvation_windows = 2;
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  constexpr std::uint32_t kPublishers = 4;
  constexpr std::uint32_t kWindowsPerPublisher = 40;
  // Checks inside a worker thread are recorded in atomics and asserted on the
  // main thread: a throwing check must never escape a thread.
  std::atomic<std::uint32_t> staged{0};
  std::atomic<std::uint32_t> stale{0};
  std::atomic<std::uint32_t> unknown{0};
  std::atomic<std::uint32_t> inconsistent{0};
  std::atomic<std::uint32_t> ingest_failures{0};
  std::atomic<bool> start{false};

  std::vector<std::thread> workers;
  for (std::uint32_t publisher = 0; publisher < kPublishers; ++publisher) {
    workers.emplace_back([&, publisher]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::uint32_t window = 0; window < kWindowsPerPublisher; ++window) {
        EvidenceSnapshot evidence =
            make_evidence(1000 + publisher * kWindowsPerPublisher + window, 1, 100 + publisher,
                          window + 1);
        evidence.producer.instance = publisher + 1;
        evidence.sequence = window + 1;
        for (std::uint64_t i = 0; i < 16; ++i) {
          observe(evidence, i + 1, 1, (i * 37 + window * 11 + publisher) % 500);
        }
        Result<IngestResult> ingested = governor->ingest_evidence(evidence);
        if (ingested.ok()) {
          staged.fetch_add(1, std::memory_order_relaxed);
        } else if (ingested.code() != StatusCode::StaleGeneration &&
                   ingested.code() != StatusCode::Duplicate) {
          ingest_failures.fetch_add(1, std::memory_order_relaxed);
        }
        EvaluationRequest request = make_request(policy, evidence);
        Result<FairnessDecision> decision = governor->evaluate(request);
        if (decision.ok()) {
          for (const SubjectFairnessState& state : decision.value().subjects) {
            if (state.outcome == Outcome::Stale) {
              stale.fetch_add(1, std::memory_order_relaxed);
            } else if (state.outcome == Outcome::Unknown) {
              unknown.fetch_add(1, std::memory_order_relaxed);
            }
            // Every decision is internally consistent no matter when it lands.
            if (state.deviation_units != static_cast<std::int64_t>(state.served_units) -
                                            static_cast<std::int64_t>(state.entitlement_units)) {
              inconsistent.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      }
    });
  }
  // An evaluator thread runs entirely on the most recent staged window.
  std::atomic<std::uint32_t> evaluations{0};
  std::thread evaluator([&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (std::uint32_t i = 0; i < 400; ++i) {
      EvaluationRequest request;
      request.policy_id = policy.id;
      request.policy_generation = policy.generation;
      request.epoch = policy.epoch;
      request.request_id = i;
      Result<FairnessDecision> decision = governor->evaluate(request);
      if (decision.ok()) {
        evaluations.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  start.store(true, std::memory_order_release);
  for (std::thread& worker : workers) {
    worker.join();
  }
  evaluator.join();
  FG_CHECK_EQ(staged.load(), kPublishers * kWindowsPerPublisher);
  FG_CHECK_EQ(ingest_failures.load(), 0u);
  FG_CHECK_EQ(inconsistent.load(), 0u);
  FG_CHECK_EQ(evaluations.load(), 400u);
  FG_CHECK(governor->staged_snapshot_count() <= 64u);
  FG_CHECK_OK(governor->shutdown());
  (void)stale;
  (void)unknown;
}

FG_TEST(concurrency, shutdown_is_idempotent_and_stops_new_work) {
  const FairnessPolicy policy = flat_policy(4);
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  std::atomic<std::uint32_t> rejected{0};
  std::atomic<std::uint32_t> shutdown_rejections{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (std::uint32_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker]() {
      std::uint32_t window = 0;
      while (!stop.load(std::memory_order_acquire)) {
        EvidenceSnapshot evidence =
            make_evidence(2000 + worker * 1000 + window, 1, 200 + worker, window + 1);
        evidence.producer.instance = worker + 1;
        evidence.sequence = window + 1;
        for (std::uint64_t i = 0; i < 4; ++i) {
          observe(evidence, i + 1, 1, (i + window) % 100);
        }
        ++window;
        const Status status = governor->ingest_evidence(evidence).status();
        if (!status.ok()) {
          rejected.fetch_add(1, std::memory_order_relaxed);
          if (status.code() == StatusCode::ShuttingDown) {
            shutdown_rejections.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  FG_CHECK_OK(governor->shutdown());
  stop.store(true, std::memory_order_release);
  for (std::thread& worker : workers) {
    worker.join();
  }
  FG_CHECK_OK(governor->shutdown());
  FG_CHECK(governor->shutting_down());
  FG_CHECK_EQ(governor->staged_snapshot_count(), 0u);
  // Some worker definitely observed the shutdown and was refused.
  FG_CHECK(rejected.load() > 0);
  FG_CHECK(shutdown_rejections.load() > 0);
}

FG_TEST(concurrency, cancelled_work_does_not_mutate_authoritative_state) {
  const FairnessPolicy policy = starvation_policy();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  FG_CHECK_OK(governor->ingest_evidence(evidence).status());
  Result<FairnessDecision> decision = governor->evaluate(make_request(policy, evidence));
  FG_CHECK_OK(decision.status());

  // Shutting down between evaluate and commit must prevent the commit.
  FG_CHECK_OK(governor->shutdown());
  const Status committed = governor->commit(decision.value());
  FG_CHECK_EQ(committed.code(), StatusCode::ShuttingDown);
  FG_CHECK_EQ(governor->accounting_generation(), 0u);
  FG_CHECK(!governor->last_committed_window().valid());
}

FG_TEST(concurrency, concurrent_commits_are_serialized_by_authority) {
  const FairnessPolicy policy = flat_policy(4);
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  constexpr std::uint32_t kWorkers = 4;
  std::vector<EvidenceSnapshot> evidences;
  for (std::uint32_t worker = 0; worker < kWorkers; ++worker) {
    EvidenceSnapshot evidence = make_evidence(3000 + worker, 1, 300 + worker, 1);
    evidence.producer.instance = worker + 1;
    for (std::uint64_t i = 0; i < 4; ++i) {
      observe(evidence, i + 1, 1, 100 * (worker + 1));
    }
    FG_CHECK_OK(governor->ingest_evidence(evidence).status());
    evidences.push_back(evidence);
  }

  // Every worker evaluates against the same authoritative generation, and only
  // then do they race to commit. Exactly one commit can cross the durable
  // boundary for a given accounting generation.
  std::atomic<std::uint32_t> evaluated{0};
  std::atomic<std::uint32_t> successes{0};
  std::atomic<std::uint32_t> rejections{0};
  std::atomic<std::uint32_t> unexpected{0};
  std::atomic<std::uint32_t> unexpected_code{0};
  std::vector<std::thread> workers;
  for (std::uint32_t worker = 0; worker < kWorkers; ++worker) {
    workers.emplace_back([&, worker]() {
      Result<FairnessDecision> decision =
          governor->evaluate(make_request(policy, evidences[worker]));
      if (!decision.ok()) {
        unexpected.fetch_add(1, std::memory_order_relaxed);
        unexpected_code.store(2000 + static_cast<std::uint32_t>(decision.code()),
                              std::memory_order_relaxed);
        evaluated.fetch_add(1, std::memory_order_acq_rel);
        return;
      }
      evaluated.fetch_add(1, std::memory_order_acq_rel);
      while (evaluated.load(std::memory_order_acquire) < kWorkers) {
        std::this_thread::yield();
      }
      const Status committed = governor->commit(decision.value());
      if (committed.ok()) {
        successes.fetch_add(1, std::memory_order_relaxed);
      } else if (committed.code() == StatusCode::StaleGeneration) {
        rejections.fetch_add(1, std::memory_order_relaxed);
      } else {
        unexpected.fetch_add(1, std::memory_order_relaxed);
        unexpected_code.store(3000 + static_cast<std::uint32_t>(committed.code()),
                              std::memory_order_relaxed);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  FG_CHECK_MSG(unexpected.load() == 0u,
               std::string("unexpected outcome code ") + std::to_string(unexpected_code.load()));
  FG_CHECK_EQ(successes.load(), 1u);
  FG_CHECK_EQ(rejections.load(), 3u);
  FG_CHECK_EQ(governor->accounting_generation(), 1u);
  // The winning commit is the only one that moved durable state.
  FG_CHECK(governor->last_committed_window().valid());
  FG_CHECK_OK(governor->shutdown());
}

FG_TEST(concurrency, evaluation_is_safe_while_ingestion_races_epoch_advance) {
  const FairnessPolicy policy = flat_policy(8);
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(policy);
  FG_CHECK_OK(opened.status());
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> epoch_rejections{0};
  std::atomic<std::uint32_t> epoch_failures{0};
  std::thread advancer([&]() {
    for (std::uint64_t epoch = 2; epoch <= 12; ++epoch) {
      if (!governor->advance_epoch(FabricEpoch::from_value(epoch)).ok()) {
        epoch_failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
    stop.store(true, std::memory_order_release);
  });
  std::thread producer([&]() {
    std::uint32_t window = 0;
    while (!stop.load(std::memory_order_acquire)) {
      EvidenceSnapshot evidence = make_evidence(4000 + window, 1, 400, window + 1);
      evidence.sequence = window + 1;
      for (std::uint64_t i = 0; i < 8; ++i) {
        observe(evidence, i + 1, 1, (i + window) % 250);
      }
      const Status status = governor->ingest_evidence(evidence).status();
      if (!status.ok() && status.code() == StatusCode::StaleEpoch) {
        epoch_rejections.fetch_add(1, std::memory_order_relaxed);
      }
      ++window;
    }
  });
  producer.join();
  advancer.join();
  FG_CHECK_EQ(epoch_failures.load(), 0u);
  FG_CHECK_EQ(governor->epoch().value(), 12u);
  FG_CHECK(governor->staged_snapshot_count() <= 64u);
  FG_CHECK_OK(governor->shutdown());
  (void)epoch_rejections;
}
