// Fairness Governor - accounting closure.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/model/accounting.hpp"

#include <algorithm>

#include "fairness_governor/core/checked.hpp"

namespace fairness_governor {

bool operator==(const SubjectAccounting& a, const SubjectAccounting& b) {
  return a.id == b.id && a.generation == b.generation &&
         a.cumulative_deficit == b.cumulative_deficit &&
         a.cumulative_surplus == b.cumulative_surplus &&
         a.discarded_deficit == b.discarded_deficit &&
         a.discarded_surplus == b.discarded_surplus && a.unserved_streak == b.unserved_streak &&
         a.below_floor_streak == b.below_floor_streak &&
         a.windows_observed == b.windows_observed && a.total_served_units == b.total_served_units &&
         a.last_correction_window == b.last_correction_window &&
         a.last_correction_generation == b.last_correction_generation;
}

bool apply_window_carry(const SubjectAccounting& current, std::int64_t net,
                           std::uint64_t max_carry, CarryOutcome& out) noexcept {
  CarryOutcome result;
  result.cumulative_deficit = current.cumulative_deficit;
  result.cumulative_surplus = current.cumulative_surplus;
  result.discarded_deficit = current.discarded_deficit;
  result.discarded_surplus = current.discarded_surplus;

  if (net >= 0) {
    std::uint64_t remaining = static_cast<std::uint64_t>(net);
    const std::uint64_t credit = std::min(remaining, result.cumulative_surplus);
    result.credit_applied = credit;
    result.cumulative_surplus -= credit;
    remaining -= credit;
    result.deficit_units = remaining;
    std::uint64_t carried = 0;
    if (!checked_add(result.cumulative_deficit, remaining, carried)) {
      return false;
    }
    result.cumulative_deficit = carried;
  } else {
    std::uint64_t remaining = magnitude(net);
    const std::uint64_t debit = std::min(remaining, result.cumulative_deficit);
    result.debit_applied = debit;
    result.cumulative_deficit -= debit;
    remaining -= debit;
    result.surplus_units = remaining;
    std::uint64_t carried = 0;
    if (!checked_add(result.cumulative_surplus, remaining, carried)) {
      return false;
    }
    result.cumulative_surplus = carried;
  }

  if (result.cumulative_deficit > max_carry) {
    const std::uint64_t excess = result.cumulative_deficit - max_carry;
    if (!checked_add(result.discarded_deficit, excess, result.discarded_deficit)) {
      return false;
    }
    result.cumulative_deficit = max_carry;
    result.saturated = true;
  }
  if (result.cumulative_surplus > max_carry) {
    const std::uint64_t excess = result.cumulative_surplus - max_carry;
    if (!checked_add(result.discarded_surplus, excess, result.discarded_surplus)) {
      return false;
    }
    result.cumulative_surplus = max_carry;
    result.saturated = true;
  }
  out = result;
  return true;
}

bool accounting_closes(const FairnessAccounting& accounting,
                       std::int64_t expected_net_sum) noexcept {
  std::int64_t residual = 0;
  for (const SubjectAccounting& entry : accounting.subjects) {
    if (entry.cumulative_deficit > static_cast<std::uint64_t>(INT64_MAX) ||
        entry.cumulative_surplus > static_cast<std::uint64_t>(INT64_MAX) ||
        entry.discarded_deficit > static_cast<std::uint64_t>(INT64_MAX) ||
        entry.discarded_surplus > static_cast<std::uint64_t>(INT64_MAX)) {
      return false;
    }
    // residual = cumulative_deficit - cumulative_surplus
    //          + discarded_deficit - discarded_surplus
    std::int64_t next = 0;
    if (!checked_add_signed(residual, static_cast<std::int64_t>(entry.cumulative_deficit), next)) {
      return false;
    }
    residual = next;
    if (!checked_sub_signed(residual, static_cast<std::int64_t>(entry.cumulative_surplus), next)) {
      return false;
    }
    residual = next;
    if (!checked_add_signed(residual, static_cast<std::int64_t>(entry.discarded_deficit), next)) {
      return false;
    }
    residual = next;
    if (!checked_sub_signed(residual, static_cast<std::int64_t>(entry.discarded_surplus), next)) {
      return false;
    }
    residual = next;
  }
  return residual == expected_net_sum;
}

bool accounting_deficit_total(const FairnessAccounting& accounting, std::uint64_t& out) noexcept {
  std::uint64_t total = 0;
  for (const SubjectAccounting& entry : accounting.subjects) {
    if (!checked_add(total, entry.cumulative_deficit, total)) {
      return false;
    }
  }
  out = total;
  return true;
}

bool accounting_surplus_total(const FairnessAccounting& accounting, std::uint64_t& out) noexcept {
  std::uint64_t total = 0;
  for (const SubjectAccounting& entry : accounting.subjects) {
    if (!checked_add(total, entry.cumulative_surplus, total)) {
      return false;
    }
  }
  out = total;
  return true;
}

}  // namespace fairness_governor
