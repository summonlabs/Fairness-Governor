# Fairness Governor

Fairness Governor is an open-source, vendor-neutral C++20 runtime for
generation-bound fairness governance across competing network tenants, flows,
classes, guarantees, priorities, starvation limits, and policy-defined value.

It answers one question, authoritatively and reproducibly:

> Given competing flows or tenants, explicit guarantees, priority/value policy,
> recent service history, starvation limits, and current generations — what
> fairness state and corrective intent are authoritative now, who is underserved
> or overserved, and what bounded intervention is justified without violating
> stronger obligations?

## What this is, and what it is not

Fairness Governor **owns**:

* fairness evaluation over a validated, versioned policy;
* deficit/credit accounting that closes exactly over time;
* starvation detection against explicit thresholds;
* bounded corrective fairness **intent**;
* a deterministic, bounded explanation of every decision.

Fairness Governor **does not** allocate bandwidth, schedule packets, choose
paths, admit traffic, enforce rates, create priority or QoS classes, or perform
global traffic-engineering optimization. It reads priority and QoS class
*references* owned by other systems; it never creates or enforces them. A
corrective intent is intent: another system decides whether to act on it.

The separations are enforced in code and covered by tests:

| Separation | Where it is enforced |
| --- | --- |
| Fairness ≠ equality | Entitlement is weight- and floor-derived, not an equal split |
| Priority ≠ unlimited starvation authority | A devalued subject still starves at its threshold |
| Observed service ≠ entitlement | Evidence can never change policy or entitlement |
| Corrective intent ≠ enforcement | The governor returns bounded intent and never allocates |
| UNKNOWN ≠ FAIR | Missing evidence yields `UNKNOWN`, never a positive outcome |
| Stale ≠ authoritative | A superseded policy, epoch, generation, or window yields `STALE` |

## Outcomes

`FAIR`, `UNDER_SERVED`, `OVER_SERVED`, `STARVATION_RISK`,
`CORRECTION_REQUIRED`, `BLOCKED_BY_STRONGER_OBLIGATION`, `UNKNOWN`, `STALE`.

## Model

Strongly typed identities with generations: `FairnessGroupId/Generation`,
`SubjectId/Generation`, `FairnessPolicyId/Generation`,
`ServiceWindowId/Generation`, `EvidenceSnapshotId/Generation`,
`InterventionId/Generation`, priority and QoS references, `FabricEpoch`,
`BootId`/`Incarnation`, `Provenance`.

Authoritative arithmetic is exact integer arithmetic. Entitlement is a weighted
share of the service actually delivered in a window, allocated top-down through
a fairness group forest by a conserving three-step algorithm (weighted share →
guarantee floors → largest remainder). Every group's shares sum to exactly the
whole, so deficits and surpluses cancel exactly within each group.

The durable accounting identity, asserted after every randomized multi-window
run and again after every restart, is:

```
cumulative_deficit - cumulative_surplus
    + discarded_deficit - discarded_surplus
        == sum over windows of (entitlement - served)
```

## Quick start

```cpp
#include "fairness_governor/fairness_governor.hpp"
using namespace fairness_governor;

// 1. Declare a policy: this is the only source of authority about fairness.
FairnessPolicy policy;
policy.id = FairnessPolicyId::from_value(1);
policy.generation = FairnessPolicyGeneration::from_value(1);
policy.epoch = FabricEpoch::from_value(1);
policy.fair_band_units = 8;
policy.correction_threshold_units = 64;
policy.max_correction_units = 100000;
policy.max_correction_bps = 5000;
policy.max_priority_modifier_bps = 1000;
policy.max_evidence_age_windows = 2;

FairnessGroup group;
group.id = FairnessGroupId::from_value(1);
group.generation = FairnessGroupGeneration::from_value(1);
policy.groups.push_back(group);

Subject tenant;
tenant.id = SubjectId::from_value(1);
tenant.generation = SubjectGeneration::from_value(1);
tenant.group = group.id;
tenant.share_weight = 1;
tenant.guarantee_floor = 200;
tenant.starvation_windows = 3;
tenant.protected_obligation = true;
tenant.obligation_rank = 1;
tenant.max_augment_units = 1000;
tenant.max_reduce_units = 1000;
policy.subjects.push_back(tenant);

// 2. Open a governor. Pass a directory to make policy and accounting durable.
GovernorConfig config;
config.policy = policy;
config.epoch = policy.epoch;
config.store_path = "fairness-state";
auto opened = FairnessGovernor::open(config);

// 3. Stage one service window of evidence, evaluate, and commit.
auto governor = opened.take();
// ... build an EvidenceSnapshot with one observation per governed subject ...
governor->ingest_evidence(evidence);
auto decision = governor->evaluate(request);
std::cout << explain(*decision) << "\n";
governor->commit(*decision);
```

See `examples/evaluate.cpp` for a complete program and
`examples/downstream_consumer/` for an independent `find_package` consumer.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements: a C++20 compiler (validated with MSVC 19.44 under `/W4 /WX`),
CMake 3.20 or newer, and a generator such as Ninja. There are no third-party
dependencies: no network access is required to build or test.

Options: `FG_BUILD_TESTS`, `FG_BUILD_TOOLS`, `FG_BUILD_BENCH`,
`FG_BUILD_EXAMPLES`, `FG_WARNINGS_AS_ERRORS` (all default `ON`) and
`FG_ENABLE_ASAN` (default `OFF`).

## Installing and consuming

```
cmake --install build --prefix /some/prefix
```

then, from an independent project:

```cmake
find_package(FairnessGovernor 1.0 CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE FairnessGovernor::fairness_governor)
```

## Tools

| Tool | Purpose |
| --- | --- |
| `fg_governor_cli` | Validate a scenario, evaluate it, optionally commit it durably |
| `fg_evidence_link` | Run the governor as a process and ingest framed evidence over loopback TCP |
| `fg_evidence_publisher` | Publish evidence frames, including deliberately malformed ones |

## Durability

Policy and fairness accounting persist as versioned, integrity-checked records
with a crash-safe commit protocol: validate → plan → write temporary → journal →
snapshot the previous record → atomic replace → retire journal and backup. A
temporary file is never adopted as committed state, a torn replace is recovered
from the backup, and corruption is reported rather than repaired silently.

Dynamic evidence is **never** restored. Durable state does not restore process
liveness, telemetry freshness, leases, worker authority, or publisher authority:
after a restart, evidence must be re-attested and publishers must re-handshake.

## Measured behaviour

* Every claim above is covered by the test suite: unit, seeded randomized
  property, adversarial, concurrency, failure-injection, persistence/restart and
  real multi-process framed-transport tests.
* The benchmark in `bench/fg_bench.cpp` is a **SYNTHETIC** in-process evaluation
  benchmark. It performs no physical-network validation of any kind, and no
  result from it says anything about a NIC, switch, DPU, or link.

## Documentation

* `docs/ARCHITECTURE.md` — layering, evaluation algorithm, durability protocol
* `docs/BOUNDARY.md` — what the runtime owns and deliberately refuses to own
* `docs/VALIDATION.md` — exactly what has been proven, and what has not
* `CHANGELOG.md` — 1.0.0 contents

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
