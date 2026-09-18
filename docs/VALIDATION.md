# Validation

This document states exactly what has been proven about Fairness Governor 1.0.0,
with what tooling, and what has deliberately not been attempted. Nothing here is
extrapolated.

## Proof surface labels

| Label | Meaning |
| --- | --- |
| **REAL** | Exercised with real operating-system processes, real sockets, real files, and real process death |
| **SYNTHETIC** | Exercised in process against generated data and generated populations |
| **UNSUPPORTED** | Not attempted, and not claimed anywhere in this repository |

## REAL

* **Multi-process framed transport (REAL).** `fg_multiprocess_tests` starts
  `fg_evidence_link.exe` and `fg_evidence_publisher.exe` as separate operating
  system processes and drives them over a real loopback TCP socket carrying real
  framed messages. Covered: successful delivery and acknowledgement, epoch
  fencing of a stale publisher, graceful frame-driven shutdown, malformed traffic
  (truncated frame, bad checksum, oversized declared length), killing a publisher
  in the middle of a frame and confirming the governor stays healthy, and
  repeated kill-and-restart of the governor process.
* **Process death and restart (REAL).** The publisher is killed with
  `TerminateProcess` mid-frame; the governor is killed and restarted; each
  restart produces a different boot identity, and the durable accounting is
  reloaded and continues to close.
* **Durable files (REAL).** The store writes real files, flushes them, performs
  real atomic replaces, and is validated against real corruption: byte flips,
  truncation, garbage replacement, oversized records, leftover journals, and
  abandoned temporaries.
* **Install and consume (REAL).** The library installs into a prefix and is
  consumed by an independent CMake project through `find_package`, compiled with
  `/W4 /WX`, run, and asserted on its output.
* **Fresh clone (REAL).** The committed sources are cloned into a clean directory
  and built and tested from scratch with no network access.

## SYNTHETIC

* **Seeded randomized properties (SYNTHETIC).** Allocation conservation, floor
  honouring, determinism, monotonicity in weight, engine invariants over random
  populations (entitlement sums, deficit/surplus cancellation, bounded
  correction, starvation precedence), multi-window accounting closure, and
  freedom from overflow at the domain bounds.
* **Adversarial input (SYNTHETIC).** Thousands of random byte strings pushed
  through every decoder; single-byte corruption sweeps of a valid policy;
  thousands of malformed scenario documents.
* **Benchmark (SYNTHETIC).** `bench/fg_bench.cpp` measures completed in-process
  evaluations of generated populations. It is labelled SYNTHETIC in its own
  output and in this document. It performs no physical-network measurement.

## UNSUPPORTED

The following are **not** implemented and **not** claimed:

* No physical network, NIC, switch, router, DPU, RDMA, NVLink, or optical
  validation of any kind. The only transport exercised is loopback TCP between
  processes on one host.
* No multi-node or multi-host deployment, no distributed consensus, no
  cross-host clock reasoning.
* No bandwidth allocation, packet scheduling, path selection, admission control,
  rate enforcement, priority/QoS class creation, or global traffic engineering.
  These are outside the runtime boundary by design.
* No cryptographic authentication of frames or durable records. Integrity is
  checked with a deterministic 64-bit non-cryptographic digest, which detects
  corruption and accident, not a hostile forger.
* No performance guarantee under real traffic. The benchmark charactersises the
  evaluator, not a deployment.

## Tooling actually run

| Tool | Configuration | Result |
| --- | --- | --- |
| MSVC 19.44 (`cl`) | Debug, `/W4 /WX /permissive- /utf-8`, full suite | Clean build, all suites pass |
| MSVC 19.44 (`cl`) | Release, `/W4 /WX /permissive- /utf-8`, full suite | Clean build, all suites pass |
| MSVC AddressSanitizer | `/fsanitize=address`, RelWithDebInfo, full suite | No sanitizer findings |
| MSVC `/analyze` | Release, external headers suppressed | No first-party findings (see below) |
| Debug runtime checks | `/RTC1` in the Debug configuration | No runtime check failures |

### Static analysis

`/analyze` reports nothing in first-party code. The only remaining diagnostic is
inside the Windows SDK header `ws2tcpip.h` (C6101 in an inline helper), which is
not part of this project. Two first-party findings were fixed as a result of this
pass: an ignored `WSAStartup` return value in two places, and several
dereferences of pointers that were only null-checked by an assertion.

## Tests

| Suite | Focus |
| --- | --- |
| `fg_unit_tests` | 128-bit arithmetic, checked arithmetic, identities, policy and evidence validation, allocation, engine outcomes, starvation, priority interaction, correction bounds, staleness, carry closure, codecs, durable store, framing, scenario parsing, explanation, lifecycle |
| `fg_property_tests` | Seeded randomized invariants: allocation, engine, multi-window closure, determinism |
| `fg_adversarial_tests` | Malformed input, durability corruption, malformed transport traffic |
| `fg_concurrency_tests` | Concurrent ingest/evaluate, shutdown, cancellation, serialized commits, epoch races |
| `fg_persistence_tests` | Restart continuity, recovery, failure injection |
| `fg_multiprocess_tests` | Real processes over real framed transport |

No test in this suite is wrapped in a timeout. Every wait is a bounded poll for a
condition that is expected to become true; a hang is treated as a defect.
