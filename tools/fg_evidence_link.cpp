// Fairness Governor - evidence link server process.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fairness_governor/fairness_governor.hpp"

namespace {

using namespace fairness_governor;

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

}  // namespace

int main(int argc, char** argv) {
  std::string scenario_path;
  std::string store_path;
  std::uint32_t port = 0;
  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--scenario" && i + 1 < argc) {
      scenario_path = argv[++i];
    } else if (argument == "--store" && i + 1 < argc) {
      store_path = argv[++i];
    } else if (argument == "--port" && i + 1 < argc) {
      port = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (argument == "--quiet") {
      quiet = true;
    } else {
      std::cerr << "unknown option: " << argument << "\n";
      return 2;
    }
  }
  if (scenario_path.empty()) {
    std::cerr << "usage: fg_evidence_link --scenario <file> [--store <dir>] [--port <n>]\n";
    return 2;
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
  if (!parsed.value().has_policy) {
    std::cerr << "scenario rejected: no policy directive\n";
    return 2;
  }

  GovernorConfig config;
  config.policy = parsed.value().policy;
  config.epoch = parsed.value().epoch;
  config.store_path = store_path;
  Result<std::unique_ptr<FairnessGovernor>> opened = FairnessGovernor::open(config);
  if (!opened.ok()) {
    std::cerr << "governor open failed: " << opened.status().to_string() << "\n";
    return 3;
  }
  std::unique_ptr<FairnessGovernor>& governor = opened.value();

  LinkServerOptions options;
  options.port = static_cast<std::uint16_t>(port);
  options.evaluate_on_ingest = true;
  Result<std::unique_ptr<EvidenceLinkServer>> started =
      EvidenceLinkServer::start(*governor, options);
  if (!started.ok()) {
    std::cerr << "link start failed: " << started.status().to_string() << "\n";
    return 3;
  }
  std::unique_ptr<EvidenceLinkServer>& server = started.value();

  std::cout << "FG-LINK-READY address=" << server->bound_address() << " port=" << server->port()
            << " epoch=" << governor->epoch().value()
            << " policy=" << governor->policy().id.value() << ":"
            << governor->policy_generation().value()
            << " boot=" << governor->incarnation().boot.value()
            << " ordinal=" << governor->incarnation().ordinal << "\n";
  std::cout.flush();

  std::thread serving([&server]() { server->serve(); });

  // The server runs until a peer asks it to stop with a shutdown-flagged goodbye
  // frame, or until the process is terminated. No wall-clock deadline is used.
  while (!server->stopped()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  server->request_stop();
  if (serving.joinable()) {
    serving.join();
  }
  const LinkStats stats = server->stats();
  (void)server->shutdown();

  const FairnessAccounting accounting = governor->accounting();
  std::cout << "FG-LINK-STOPPED connections=" << stats.connections_accepted
            << " rejected_connections=" << stats.connections_rejected
            << " frames=" << stats.frames_received << " rejected_frames=" << stats.frames_rejected
            << " evidence=" << stats.evidence_accepted
            << " duplicates=" << stats.evidence_duplicate
            << " rejected_evidence=" << stats.evidence_rejected
            << " checksum_failures=" << stats.checksum_failures
            << " oversized=" << stats.oversized_frames
            << " sequence_violations=" << stats.sequence_violations
            << " handshakes=" << stats.handshakes_completed
            << " rejected_handshakes=" << stats.handshakes_rejected
            << " windows_committed=" << stats.windows_committed
            << " evaluation_failures=" << stats.evaluations_failed
            << " idle_dropped=" << stats.connections_idle_dropped << "\n";
  std::cout << "FG-LINK-ACCOUNTING generation=" << accounting.generation
            << " last_window=" << accounting.last_window.value()
            << " subjects=" << accounting.subjects.size() << "\n";
  std::cout.flush();
  (void)governor->shutdown();
  if (!quiet) {
    std::cout << "FG-LINK-EXIT ok\n";
  }
  return 0;
}
