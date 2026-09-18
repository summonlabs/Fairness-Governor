// Fairness Governor - identity, generation, and digest tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include <set>
#include <string>

#include "fairness_governor/core/ids.hpp"
#include "test_harness.hpp"

using namespace fairness_governor;
using namespace fgtest;

FG_TEST(ids, identities_are_strongly_typed_and_ordered) {
  const SubjectId first = SubjectId::from_value(5);
  const SubjectId second = SubjectId::from_value(9);
  FG_CHECK(first == SubjectId::from_value(5));
  FG_CHECK(first != second);
  FG_CHECK(first < second);
  FG_CHECK(!SubjectId::none().valid());
  FG_CHECK(SubjectId::from_value(1).valid());
  std::set<SubjectId> ordered{second, first};
  FG_CHECK(ordered.begin()->value() == 5);
}

FG_TEST(ids, generation_saturates_instead_of_wrapping) {
  const Generation<SubjectIdTag> top = Generation<SubjectIdTag>::from_value(UINT64_MAX);
  FG_CHECK_EQ(top.next().value(), UINT64_MAX);
  const Generation<SubjectIdTag> initial = Generation<SubjectIdTag>::initial();
  FG_CHECK_EQ(initial.value(), 1u);
  FG_CHECK_EQ(initial.next().value(), 2u);
  FG_CHECK(!Generation<SubjectIdTag>{}.valid());
}

FG_TEST(ids, incarnation_requires_both_parts) {
  Incarnation incarnation;
  FG_CHECK(!incarnation.valid());
  incarnation.boot = BootId::from_value(7);
  FG_CHECK(!incarnation.valid());
  incarnation.ordinal = 1;
  FG_CHECK(incarnation.valid());
  const Incarnation same{BootId::from_value(7), 1};
  const Incarnation other{BootId::from_value(8), 1};
  FG_CHECK(incarnation == same);
  FG_CHECK(incarnation != other);
}

FG_TEST(ids, digest_is_deterministic_and_length_sensitive) {
  const std::uint8_t data[] = {1, 2, 3, 4};
  const std::uint8_t extended[] = {1, 2, 3, 4, 5};
  const std::uint64_t first = fnv1a64(data, sizeof(data));
  FG_CHECK_EQ(first, fnv1a64(data, sizeof(data)));
  FG_CHECK(first != fnv1a64(extended, sizeof(extended)));
  FG_CHECK_EQ(fnv1a64_continue(kFnvOffsetBasis, data, sizeof(data)), first);
}

FG_TEST(ids, decimal_rendering_is_exact) {
  FG_CHECK_EQ(to_decimal(0), std::string("0"));
  FG_CHECK_EQ(to_decimal(9), std::string("9"));
  FG_CHECK_EQ(to_decimal(10), std::string("10"));
  FG_CHECK_EQ(to_decimal(UINT64_MAX), std::string("18446744073709551615"));
}
