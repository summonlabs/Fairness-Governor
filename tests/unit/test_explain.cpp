// Fairness Governor - explanation rendering tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/eval/explain.hpp"
#include "fairness_governor/eval/governor.hpp"
#include "test_harness.hpp"
#include "fixtures.hpp"

using namespace fairness_governor;
using namespace fgtest;

namespace {

Result<FairnessDecision> starving_decision() {
  const FairnessPolicy policy = starvation_policy();
  EvidenceSnapshot evidence = make_evidence(500, 1);
  observe(evidence, 1, 1, 0, 3);
  observe(evidence, 2, 1, 800);
  return run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
}

}  // namespace

FG_TEST(explain, exposes_the_full_authority_vector) {
  Result<FairnessDecision> decision = starving_decision();
  FG_CHECK_OK(decision.status());
  const std::string text = explain(decision.value());
  FG_CHECK(text.find("outcome STARVATION_RISK") != std::string::npos);
  FG_CHECK(text.find("authority policy=1:1") != std::string::npos);
  FG_CHECK(text.find("epoch=1") != std::string::npos);
  FG_CHECK(text.find("evidence=10:1") != std::string::npos);
  FG_CHECK(text.find("window=500:1") != std::string::npos);
  FG_CHECK(text.find("accounting_generation=0") != std::string::npos);
  // The pure engine does not fabricate an incarnation; the field is present and
  // is filled in by a governor instance.
  FG_CHECK(text.find("governor_ordinal=") != std::string::npos);
  FG_CHECK(text.find("governor_boot=") != std::string::npos);
}

FG_TEST(explain, exposes_entitlement_service_and_deviation) {
  Result<FairnessDecision> decision = starving_decision();
  FG_CHECK_OK(decision.status());
  const std::string text = explain(decision.value());
  FG_CHECK(text.find("served=0") != std::string::npos);
  FG_CHECK(text.find("entitlement=200") != std::string::npos);
  FG_CHECK(text.find("deviation=-200") != std::string::npos);
  FG_CHECK(text.find("deficit=200") != std::string::npos);
  FG_CHECK(text.find("surplus=200") != std::string::npos);
}

FG_TEST(explain, exposes_starvation_modifiers_and_obligations) {
  Result<FairnessDecision> decision = starving_decision();
  FG_CHECK_OK(decision.status());
  const std::string text = explain(decision.value());
  FG_CHECK(text.find("unserved_streak=3") != std::string::npos);
  FG_CHECK(text.find("unserved_limit=3") != std::string::npos);
  FG_CHECK(text.find("modifier_bps=0") != std::string::npos);
  FG_CHECK(text.find("protected=1") != std::string::npos);
  FG_CHECK(text.find("obligation_rank=1") != std::string::npos);
  FG_CHECK(text.find("priority_delta=") != std::string::npos);
}

FG_TEST(explain, exposes_the_proposed_correction) {
  Result<FairnessDecision> decision = starving_decision();
  FG_CHECK_OK(decision.status());
  const std::string text = explain(decision.value());
  FG_CHECK(text.find("correction pool=") != std::string::npos);
  FG_CHECK(text.find("augment=200") != std::string::npos);
  FG_CHECK(text.find("reduce=200") != std::string::npos);
  FG_CHECK(text.find("intent ") != std::string::npos);
  FG_CHECK(text.find("direction=AUGMENT") != std::string::npos);
  FG_CHECK(text.find("direction=REDUCE") != std::string::npos);
}

FG_TEST(explain, is_deterministic) {
  Result<FairnessDecision> first = starving_decision();
  Result<FairnessDecision> second = starving_decision();
  FG_CHECK_OK(first.status());
  FG_CHECK_OK(second.status());
  FG_CHECK_EQ(explain(first.value()), explain(second.value()));
}

FG_TEST(explain, honours_the_section_switches) {
  Result<FairnessDecision> decision = starving_decision();
  FG_CHECK_OK(decision.status());
  ExplainOptions options;
  options.include_subjects = false;
  options.include_groups = false;
  options.include_intents = false;
  options.include_notes = false;
  const std::string text = explain(decision.value(), options);
  FG_CHECK(text.find("subject id=") == std::string::npos);
  FG_CHECK(text.find("group id=") == std::string::npos);
  FG_CHECK(text.find("intent id=") == std::string::npos);
  FG_CHECK(text.find("outcome ") != std::string::npos);
}

FG_TEST(explain, output_is_bounded) {
  FairnessPolicy policy = flat_policy(256);
  EvidenceSnapshot evidence = make_evidence(10, 1);
  for (std::uint64_t i = 0; i < 256; ++i) {
    observe(evidence, i + 1, 1, i * 7);
  }
  Result<FairnessDecision> decision =
      run_engine(policy, evidence, make_request(policy, evidence), evidence.window);
  FG_CHECK_OK(decision.status());
  ExplainOptions options;
  options.max_bytes = 512;
  const std::string text = explain(decision.value(), options);
  FG_CHECK(text.size() <= options.max_bytes + 64);
  FG_CHECK(text.find("[explanation truncated]") != std::string::npos);
}

FG_TEST(explain, summary_is_single_line) {
  Result<FairnessDecision> decision = starving_decision();
  FG_CHECK_OK(decision.status());
  const std::string text = summarize(decision.value());
  FG_CHECK(text.find('\n') == std::string::npos);
  FG_CHECK(text.find("STARVATION_RISK") != std::string::npos);
  FG_CHECK(text.find("augment=200") != std::string::npos);
}
