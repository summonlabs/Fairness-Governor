# Architecture

## Layering

```
core/      identities, generations, epochs, incarnations, checked and 128-bit
           arithmetic, bounded byte codecs, status and result types
model/     refs and provenance, policy, evidence, durable accounting, outcomes
eval/      the exact allocation primitive, the pure fairness engine, the stateful
           governor lifecycle, deterministic explanation
persist/   bounded filesystem primitives, versioned crash-safe record store,
           strict codecs for durable state and wire payloads
link/      framed protocol and the loopback evidence link (real OS processes)
scenario   strict textual scenario format and result rendering
```

The dependency direction is one-way: `core` ← `model` ← `eval` ←
{`persist`, `link`, `scenario`}. Nothing in `core` or `model` knows about
sockets, files, or the command line.

## Evaluation

`evaluate_fairness` is a pure function of an explicit context: policy,
accounting snapshot, evidence snapshot, request identity, current epoch, current
window, and last committed window. It performs no I/O, takes no locks of its own,
reads no clock, and has no hidden state. Identical contexts produce identical
decisions, including the content digest.

1. **Eligibility.** The request must name the installed policy generation, the
   live fabric epoch, and an evidence identity whose generation, window, and
   window generation all match. The window must be newer than the last committed
   window and no older than the policy's age bound. A failure here is `STALE`
   (or `UNKNOWN` when nothing was supplied), and it plans nothing.
2. **Authority per subject.** A subject missing from the snapshot, carrying a
   different generation, or reporting a different priority/QoS reference is
   `UNKNOWN` or `STALE`. Evidence can never be promoted into authority.
3. **Allocation.** The whole being distributed is the service actually observed
   in the window. It is split top-down through the group forest by
   `allocate_whole`, which runs twice: once with unmodified weights (the base
   entitlement used for explanation) and once with policy-modified effective
   weights (the authoritative entitlement). Both passes conserve the whole
   exactly, so the priority/value modifier redistributes value and never creates
   or destroys units.
4. **Carry.** Each subject's `entitlement - served` is applied to its durable
   carry: credit is consumed before debt is recorded, debt is consumed before
   surplus is recorded, and saturation at the configured bound is accounted in
   explicit discarded counters so the closure identity stays exact.
5. **Starvation.** The effective streak is the larger of the governor's durable
   streak and the producer-reported streak, so a producer cannot reset a
   starvation streak by restarting. Starvation is decided only against the
   policy's explicit thresholds.
6. **Correction.** Demand is the recognized deficit plus carried debt, capped by
   the subject's augment cap. Supply is the surplus that policy allows to be
   reduced, capped by the subject's reduce cap and, for a protected obligation,
   by the headroom above its guaranteed floor. The authorized budget is the
   smaller of supply, demand, the policy total, and the policy basis-point bound.
   It is granted to demand in a fixed order (starvation, then largest deficit,
   then stronger obligation, then identity) and drawn from supply in a fixed
   order (largest reducible, then weaker obligation, then identity). Augmentation
   and reduction always total exactly the same number of units.
7. **Aggregation.** The population outcome is the most severe per-subject outcome
   with `UNKNOWN` and `STALE` dominating everything, because they are
   absence-of-authority states.

### Why entitlement is a share of observed service

Entitlement is a share of what was actually delivered, not a share of an
assumed capacity. That is what keeps the runtime inside its boundary: it never
has to know, guess, or assume the capacity of the fabric it is governing. The
consequence is deliberate and documented: **corrective intent redistributes
service; it can never create it.** If nothing was served in a window, every
entitlement is zero and no deficit accrues; starvation prevention is then driven
by the explicit streak thresholds, which is why those thresholds are policy
fields rather than inference.

## Stateful lifecycle

```
open()            load durable policy and accounting, report recovery
install_policy()  validate, fence by epoch, commit durably
advance_epoch()   advance the fabric epoch, invalidate every staged snapshot
ingest_evidence() validate, fence by epoch/generation/sequence, stage
evaluate()        pure; no durable mutation
commit()          the durable boundary; refuses stale authority
shutdown()        stop accepting work, drop staged evidence, release the store
```

The decision's authority vector binds it to the exact policy generation, epoch,
evidence identity, window, accounting generation, incarnation, request, and
attempt that justified it. `commit` re-checks every one of them, re-derives the
content digest, re-verifies that the corrective plan conserves units, and only
then crosses the durable boundary. A decision computed against a superseded
policy, a superseded accounting generation, a superseded epoch, another
incarnation, or an already-committed window is refused and mutates nothing.

## Lock discipline

One state mutex per governor. Public operations take it for one state transition
and never call another public operation, never invoke a callback, never join a
thread, and never emit an event while holding it. Durable I/O happens under the
lock because the store is single-writer and the ordering matters; no caller code
runs there. The link server keeps a leaf statistics mutex, a worker-registry
mutex that is held only to move finished workers out of the registry, and joins
workers outside both. There is no path that takes two governor-level locks.

## Durability

On-disk record layout: a 64-byte header (magic, format version, kind, flags,
generation, writer boot and ordinal, payload length, payload checksum, header
checksum) followed by the payload. The header checksum covers the first 64 bytes;
the checksum field begins exactly where that region ends, so it never covers
itself.

Commit protocol:

```
validate -> plan -> write .tmp + flush -> write .jnl + flush
   -> snapshot the committed record to .prev
   -> atomic replace main <- .tmp
   -> drop .jnl -> drop .prev
```

Recovery distinguishes four cases and never guesses: a completed replace with a
leftover journal (the committed record stands), a torn replace (recovered from
`.prev`), an abandoned temporary with no journal (discarded, never adopted), and
corruption (reported; nothing is repaired silently, and a commit against an
unusable record is refused).

## Transport

Fixed 80-byte frame header: magic, version, kind, flags, reserved, payload
length, session, epoch, publisher identity, publisher boot and ordinal, sequence,
frame checksum over the first 64 header bytes and then the payload, and a
reserved tail that must be zero. A frame is rejected before its payload is
trusted: wrong magic, wrong version, unknown kind, unknown or unsupported flags,
non-zero reserved fields, a payload beyond the negotiated bound, a bad checksum,
or a short read.

Handshake performs epoch and incarnation fencing. The server tracks an expected
sequence per connection and refuses a skipped or replayed sequence. A peer that
stops sending inside a frame is dropped once its idle budget is spent, so it
cannot hold a connection slot indefinitely.
