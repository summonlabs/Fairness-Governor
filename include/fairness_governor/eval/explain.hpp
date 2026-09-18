// Fairness Governor - deterministic explanation rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Explanation is a first-class output: it exposes entitlement, observed service,
// deficit/surplus, starvation state, modifiers, protected obligations, the
// proposed correction, and the authority vector that justified everything.
// Rendering is bounded and deterministic; there are no timestamps other than
// the ones supplied in the request.
#ifndef FAIRNESS_GOVERNOR_EVAL_EXPLAIN_HPP
#define FAIRNESS_GOVERNOR_EVAL_EXPLAIN_HPP

#include <string>

#include "fairness_governor/core/limits.hpp"
#include "fairness_governor/model/outcome.hpp"

namespace fairness_governor {

struct ExplainOptions {
  /// Include one line per subject.
  bool include_subjects{true};
  /// Include group rollups.
  bool include_groups{true};
  /// Include the bounded corrective intent list.
  bool include_intents{true};
  /// Include diagnostic notes.
  bool include_notes{true};
  /// Hard cap on rendered size.
  std::uint64_t max_bytes{kMaxExplanationBytes};
};

/// Renders a decision as stable, line-oriented text. Identical decisions render
/// identically.
[[nodiscard]] std::string explain(const FairnessDecision& decision,
                                  const ExplainOptions& options = {});

/// Single-line summary, suitable for logs and test assertions.
[[nodiscard]] std::string summarize(const FairnessDecision& decision);

}  // namespace fairness_governor

#endif  // FAIRNESS_GOVERNOR_EVAL_EXPLAIN_HPP
