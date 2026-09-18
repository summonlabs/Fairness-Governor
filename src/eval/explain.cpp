// Fairness Governor - deterministic explanation rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/eval/explain.hpp"

#include <string>

#include "fairness_governor/core/checked.hpp"
#include "fairness_governor/core/ids.hpp"

namespace fairness_governor {
namespace {

class BoundedText {
 public:
  explicit BoundedText(std::uint64_t limit) : limit_(limit) {}

  void append(const std::string& text) {
    if (truncated_) {
      return;
    }
    if (size_ + text.size() > limit_) {
      buffer_.append(text, 0, static_cast<std::size_t>(limit_ > size_ ? limit_ - size_ : 0));
      buffer_ += "\n[explanation truncated]\n";
      truncated_ = true;
      size_ = limit_;
      return;
    }
    buffer_ += text;
    size_ += text.size();
  }

  void line(const std::string& text) {
    append(text);
    append("\n");
  }

  [[nodiscard]] std::string take() { return std::move(buffer_); }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }

 private:
  std::uint64_t limit_;
  std::uint64_t size_{0};
  bool truncated_{false};
  std::string buffer_;
};

void append_u64(std::string& out, const char* name, std::uint64_t value) {
  out += ' ';
  out += name;
  out += '=';
  out += to_decimal(value);
}

void append_i64(std::string& out, const char* name, std::int64_t value) {
  out += ' ';
  out += name;
  out += '=';
  if (value < 0) {
    out += '-';
  }
  out += to_decimal(magnitude(value));
}

const char* yes_no(bool value) noexcept { return value ? "1" : "0"; }

}  // namespace

std::string explain(const FairnessDecision& decision, const ExplainOptions& options) {
  BoundedText text(options.max_bytes);
  const AuthorityVector& authority = decision.authority;

  text.line(std::string("outcome ") + std::string(to_string(decision.outcome)));
  {
    std::string line = "authority policy=";
    line += to_decimal(authority.policy_id.value());
    line += ':';
    line += to_decimal(authority.policy_generation.value());
    append_u64(line, "epoch", authority.epoch.value());
    line += " evidence=";
    line += to_decimal(authority.evidence_id.value());
    line += ':';
    line += to_decimal(authority.evidence_generation.value());
    line += " window=";
    line += to_decimal(authority.window.value());
    line += ':';
    line += to_decimal(authority.window_generation.value());
    append_u64(line, "accounting_generation", authority.accounting_generation);
    append_u64(line, "governor_boot", authority.governor.boot.value());
    append_u64(line, "governor_ordinal", authority.governor.ordinal);
    append_u64(line, "request", authority.request_id);
    append_u64(line, "attempt", authority.attempt);
    text.line(line);
  }
  {
    std::string line = "counts";
    append_u64(line, "fair", decision.counts.fair);
    append_u64(line, "over_served", decision.counts.over_served);
    append_u64(line, "under_served", decision.counts.under_served);
    append_u64(line, "starvation_risk", decision.counts.starvation_risk);
    append_u64(line, "correction_required", decision.counts.correction_required);
    append_u64(line, "blocked_by_stronger_obligation",
               decision.counts.blocked_by_stronger_obligation);
    append_u64(line, "unknown", decision.counts.unknown);
    append_u64(line, "stale", decision.counts.stale);
    text.line(line);
  }
  {
    const CorrectionSummary& summary = decision.correction;
    std::string line = "correction";
    append_u64(line, "pool", summary.reducible_pool_units);
    append_u64(line, "demand", summary.demand_units);
    append_u64(line, "authorized", summary.authorized_budget_units);
    append_u64(line, "augment", summary.augment_units);
    append_u64(line, "reduce", summary.reduce_units);
    append_u64(line, "withheld", summary.withheld_units);
    append_u64(line, "unsatisfied", summary.unsatisfied_demand_units);
    append_u64(line, "cooldown_suppressed", summary.cooldown_suppressed);
    append_u64(line, "bounded_by_total", summary.bounded_by_policy_total ? 1 : 0);
    append_u64(line, "bounded_by_bps", summary.bounded_by_policy_bps ? 1 : 0);
    append_u64(line, "protected_blocked", summary.protected_obligation_blocked ? 1 : 0);
    append_u64(line, "authority_missing", summary.authority_missing ? 1 : 0);
    text.line(line);
  }

  if (options.include_groups) {
    for (const GroupFairnessState& group : decision.groups) {
      std::string line = "group id=";
      line += to_decimal(group.id.value());
      line += ':';
      line += to_decimal(group.generation.value());
      line += " parent=";
      line += to_decimal(group.parent.value());
      append_u64(line, "depth", group.depth);
      append_u64(line, "entitlement", group.entitlement_units);
      append_u64(line, "served", group.served_units);
      append_i64(line, "deviation", group.deviation_units);
      append_u64(line, "floor", group.guarantee_floor_units);
      append_u64(line, "protected", group.is_protected_obligation ? 1 : 0);
      append_u64(line, "obligation_rank", group.obligation_rank);
      append_u64(line, "withheld_correction", group.withheld_correction ? 1 : 0);
      if (!group.label.empty()) {
        line += " label=";
        line += group.label;
      }
      text.line(line);
    }
  }

  if (options.include_subjects) {
    for (const SubjectFairnessState& subject : decision.subjects) {
      std::string line = "subject id=";
      line += to_decimal(subject.id.value());
      line += ':';
      line += to_decimal(subject.generation.value());
      line += " outcome=";
      line += to_string(subject.outcome);
      line += " reason=";
      line += to_string(subject.reason);
      append_u64(line, "authority", subject.has_authority ? 1 : 0);
      append_u64(line, "group", subject.group.value());
      append_u64(line, "group_entitlement", subject.group_entitlement_units);
      append_u64(line, "entitlement", subject.entitlement_units);
      append_u64(line, "base_entitlement", subject.base_entitlement_units);
      append_u64(line, "floor", subject.guarantee_floor_units);
      append_i64(line, "priority_delta", subject.priority_delta_units);
      append_i64(line, "modifier_bps", subject.applied_modifier_bps);
      append_u64(line, "modifier_clipped", subject.modifier_clipped ? 1 : 0);
      append_u64(line, "served", subject.served_units);
      append_i64(line, "deviation", subject.deviation_units);
      append_u64(line, "deficit", subject.deficit_units);
      append_u64(line, "surplus", subject.surplus_units);
      append_u64(line, "carry_deficit", subject.cumulative_deficit_after);
      append_u64(line, "carry_surplus", subject.cumulative_surplus_after);
      append_u64(line, "unserved_streak", subject.unserved_streak);
      append_u64(line, "unserved_limit", subject.starvation_limit_windows);
      append_u64(line, "below_floor_streak", subject.below_floor_streak);
      append_u64(line, "below_floor_limit", subject.guarantee_starvation_limit_windows);
      append_u64(line, "protected", subject.is_protected_obligation ? 1 : 0);
      append_u64(line, "obligation_rank", subject.obligation_rank);
      append_u64(line, "augment", subject.proposed_augment_units);
      append_u64(line, "reduce", subject.proposed_reduce_units);
      append_u64(line, "unsatisfied", subject.unsatisfied_demand_units);
      if (subject.expected_priority.present()) {
        append_u64(line, "priority_ref", subject.expected_priority.id.value());
      }
      if (subject.observed_priority.present()) {
        append_u64(line, "observed_priority_ref", subject.observed_priority.id.value());
      }
      if (!subject.label.empty()) {
        line += " label=";
        line += subject.label;
      }
      text.line(line);
    }
  }

  if (options.include_intents) {
    for (const CorrectiveIntent& intent : decision.intents) {
      std::string line = "intent id=";
      line += to_decimal(intent.id.value());
      line += ':';
      line += to_decimal(intent.generation.value());
      line += " direction=";
      line += to_string(intent.direction);
      append_u64(line, "subject", intent.subject.value());
      append_u64(line, "group", intent.group.value());
      append_u64(line, "units", intent.units);
      line += " bound=";
      line += to_string(intent.bound);
      line += " reason=";
      line += to_string(intent.reason);
      append_u64(line, "starvation", intent.from_starvation ? 1 : 0);
      append_u64(line, "protected", intent.from_protected_obligation ? 1 : 0);
      append_u64(line, "obligation_rank", intent.obligation_rank);
      text.line(line);
    }
  }

  if (options.include_notes) {
    for (const DiagnosticNote& note : decision.notes) {
      std::string line = "note code=";
      line += to_string(note.code);
      line += " reason=";
      line += to_string(note.reason);
      append_u64(line, "subject", note.subject.value());
      if (!note.detail.empty()) {
        line += " detail=";
        line += note.detail;
      }
      text.line(line);
    }
  }

  {
    std::string line = "digest=";
    line += to_decimal(decision.content_digest);
    text.line(line);
  }
  return text.take();
}

std::string summarize(const FairnessDecision& decision) {
  std::string out(to_string(decision.outcome));
  out += " fair=";
  out += to_decimal(decision.counts.fair);
  out += " under=";
  out += to_decimal(decision.counts.under_served);
  out += " over=";
  out += to_decimal(decision.counts.over_served);
  out += " starve=";
  out += to_decimal(decision.counts.starvation_risk);
  out += " correct=";
  out += to_decimal(decision.counts.correction_required);
  out += " blocked=";
  out += to_decimal(decision.counts.blocked_by_stronger_obligation);
  out += " unknown=";
  out += to_decimal(decision.counts.unknown);
  out += " stale=";
  out += to_decimal(decision.counts.stale);
  out += " augment=";
  out += to_decimal(decision.correction.augment_units);
  out += " reduce=";
  out += to_decimal(decision.correction.reduce_units);
  return out;
}

}  // namespace fairness_governor
