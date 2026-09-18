# Changelog

All notable changes to this project are documented here. This project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-01-01

Initial release.

### Added

* Exact integer fairness evaluation: conserving top-down allocation through a
  group forest (weighted share → guarantee floors → largest remainder), with a
  portable 128-bit multiply/divide primitive and bounded input domain.
* Policy model with share weights, absolute guarantee floors, starvation
  thresholds, priority/value modifiers, protected obligations, hysteresis,
  cooldown, and explicit bounds on corrective intent.
* Eight-outcome vocabulary: `FAIR`, `UNDER_SERVED`, `OVER_SERVED`,
  `STARVATION_RISK`, `CORRECTION_REQUIRED`,
  `BLOCKED_BY_STRONGER_OBLIGATION`, `UNKNOWN`, `STALE`.
* Durable deficit/credit accounting with a provable closure identity, including
  explicit saturation accounting.
* Bounded corrective intent that provably conserves units: augmentation and
  reduction always total the same number of units.
* Deterministic explanation exposing entitlement, service, deviation,
  deficit/surplus, starvation state, modifiers, protected obligations, the
  proposed correction, and the full authority vector.
* Versioned, integrity-checked, crash-safe durable store with journal, atomic
  replace, backup recovery, and refusal to adopt an unfinished attempt.
* Framed evidence transport over loopback TCP with epoch and incarnation
  fencing, per-connection sequence enforcement, an idle budget, and a bounded
  connection pool.
* Strict textual scenario format and result rendering.
* Tools: `fg_governor_cli`, `fg_evidence_link`, `fg_evidence_publisher`.
* CMake package export with an independent `find_package` consumer.
* Synthetic in-process evaluation benchmark.

### Fixed during development (found by the suite, kept as regression tests)

* The deficit/surplus closure predicate had a sign error that made every closure
  assertion meaningless until the seeded property test compared it against an
  independently accumulated sum.
* The observation digest was documented as order-independent but combined
  per-observation digests with an order-dependent mix.
* The carry projection used in a decision had the opposite sign to the carry
  applied at commit, so a decision's projected carry disagreed with the durable
  result.
* The base (unmodified-weight) allocation pass never descended into the group
  forest, so the priority delta was computed against a zero base.
* The frame checksum helper wrote the checksum field eight bytes past the end of
  its 64-byte coverage buffer (caught by the debug runtime checks).
* `Status` stored a `string_view` into a caller's temporary, so every formatted
  failure message dangled.
* The link client never set its publisher incarnation, so every cross-process
  handshake was rejected before any evidence could be delivered.
* A peer that opened a connection and stopped sending could hold a worker slot
  indefinitely; connections now carry an idle budget.
* `shutdown` joined workers while holding the registry lock and could race a
  still-running accept loop.
* The unsigned weight domain lost basis-point modifier precision for small
  weights; weights are now carried in a 1e-4 scaled domain.

[1.0.0]: https://github.com/summonlabs/Fairness-Governor/releases/tag/v1.0.0
