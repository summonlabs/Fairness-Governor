// Fairness Governor - strict textual scenario format.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The scenario format exists so policies, evidence and requests can be expressed
// outside the process and validated strictly. Unknown directives, unknown keys,
// duplicate keys, out-of-range values, and trailing garbage are all errors: a
// scenario that parses is a scenario the engine will accept.
#ifndef FAIRNESS_GOVERNOR_SCENARIO_HPP
#define FAIRNESS_GOVERNOR_SCENARIO_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fairness_governor/core/status.hpp"
#include "fairness_governor/eval/governor.hpp"
#include "fairness_governor/model/evidence.hpp"
#include "fairness_governor/model/policy.hpp"

namespace fairness_governor {

/// A parsed scenario: policy, fabric epoch, evidence, and an optional request.
struct Scenario {
  bool has_policy{false};
  FairnessPolicy policy{};
  FabricEpoch epoch{};
  std::vector<EvidenceSnapshot> evidence;
  bool has_request{false};
  EvaluationRequest request{};
  /// Source text digest, for reproducible reporting.
  std::uint64_t source_digest{0};
};

/// Parses a scenario. Returns the first error found, with its 1-based line.
[[nodiscard]] Result<Scenario> parse_scenario(std::string_view text);

/// Renders a decision in the stable machine-readable result format.
[[nodiscard]] std::string format_result(const FairnessDecision& decision);

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_SCENARIO_HPP
