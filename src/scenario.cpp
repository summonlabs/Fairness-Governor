// Fairness Governor - strict textual scenario parsing and result rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "fairness_governor/scenario.hpp"

#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fairness_governor/core/bytes.hpp"
#include "fairness_governor/core/checked.hpp"
#include "fairness_governor/core/limits.hpp"
#include "fairness_governor/eval/explain.hpp"

namespace fairness_governor {
namespace {

struct ParseError {
  std::size_t line{0};
  std::string message;
};

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  /// Parses the whole document. Returns false and fills `error` on failure.
  [[nodiscard]] bool parse(Scenario& out, ParseError& error);

 private:
  struct Line {
    std::size_t number{0};
    std::vector<std::string> tokens;
    std::string label;
    bool has_label{false};
  };

  [[nodiscard]] bool next_line(Line& line, bool& exhausted);
  [[nodiscard]] static bool parse_u64(const std::string& text, std::uint64_t& out);
  [[nodiscard]] static bool parse_i64(const std::string& text, std::int64_t& out);
  [[nodiscard]] static bool parse_u32(const std::string& text, std::uint32_t& out);
  [[nodiscard]] static bool parse_i32(const std::string& text, std::int32_t& out);
  [[nodiscard]] static bool parse_id_pair(const std::string& text, std::uint64_t& id,
                                          std::uint64_t& generation);

  void set_error(ParseError& error, const std::string& message);

  std::string_view text_;
  std::size_t offset_{0};
  std::size_t line_number_{0};
  ParseError pending_{};
  bool failed_{false};
};

void Parser::set_error(ParseError& error, const std::string& message) {
  if (!failed_) {
    failed_ = true;
    error.line = line_number_;
    error.message = message;
  }
}

bool Parser::parse_u64(const std::string& text, std::uint64_t& out) {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (!checked_mul(value, std::uint64_t{10}, value) || !checked_add(value, digit, value)) {
      return false;
    }
  }
  out = value;
  return true;
}

bool Parser::parse_i64(const std::string& text, std::int64_t& out) {
  if (text.empty()) {
    return false;
  }
  if (text[0] == '-') {
    std::uint64_t magnitude_value = 0;
    if (!parse_u64(text.substr(1), magnitude_value) ||
        magnitude_value > static_cast<std::uint64_t>(INT64_MAX) + 1ULL) {
      return false;
    }
    if (magnitude_value == static_cast<std::uint64_t>(INT64_MAX) + 1ULL) {
      out = INT64_MIN;
      return true;
    }
    out = -static_cast<std::int64_t>(magnitude_value);
    return true;
  }
  std::uint64_t value = 0;
  if (!parse_u64(text, value) || value > static_cast<std::uint64_t>(INT64_MAX)) {
    return false;
  }
  out = static_cast<std::int64_t>(value);
  return true;
}

bool Parser::parse_u32(const std::string& text, std::uint32_t& out) {
  std::uint64_t value = 0;
  if (!parse_u64(text, value) || value > UINT32_MAX) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

bool Parser::parse_i32(const std::string& text, std::int32_t& out) {
  std::int64_t value = 0;
  if (!parse_i64(text, value) || value > INT32_MAX || value < INT32_MIN) {
    return false;
  }
  out = static_cast<std::int32_t>(value);
  return true;
}

bool Parser::parse_id_pair(const std::string& text, std::uint64_t& id, std::uint64_t& generation) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return false;
  }
  return parse_u64(text.substr(0, colon), id) && parse_u64(text.substr(colon + 1), generation);
}

bool Parser::next_line(Line& line, bool& exhausted) {
  for (;;) {
    if (offset_ >= text_.size()) {
      exhausted = true;
      return true;
    }
    const std::size_t start = offset_;
    std::size_t end = text_.find('\n', start);
    if (end == std::string_view::npos) {
      end = text_.size();
    }
    offset_ = end + 1;
    ++line_number_;
    std::string_view raw = text_.substr(start, end - start);
    if (!raw.empty() && raw.back() == '\r') {
      raw.remove_suffix(1);
    }
    line = Line{};
    line.number = line_number_;
    std::size_t index = 0;
    while (index < raw.size()) {
      while (index < raw.size() && (raw[index] == ' ' || raw[index] == '\t')) {
        ++index;
      }
      if (index >= raw.size()) {
        break;
      }
      if (raw[index] == '#') {
        break;
      }
      const std::size_t token_start = index;
      while (index < raw.size() && raw[index] != ' ' && raw[index] != '\t') {
        ++index;
      }
      std::string token(raw.substr(token_start, index - token_start));
      if (token.rfind("label=", 0) == 0) {
        line.has_label = true;
        // The label may be written inline as label=value, or as a trailing
        // label= followed by the rest of the line. An inline value wins; any
        // remaining text after it is appended.
        std::string inline_value = token.substr(6);
        std::size_t label_start = index;
        while (label_start < raw.size() && (raw[label_start] == ' ' || raw[label_start] == '\t')) {
          ++label_start;
        }
        std::size_t label_end = raw.size();
        while (label_end > label_start &&
               (raw[label_end - 1] == ' ' || raw[label_end - 1] == '\t')) {
          --label_end;
        }
        if (!inline_value.empty()) {
          line.label = std::move(inline_value);
          if (label_end > label_start) {
            line.label += ' ';
            line.label.append(raw.substr(label_start, label_end - label_start));
          }
        } else {
          line.label.assign(raw.substr(label_start, label_end - label_start));
        }
        index = raw.size();
        break;
      }
      line.tokens.push_back(std::move(token));
    }
    if (line.tokens.empty() && !line.has_label) {
      continue;
    }
    exhausted = false;
    return true;
  }
}

bool Parser::parse(Scenario& out, ParseError& error) {
  Scenario scenario;
  bool saw_version = false;
  bool saw_body = false;
  bool in_policy = false;
  bool in_evidence = false;
  EvidenceSnapshot* current_evidence = nullptr;

  bool exhausted = false;
  Line line;
  while (true) {
    if (!next_line(line, exhausted) || exhausted) {
      break;
    }
    if (line.tokens.empty()) {
      set_error(error, "line carries only a label attribute");
      return false;
    }
    const std::string& directive = line.tokens[0];
    if (directive == "version") {
      if (saw_version || line.tokens.size() != 2) {
        set_error(error, "version must appear exactly once with one value");
        return false;
      }
      std::uint64_t value = 0;
      if (!parse_u64(line.tokens[1], value) || value != 1) {
        set_error(error, "unsupported scenario format version");
        return false;
      }
      saw_version = true;
      continue;
    }
    if (!saw_version) {
      set_error(error, "the first directive must be: version 1");
      return false;
    }
    saw_body = true;

    if (directive == "epoch") {
      if (line.tokens.size() != 2) {
        set_error(error, "epoch takes exactly one value");
        return false;
      }
      std::uint64_t value = 0;
      if (!parse_u64(line.tokens[1], value)) {
        set_error(error, "epoch is not an unsigned integer");
        return false;
      }
      scenario.epoch = FabricEpoch::from_value(value);
      continue;
    }

    if (directive == "policy") {
      if (line.tokens.size() < 3) {
        set_error(error, "policy requires an identity and a generation");
        return false;
      }
      std::uint64_t id = 0;
      std::uint64_t generation = 0;
      if (!parse_u64(line.tokens[1], id) || !parse_u64(line.tokens[2], generation)) {
        set_error(error, "policy identity and generation must be unsigned integers");
        return false;
      }
      scenario.policy.id = FairnessPolicyId::from_value(id);
      scenario.policy.generation = FairnessPolicyGeneration::from_value(generation);
      for (std::size_t i = 3; i < line.tokens.size(); ++i) {
        const std::string& token = line.tokens[i];
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
          set_error(error, "policy attribute is not key=value: " + token);
          return false;
        }
        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        std::uint64_t number = 0;
        std::int64_t signed_number = 0;
        std::uint32_t small = 0;
        bool ok = true;
        if (key == "window_units") {
          ok = parse_u64(value, number);
          scenario.policy.window_units = number;
        } else if (key == "fair_band") {
          ok = parse_u64(value, number);
          scenario.policy.fair_band_units = number;
        } else if (key == "correction_threshold") {
          ok = parse_u64(value, number);
          scenario.policy.correction_threshold_units = number;
        } else if (key == "cooldown") {
          ok = parse_u32(value, small);
          scenario.policy.cooldown_windows = small;
        } else if (key == "max_correction") {
          ok = parse_u64(value, number);
          scenario.policy.max_correction_units = number;
        } else if (key == "max_correction_bps") {
          ok = parse_u32(value, small);
          scenario.policy.max_correction_bps = small;
        } else if (key == "max_modifier_bps") {
          ok = parse_u32(value, small);
          scenario.policy.max_priority_modifier_bps = small;
        } else if (key == "max_age_windows") {
          ok = parse_u32(value, small);
          scenario.policy.max_evidence_age_windows = small;
        } else if (key == "max_age_ns") {
          ok = parse_u64(value, number);
          scenario.policy.max_evidence_age_ns = number;
        } else if (key == "max_carry") {
          ok = parse_u64(value, number);
          scenario.policy.max_carry_units = number;
        } else if (key == "enforce_floors") {
          ok = parse_u32(value, small) && small <= 1;
          scenario.policy.enforce_guarantee_floors = small != 0;
        } else if (key == "source") {
          scenario.policy.provenance.source = value;
        } else if (key == "revision") {
          ok = parse_u64(value, number);
          scenario.policy.provenance.revision = number;
        } else if (key == "digest") {
          ok = parse_u64(value, number);
          scenario.policy.provenance.source_digest = number;
        } else {
          set_error(error, "unknown policy attribute: " + key);
          return false;
        }
        if (!ok) {
          set_error(error, "invalid value for policy attribute " + key);
          return false;
        }
        (void)signed_number;
      }
      scenario.has_policy = true;
      in_policy = true;
      in_evidence = false;
      current_evidence = nullptr;
      continue;
    }

    if (directive == "group") {
      if (!in_policy) {
        set_error(error, "group must follow a policy directive");
        return false;
      }
      if (line.tokens.size() < 3) {
        set_error(error, "group requires an identity and a generation");
        return false;
      }
      FairnessGroup group;
      std::uint64_t id = 0;
      std::uint64_t generation = 0;
      if (!parse_u64(line.tokens[1], id) || !parse_u64(line.tokens[2], generation)) {
        set_error(error, "group identity and generation must be unsigned integers");
        return false;
      }
      group.id = FairnessGroupId::from_value(id);
      group.generation = FairnessGroupGeneration::from_value(generation);
      std::unordered_set<std::string> seen;
      for (std::size_t i = 3; i < line.tokens.size(); ++i) {
        const std::string& token = line.tokens[i];
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
          set_error(error, "group attribute is not key=value: " + token);
          return false;
        }
        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (!seen.insert(key).second) {
          set_error(error, "duplicate group attribute: " + key);
          return false;
        }
        std::uint64_t number = 0;
        std::uint32_t small = 0;
        bool ok = true;
        if (key == "parent") {
          ok = parse_u64(value, number);
          group.parent = FairnessGroupId::from_value(number);
        } else if (key == "weight") {
          ok = parse_u32(value, small);
          group.share_weight = small;
        } else if (key == "floor") {
          ok = parse_u64(value, number);
          group.guarantee_floor = number;
        } else if (key == "rank") {
          ok = parse_u32(value, small);
          group.obligation_rank = small;
        } else if (key == "protected") {
          ok = parse_u32(value, small) && small <= 1;
          group.protected_obligation = small != 0;
        } else {
          set_error(error, "unknown group attribute: " + key);
          return false;
        }
        if (!ok) {
          set_error(error, "invalid value for group attribute " + key);
          return false;
        }
      }
      if (line.has_label) {
        group.label = line.label;
      }
      scenario.policy.groups.push_back(std::move(group));
      continue;
    }

    if (directive == "subject") {
      if (!in_policy) {
        set_error(error, "subject must follow a policy directive");
        return false;
      }
      if (line.tokens.size() < 3) {
        set_error(error, "subject requires an identity and a generation");
        return false;
      }
      Subject subject;
      std::uint64_t id = 0;
      std::uint64_t generation = 0;
      if (!parse_u64(line.tokens[1], id) || !parse_u64(line.tokens[2], generation)) {
        set_error(error, "subject identity and generation must be unsigned integers");
        return false;
      }
      subject.id = SubjectId::from_value(id);
      subject.generation = SubjectGeneration::from_value(generation);
      std::unordered_set<std::string> seen;
      for (std::size_t i = 3; i < line.tokens.size(); ++i) {
        const std::string& token = line.tokens[i];
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
          set_error(error, "subject attribute is not key=value: " + token);
          return false;
        }
        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (!seen.insert(key).second) {
          set_error(error, "duplicate subject attribute: " + key);
          return false;
        }
        std::uint64_t number = 0;
        std::uint32_t small = 0;
        std::int32_t signed_value = 0;
        std::uint64_t ref_id = 0;
        std::uint64_t ref_generation = 0;
        bool ok = true;
        if (key == "group") {
          ok = parse_u64(value, number);
          subject.group = FairnessGroupId::from_value(number);
        } else if (key == "tenant") {
          ok = parse_u64(value, number);
          subject.tenant = TenantId::from_value(number);
        } else if (key == "weight") {
          ok = parse_u32(value, small);
          subject.share_weight = small;
        } else if (key == "floor") {
          ok = parse_u64(value, number);
          subject.guarantee_floor = number;
        } else if (key == "starvation") {
          ok = parse_u32(value, small);
          subject.starvation_windows = small;
        } else if (key == "guarantee_starvation") {
          ok = parse_u32(value, small);
          subject.guarantee_starvation_windows = small;
        } else if (key == "rank") {
          ok = parse_u32(value, small);
          subject.obligation_rank = small;
        } else if (key == "protected") {
          ok = parse_u32(value, small) && small <= 1;
          subject.protected_obligation = small != 0;
        } else if (key == "reduce_cap") {
          ok = parse_u64(value, number);
          subject.max_reduce_units = number;
        } else if (key == "augment_cap") {
          ok = parse_u64(value, number);
          subject.max_augment_units = number;
        } else if (key == "modifier_bps") {
          ok = parse_i32(value, signed_value);
          subject.priority_modifier_bps = signed_value;
        } else if (key == "priority_rank") {
          ok = parse_u32(value, small);
          subject.priority_rank = small;
        } else if (key == "priority") {
          ok = parse_id_pair(value, ref_id, ref_generation);
          subject.priority.id = PriorityRef::from_value(ref_id);
          subject.priority.generation = Generation<PriorityRefTag>::from_value(ref_generation);
        } else if (key == "qos") {
          ok = parse_id_pair(value, ref_id, ref_generation);
          subject.qos.id = QosRef::from_value(ref_id);
          subject.qos.generation = Generation<QosRefTag>::from_value(ref_generation);
        } else {
          set_error(error, "unknown subject attribute: " + key);
          return false;
        }
        if (!ok) {
          set_error(error, "invalid value for subject attribute " + key);
          return false;
        }
      }
      if (line.has_label) {
        subject.label = line.label;
      }
      scenario.policy.subjects.push_back(std::move(subject));
      continue;
    }

    if (directive == "evidence") {
      if (line.tokens.size() < 3) {
        set_error(error, "evidence requires an identity and a generation");
        return false;
      }
      EvidenceSnapshot snapshot;
      std::uint64_t id = 0;
      std::uint64_t generation = 0;
      if (!parse_u64(line.tokens[1], id) || !parse_u64(line.tokens[2], generation)) {
        set_error(error, "evidence identity and generation must be unsigned integers");
        return false;
      }
      snapshot.id = EvidenceSnapshotId::from_value(id);
      snapshot.generation = EvidenceSnapshotGeneration::from_value(generation);
      std::unordered_set<std::string> seen;
      for (std::size_t i = 3; i < line.tokens.size(); ++i) {
        const std::string& token = line.tokens[i];
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
          set_error(error, "evidence attribute is not key=value: " + token);
          return false;
        }
        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (!seen.insert(key).second) {
          set_error(error, "duplicate evidence attribute: " + key);
          return false;
        }
        std::uint64_t number = 0;
        std::int64_t signed_value = 0;
        bool ok = true;
        if (key == "window") {
          ok = parse_u64(value, number);
          snapshot.window = ServiceWindowId::from_value(number);
        } else if (key == "window_gen") {
          ok = parse_u64(value, number);
          snapshot.window_generation = ServiceWindowGeneration::from_value(number);
        } else if (key == "epoch") {
          ok = parse_u64(value, number);
          snapshot.epoch = FabricEpoch::from_value(number);
        } else if (key == "captured_ns") {
          ok = parse_i64(value, signed_value);
          snapshot.captured_at_ns = signed_value;
        } else if (key == "sequence") {
          ok = parse_u64(value, number);
          snapshot.sequence = number;
        } else if (key == "producer") {
          snapshot.producer.name = value;
        } else if (key == "instance") {
          ok = parse_u64(value, number);
          snapshot.producer.instance = number;
        } else if (key == "version") {
          ok = parse_u64(value, number);
          snapshot.producer.version = number;
        } else if (key == "declared_digest") {
          ok = parse_u64(value, number);
          snapshot.declared_digest = number;
        } else {
          set_error(error, "unknown evidence attribute: " + key);
          return false;
        }
        if (!ok) {
          set_error(error, "invalid value for evidence attribute " + key);
          return false;
        }
      }
      scenario.evidence.push_back(std::move(snapshot));
      current_evidence = &scenario.evidence.back();
      in_policy = false;
      in_evidence = true;
      continue;
    }

    if (directive == "obs") {
      if (!in_evidence || current_evidence == nullptr) {
        set_error(error, "obs must follow an evidence directive");
        return false;
      }
      if (line.tokens.size() < 3) {
        set_error(error, "obs requires a subject identity and a generation");
        return false;
      }
      SubjectObservation observation;
      std::uint64_t id = 0;
      std::uint64_t generation = 0;
      if (!parse_u64(line.tokens[1], id) || !parse_u64(line.tokens[2], generation)) {
        set_error(error, "obs identity and generation must be unsigned integers");
        return false;
      }
      observation.id = SubjectId::from_value(id);
      observation.generation = SubjectGeneration::from_value(generation);
      std::unordered_set<std::string> seen;
      for (std::size_t i = 3; i < line.tokens.size(); ++i) {
        const std::string& token = line.tokens[i];
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
          set_error(error, "obs attribute is not key=value: " + token);
          return false;
        }
        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (!seen.insert(key).second) {
          set_error(error, "duplicate obs attribute: " + key);
          return false;
        }
        std::uint64_t number = 0;
        std::uint32_t small = 0;
        std::uint64_t ref_id = 0;
        std::uint64_t ref_generation = 0;
        bool ok = true;
        if (key == "served") {
          ok = parse_u64(value, number);
          observation.served_units = number;
        } else if (key == "unserved") {
          ok = parse_u32(value, small);
          observation.unserved_streak = small;
        } else if (key == "below") {
          ok = parse_u32(value, small);
          observation.below_floor_streak = small;
        } else if (key == "priority") {
          ok = parse_id_pair(value, ref_id, ref_generation);
          observation.priority.id = PriorityRef::from_value(ref_id);
          observation.priority.generation = Generation<PriorityRefTag>::from_value(ref_generation);
        } else if (key == "qos") {
          ok = parse_id_pair(value, ref_id, ref_generation);
          observation.qos.id = QosRef::from_value(ref_id);
          observation.qos.generation = Generation<QosRefTag>::from_value(ref_generation);
        } else {
          set_error(error, "unknown obs attribute: " + key);
          return false;
        }
        if (!ok) {
          set_error(error, "invalid value for obs attribute " + key);
          return false;
        }
      }
      current_evidence->observations.push_back(std::move(observation));
      continue;
    }

    if (directive == "request") {
      EvaluationRequest request;
      std::unordered_set<std::string> seen;
      for (std::size_t i = 1; i < line.tokens.size(); ++i) {
        const std::string& token = line.tokens[i];
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
          set_error(error, "request attribute is not key=value: " + token);
          return false;
        }
        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (!seen.insert(key).second) {
          set_error(error, "duplicate request attribute: " + key);
          return false;
        }
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        std::uint64_t number = 0;
        std::uint32_t small = 0;
        std::int64_t signed_value = 0;
        bool ok = true;
        if (key == "policy") {
          ok = parse_id_pair(value, id, generation);
          request.policy_id = FairnessPolicyId::from_value(id);
          request.policy_generation = FairnessPolicyGeneration::from_value(generation);
        } else if (key == "evidence") {
          ok = parse_id_pair(value, id, generation);
          request.evidence_id = EvidenceSnapshotId::from_value(id);
          request.evidence_generation = EvidenceSnapshotGeneration::from_value(generation);
        } else if (key == "window") {
          ok = parse_id_pair(value, id, generation);
          request.window = ServiceWindowId::from_value(id);
          request.window_generation = ServiceWindowGeneration::from_value(generation);
        } else if (key == "epoch") {
          ok = parse_u64(value, number);
          request.epoch = FabricEpoch::from_value(number);
        } else if (key == "now_ns") {
          ok = parse_i64(value, signed_value);
          request.now_ns = signed_value;
        } else if (key == "request_id") {
          ok = parse_u64(value, number);
          request.request_id = number;
        } else if (key == "attempt") {
          ok = parse_u32(value, small);
          request.attempt = small;
        } else if (key == "missing_unknown") {
          ok = parse_u32(value, small) && small <= 1;
          request.treat_missing_as_unknown = small != 0;
        } else {
          set_error(error, "unknown request attribute: " + key);
          return false;
        }
        if (!ok) {
          set_error(error, "invalid value for request attribute " + key);
          return false;
        }
      }
      scenario.request = request;
      scenario.has_request = true;
      in_policy = false;
      in_evidence = false;
      current_evidence = nullptr;
      continue;
    }

    set_error(error, "unknown directive: " + directive);
    return false;
  }

  if (!saw_version) {
    set_error(error, "the document must begin with: version 1");
    return false;
  }
  if (!saw_body) {
    set_error(error, "the document declares no policy, evidence, or request");
    return false;
  }
  if (scenario.has_policy) {
    if (!scenario.epoch.valid()) {
      scenario.epoch = scenario.policy.epoch;
    }
    scenario.policy.epoch = scenario.epoch;
    const PolicyValidation validation = validate_policy(scenario.policy);
    if (!validation.ok()) {
      error.line = 0;
      error.message = std::string("policy is not valid: ") +
                      std::string(to_string(validation.code)) + " " + validation.detail;
      return false;
    }
  }
  for (const EvidenceSnapshot& snapshot : scenario.evidence) {
    const Status validation = validate_evidence(snapshot);
    if (!validation.ok()) {
      error.line = 0;
      error.message = std::string("evidence is not valid: ") + validation.to_string();
      return false;
    }
  }
  scenario.source_digest = checksum64(reinterpret_cast<const std::uint8_t*>(text_.data()), text_.size());
  out = std::move(scenario);
  return true;
}

}  // namespace

Result<Scenario> parse_scenario(std::string_view text) {
  Scenario scenario;
  Parser parser(text);
  ParseError error;
  if (!parser.parse(scenario, error)) {
    std::string detail = "line " + to_decimal(error.line) + ": " + error.message;
    return Status::failure(StatusCode::InvalidArgument, std::move(detail));
  }
  return scenario;
}

std::string format_result(const FairnessDecision& decision) {
  std::string out = explain(decision, ExplainOptions{});
  out += "summary ";
  out += summarize(decision);
  out += "\n";
  return out;
}

}  // namespace fairness_governor
