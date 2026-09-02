# What an intent-holding participant's own release costs: 720× its acknowledgement, and nothing the client can see

| | |
|---|---|
| Measured at | `v2.7.0-101-ged47cfc` (`git describe --tags`); the binary is `ea401d5`'s tree, which is the same code — everything since is `bench/` and `docs/` |
| Build | `build-release` (Release) |
| Driver | `bench/fk_crossing_cost_probe.py --mode participant-release` (new), 4 cores, three blocks of 60 transactions per arm |
| Host | 8 cores, AMD EPYC 9V74; `/proc/loadavg` **2.14 before, 2.42 after** — not a quiet host, and §4 says what that does and does not permit |
| Raw | `bench/v2.8.0/archive/ah-t6-participant-release-group.json`, `…-relaxed.json` |

**What was owed.** `foreign-keys.md` §2b names a path and says its cost is
unmeasured: when a cross-owner transaction's **participant** also holds a
foreign-key reference intent, the `COMMIT` that applies the coordinator's
decision forks into the cross-owner path instead of the ordinary one — so it
takes a durability wait and a decide round trip of its own before
acknowledging its coordinator, and XE1's `kAtAppend` saving does not apply.
This measures it.

**The two arms differ in one thing.** A client on core 0 opens a
transaction, ships one write to the relation on core 2, and commits. In one
arm that relation carries a foreign key whose parent is on core 3; in the
other it carries none. Same client, same coordinator, same participant core,
same statement shape, same row count. Placement is read back from `DESCRIBE`
and the probe count is checked per block (60 rounds per 60 transactions in
the intent arm, 0 in the other) — a wrong shape fails the run.

## 1. The participant's acknowledgement leg: 4.4 µs → 3.1 ms

`xowner_part_ack` is the span from a decide arriving at the participant to
the participant acknowledging it — XE1's own subject, the leg it moved off
the chain by acking at the append.

| | no intent | intent-holding | |
|---|---|---|---|
| `group` (shipped default) | **4.4 µs** (4.3, 4.5, 4.3) | **3142.6 µs** (1967.6, 5610.5, 1849.7) | **720×** |
| `relaxed` | **4.5 µs** (4.5, 4.7, 4.4) | **2269.0 µs** (2786.5, 1974.0, 2046.5) | **499×** |

Mean µs per decide, per block in brackets. The no-intent arm is XE1 working
exactly as measured: an acknowledgement that leaves at the append costs
microseconds. **The intent-holding arm loses all of it.**

## 2. And it is *not* the fdatasync — which corrects what §2b assumed

§2b predicted the participant "re-acquires the `fdatasync` wait XE1's
`kAtAppend` removed". If that were the whole story, `relaxed` would collapse
it: that class stages no group commit at all. It does not collapse — 2269 µs
against `group`'s 3142 µs, the same order.

What the participant actually pays is the **cross-owner commit path's own
decision-durability wait**, which is unconditional by design and says so at
its site: *"the decision is made durable before anyone is told, and whatever
the durability class — `relaxed`'s window is a promise about this stream's
own recent commits, not about a record another core is about to act on."*
That is a *different* wait from the one XE1 moved, taken by a different rule,
and it is joined by the participant's own decide round trip to the core
holding the intent.

So the correct sentence is: **an intent-holding participant stops being a
participant that acks cheaply and becomes a coordinator that must make its
own decision durable** — and a coordinator's durability wait was never
XE1's to remove.

## 3. Whether the client feels it depends on what else is already waiting

| | no intent | intent-holding | delta |
|---|---|---|---|
| `group` commit p50 | 5679 µs | 5602 µs | **−77 µs (−1.4%)** |
| `relaxed` commit p50 | 3608 µs | 5548 µs | **+1940 µs (+53.8%)** |

Client-visible `COMMIT` round trip, pooled over 180 transactions per arm.

**Under the shipped durability class the 3 ms is invisible** — the delta is
negative and inside the run's own noise. The coordinator is already waiting
on its own device, and the participant's extra wait happens inside that
window. **Under `relaxed` it is nearly all visible**: 1940 µs of the 2269 µs
leg reaches the client, because there is no longer anything for it to hide
behind.

This is the third time this subject has produced the same shape — the matrix
file found it at the mean, the maxima file at the maximum, and here it is
across durability classes. **A leg's cost is only visible where nothing else
is already waiting**, and the corollary matters for planning: this one is
free today and would stop being free the moment the coordinator's own device
wait went away.

## 4. What this host permits and what it does not

`loadavg` 2.14 → 2.42 with other Claude sessions on the machine. **The ratios
are robust to that** — 4.4 µs against 3.1 ms is not a scheduling artifact,
and it reproduces across all six blocks and both classes. **The tails are
not**: the pooled p100 reaches 340 ms in the *no-intent* `group` arm, which
is the host and not the engine, and nothing in this file rests on a
percentile above p90.

## 5. What it means for the design, stated but not decided

The intent-holding participant is the one shape where a foreign-key crossing
costs milliseconds rather than tens of microseconds, and it costs them **on
the participant**, not on the client. Three things follow, none of them
taken here:

- The cost is the *cross-owner commit path*, not the foreign key. Any future
  reason for a participant to have its own participants would meet the same
  wait.
- It is bounded by one round trip and one durability wait, not by the number
  of intents — the decide addresses every holder at once.
- Removing it means giving an intent holder a decide that does not have to be
  durable first, which is a protocol decision (the intent holder never
  prepared, so the argument `AbortAndForget` makes for the abort leg may
  extend to it) and belongs to whoever reopens `cross-owner-txn.md` §2.
