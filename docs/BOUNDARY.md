# Boundary

## The runtime owns

* **Fairness evaluation.** Turning a validated policy plus one evidence window
  into an authoritative fairness state for every governed subject.
* **Corrective fairness intent.** Bounded, conserving, explained proposals to
  augment or reduce service, with the policy bound that limited each one named.
* **Durable fairness accounting.** Deficit, credit, streaks, and the exact
  closure identity that ties them to the service that was actually delivered.
* **Explanation.** Entitlement, observed service, deviation, deficit/surplus,
  starvation state, modifiers, protected obligations, proposed correction, and
  the authority vector — bounded and deterministic.

## The runtime refuses to own

| Adjacent system | Why it is out of scope |
| --- | --- |
| Bandwidth allocation | The governor never has or assumes capacity |
| Packet scheduling | It produces intent, not schedule entries |
| Path selection | Routing is a different authority domain |
| Admission control | It cannot create or destroy demand |
| Rate enforcement | Intent is not enforcement |
| Priority / QoS class creation | It references classes owned elsewhere, by generation |
| Global traffic engineering | It evaluates fairness, not global optimality |

These are not stylistic preferences. They are the reason the runtime can be
vendor-neutral, and they are why `evaluate_fairness` is a pure function with no
capacity input at all.

## Consequential design decisions

**Entitlement is a share of delivered service.** The governor cannot allocate
what it does not control, so it evaluates the share of what was actually served.
The consequence is stated plainly in the README and in
`docs/ARCHITECTURE.md`: corrective intent redistributes service and can never
create it. A total outage produces `STARVATION_RISK` from the explicit streak
threshold, not a fabricated deficit.

**Priority modifies weight, never authority.** A policy-defined value modifier is
applied to the share weight, so it changes how the whole is divided without
changing the whole. A modifier cannot reduce a weight to zero, and it cannot
suppress a starvation threshold, a guarantee floor, or a protected obligation.

**A protected obligation is a hard ceiling on correction.** When a subject (or
group) holds a protected obligation, the reducible surplus is limited to the
headroom above its guaranteed floor. Whatever the obligation holds back is
reported as `withheld_units`, and a subject that cannot be made whole because of
it is reported as `BLOCKED_BY_STRONGER_OBLIGATION` — even when a partial
correction was still proposed.

**Absence of authority is never a positive outcome.** Missing evidence is
`UNKNOWN`. Superseded policy, epoch, generation, window, or priority reference is
`STALE`. Neither ever plans a correction, and neither is ever counted as fair.
