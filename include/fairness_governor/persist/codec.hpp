// Fairness Governor - strict, bounded codecs for durable state and wire payloads.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every decoder validates lengths, bounds, distinctness, and semantic coherence
// before returning. A decoded object is always a legal object; the evaluator can
// therefore assume its domain invariants hold. Trailing bytes are an error, not
// something to ignore.
#ifndef FAIRNESS_GOVERNOR_PERSIST_CODEC_HPP
#define FAIRNESS_GOVERNOR_PERSIST_CODEC_HPP

#include <cstdint>

#include "fairness_governor/core/bytes.hpp"
#include "fairness_governor/core/status.hpp"
#include "fairness_governor/model/accounting.hpp"
#include "fairness_governor/model/evidence.hpp"
#include "fairness_governor/model/outcome.hpp"
#include "fairness_governor/model/policy.hpp"

namespace fairness_governor::codec {

[[nodiscard]] ByteBuffer encode_policy(const FairnessPolicy& policy);
[[nodiscard]] Status decode_policy(const ByteBuffer& bytes, FairnessPolicy& out);

[[nodiscard]] ByteBuffer encode_accounting(const FairnessAccounting& accounting);
[[nodiscard]] Status decode_accounting(const ByteBuffer& bytes, FairnessAccounting& out);

[[nodiscard]] ByteBuffer encode_evidence(const EvidenceSnapshot& snapshot);
[[nodiscard]] Status decode_evidence(const ByteBuffer& bytes, EvidenceSnapshot& out);

/// Deterministic digest of a decision's authoritative content.
[[nodiscard]] std::uint64_t decision_digest(const FairnessDecision& decision);

/// Deterministic digest of a policy's authoritative content.
[[nodiscard]] std::uint64_t policy_digest(const FairnessPolicy& policy);

/// Deterministic digest of an evidence snapshot's content.
[[nodiscard]] std::uint64_t evidence_digest(const EvidenceSnapshot& snapshot);

}  // namespace fairness_governor::codec

#endif  // FAIRNESS_GOVERNOR_PERSIST_CODEC_HPP
