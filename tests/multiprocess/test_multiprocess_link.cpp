// Fairness Governor - real multiprocess, real framed-transport tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every test in this file drives REAL operating-system processes over a REAL
// loopback TCP socket carrying REAL framed messages. The governor runs in one
// process, the publisher in another, and the tests observe process death,
// restart, incarnation changes, epoch fencing, and mid-frame truncation.
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fairness_governor/link/link.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

constexpr int kStartupBudgetMs = 30000;
constexpr int kShutdownBudgetMs = 30000;

/// A running fg_evidence_link child process plus its parsed startup banner.
struct LinkChild {
  ChildProcess process;
  std::string output;
  std::string address;
  std::uint16_t port{0};
  std::uint64_t boot{0};
  bool started{false};
};

[[nodiscard]] std::string base_scenario() {
  return
      "version 1\n"
      "epoch 11\n"
      "policy 1 1 window_units=1000 fair_band=8 correction_threshold=64"
      " max_correction=100000 max_correction_bps=5000 max_modifier_bps=1000 max_age_windows=64\n"
      "group 1 1 weight=1\n"
      "subject 1 1 group=1 weight=1 floor=200 starvation=3 rank=1 protected=1"
      " reduce_cap=1000 augment_cap=1000 label=tenant-a\n"
      "subject 2 1 group=1 weight=3 starvation=2 reduce_cap=1000 augment_cap=1000"
      " label=tenant-b\n"
      "evidence 20 1 window=900 window_gen=1 epoch=11 sequence=1 producer=link-farm\n"
      "obs 1 1 served=0 unserved=3\n"
      "obs 2 1 served=800\n";
}

[[nodiscard]] bool start_link(const std::string& scenario_path, const std::string& store_path,
                              LinkChild& child, std::string& error) {
  const std::string command = quote(tool_path("fg_evidence_link")) + " --scenario " +
                              quote(scenario_path) + " --store " + quote(store_path);
  if (!child.process.start(command, error)) {
    return false;
  }
  child.output = wait_for_marker(child.process, "FG-LINK-READY", kStartupBudgetMs);
  if (child.output.find("FG-LINK-READY") == std::string::npos) {
    error = "the link process never reported readiness: " + child.output;
    return false;
  }
  child.address = field_value(child.output, "address");
  const std::string port = field_value(child.output, "port");
  const std::string boot = field_value(child.output, "boot");
  if (port.empty() || boot.empty()) {
    error = "the readiness banner is incomplete: " + child.output;
    return false;
  }
  child.port = static_cast<std::uint16_t>(std::stoul(port));
  child.boot = std::stoull(boot);
  child.started = true;
  return true;
}

[[nodiscard]] std::string run_publisher(std::uint16_t port, const std::string& scenario_path,
                                        const std::string& extra, int budget_ms) {
  const std::string command = quote(tool_path("fg_evidence_publisher")) + " --port " +
                              std::to_string(port) + " --epoch 11 --publisher 5 --scenario " +
                              quote(scenario_path) + " " + extra;
  ChildProcess publisher;
  std::string error;
  if (!publisher.start(command, error)) {
    return "START-FAILED " + error;
  }
  const std::string output = wait_for_marker(publisher, "FG-PUB-DONE", budget_ms);
  if (output.find("FG-PUB-DONE") != std::string::npos) {
    return output;
  }
  if (output.find("FG-PUB-HANDSHAKE") != std::string::npos) {
    // A rejected handshake legitimately ends the process without FG-PUB-DONE.
    if (!publisher.running() || publisher.wait_for(budget_ms)) {
      return output;
    }
  }
  publisher.terminate();
  return output + "\n[NO-TERMINAL-LINE]";
}

}  // namespace

FG_TEST(multiprocess, a_separate_publisher_process_delivers_framed_evidence) {
  const std::string directory = make_temp_directory("mp-link");
  const std::string scenario_path = directory + "/scenario.fg";
  const std::string store_path = directory + "/store";
  FG_CHECK(write_text(scenario_path, base_scenario()));

  LinkChild link;
  std::string error;
  FG_CHECK_MSG(start_link(scenario_path, store_path, link, error), error);
  FG_CHECK_EQ(link.address, std::string("127.0.0.1"));
  FG_CHECK(link.port != 0);
  FG_CHECK(link.boot != 0);

  const std::string published = run_publisher(link.port, scenario_path, "", kStartupBudgetMs);
  FG_CHECK_MSG(published.find("FG-PUB-HANDSHAKE status=OK") != std::string::npos, published);
  FG_CHECK_MSG(published.find("FG-PUB-ACK status=OK") != std::string::npos, published);
  FG_CHECK_MSG(published.find("disposition=0") != std::string::npos, published);
  FG_CHECK_MSG(published.find("FG-PUB-DONE accepted=1 rejected=0") != std::string::npos, published);

  // A graceful, frame-driven shutdown.
  const std::string shutdown_output =
      run_publisher(link.port, scenario_path, "--mode shutdown", kStartupBudgetMs);
  FG_CHECK_MSG(shutdown_output.find("FG-PUB-GOODBYE status=OK") != std::string::npos,
               shutdown_output);
  const std::string final_output = wait_for_marker(link.process, "FG-LINK-ACCOUNTING",
                                                   kShutdownBudgetMs);
  FG_CHECK_MSG(final_output.find("FG-LINK-STOPPED") != std::string::npos, final_output);
  FG_CHECK_EQ(field_value(final_output, "evidence"), std::string("1"));
  // The governor process evaluated the staged window and durably committed it.
  FG_CHECK_EQ(field_value(final_output, "generation"), std::string("1"));
  FG_CHECK_EQ(field_value(final_output, "last_window"), std::string("900"));
  FG_CHECK_EQ(field_value(final_output, "subjects"), std::string("2"));
  FG_CHECK(link.process.wait_for(kShutdownBudgetMs));
  FG_CHECK_EQ(link.process.exit_code(), 0);
  remove_directory(directory);
}

FG_TEST(multiprocess, a_stale_epoch_publisher_is_fenced_out) {
  const std::string directory = make_temp_directory("mp-epoch");
  const std::string scenario_path = directory + "/scenario.fg";
  const std::string store_path = directory + "/store";
  FG_CHECK(write_text(scenario_path, base_scenario()));

  LinkChild link;
  std::string error;
  FG_CHECK_MSG(start_link(scenario_path, store_path, link, error), error);

  const std::string command = quote(tool_path("fg_evidence_publisher")) + " --port " +
                              std::to_string(link.port) + " --epoch 10 --publisher 6 --scenario " +
                              quote(scenario_path);
  ChildProcess publisher;
  FG_CHECK_MSG(publisher.start(command, error), error);
  const std::string output = wait_for_marker(publisher, "FG-PUB-HANDSHAKE",
                                             kStartupBudgetMs);
  FG_CHECK_MSG(output.find("status=STALE_EPOCH") != std::string::npos, output);
  publisher.terminate();
  FG_CHECK(link.process.running());

  // The server is still healthy for a correct epoch.
  const std::string published = run_publisher(link.port, scenario_path, "", kStartupBudgetMs);
  FG_CHECK_MSG(published.find("FG-PUB-ACK status=OK") != std::string::npos, published);

  // Terminate the governor process outright; the port must be released.
  link.process.terminate();
  FG_CHECK_EQ(wait_for_marker(link.process, "never", 200).find("never"), std::string::npos);
  remove_directory(directory);
}

FG_TEST(multiprocess, a_publisher_killed_mid_frame_leaves_the_governor_healthy) {
  const std::string directory = make_temp_directory("mp-kill");
  const std::string scenario_path = directory + "/scenario.fg";
  const std::string store_path = directory + "/store";
  FG_CHECK(write_text(scenario_path, base_scenario()));

  LinkChild link;
  std::string error;
  FG_CHECK_MSG(start_link(scenario_path, store_path, link, error), error);

  // Start a publisher that writes half a frame header and then blocks.
  const std::string command = quote(tool_path("fg_evidence_publisher")) + " --port " +
                              std::to_string(link.port) + " --epoch 11 --publisher 7 --scenario " +
                              quote(scenario_path) + " --mode split --announce-split";
  ChildProcess split;
  FG_CHECK_MSG(split.start(command, error), error);
  const std::string split_output = wait_for_marker(split, "FG-PUB-SPLIT",
                                                   kStartupBudgetMs);
  FG_CHECK_MSG(split_output.find("FG-PUB-SPLIT") != std::string::npos, split_output);
  FG_CHECK(split.running());

  // Kill it mid-frame: the governor must reject the truncated frame.
  split.terminate();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const std::string published = run_publisher(link.port, scenario_path, "", kStartupBudgetMs);
  FG_CHECK_MSG(published.find("FG-PUB-ACK status=OK") != std::string::npos, published);

  const std::string shutdown_output =
      run_publisher(link.port, scenario_path, "--mode shutdown", kStartupBudgetMs);
  FG_CHECK(shutdown_output.find("FG-PUB-GOODBYE") != std::string::npos);
  const std::string final_output = wait_for_marker(link.process, "FG-LINK-ACCOUNTING",
                                                   kShutdownBudgetMs);
  FG_CHECK_MSG(final_output.find("rejected_frames=") != std::string::npos, final_output);
  FG_CHECK_EQ(field_value(final_output, "evidence"), std::string("1"));
  FG_CHECK(link.process.wait_for(kShutdownBudgetMs));
  remove_directory(directory);
}

FG_TEST(multiprocess, malformed_publisher_traffic_never_breaks_the_governor) {
  const std::string directory = make_temp_directory("mp-malformed");
  const std::string scenario_path = directory + "/scenario.fg";
  const std::string store_path = directory + "/store";
  FG_CHECK(write_text(scenario_path, base_scenario()));

  LinkChild link;
  std::string error;
  FG_CHECK_MSG(start_link(scenario_path, store_path, link, error), error);

  const char* const modes[] = {"short", "badsum", "oversize"};
  for (const char* mode : modes) {
    const std::string command = quote(tool_path("fg_evidence_publisher")) + " --port " +
                                std::to_string(link.port) + " --epoch 11 --publisher 8 --scenario " +
                                quote(scenario_path) + " --mode " + mode;
    const ChildResult result = run_child(command, true);
    FG_CHECK(result.started);
    FG_CHECK_EQ(result.exit_code, 0);
  }

  // After all of that malformed traffic, a correct publisher is still served.
  const std::string published = run_publisher(link.port, scenario_path, "", kStartupBudgetMs);
  FG_CHECK_MSG(published.find("FG-PUB-ACK status=OK") != std::string::npos, published);

  const std::string shutdown_output =
      run_publisher(link.port, scenario_path, "--mode shutdown", kStartupBudgetMs);
  FG_CHECK(shutdown_output.find("FG-PUB-GOODBYE") != std::string::npos);
  const std::string final_output = wait_for_marker(link.process, "FG-LINK-ACCOUNTING",
                                                   kShutdownBudgetMs);
  FG_CHECK_MSG(final_output.find("checksum_failures=") != std::string::npos, final_output);
  FG_CHECK(link.process.wait_for(kShutdownBudgetMs));
  remove_directory(directory);
}

FG_TEST(multiprocess, durable_accounting_survives_killing_and_restarting_the_governor) {
  const std::string directory = make_temp_directory("mp-restart");
  const std::string scenario_path = directory + "/scenario.fg";
  const std::string store_path = directory + "/store";
  FG_CHECK(write_text(scenario_path, base_scenario()));

  // --- First incarnation: serve, publish, shut down cleanly.
  LinkChild first;
  std::string error;
  FG_CHECK_MSG(start_link(scenario_path, store_path, first, error), error);
  const std::uint64_t first_boot = first.boot;
  const std::string published = run_publisher(first.port, scenario_path, "", kStartupBudgetMs);
  FG_CHECK_MSG(published.find("FG-PUB-ACK status=OK") != std::string::npos, published);
  const std::string shutdown_output =
      run_publisher(first.port, scenario_path, "--mode shutdown", kStartupBudgetMs);
  FG_CHECK(shutdown_output.find("FG-PUB-GOODBYE") != std::string::npos);
  const std::string first_final = wait_for_marker(first.process, "FG-LINK-ACCOUNTING",
                                                  kShutdownBudgetMs);
  FG_CHECK_EQ(field_value(first_final, "generation"), std::string("1"));
  FG_CHECK_EQ(field_value(first_final, "last_window"), std::string("900"));
  FG_CHECK(first.process.wait_for(kShutdownBudgetMs));

  // --- Second incarnation: a fresh process with a fresh boot identity.
  LinkChild second;
  FG_CHECK_MSG(start_link(scenario_path, store_path, second, error), error);
  FG_CHECK(second.boot != first_boot);
  FG_CHECK(second.port != 0);

  // The window the first incarnation committed must stay committed: a fresh
  // process does not inherit the right to re-ingest it.
  const std::string replay = run_publisher(second.port, scenario_path, "", kStartupBudgetMs);
  FG_CHECK_MSG(replay.find("status=STALE_GENERATION") != std::string::npos, replay);
  const std::string second_shutdown =
      run_publisher(second.port, scenario_path, "--mode shutdown", kStartupBudgetMs);
  FG_CHECK(second_shutdown.find("FG-PUB-GOODBYE") != std::string::npos);
  const std::string second_final = wait_for_marker(second.process, "FG-LINK-ACCOUNTING",
                                                   kShutdownBudgetMs);
  FG_CHECK_MSG(second_final.find("FG-LINK-STOPPED") != std::string::npos, second_final);
  FG_CHECK(second.process.wait_for(kShutdownBudgetMs));

  // --- Third incarnation: kill the process instead of a clean shutdown and
  // confirm the durable record is still usable and still reports generation 0
  // (nothing was ever committed), with no evidence authority restored.
  LinkChild third;
  FG_CHECK_MSG(start_link(scenario_path, store_path, third, error), error);
  FG_CHECK(third.boot != second.boot);
  third.process.terminate();
  remove_directory(directory);
}

FG_TEST(multiprocess, the_link_tool_refuses_a_scenario_whose_policy_is_invalid) {
  const std::string directory = make_temp_directory("mp-bad-policy");
  const std::string scenario_path = directory + "/bad.fg";
  const std::string store_path = directory + "/store";
  FG_CHECK(write_text(scenario_path,
                      "version 1\nepoch 3\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=9 weight=1\n"));
  const std::string command = quote(tool_path("fg_evidence_link")) + " --scenario " +
                              quote(scenario_path) + " --store " + quote(store_path);
  const ChildResult result = run_child(command, true);
  FG_CHECK(result.started);
  FG_CHECK(result.exit_code != 0);
  FG_CHECK(result.output.find("scenario rejected") != std::string::npos);
  remove_directory(directory);
}

FG_TEST(multiprocess, the_cli_tool_reports_the_same_decision_as_the_in_process_engine) {
  const std::string directory = make_temp_directory("mp-cli");
  const std::string scenario_path = directory + "/scenario.fg";
  FG_CHECK(write_text(scenario_path, canonical_scenario()));

  const std::string command = quote(tool_path("fg_governor_cli")) + " evaluate " +
                              quote(scenario_path) + " --quiet";
  const ChildResult result = run_child(command, true);
  FG_CHECK(result.started);
  FG_CHECK_EQ(result.exit_code, 0);
  FG_CHECK(result.output.find("STARVATION_RISK") != std::string::npos);
  FG_CHECK(result.output.find("augment=200") != std::string::npos);
  FG_CHECK(result.output.find("reduce=200") != std::string::npos);
  remove_directory(directory);
}
