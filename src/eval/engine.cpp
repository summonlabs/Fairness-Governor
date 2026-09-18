// Fairness Governor - exact allocation and the pure fairness evaluation engine.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fairness_governor/core/checked.hpp"
#include "fairness_governor/core/u128.hpp"
#include "fairness_governor/eval/governor.hpp"
#include "fairness_governor/persist/codec.hpp"

namespace fairness_governor {
namespace {

constexpr std::size_t kMaxNotes = 64;
constexpr std::size_t kMaxNoteDetail = 192;

/// Groups sort before subjects at the same numeric identity, so tie-breaks are
/// total and deterministic even when a group and a subject share a raw id.
[[nodiscard]] std::uint64_t composite_key(bool is_group, std::uint64_t id) noexcept {
  return is_group ? id : (id | (std::uint64_t{1} << 63));
}

void add_note(std::vector<DiagnosticNote>& notes, StatusCode code, ReasonCode reason, SubjectId id,
              std::string detail) {
  if (notes.size() >= kMaxNotes) {
    return;
  }
  DiagnosticNote note;
  note.code = code;
  note.reason = reason;
  note.subject = id;
  if (detail.size() > kMaxNoteDetail) {
    detail.resize(kMaxNoteDetail);
  }
  note.detail = std::move(detail);
  notes.push_back(std::move(note));
}

struct PassState {
  bool overcommit{false};
  std::uint64_t overcommit_shortfall{0};
};

// --- Group tree and top-down allocation --------------------------------------

struct Distributor {
  struct GroupNode {
    std::size_t parent_node{0};
    bool has_parent{false};
    std::uint32_t depth{1};
    std::uint64_t allocation{0};
    std::uint64_t base_allocation{0};
    std::uint64_t served{0};
    std::vector<std::size_t> child_groups;
    std::vector<std::size_t> child_subjects;
  };

  const FairnessPolicy* policy{nullptr};
  std::vector<GroupNode> nodes;
  std::unordered_map<std::uint64_t, std::size_t> by_id;
  std::vector<std::uint64_t> subject_base;
  std::vector<std::uint64_t> subject_effective;
  std::vector<std::uint8_t> subject_floor_binding;
  std::vector<std::size_t> roots;
  PassState base_pass;
  PassState effective_pass;

  [[nodiscard]] Status build(const FairnessPolicy& in_policy) {
    policy = &in_policy;
    nodes.assign(in_policy.groups.size(), GroupNode{});
    by_id.clear();
    by_id.reserve(in_policy.groups.size() * 2 + 1);
    for (std::size_t i = 0; i < in_policy.groups.size(); ++i) {
      if (!in_policy.groups[i].id.valid()) {
        return Status(StatusCode::PolicyInvalid, "group id must be non-zero");
      }
      if (!by_id.emplace(in_policy.groups[i].id.value(), i).second) {
        return Status(StatusCode::AlreadyExists, "duplicate group id");
      }
    }
    for (std::size_t i = 0; i < in_policy.groups.size(); ++i) {
      const FairnessGroupId parent = in_policy.groups[i].parent;
      if (!parent.valid()) {
        continue;
      }
      const auto it = by_id.find(parent.value());
      if (it == by_id.end()) {
        return Status(StatusCode::NotFound, "group parent does not exist");
      }
      nodes[i].has_parent = true;
      nodes[i].parent_node = it->second;
      nodes[it->second].child_groups.push_back(i);
    }
    std::uint32_t max_depth = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      std::uint32_t depth = 1;
      std::size_t cursor = i;
      while (nodes[cursor].has_parent) {
        cursor = nodes[cursor].parent_node;
        ++depth;
        if (depth > kMaxGroupDepth) {
          return Status(StatusCode::GroupDepthExceeded, "group nesting exceeds kMaxGroupDepth");
        }
      }
      nodes[i].depth = depth;
      max_depth = std::max(max_depth, depth);
    }
    if (in_policy.groups.empty()) {
      return Status(StatusCode::PolicyInvalid, "policy must declare at least one group");
    }
    if (max_depth == 0) {
      return Status(StatusCode::PolicyInvalid, "policy must declare at least one group");
    }
    subject_base.assign(in_policy.subjects.size(), 0);
    subject_effective.assign(in_policy.subjects.size(), 0);
    subject_floor_binding.assign(in_policy.subjects.size(), 0);
    for (std::size_t s = 0; s < in_policy.subjects.size(); ++s) {
      const auto it = by_id.find(in_policy.subjects[s].group.value());
      if (it == by_id.end()) {
        return Status(StatusCode::NotFound, "subject references an unknown fairness group");
      }
      nodes[it->second].child_subjects.push_back(s);
    }
    roots.clear();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (!nodes[i].has_parent) {
        roots.push_back(i);
      }
    }
    if (roots.empty()) {
      return Status(StatusCode::PolicyInvalid, "policy must declare at least one root group");
    }
    return Status::success();
  }

  [[nodiscard]] std::vector<AllocationInput> inputs_for(std::size_t node_index,
                                                        bool use_modifier) const {
    const GroupNode& node = nodes[node_index];
    std::vector<AllocationInput> inputs;
    inputs.reserve(node.child_groups.size() + node.child_subjects.size());
    for (const std::size_t child : node.child_groups) {
      const FairnessGroup& group = policy->groups[child];
      AllocationInput input;
      input.key = composite_key(true, group.id.value());
      input.weight = effective_share_weight(group.share_weight, 0);
      input.floor = group.guarantee_floor;
      input.obligation_rank = group.obligation_rank;
      inputs.push_back(input);
    }
    for (const std::size_t child : node.child_subjects) {
      const Subject& subject = policy->subjects[child];
      AllocationInput input;
      input.key = composite_key(false, subject.id.value());
      input.weight = use_modifier
                         ? effective_share_weight(subject.share_weight,
                                                  subject.priority_modifier_bps)
                         : effective_share_weight(subject.share_weight, 0);
      input.floor = subject.guarantee_floor;
      input.obligation_rank = subject.obligation_rank;
      inputs.push_back(input);
    }
    return inputs;
  }

  [[nodiscard]] Status scatter(std::size_t node_index, std::uint64_t whole, bool use_modifier,
                               PassState& pass) {
    const std::vector<AllocationInput> inputs = inputs_for(node_index, use_modifier);
    if (inputs.empty()) {
      return Status::success();
    }
    Result<AllocationResult> allocated = allocate_whole(inputs, whole);
    if (!allocated.ok()) {
      return allocated.status();
    }
    const AllocationResult& result = allocated.value();
    if (result.overcommit) {
      pass.overcommit = true;
      if (!checked_add(pass.overcommit_shortfall, result.overcommit_shortfall,
                       pass.overcommit_shortfall)) {
        return Status(StatusCode::ArithmeticOverflow, "overcommit shortfall overflows");
      }
    }
    const GroupNode& node = nodes[node_index];
    std::size_t cursor = 0;
    for (const std::size_t child : node.child_groups) {
      if (use_modifier) {
        nodes[child].allocation = result.allocations[cursor];
      } else {
        nodes[child].base_allocation = result.allocations[cursor];
      }
      ++cursor;
    }
    for (const std::size_t child : node.child_subjects) {
      if (use_modifier) {
        subject_effective[child] = result.allocations[cursor];
        subject_floor_binding[child] = result.floor_binding[cursor];
      } else {
        subject_base[child] = result.allocations[cursor];
      }
      ++cursor;
    }
    // Both passes recurse: the base pass fills the unmodified entitlement that
    // the explanation uses to attribute the priority/value delta.
    for (const std::size_t child : node.child_groups) {
      const std::uint64_t child_whole =
          use_modifier ? nodes[child].allocation : nodes[child].base_allocation;
      const Status status = scatter(child, child_whole, use_modifier, pass);
      if (!status.ok()) {
        return status;
      }
    }
    return Status::success();
  }

  [[nodiscard]] Status distribute(std::uint64_t whole) {
    std::vector<AllocationInput> root_inputs;
    root_inputs.reserve(roots.size());
    for (const std::size_t root : roots) {
      const FairnessGroup& group = policy->groups[root];
      AllocationInput input;
      input.key = composite_key(true, group.id.value());
      input.weight = effective_share_weight(group.share_weight, 0);
      input.floor = group.guarantee_floor;
      input.obligation_rank = group.obligation_rank;
      root_inputs.push_back(input);
    }
    for (int pass_index = 0; pass_index < 2; ++pass_index) {
      const bool use_modifier = pass_index == 1;
      PassState& pass = use_modifier ? effective_pass : base_pass;
      Result<AllocationResult> allocated = allocate_whole(root_inputs, whole);
      if (!allocated.ok()) {
        return allocated.status();
      }
      const AllocationResult& result = allocated.value();
      if (result.overcommit) {
        pass.overcommit = true;
        if (!checked_add(pass.overcommit_shortfall, result.overcommit_shortfall,
                         pass.overcommit_shortfall)) {
          return Status(StatusCode::ArithmeticOverflow, "overcommit shortfall overflows");
        }
      }
      for (std::size_t r = 0; r < roots.size(); ++r) {
        if (use_modifier) {
          nodes[roots[r]].allocation = result.allocations[r];
        } else {
          nodes[roots[r]].base_allocation = result.allocations[r];
        }
      }
      for (std::size_t r = 0; r < roots.size(); ++r) {
        const std::uint64_t root_whole =
            use_modifier ? nodes[roots[r]].allocation : nodes[roots[r]].base_allocation;
        const Status status = scatter(roots[r], root_whole, use_modifier, pass);
        if (!status.ok()) {
          return status;
        }
      }
    }
    return Status::success();
  }

  /// Sums the served units of every subject under each group, bottom-up.
  void accumulate_served(const std::vector<std::uint64_t>& served) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      nodes[i].served = 0;
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      std::uint64_t total = 0;
      for (const std::size_t child : nodes[i].child_subjects) {
        total += served[child];
      }
      std::size_t cursor = i;
      bool guard = true;
      while (guard) {
        nodes[cursor].served += total;
        if (!nodes[cursor].has_parent) {
          guard = false;
        } else {
          cursor = nodes[cursor].parent_node;
        }
      }
    }
  }

  /// True when the group or any of its descendants is a protected obligation.
  [[nodiscard]] bool subtree_has_protected(std::size_t node_index) const {
    const FairnessGroup& group = policy->groups[node_index];
    if (group.protected_obligation) {
      return true;
    }
    for (const std::size_t child : nodes[node_index].child_groups) {
      if (subtree_has_protected(child)) {
        return true;
      }
    }
    return false;
  }
};

// --- Per-subject authority ---------------------------------------------------

struct SubjectAuthority {
  bool has_authority{false};
  Outcome outcome{Outcome::Unknown};
  ReasonCode reason{ReasonCode::EvidenceAbsent};
  std::uint64_t served{0};
  std::uint32_t observed_unserved_streak{0};
  std::uint32_t observed_below_floor_streak{0};
  PriorityClassRef observed_priority{};
  QosClassRef observed_qos{};
};

[[nodiscard]] bool same_priority(const PriorityClassRef& a, const PriorityClassRef& b) noexcept {
  return a.id == b.id && a.generation == b.generation;
}

[[nodiscard]] bool same_qos(const QosClassRef& a, const QosClassRef& b) noexcept {
  return a.id == b.id && a.generation == b.generation;
}

}  // namespace

// --- Public allocation primitive ---------------------------------------------

std::uint64_t effective_share_weight(std::uint32_t weight, std::int32_t modifier_bps) noexcept {
  const std::int64_t factor = kBasisPointScale + static_cast<std::int64_t>(modifier_bps);
  const std::uint64_t scale = factor <= 0 ? 1ULL : static_cast<std::uint64_t>(factor);
  // weight <= 2^31, scale <= 2^31 in the widest declared domain, so the product
  // always fits in 64 bits; mul_wide keeps the reasoning explicit and portable.
  const U128 product = mul_wide(static_cast<std::uint64_t>(weight), scale);
  if (!product.fits_u64() || product.lo == 0) {
    return 1;
  }
  return product.lo;
}

Result<AllocationResult> allocate_whole(const std::vector<AllocationInput>& children,
                                        std::uint64_t whole) {
  if (children.empty()) {
    return Status(StatusCode::InvalidArgument, "allocation requires at least one competitor");
  }
  if (children.size() > static_cast<std::size_t>(kMaxSubjects) + kMaxGroups) {
    return Status(StatusCode::LimitExceeded, "allocation competitor count exceeds the bound");
  }
  std::uint64_t weight_sum = 0;
  for (const AllocationInput& child : children) {
    if (child.weight == 0) {
      return Status(StatusCode::OutOfRange, "share weight must be >= 1");
    }
    if (!checked_add(weight_sum, static_cast<std::uint64_t>(child.weight), weight_sum)) {
      return Status(StatusCode::ArithmeticOverflow, "share weight sum overflows");
    }
  }

  const std::size_t count = children.size();
  std::vector<std::uint64_t> share(count, 0);
  std::vector<std::uint64_t> residue(count, 0);
  std::vector<std::uint64_t> allocation(count, 0);
  std::uint64_t assigned = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const MulDivResult computed = mul_div(whole, children[i].weight, weight_sum);
    if (!computed.ok()) {
      return Status(StatusCode::ArithmeticOverflow, "weighted share overflowed");
    }
    share[i] = computed.value;
    residue[i] = computed.remainder;
    if (!checked_add(assigned, share[i], assigned)) {
      return Status(StatusCode::ArithmeticOverflow, "assigned total overflows");
    }
  }
  if (assigned > whole) {
    return Status(StatusCode::Internal, "weighted shares exceeded the whole");
  }
  const std::uint64_t leftover = whole - assigned;

  std::uint64_t need = 0;
  std::uint64_t slack = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (children[i].floor > share[i]) {
      if (!checked_add(need, children[i].floor - share[i], need)) {
        return Status(StatusCode::ArithmeticOverflow, "floor shortfall overflows");
      }
    } else {
      if (!checked_add(slack, share[i] - children[i].floor, slack)) {
        return Status(StatusCode::ArithmeticOverflow, "floor slack overflows");
      }
    }
  }

  AllocationResult result;
  result.allocations.assign(count, 0);
  result.floor_binding.assign(count, 0);

  if (need == 0) {
    allocation = share;
  } else if (need <= slack) {
    allocation = share;
    std::uint64_t remaining = need;
    std::vector<std::size_t> donors;
    donors.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      if (share[i] > children[i].floor) {
        donors.push_back(i);
      }
    }
    std::sort(donors.begin(), donors.end(), [&](std::size_t a, std::size_t b) {
      const std::uint64_t slack_a = share[a] - children[a].floor;
      const std::uint64_t slack_b = share[b] - children[b].floor;
      if (slack_a != slack_b) {
        return slack_a > slack_b;
      }
      if (children[a].obligation_rank != children[b].obligation_rank) {
        return children[a].obligation_rank < children[b].obligation_rank;
      }
      return children[a].key < children[b].key;
    });
    for (const std::size_t index : donors) {
      if (remaining == 0) {
        break;
      }
      const std::uint64_t room = share[index] - children[index].floor;
      const std::uint64_t take = std::min(room, remaining);
      allocation[index] -= take;
      remaining -= take;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (children[i].floor > share[i]) {
        allocation[i] = children[i].floor;
        result.floor_binding[i] = 1;
      }
    }
    if (remaining != 0) {
      return Status(StatusCode::Internal, "floor reallocation did not close");
    }
  } else {
    result.overcommit = true;
    std::uint64_t available = whole;
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      if (children[a].obligation_rank != children[b].obligation_rank) {
        return children[a].obligation_rank > children[b].obligation_rank;
      }
      if (children[a].floor != children[b].floor) {
        return children[a].floor > children[b].floor;
      }
      return children[a].key < children[b].key;
    });
    for (const std::size_t index : order) {
      const std::uint64_t grant = std::min(children[index].floor, available);
      allocation[index] = grant;
      available -= grant;
    }
    std::uint64_t extra_assigned = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const MulDivResult computed = mul_div(available, children[i].weight, weight_sum);
      if (!computed.ok()) {
        return Status(StatusCode::ArithmeticOverflow, "overcommit share overflowed");
      }
      allocation[i] += computed.value;
      residue[i] = computed.remainder;
      if (!checked_add(extra_assigned, computed.value, extra_assigned)) {
        return Status(StatusCode::ArithmeticOverflow, "overcommit total overflows");
      }
    }
    const std::uint64_t extra_left = available - extra_assigned;
    if (extra_left >= static_cast<std::uint64_t>(count) + 1) {
      return Status(StatusCode::Internal, "overcommit rounding residue exceeded the bound");
    }
    std::vector<std::size_t> extra_order(count);
    std::iota(extra_order.begin(), extra_order.end(), std::size_t{0});
    std::sort(extra_order.begin(), extra_order.end(), [&](std::size_t a, std::size_t b) {
      if (residue[a] != residue[b]) {
        return residue[a] > residue[b];
      }
      return children[a].key < children[b].key;
    });
    for (std::size_t k = 0; k < extra_left; ++k) {
      allocation[extra_order[k]] += 1;
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (children[i].floor > allocation[i]) {
        if (!checked_add(result.overcommit_shortfall, children[i].floor - allocation[i],
                         result.overcommit_shortfall)) {
          return Status(StatusCode::ArithmeticOverflow, "overcommit shortfall overflows");
        }
      } else if (children[i].floor > share[i]) {
        result.floor_binding[i] = 1;
      }
    }
  }

  if (!result.overcommit && leftover > 0) {
    if (leftover >= static_cast<std::uint64_t>(count) + 1) {
      return Status(StatusCode::Internal, "rounding residue exceeded the competitor count");
    }
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
      if (residue[a] != residue[b]) {
        return residue[a] > residue[b];
      }
      return children[a].key < children[b].key;
    });
    for (std::size_t k = 0; k < leftover; ++k) {
      allocation[order[k]] += 1;
    }
  }

  std::uint64_t total = 0;
  for (const std::uint64_t value : allocation) {
    if (!checked_add(total, value, total)) {
      return Status(StatusCode::ArithmeticOverflow, "allocation total overflows");
    }
  }
  if (total != whole) {
    return Status(StatusCode::Internal, "allocation did not close over the whole");
  }

  result.allocations = std::move(allocation);
  return result;
}

// --- Pure evaluation ----------------------------------------------------------

namespace {

/// Everything the planner needs about one subject after entitlement is known.
struct SubjectPlan {
  bool has_authority{false};
  Outcome outcome{Outcome::Unknown};
  ReasonCode reason{ReasonCode::EvidenceAbsent};
  std::uint64_t served{0};
  std::uint64_t entitlement{0};
  std::uint64_t base_entitlement{0};
  std::uint64_t group_entitlement{0};
  bool floor_binding{false};
  std::uint64_t this_window_deficit{0};
  std::uint64_t effective_deficit{0};
  std::uint64_t surplus{0};
  std::int64_t deviation{0};
  std::uint32_t unserved_streak{0};
  std::uint32_t below_floor_streak{0};
  bool starving{false};
  bool cooling{false};
  std::uint64_t demand{0};
  std::uint64_t reducible{0};
  std::uint64_t withheld{0};
  std::uint64_t augment{0};
  std::uint64_t reduce{0};
  bool blocked{false};
  CarryOutcome carry{};
  PriorityClassRef observed_priority{};
  QosClassRef observed_qos{};
};

constexpr std::uint32_t kStreakSaturation = 1000000u;

[[nodiscard]] std::uint32_t bump_streak(std::uint32_t value) noexcept {
  if (value >= kStreakSaturation) {
    return kStreakSaturation;
  }
  return value + 1;
}

[[nodiscard]] std::uint64_t intervention_id(std::uint64_t window, std::uint64_t subject,
                                            std::uint8_t direction,
                                            std::uint32_t attempt) noexcept {
  std::uint8_t bytes[21];
  store_le<std::uint64_t>(bytes + 0, window);
  store_le<std::uint64_t>(bytes + 8, subject);
  bytes[16] = direction;
  store_le<std::uint32_t>(bytes + 17, attempt);
  const std::uint64_t digest = fnv1a64(bytes, sizeof(bytes));
  return digest == 0 ? 1 : digest;
}

}  // namespace

Result<FairnessDecision> evaluate_fairness(const EvaluationContext& context) {
  if (context.policy == nullptr || context.accounting == nullptr) {
    return Status(StatusCode::InvalidArgument, "policy and accounting are required");
  }
  const FairnessPolicy& policy = *context.policy;
  const FairnessAccounting& accounting = *context.accounting;
  const EvaluationRequest& request = context.request;

  FairnessDecision decision;
  decision.authority.policy_id = policy.id;
  decision.authority.policy_generation = policy.generation;
  decision.authority.epoch = context.current_epoch;
  decision.authority.evidence_id = request.evidence_id;
  decision.authority.evidence_generation = request.evidence_generation;
  decision.authority.window = request.window;
  decision.authority.window_generation = request.window_generation;
  decision.authority.accounting_generation = accounting.generation;
  decision.authority.request_id = request.request_id;
  decision.authority.attempt = request.attempt;

  Distributor distributor;
  const Status built = distributor.build(policy);
  if (!built.ok()) {
    return built;
  }

  // Index the durable accounting and the governed subject set once. Looking a
  // subject up linearly inside the per-subject loops would make evaluation
  // quadratic in the population size, which the benchmark exposes directly.
  std::unordered_map<std::uint64_t, const SubjectAccounting*> accounting_index;
  accounting_index.reserve(accounting.subjects.size() * 2 + 1);
  for (const SubjectAccounting& entry : accounting.subjects) {
    accounting_index.emplace(entry.id.value(), &entry);
  }
  const auto find_accounting = [&accounting_index](SubjectId wanted) {
    const auto it = accounting_index.find(wanted.value());
    return it == accounting_index.end() ? nullptr : it->second;
  };
  std::unordered_set<std::uint64_t> governed_subjects;
  governed_subjects.reserve(policy.subjects.size() * 2 + 1);
  for (const Subject& subject : policy.subjects) {
    governed_subjects.insert(subject.id.value());
  }

  // --- Global evidence eligibility ------------------------------------------
  bool global_ok = true;
  Outcome failure_outcome = Outcome::Stale;
  ReasonCode failure_reason = ReasonCode::None;
  StatusCode failure_code = StatusCode::Ok;

  if (request.policy_id != policy.id || request.policy_generation != policy.generation) {
    global_ok = false;
    failure_outcome = Outcome::Stale;
    failure_reason = ReasonCode::PolicyGenerationMismatch;
    failure_code = StatusCode::PolicyGenerationMismatch;
  } else if (!context.current_epoch.valid() || policy.epoch != context.current_epoch ||
             request.epoch != context.current_epoch) {
    global_ok = false;
    failure_outcome = Outcome::Stale;
    failure_reason = ReasonCode::EvidenceStaleEpoch;
    failure_code = StatusCode::StaleEpoch;
  } else if (context.evidence == nullptr) {
    global_ok = false;
    failure_outcome = Outcome::Unknown;
    failure_reason = ReasonCode::EvidenceAbsent;
    failure_code = StatusCode::EvidenceMissing;
  } else {
    const EvidenceSnapshot& evidence = *context.evidence;
    if (evidence.id != request.evidence_id || evidence.generation != request.evidence_generation) {
      global_ok = false;
      failure_outcome = Outcome::Stale;
      failure_reason = ReasonCode::EvidenceStaleGeneration;
      failure_code = StatusCode::StaleEvidence;
    } else if (evidence.epoch != context.current_epoch) {
      global_ok = false;
      failure_outcome = Outcome::Stale;
      failure_reason = ReasonCode::EvidenceStaleEpoch;
      failure_code = StatusCode::StaleEpoch;
    } else if (evidence.window != request.window ||
               evidence.window_generation != request.window_generation) {
      global_ok = false;
      failure_outcome = Outcome::Stale;
      failure_reason = ReasonCode::EvidenceStaleWindow;
      failure_code = StatusCode::StaleEvidence;
    } else if (context.last_committed_window.valid() && request.window.valid() &&
               request.window.value() <= context.last_committed_window.value()) {
      global_ok = false;
      failure_outcome = Outcome::Stale;
      failure_reason = ReasonCode::EvidenceStaleWindow;
      failure_code = StatusCode::StaleEvidence;
    } else if (context.current_window.valid() && request.window.valid()) {
      if (request.window.value() > context.current_window.value()) {
        global_ok = false;
        failure_outcome = Outcome::Stale;
        failure_reason = ReasonCode::EvidenceStaleWindow;
        failure_code = StatusCode::EvidenceMissing;
      } else if (context.current_window.value() - request.window.value() >
                 policy.max_evidence_age_windows) {
        global_ok = false;
        failure_outcome = Outcome::Stale;
        failure_reason = ReasonCode::EvidenceStaleAge;
        failure_code = StatusCode::StaleEvidence;
      }
    }
    if (global_ok && policy.max_evidence_age_ns > 0) {
      if (evidence.captured_at_ns > request.now_ns) {
        global_ok = false;
        failure_outcome = Outcome::Stale;
        failure_reason = ReasonCode::EvidenceStaleAge;
        failure_code = StatusCode::StaleEvidence;
      } else if (static_cast<std::uint64_t>(request.now_ns - evidence.captured_at_ns) >
                 policy.max_evidence_age_ns) {
        global_ok = false;
        failure_outcome = Outcome::Stale;
        failure_reason = ReasonCode::EvidenceStaleAge;
        failure_code = StatusCode::StaleEvidence;
      }
    }
  }

  const std::size_t subject_count = policy.subjects.size();
  std::vector<SubjectPlan> plans(subject_count);

  if (!global_ok) {
    for (std::size_t i = 0; i < subject_count; ++i) {
      SubjectPlan& plan = plans[i];
      plan.has_authority = false;
      plan.outcome = (failure_outcome == Outcome::Unknown) ? Outcome::Unknown : Outcome::Stale;
      if (failure_outcome == Outcome::Unknown && !request.treat_missing_as_unknown) {
        plan.outcome = Outcome::Stale;
      }
      plan.reason = failure_reason;
    }
    add_note(decision.notes, failure_code, failure_reason, SubjectId{},
             "no evidence authority for this request");
  } else {
    const EvidenceSnapshot& evidence = *context.evidence;
    for (std::size_t i = 0; i < subject_count; ++i) {
      const Subject& subject = policy.subjects[i];
      SubjectPlan& plan = plans[i];
      const SubjectObservation* observation = evidence.find(subject.id);
      if (observation == nullptr) {
        plan.outcome = request.treat_missing_as_unknown ? Outcome::Unknown : Outcome::Stale;
        plan.reason = ReasonCode::EvidenceAbsent;
        continue;
      }
      if (observation->generation != subject.generation) {
        plan.outcome = Outcome::Stale;
        plan.reason = ReasonCode::SubjectGenerationMismatch;
        continue;
      }
      if (subject.priority.present() && !same_priority(observation->priority, subject.priority)) {
        plan.outcome = Outcome::Stale;
        plan.reason = ReasonCode::PriorityReferenceMismatch;
        continue;
      }
      if (subject.qos.present() && !same_qos(observation->qos, subject.qos)) {
        plan.outcome = Outcome::Stale;
        plan.reason = ReasonCode::PriorityReferenceMismatch;
        continue;
      }
      const SubjectAccounting* entry = find_accounting(subject.id);
      if (entry != nullptr && entry->generation != subject.generation) {
        plan.outcome = Outcome::Stale;
        plan.reason = ReasonCode::SubjectGenerationMismatch;
        continue;
      }
      plan.has_authority = true;
      plan.outcome = Outcome::Fair;
      plan.reason = ReasonCode::WithinFairBand;
      plan.served = observation->served_units;
      plan.observed_priority = observation->priority;
      plan.observed_qos = observation->qos;
    }
    for (const SubjectObservation& observation : evidence.observations) {
      if (governed_subjects.find(observation.id.value()) == governed_subjects.end()) {
        add_note(decision.notes, StatusCode::NotFound, ReasonCode::None, observation.id,
                 "evidence names a subject that the policy does not govern; ignored");
      }
    }
  }

  // --- Whole to distribute ---------------------------------------------------
  std::uint64_t whole = 0;
  {
    std::vector<std::uint64_t> served(subject_count, 0);
    for (std::size_t i = 0; i < subject_count; ++i) {
      served[i] = plans[i].has_authority ? plans[i].served : 0;
      if (!checked_add(whole, served[i], whole)) {
        return Status(StatusCode::ArithmeticOverflow, "total observed service overflows");
      }
    }
    const Status distributed = distributor.distribute(whole);
    if (!distributed.ok()) {
      return distributed;
    }
  }

  if (distributor.effective_pass.overcommit) {
    add_note(decision.notes, StatusCode::PolicyOvercommit, ReasonCode::OvercommitDetected,
             SubjectId{}, "declared guarantee floors exceed the whole being distributed");
  }

  // --- Per-subject authoritative numbers ------------------------------------
  for (std::size_t i = 0; i < subject_count; ++i) {
    SubjectPlan& plan = plans[i];
    if (!plan.has_authority) {
      continue;
    }
    const Subject& subject = policy.subjects[i];
    plan.entitlement = distributor.subject_effective[i];
    plan.base_entitlement = distributor.subject_base[i];
    plan.floor_binding = distributor.subject_floor_binding[i] != 0;
    const auto group_it = distributor.by_id.find(subject.group.value());
    const std::size_t node_index = group_it == distributor.by_id.end() ? 0 : group_it->second;
    plan.group_entitlement = distributor.nodes[node_index].allocation;

    const SubjectAccounting* entry = find_accounting(subject.id);
    const std::uint64_t carried_deficit =
        entry == nullptr ? 0 : std::min(entry->cumulative_deficit, policy.max_carry_units);
    const std::uint64_t this_window_deficit =
        plan.served < plan.entitlement ? plan.entitlement - plan.served : 0;
    const std::uint64_t this_window_surplus =
        plan.served > plan.entitlement ? plan.served - plan.entitlement : 0;
    plan.surplus = this_window_surplus;
    std::uint64_t effective_deficit = 0;
    if (!checked_add(this_window_deficit, carried_deficit, effective_deficit)) {
      return Status(StatusCode::ArithmeticOverflow, "effective deficit overflows");
    }
    plan.effective_deficit = effective_deficit;
    plan.this_window_deficit = this_window_deficit;
    plan.deviation = static_cast<std::int64_t>(plan.served) - static_cast<std::int64_t>(plan.entitlement);

    const std::uint32_t durable_unserved = entry == nullptr ? 0 : entry->unserved_streak;
    const std::uint32_t durable_below = entry == nullptr ? 0 : entry->below_floor_streak;
    const std::uint32_t after_unserved = plan.served == 0 ? bump_streak(durable_unserved) : 0;
    const std::uint32_t after_below =
        (subject.guarantee_floor > 0 && plan.served < subject.guarantee_floor)
            ? bump_streak(durable_below)
            : 0;
    plan.unserved_streak = after_unserved;
    plan.below_floor_streak = after_below;

    // The carry is driven by entitlement minus observed service: a positive net
    // is service still owed, a negative net is credit against future service.
    const std::int64_t net = static_cast<std::int64_t>(plan.entitlement) -
                             static_cast<std::int64_t>(plan.served);
    CarryOutcome carry;
    if (!apply_window_carry(entry == nullptr ? SubjectAccounting{} : *entry, net,
                            policy.max_carry_units, carry)) {
      return Status(StatusCode::ArithmeticOverflow, "carry application overflows");
    }
    plan.carry = carry;
  }

  // Observations carry their own streaks; the governor trusts the larger of the
  // two so that a producer cannot reset a starvation streak by restarting.
  if (global_ok && context.evidence != nullptr) {
    for (std::size_t i = 0; i < subject_count; ++i) {
      SubjectPlan& plan = plans[i];
      if (!plan.has_authority) {
        continue;
      }
      const SubjectObservation* observation = context.evidence->find(policy.subjects[i].id);
      if (observation == nullptr) {
        continue;
      }
      plan.unserved_streak = std::max(plan.unserved_streak, observation->unserved_streak);
      plan.below_floor_streak = std::max(plan.below_floor_streak, observation->below_floor_streak);
      const Subject& subject = policy.subjects[i];
      const bool unserved_starving = subject.starvation_windows > 0 &&
                                     plan.unserved_streak >= subject.starvation_windows;
      const bool floor_starving = subject.guarantee_starvation_windows > 0 &&
                                  subject.guarantee_floor > 0 &&
                                  plan.below_floor_streak >= subject.guarantee_starvation_windows;
      plan.starving = unserved_starving || floor_starving;
      if (plan.starving) {
        plan.reason = unserved_starving ? ReasonCode::StarvationUnserved
                                        : ReasonCode::StarvationBelowFloor;
      }
    }
  }

  // --- Demand, supply, bounds ------------------------------------------------
  for (std::size_t i = 0; i < subject_count; ++i) {
    SubjectPlan& plan = plans[i];
    if (!plan.has_authority) {
      continue;
    }
    const Subject& subject = policy.subjects[i];
    const SubjectAccounting* entry = find_accounting(subject.id);
    if (policy.cooldown_windows > 0 && entry != nullptr && entry->last_correction_window.valid() &&
        request.window.valid() &&
        request.window.value() >= entry->last_correction_window.value() &&
        request.window.value() - entry->last_correction_window.value() < policy.cooldown_windows &&
        !plan.starving) {
      plan.cooling = true;
    }

    const bool demand_side = plan.effective_deficit > policy.fair_band_units;
    if (demand_side && !plan.cooling) {
      plan.demand = std::min({plan.effective_deficit, subject.max_augment_units, kMaxServiceUnits});
    }

    const bool supply_side = plan.surplus > 0;
    if (supply_side) {
      const std::uint64_t cap = std::min(plan.surplus, subject.max_reduce_units);
      std::uint64_t allowed = cap;
      if (subject.protected_obligation) {
        const std::uint64_t headroom =
            plan.served > subject.guarantee_floor ? plan.served - subject.guarantee_floor : 0;
        allowed = std::min(cap, headroom);
      }
      plan.reducible = allowed;
      plan.withheld = cap - allowed;
    }
  }

  std::uint64_t pool = 0;
  std::uint64_t demand_total = 0;
  std::uint64_t withheld_total = 0;
  for (const SubjectPlan& plan : plans) {
    if (!checked_add(pool, plan.reducible, pool) ||
        !checked_add(demand_total, plan.demand, demand_total) ||
        !checked_add(withheld_total, plan.withheld, withheld_total)) {
      return Status(StatusCode::ArithmeticOverflow, "correction totals overflow");
    }
  }

  std::uint64_t budget_bps = kMaxServiceUnits * static_cast<std::uint64_t>(kMaxSubjects);
  if (policy.max_correction_bps < 100000u) {
    const MulDivResult computed =
        mul_div(whole, policy.max_correction_bps, static_cast<std::uint64_t>(kBasisPointScale));
    if (!computed.ok()) {
      return Status(StatusCode::ArithmeticOverflow, "basis point budget overflows");
    }
    budget_bps = computed.value;
  }

  std::uint64_t authorized = std::min({pool, demand_total, policy.max_correction_units, budget_bps});
  decision.correction.reducible_pool_units = pool;
  decision.correction.demand_units = demand_total;
  decision.correction.authorized_budget_units = authorized;
  decision.correction.withheld_units = withheld_total;
  decision.correction.bounded_by_policy_total =
      policy.max_correction_units < std::min(pool, demand_total) &&
      policy.max_correction_units <= budget_bps;
  decision.correction.bounded_by_policy_bps =
      budget_bps < std::min(pool, demand_total) && budget_bps < policy.max_correction_units;
  decision.correction.authority_missing = !global_ok;

  // Augmentation is granted in a fixed order: starvation first, then the largest
  // recognized deficit, then the stronger obligation, then identity.
  std::vector<std::size_t> demand_order;
  demand_order.reserve(subject_count);
  for (std::size_t i = 0; i < subject_count; ++i) {
    if (plans[i].has_authority && plans[i].demand > 0) {
      demand_order.push_back(i);
    }
  }
  std::sort(demand_order.begin(), demand_order.end(), [&](std::size_t a, std::size_t b) {
    if (plans[a].starving != plans[b].starving) {
      return plans[a].starving;
    }
    if (plans[a].demand != plans[b].demand) {
      return plans[a].demand > plans[b].demand;
    }
    if (policy.subjects[a].obligation_rank != policy.subjects[b].obligation_rank) {
      return policy.subjects[a].obligation_rank > policy.subjects[b].obligation_rank;
    }
    return policy.subjects[a].id.value() < policy.subjects[b].id.value();
  });

  std::uint64_t remaining = authorized;
  for (const std::size_t index : demand_order) {
    if (remaining == 0) {
      break;
    }
    const std::uint64_t grant = std::min(plans[index].demand, remaining);
    plans[index].augment = grant;
    remaining -= grant;
  }
  const std::uint64_t augment_total = authorized - remaining;
  if (augment_total != authorized) {
    return Status(StatusCode::Internal, "augmentation did not consume the authorized budget");
  }

  std::vector<std::size_t> supply_order;
  supply_order.reserve(subject_count);
  for (std::size_t i = 0; i < subject_count; ++i) {
    if (plans[i].has_authority && plans[i].reducible > 0) {
      supply_order.push_back(i);
    }
  }
  std::sort(supply_order.begin(), supply_order.end(), [&](std::size_t a, std::size_t b) {
    if (plans[a].reducible != plans[b].reducible) {
      return plans[a].reducible > plans[b].reducible;
    }
    if (policy.subjects[a].obligation_rank != policy.subjects[b].obligation_rank) {
      return policy.subjects[a].obligation_rank < policy.subjects[b].obligation_rank;
    }
    return policy.subjects[a].id.value() < policy.subjects[b].id.value();
  });
  std::uint64_t reduce_remaining = authorized;
  for (const std::size_t index : supply_order) {
    if (reduce_remaining == 0) {
      break;
    }
    const std::uint64_t take = std::min(plans[index].reducible, reduce_remaining);
    plans[index].reduce = take;
    reduce_remaining -= take;
  }
  if (reduce_remaining != 0) {
    return Status(StatusCode::Internal, "reductions did not cover the authorized budget");
  }

  // Unsatisfied demand is measured against the recognized deficit, not against
  // the per-subject ask that the caps already reduced. It therefore answers the
  // operator's real question: how much service is this subject still short of,
  // after everything the governor is allowed to propose? A subject that is
  // deliberately cooling down is excluded: that shortfall is policy hysteresis,
  // not unmet demand.
  std::uint64_t unsatisfied = 0;
  for (std::size_t i = 0; i < subject_count; ++i) {
    SubjectPlan& plan = plans[i];
    if (!plan.has_authority || plan.cooling || plan.effective_deficit <= policy.fair_band_units) {
      continue;
    }
    const std::uint64_t unmet = plan.effective_deficit - plan.augment;
    if (!checked_add(unsatisfied, unmet, unsatisfied)) {
      return Status(StatusCode::ArithmeticOverflow, "unsatisfied demand overflows");
    }
  }
  // The plan is obligation-limited only when units that could have funded the
  // demand were actually held back by a protected obligation.
  const bool blocked = unsatisfied > 0 && withheld_total > 0 && authorized < demand_total;
  decision.correction.protected_obligation_blocked = blocked;
  decision.correction.augment_units = authorized;
  decision.correction.reduce_units = authorized;
  decision.correction.unsatisfied_demand_units = unsatisfied;
  for (const std::size_t index : demand_order) {
    SubjectPlan& plan = plans[index];
    const std::uint64_t subject_cap = policy.subjects[index].max_augment_units;
    const std::uint64_t cap_unmet = plan.effective_deficit > subject_cap
                                        ? plan.effective_deficit - subject_cap
                                        : 0;
    const std::uint64_t unmet = plan.effective_deficit - plan.augment;
    // Only the part of the shortfall that this subject's own cap does not
    // explain can be attributed to the withheld budget.
    plan.blocked = blocked && unmet > cap_unmet;
  }

  // --- Outcome assignment ----------------------------------------------------
  for (std::size_t i = 0; i < subject_count; ++i) {
    SubjectPlan& plan = plans[i];
    if (!plan.has_authority) {
      continue;
    }
    const Subject& subject = policy.subjects[i];
    const bool deficit_side = plan.effective_deficit > policy.fair_band_units;
    const bool surplus_side = plan.surplus > policy.fair_band_units;
    const ReasonCode deficit_reason =
        plan.floor_binding ? ReasonCode::GuaranteeFloorDeficit : ReasonCode::WeightedShareDeficit;

    if (plan.starving) {
      if (plan.blocked) {
        // A stronger obligation is what stands between this subject and the
        // service it needs; that is reported ahead of the starvation state.
        plan.outcome = Outcome::BlockedByStrongerObligation;
        plan.reason = ReasonCode::ProtectedObligationHeld;
      } else {
        plan.outcome = Outcome::StarvationRisk;
      }
      continue;
    }
    if (deficit_side) {
      const bool severe = plan.effective_deficit >= policy.correction_threshold_units;
      if (plan.blocked) {
        plan.outcome = Outcome::BlockedByStrongerObligation;
        plan.reason = ReasonCode::ProtectedObligationHeld;
      } else if (plan.augment > 0) {
        plan.outcome = severe ? Outcome::CorrectionRequired : Outcome::UnderServed;
        plan.reason = deficit_reason;
      } else {
        plan.outcome = Outcome::UnderServed;
        if (plan.cooling) {
          plan.reason = ReasonCode::CorrectiveCooldown;
        } else if (subject.max_augment_units == 0) {
          plan.reason = ReasonCode::PolicyBoundReached;
        } else if (plan.effective_deficit > plan.demand) {
          plan.reason = ReasonCode::SubjectCapReached;
        } else {
          plan.reason = ReasonCode::CorrectiveBudgetExhausted;
        }
      }
      continue;
    }
    if (surplus_side) {
      plan.outcome = Outcome::OverServed;
      plan.reason = plan.floor_binding ? ReasonCode::GuaranteeFloorSurplus
                                       : ReasonCode::WeightedShareSurplus;
      continue;
    }
    plan.outcome = Outcome::Fair;
    plan.reason = ReasonCode::WithinFairBand;
  }

  // --- Materialize subject states -------------------------------------------
  decision.subjects.reserve(subject_count);
  for (std::size_t i = 0; i < subject_count; ++i) {
    const Subject& subject = policy.subjects[i];
    const SubjectPlan& plan = plans[i];
    SubjectFairnessState state;
    state.id = subject.id;
    state.generation = subject.generation;
    state.group = subject.group;
    state.label = subject.label;
    state.outcome = plan.outcome;
    state.reason = plan.reason;
    state.has_authority = plan.has_authority;
    state.is_protected_obligation = subject.protected_obligation;
    state.obligation_rank = subject.obligation_rank;
    state.guarantee_floor_units = subject.guarantee_floor;
    state.served_units = plan.served;
    state.entitlement_units = plan.entitlement;
    state.base_entitlement_units = plan.base_entitlement;
    state.group_entitlement_units = plan.group_entitlement;
    state.deviation_units = plan.deviation;
    state.deficit_units = plan.this_window_deficit;
    state.surplus_units = plan.surplus;
    state.cumulative_deficit_after = plan.carry.cumulative_deficit;
    state.cumulative_surplus_after = plan.carry.cumulative_surplus;
    state.unserved_streak = plan.unserved_streak;
    state.below_floor_streak = plan.below_floor_streak;
    state.starvation_limit_windows = subject.starvation_windows;
    state.guarantee_starvation_limit_windows = subject.guarantee_starvation_windows;
    state.proposed_augment_units = plan.augment;
    state.proposed_reduce_units = plan.reduce;
    state.unsatisfied_demand_units =
        plan.effective_deficit > policy.fair_band_units ? plan.effective_deficit - plan.augment : 0;
    state.expected_priority = subject.priority;
    state.observed_priority = plan.observed_priority;
    const std::int64_t delta = static_cast<std::int64_t>(plan.entitlement) -
                               static_cast<std::int64_t>(plan.base_entitlement);
    state.priority_delta_units = delta;
    state.applied_modifier_bps = subject.priority_modifier_bps;
    state.modifier_clipped = false;
    if (subject.priority_modifier_bps > static_cast<std::int32_t>(policy.max_priority_modifier_bps)) {
      state.applied_modifier_bps = static_cast<std::int32_t>(policy.max_priority_modifier_bps);
      state.modifier_clipped = true;
    } else if (subject.priority_modifier_bps <
               -static_cast<std::int32_t>(policy.max_priority_modifier_bps)) {
      state.applied_modifier_bps = -static_cast<std::int32_t>(policy.max_priority_modifier_bps);
      state.modifier_clipped = true;
    }
    (void)plan.observed_qos;
    decision.subjects.push_back(std::move(state));
  }

  // --- Group rollups ---------------------------------------------------------
  {
    std::vector<std::uint64_t> group_withheld(distributor.nodes.size(), 0);
    for (std::size_t i = 0; i < subject_count; ++i) {
      const Subject& subject = policy.subjects[i];
      const auto it = distributor.by_id.find(subject.group.value());
      if (it == distributor.by_id.end()) {
        continue;
      }
      std::size_t cursor = it->second;
      for (;;) {
        if (plans[i].has_authority) {
          distributor.nodes[cursor].served += plans[i].served;
        }
        if (plans[i].withheld > 0) {
          group_withheld[cursor] += plans[i].withheld;
        }
        if (!distributor.nodes[cursor].has_parent) {
          break;
        }
        cursor = distributor.nodes[cursor].parent_node;
      }
    }
    decision.groups.reserve(distributor.nodes.size());
    for (std::size_t g = 0; g < distributor.nodes.size(); ++g) {
      const FairnessGroup& group = policy.groups[g];
      GroupFairnessState state;
      state.id = group.id;
      state.generation = group.generation;
      state.parent = group.parent;
      state.depth = distributor.nodes[g].depth;
      state.entitlement_units = distributor.nodes[g].allocation;
      state.served_units = distributor.nodes[g].served;
      state.deviation_units = static_cast<std::int64_t>(state.served_units) -
                              static_cast<std::int64_t>(state.entitlement_units);
      state.is_protected_obligation = group.protected_obligation;
      state.obligation_rank = group.obligation_rank;
      state.guarantee_floor_units = group.guarantee_floor;
      state.withheld_correction = group_withheld[g] > 0;
      state.label = group.label;
      decision.groups.push_back(std::move(state));
    }
  }

  // --- Bounded corrective intent --------------------------------------------
  InterventionGeneration generation = accounting.intervention_generation;
  const std::uint64_t window_value = request.window.value();
  auto next_generation = [&]() {
    generation = generation.next();
    return generation;
  };
  for (const std::size_t index : demand_order) {
    const SubjectPlan& plan = plans[index];
    if (plan.augment == 0) {
      continue;
    }
    if (decision.intents.size() >= kMaxCorrectiveIntents) {
      return Status(StatusCode::LimitExceeded, "corrective intent count exceeds the bound");
    }
    const Subject& subject = policy.subjects[index];
    CorrectiveIntent intent;
    intent.id = InterventionId::from_value(
        intervention_id(window_value, subject.id.value(),
                        static_cast<std::uint8_t>(CorrectionDirection::Augment), request.attempt));
    intent.generation = next_generation();
    intent.direction = CorrectionDirection::Augment;
    intent.subject = subject.id;
    intent.subject_generation = subject.generation;
    intent.group = subject.group;
    intent.units = plan.augment;
    intent.bound = plan.augment < plan.demand ? CorrectionBound::BudgetExhausted
                                              : CorrectionBound::DemandSatisfied;
    intent.reason = plan.reason;
    intent.from_starvation = plan.starving;
    intent.from_protected_obligation = subject.protected_obligation;
    intent.obligation_rank = subject.obligation_rank;
    decision.intents.push_back(std::move(intent));
  }
  for (const std::size_t index : supply_order) {
    const SubjectPlan& plan = plans[index];
    if (plan.reduce == 0) {
      continue;
    }
    if (decision.intents.size() >= kMaxCorrectiveIntents) {
      return Status(StatusCode::LimitExceeded, "corrective intent count exceeds the bound");
    }
    const Subject& subject = policy.subjects[index];
    CorrectiveIntent intent;
    intent.id = InterventionId::from_value(
        intervention_id(window_value, subject.id.value(),
                        static_cast<std::uint8_t>(CorrectionDirection::Reduce), request.attempt));
    intent.generation = next_generation();
    intent.direction = CorrectionDirection::Reduce;
    intent.subject = subject.id;
    intent.subject_generation = subject.generation;
    intent.group = subject.group;
    intent.units = plan.reduce;
    intent.bound = plan.reduce < plan.reducible ? CorrectionBound::BudgetExhausted
                                                : CorrectionBound::PolicyTotalBudget;
    if (plan.reduce == plan.reducible) {
      intent.bound = plan.withheld > 0 ? CorrectionBound::ProtectedObligation
                                       : CorrectionBound::DemandSatisfied;
    }
    intent.reason = ReasonCode::WeightedShareSurplus;
    intent.from_starvation = false;
    intent.from_protected_obligation = subject.protected_obligation;
    intent.obligation_rank = subject.obligation_rank;
    decision.intents.push_back(std::move(intent));
  }

  // --- Aggregation -----------------------------------------------------------
  for (const SubjectPlan& plan : plans) {
    switch (plan.outcome) {
      case Outcome::Fair: ++decision.counts.fair; break;
      case Outcome::OverServed: ++decision.counts.over_served; break;
      case Outcome::UnderServed: ++decision.counts.under_served; break;
      case Outcome::StarvationRisk: ++decision.counts.starvation_risk; break;
      case Outcome::CorrectionRequired: ++decision.counts.correction_required; break;
      case Outcome::BlockedByStrongerObligation:
        ++decision.counts.blocked_by_stronger_obligation;
        break;
      case Outcome::Unknown: ++decision.counts.unknown; break;
      case Outcome::Stale: ++decision.counts.stale; break;
    }
  }
  Outcome aggregate = Outcome::Fair;
  for (const SubjectPlan& plan : plans) {
    if (outcome_rank(plan.outcome) > outcome_rank(aggregate)) {
      aggregate = plan.outcome;
    }
  }
  decision.outcome = aggregate;
  decision.content_digest = codec::decision_digest(decision);
  return decision;
}

}  // namespace fairness_governor
