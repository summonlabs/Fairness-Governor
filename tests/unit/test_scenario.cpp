// Fairness Governor - scenario parser tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/scenario.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(scenario, canonical_document_parses) {
  Result<Scenario> parsed = parse_scenario(canonical_scenario());
  FG_CHECK_OK(parsed.status());
  const Scenario& scenario = parsed.value();
  FG_CHECK(scenario.has_policy);
  FG_CHECK_EQ(scenario.epoch.value(), 7u);
  FG_CHECK_EQ(scenario.policy.subjects.size(), 2u);
  FG_CHECK_EQ(scenario.policy.groups.size(), 1u);
  FG_CHECK_EQ(scenario.evidence.size(), 1u);
  FG_CHECK(scenario.has_request);
  FG_CHECK_EQ(scenario.request.window.value(), 500u);
  FG_CHECK_EQ(scenario.policy.subjects[0].label, std::string("tenant-a"));
  FG_CHECK(scenario.policy.subjects[0].protected_obligation);
  FG_CHECK(scenario.source_digest != 0);
}

FG_TEST(scenario, comments_and_blank_lines_are_ignored) {
  const std::string text =
      "# a leading comment\n"
      "version 1\n"
      "\n"
      "epoch 3   # trailing comment\n"
      "policy 1 1 fair_band=0 correction_threshold=0\n"
      "group 1 1 weight=1\n"
      "subject 1 1 group=1 weight=1\n";
  Result<Scenario> parsed = parse_scenario(text);
  FG_CHECK_OK(parsed.status());
  FG_CHECK_EQ(parsed.value().policy.subjects.size(), 1u);
}

FG_TEST(scenario, the_version_directive_is_mandatory_and_first) {
  Result<Scenario> missing = parse_scenario("epoch 3\n");
  FG_CHECK(!missing.ok());
  Result<Scenario> late = parse_scenario("epoch 3\nversion 1\n");
  FG_CHECK(!late.ok());
  Result<Scenario> wrong = parse_scenario("version 2\n");
  FG_CHECK(!wrong.ok());
}

FG_TEST(scenario, unknown_directives_and_keys_are_rejected) {
  FG_CHECK(!parse_scenario("version 1\nfrobnicate 1\n").ok());
  FG_CHECK(!parse_scenario(
                std::string("version 1\nepoch 1\npolicy 1 1\ngroup 1 1 bogus=1\nsubject 1 1 "
                            "group=1\n"))
                .ok());
  FG_CHECK(!parse_scenario("version 1\nepoch 1\npolicy 1 1 bogus=2\n").ok());
  FG_CHECK(!parse_scenario(
                std::string("version 1\nepoch 1\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=1 "
                            "bogus=2\n"))
                .ok());
}

FG_TEST(scenario, duplicate_keys_are_rejected) {
  const std::string text =
      "version 1\nepoch 1\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=1 group=1\n";
  FG_CHECK(!parse_scenario(text).ok());
}

FG_TEST(scenario, malformed_numbers_are_rejected) {
  FG_CHECK(!parse_scenario("version 1\nepoch abc\n").ok());
  FG_CHECK(!parse_scenario("version 1\nepoch -1\n").ok());
  FG_CHECK(!parse_scenario("version 1\nepoch 99999999999999999999999\n").ok());
  FG_CHECK(!parse_scenario("version 1\nepoch 1\npolicy 1 1 fair_band=-4\n").ok());
}

FG_TEST(scenario, a_structurally_invalid_policy_is_rejected) {
  const std::string text =
      "version 1\nepoch 1\npolicy 1 1\ngroup 1 1\nsubject 1 1 group=2 weight=1\n";
  const Result<Scenario> parsed = parse_scenario(text);
  FG_CHECK(!parsed.ok());
}

FG_TEST(scenario, a_signed_modifier_is_accepted) {
  const std::string text =
      "version 1\nepoch 1\npolicy 1 1 max_modifier_bps=2000\ngroup 1 1\nsubject 1 1 group=1 "
      "weight=1 modifier_bps=-1500\n";
  Result<Scenario> parsed = parse_scenario(text);
  FG_CHECK_OK(parsed.status());
  FG_CHECK_EQ(parsed.value().policy.subjects[0].priority_modifier_bps, -1500);
}

FG_TEST(scenario, an_evidence_only_document_parses) {
  const std::string text =
      "version 1\nepoch 4\nevidence 5 1 window=9 window_gen=1 epoch=4 sequence=2\n"
      "obs 1 1 served=100\n";
  Result<Scenario> parsed = parse_scenario(text);
  FG_CHECK_OK(parsed.status());
  FG_CHECK(!parsed.value().has_policy);
  FG_CHECK_EQ(parsed.value().evidence.size(), 1u);
  FG_CHECK_EQ(parsed.value().evidence[0].observations[0].served_units, 100u);
}

FG_TEST(scenario, contract_violations_are_rejected) {
  FG_CHECK(!parse_scenario("version 1\nepoch 1\nobs 1 1 served=1\n").ok());
  FG_CHECK(!parse_scenario(
                std::string("version 1\nepoch 1\npolicy 1 1\nsubject 1 1 group=1\n"))
                .ok());
}

FG_TEST(scenario, an_overlong_identifier_is_rejected) {
  const std::string huge(30, '9');
  const std::string text = "version 1\nepoch " + huge + "\n";
  FG_CHECK(!parse_scenario(text).ok());
}

FG_TEST(scenario, the_error_reports_a_line_number) {
  const Result<Scenario> parsed = parse_scenario("version 1\nepoch abc\n");
  FG_CHECK(!parsed.ok());
  FG_CHECK(parsed.status().detail().find("line 2") != std::string::npos);
}

FG_TEST(scenario, result_rendering_is_deterministic) {
  Result<Scenario> parsed = parse_scenario(canonical_scenario());
  FG_CHECK_OK(parsed.status());
  Scenario& scenario = parsed.value();
  Result<std::unique_ptr<FairnessGovernor>> opened =
      FairnessGovernor::open_in_memory(scenario.policy);
  FG_CHECK_OK(opened.status());
  FG_CHECK_OK(opened.value()->ingest_evidence(scenario.evidence[0]).status());
  Result<FairnessDecision> decision = opened.value()->evaluate(scenario.request);
  FG_CHECK_OK(decision.status());
  const std::string first = format_result(decision.value());
  const std::string second = format_result(decision.value());
  FG_CHECK_EQ(first, second);
  FG_CHECK(first.find("summary ") != std::string::npos);
  FG_CHECK(first.find("STARVATION_RISK") != std::string::npos);
  FG_CHECK_OK(opened.value()->shutdown());
}
