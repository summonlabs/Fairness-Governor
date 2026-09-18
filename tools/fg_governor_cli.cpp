// Fairness Governor - command line entry point.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "fairness_governor/fairness_governor.hpp"

namespace {

using namespace fairness_governor;

void print_usage() {
  std::cout <<
      "Fairness Governor " << version_string() << "\n"
      "usage:\n"
      "  fg_governor_cli version\n"
      "  fg_governor_cli check <scenario>\n"
      "  fg_governor_cli evaluate <scenario> [options]\n"
      "\n"
      "options:\n"
      "  --store <dir>       enable durable policy and accounting state\n"
      "  --commit            durably commit the decision after evaluating it\n"
      "  --result            print the machine-readable result block\n"
      "  --latest            synthesize the request from the newest evidence\n"
      "  --recovery          print the recovery report\n"
      "  --quiet             print only the summary line\n";
}

[[nodiscard]] bool read_file(const std::string& path, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out = buffer.str();
  return true;
}

[[nodiscard]] EvaluationRequest latest_request(const Scenario& scenario) {
  EvaluationRequest request;
  request.policy_id = scenario.policy.id;
  request.policy_generation = scenario.policy.generation;
  request.epoch = scenario.epoch;
  if (!scenario.evidence.empty()) {
    const EvidenceSnapshot& newest = scenario.evidence.back();
    request.evidence_id = newest.id;
    request.evidence_generation = newest.generation;
    request.window = newest.window;
    request.window_generation = newest.window_generation;
    request.now_ns = newest.captured_at_ns;
  }
  request.request_id = 1;
  request.attempt = 0;
  return request;
}

void print_recovery(const RecoveryReport& report) {
  std::cout << "recovery store=" << (report.store_present ? 1 : 0)
            << " policy_loaded=" << (report.policy_loaded ? 1 : 0)
            << " accounting_loaded=" << (report.accounting_loaded ? 1 : 0)
            << " foreign_incarnation=" << (report.foreign_incarnation ? 1 : 0)
            << " unfinished_attempt_discarded=" << (report.unfinished_attempt_discarded ? 1 : 0)
            << " backup_recovered=" << (report.backup_recovered ? 1 : 0)
            << " corruption_detected=" << (report.corruption_detected ? 1 : 0)
            << " evidence_authority_restored=" << (report.evidence_authority_restored ? 1 : 0)
            << " publisher_authority_restored=" << (report.publisher_authority_restored ? 1 : 0)
            << " accounting_generation=" << report.loaded_accounting_generation << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }
  if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h") {
    print_usage();
    return 0;
  }
  if (arguments[0] == "version") {
    std::cout << library_name() << " " << version_string() << " by " << vendor_name() << "\n";
    return 0;
  }
  const std::string command = arguments[0];
  if (command != "check" && command != "evaluate") {
    std::cerr << "unknown command: " << command << "\n";
    print_usage();
    return 2;
  }
  if (arguments.size() < 2) {
    std::cerr << "a scenario file is required\n";
    return 2;
  }
  const std::string scenario_path = arguments[1];
  bool commit = false;
  bool result = false;
  bool latest = false;
  bool recovery = false;
  bool quiet = false;
  std::string store_path;
  for (std::size_t i = 2; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    if (argument == "--commit") {
      commit = true;
    } else if (argument == "--result") {
      result = true;
    } else if (argument == "--latest") {
      latest = true;
    } else if (argument == "--recovery") {
      recovery = true;
    } else if (argument == "--quiet") {
      quiet = true;
    } else if (argument == "--store") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "--store requires a directory\n";
        return 2;
      }
      store_path = arguments[++i];
    } else {
      std::cerr << "unknown option: " << argument << "\n";
      return 2;
    }
  }

  std::string text;
  if (!read_file(scenario_path, text)) {
    std::cerr << "cannot read scenario: " << scenario_path << "\n";
    return 2;
  }
  Result<Scenario> parsed = parse_scenario(text);
  if (!parsed.ok()) {
    std::cerr << "scenario rejected: " << parsed.status().to_string() << "\n";
    return 2;
  }
  Scenario& scenario = parsed.value();
  if (!scenario.has_policy) {
    std::cerr << "scenario rejected: no policy directive\n";
    return 2;
  }
  if (command == "check") {
    std::cout << "scenario ok subjects=" << scenario.policy.subjects.size()
              << " groups=" << scenario.policy.groups.size()
              << " evidence=" << scenario.evidence.size()
              << " request=" << (scenario.has_request ? 1 : 0) << "\n";
    return 0;
  }

  GovernorConfig config;
  config.policy = scenario.policy;
  config.epoch = scenario.epoch;
  config.store_path = store_path;
  Result<std::unique_ptr<FairnessGovernor>> opened = FairnessGovernor::open(config);
  if (!opened.ok()) {
    std::cerr << "governor open failed: " << opened.status().to_string() << "\n";
    return 3;
  }
  std::unique_ptr<FairnessGovernor>& governor = opened.value();
  if (recovery) {
    print_recovery(governor->recovery());
  }
  for (const EvidenceSnapshot& snapshot : scenario.evidence) {
    Result<IngestResult> ingested = governor->ingest_evidence(snapshot);
    if (!ingested.ok()) {
      std::cerr << "evidence rejected: " << ingested.status().to_string() << "\n";
      return 3;
    }
  }
  EvaluationRequest request;
  if (scenario.has_request) {
    request = scenario.request;
  } else if (latest) {
    request = latest_request(scenario);
  } else {
    std::cerr << "scenario has no request directive; pass --latest to use the newest evidence\n";
    return 2;
  }
  Result<FairnessDecision> decision = governor->evaluate(request);
  if (!decision.ok()) {
    std::cerr << "evaluation failed: " << decision.status().to_string() << "\n";
    return 3;
  }
  if (quiet) {
    std::cout << summarize(decision.value()) << "\n";
  } else if (result) {
    std::cout << format_result(decision.value());
  } else {
    std::cout << explain(decision.value());
  }
  if (commit) {
    const Status committed = governor->commit(decision.value());
    if (!committed.ok()) {
      std::cerr << "commit failed: " << committed.to_string() << "\n";
      return 4;
    }
    std::cout << "committed accounting_generation=" << governor->accounting_generation()
              << " last_window=" << governor->last_committed_window().value() << "\n";
  }
  const Status stopped = governor->shutdown();
  if (!stopped.ok()) {
    std::cerr << "shutdown failed: " << stopped.to_string() << "\n";
    return 5;
  }
  return 0;
}
