# Work order RS — the self-directed stage, and a fan-in client on every core

Drafted 2026-08-29 against `main` at `949a7d4` (`v2.2.1-132-g949a7d4`).

Two mechanisms, no measurement beyond what proves them. R4-M's §8 named
both, priced them against the three ways out of the read ceiling, and
recorded that **neither has an owner anywhere**. This order is that owner.

## 0. What refuses today, and why neither existing lever moves it

R4-M measured a spread relation losing all five read shapes at **≈395
rows**, three orders of magnitude below §3's arithmetic ceiling of 262,144.
Six of twenty-four scenario relations spread; all six lost every shape; all
four drivers read them; the aggregate cell was reported as not run.

The refusal is at `command_dispatcher.cpp:4826-4831`:

> relation '…' has ranges on another core and this shape cannot fan in over
> them; reading it here would answer short

It is `Status::Unsupported`, deliberately — not `TxnConflict`. Nothing
arrives on a retry: the relation still has two owners and this core still
cannot fan in. **The refusal is correct and permanent**, and §15d says so
in the same breath as deferring the fix:

> `WhollyOwnedBy` was added for the "owned here but not wholly here" case
> and never called. Such a relation took neither the fan-in nor a full
> local walk, and **returned half its rows**. `CheckReadAffinity` now
> refuses it — an honest refusal; widening the fan-in gate to cover it
> needs a **self-directed stage**, which is a design question and not this
> row's.

**And R4-M §8 established that no constant moves this.** Raising
`range_size_ids`, raising `kMaxFanInUpstreams` 64 → 255, and the per-core
stripe each move the ceiling and leave both refusals standing, because what
refuses is *a second owner*, not *many ranges*. A production block size
delays the refusal by exactly one block.

So the two mechanisms below are the whole of it. Between them they are
**wider than a config value and narrower than reversing D6.**

## 1. What the source already settles

Both mechanisms are less speculative than §15d's "design question"
suggests, and this order should build from these facts rather than
re-derive them.

**The two-claimants problem is already solved, on core 0.** A scheduler
holds exactly one handler per message kind — the map assigns, so a second
registration is a silent overwrite, not a fan-out. `core_runtime.cpp:539-542`
records this as the reason a peer must not have a client. But
`expeditor.cpp:1512-1533` shows core 0's answer, in place and commented:

> The two kinds both consumers hear: a scheduler holds exactly one handler
> per kind (the map assigns), so core 0 — the one core hosting a session
> client *and* a step server — **fans each payload to both. Safe because
> the tag is the demultiplexer: each consumer discards a tag it does not
> hold, silently.**

**That is the whole obstacle, and it has a landed pattern.** A peer needs
the same two handlers fanned to both consumers, plus `kStepError`, which
core 0 registers to the client alone.

**The server half exists on every core already.** `CoreRuntime` constructs
`remote_steps_` and registers `kStepOpen`, `kStepCredit`, `kStepCancel`,
`kStepBatch`, `kStepEof`. What a peer lacks is `SessionStepClient`, which
`Expeditor` holds as `remote_reads_`.

**The stage granularity is settled and is not per core.** RB4 corrected its
own first build: stages are one per **maximal contiguous run** of
same-owner ranges, in `lo` order, not one per owner core — grouping by core
emits `A₁, A₃, B₂` where the unsplit relation emits `1, 2, 3`, and §8 test 9
requires byte-identical. **A self-directed stage is the case where one of
those runs belongs to the core running the plan.**

## 2. A stale comment this order must correct

`remote_step_service.hpp:145-152` justifies `kMaxFanInUpstreams = 64`:

> A relation of k ranges does *not* open k stages: consecutive ranges on
> one core are walked by one stage, so **the width is the number of
> distinct owner cores, never the number of boundaries.** A 10 M-row
> relation at D6's size has ~2,441 ranges and **at most `cores` stages**,
> which is the difference between a plan and an absurdity.

**That is false under R4, and R4-M measured it false.** Interleaved
spreading produces `ABAB…`, so a maximal run is one range and the width is
the range count: the refusal fired at **65–72 stages** with `ids/stage`
matching the block size to within 4%.

The bound of 64 is still safe for the one-byte wire field. What is gone is
the argument that made it comfortable. **RS0 corrects the comment before
any code depends on reading it correctly**, stating the width as *the
number of maximal same-owner runs* — which equals the core count only under
contiguous ownership and equals the range count under interleaved.

## 3. The conclusions this milestone must produce

**CS1 — what a self-directed stage is, in the fan-in's existing
vocabulary.** `StepOpenParts::upstreams` is a vector of `StepOpenUpstream`,
each naming an `upstream_core`, a forwarded column layout, and an enclosed
`STEP_OPEN`. The conclusion states whether a local run is an entry in that
vector with a special core, an entry outside it, or a producer joining the
merge by another door — and what each choice costs the **range-order**
guarantee RB4 had to correct its first build to keep. The merge must still
emit in `lo` order across local and remote runs alike.

**CS2 — whether the ceiling moves, and in which direction.** A local run
costs no upstream slot if it is not an entry in the vector. If so,
`kMaxFanInUpstreams` bounds *remote* runs only, and a relation's readable
size rises by the share of runs the reading core owns. **State the new
arithmetic**, because §3's `64 × range_size_ids` is what `known-gaps.md`
and `workplan-insert-spreading.md` §9 currently carry.

**CS3 — what the per-core client costs a peer that never uses it.** Core 0
pays two extra calls per `kStepBatch` and per `kStepEof`, each discarding a
tag it does not hold. A peer would pay the same. State it, and state
whether a peer with no open fan-in can skip the call rather than discard
inside it — the difference between **absent and zeroed**, which this
repository already applies to counters and should apply here.

**CS4 — the read surface after both, enumerated rather than claimed.**
R4-M enumerated the surface as one shape from one core. Enumerate it again:
IM0's nine equivalence shapes and H5's four, each run against a spread
relation from **every** core, each reachable or refused with the reason. A
shape reachable but wrong is worse than one refused, so the enumeration is
checked against the equivalence suite rather than asserted.

## 4. Hypotheses — each with its falsifier

**HS1 — the per-core client is core 0's construction plus core 0's fan-out
pattern.** No new mechanism: construct `SessionStepClient` on
`CoreRuntime`, fan `kStepBatch` and `kStepEof` to both consumers, register
`kStepError` to the client.
*Falsifier*: something the client needs is core 0's — the catalog write
path, file growth, or whatever keeps `SessionStepClient` core-0-only for a
reason not yet named. **If so this row reports and stops**: that dependency
belongs to M5, and working around M5 quietly is how M5 stops being one
decision.

**HS2 — the tag demultiplexes correctly on a peer.** Core 0's fan-out is
safe because each consumer discards a tag it does not hold; a peer's
consumers hold disjoint tag sets for the same reason.
*Falsifier*: any tag a peer's client and server could both claim. This is
what the two-claimants comment is really about and it should be tested
rather than inherited — `core_runtime.cpp:539-542` calls a double claim *"a
silent drop, not a fan-out"*, and **silent** is the word that matters.

**HS3 — the self-directed stage keeps range order by construction.** The
merge already orders runs by `lo`; a local run carries the same `lo` and
enters at the same point.
*Falsifier*: the local producer's rows arrive where the ordering cannot
place them, and range order becomes a check rather than a property. RB4 had
to correct exactly this once already.

**HS4 — the unsplit path is unchanged.** `access.ranges.empty()`
short-circuits before any of this, as it has since RB1.
*Falsifier*: RD9(a)'s re-run moves outside its band on either of CD1's two
paths — statement path or cache-fill path, which RB1 separated for this
reason. Stated knowing the analogous invariant went unconfirmed once
(`af36f24`, HR5).

**HS5 — `cores = 1` is untouched.** One core, one range, no peer, no second
consumer to fan to.
*Falsifier*: any measurable change at `cores = 1`. Guideline 2, twelfth
consecutive order.

## 5. Task series — RS rows

| # | Task | Gate |
|---|---|---|
| **RS0** | **The width comment corrected** (§2), and **CS1's design answer written** — §15d's deferred question, in the workplan, before code. `core_affinity.cpp:39`'s message is stale too: it says cross-core reads *"need the step pipeline, which is not built"*, and the pipeline has existed since RB4. It is not built **here**, which is a different sentence | none |
| **RS1** | **The fan-in client on every core**, and **HS1**, **HS2**, **CS3**. `SessionStepClient` on `CoreRuntime`; `kStepBatch` and `kStepEof` fanned to both consumers as `expeditor.cpp:1512-1533` does; `kStepError` to the client. **If HS1's falsifier fires, this row reports and stops** | RS0 |
| **RS2** | **The self-directed stage**, and **HS3**, **CS2**. A core owning some of a relation's runs walks its own locally and fans in over the rest, emitting in `lo` order across both. The ceiling's new arithmetic stated | RS0 |
| **RS3** | **`CheckReadAffinity`'s refusal narrowed to what still cannot be answered.** `WhollyOwnedBy`'s branch is §15d's honest refusal; after RS1 and RS2 the shapes it covers are answerable, and it should cover only what is not. **Narrowed, not deleted** — RB3's lesson is that a route closed at the symptom leaves the class open | RS1, RS2 |
| **RS4** | **CS4's enumeration**, checked against the equivalence suite: IM0's nine shapes and H5's four, from every core, each reachable or refused with its reason | RS3 |
| **RS5** | **The equivalence gate on a peer.** RB5's discipline applied to the case that did not exist until RS1: byte-identical against the same rows unsplit, **straddling the boundary**, from a non-zero core, with RB5's vacuity matrix run over each new mechanism | RS4 |

## 6. Correctness — ahead of any number

- **RS5 is a gate, not a cell.** A peer reading a spread relation and
  returning different rows is wrong, not slow.
- **The vacuity matrix is required, not optional.** RB5 established it and
  IM0 found what it catches: a test can be vacuous twice — asserting the
  wrong half *and* resting on a statement the engine refuses. Revert each
  mechanism, count what catches it, report the count.
- **Two or more contending peers in every spread fixture.** R4-M refuted
  HK4's strong form: `range_alloc.cpp` suppresses on top-owner identity, so
  **a relation written by one peer settles at two ranges forever** and
  never reaches a third. A fixture with one writer measures a two-range
  relation and reports it as spread.
- **The straddling case is required**, and now from a peer. Every defect
  this line has found — the insert head, the range-blind walk, the resumed
  prefix, the fan-in's grouping — returned correct answers for data on one
  side of the cut.
- **`cores = 1` unchanged** (HS5).
- **The kill −9 matrix re-run**, 12 cells × 3 passes. A peer that can now
  read a spread relation reaches cross-owner states by a route that did not
  exist.
- Full suite and `scripts/sim.sh` green before RS1 and after RS5, both
  `build-release`, one sitting, which sitting named.

## 7. Measurement — only what proves the mechanism

This order is not the aggregate's. Four cells, each tied to a hypothesis;
the workload questions belong to whatever runs after it.

`build-release`, rep-interleaved arms — standing rule since RD9(a) reversed
a sign. Fresh server and data file per invocation, per-rep spreads before
any median, rows in = rows out. Anything not run is reported as **not run**.

| cell | measures | reads against |
|---|---|---|
| **S-a** | A self-directed stage against the same run fetched remotely, and against no stage at all | **CS1**, and whether local is worth distinguishing |
| **S-b** | The ceiling re-measured: refusal point and stage count, at k = 4 and k = 8 | **CS2** — the arithmetic §9 and `known-gaps.md` carry |
| **S-c** | A peer's per-`kStepBatch` cost with no fan-in open | **CS3**, absent versus zeroed |
| **S-d** | RD9(a) re-run, both of CD1's paths separately | **HS4** |

**Not here**: the aggregate, and read throughput against §4b's ~95 k
reads/s plateau — *"the number the spread arm has to beat"*. Both want this
mechanism to exist first, and running them inside the order that builds it
would measure a moving subject.

Results to `bench/v2.6.0/`, named by `git describe --tags`, in R4-M's
format: host with physical-versus-logical cores and SMT state, filesystem,
build flags, binary provenance, per-rep tables, findings tagged **measured**
or **source-read** with sites, and a §"what this run does not measure".

## 8. Improvement — what this leaves better

- **A spread relation becomes readable by the workloads that write it.**
  R4-M's reason for reporting the aggregate as not run was not a bad number
  but an impossibility, and this removes it.
- **A session stops depending on which core accepted it.** Today a client's
  ability to read a spread relation depends on where `SO_REUSEPORT` put the
  accept — **not a property a client can choose or observe**, which is the
  same class of leak the cross-owner line closed for writes.
- **A refusal narrowed rather than deleted.** §15d called
  `CheckReadAffinity`'s branch an honest refusal; after this it covers only
  what is still unanswerable, and the honest part survives.
- **Two stale sentences replaced.** The width comment (§2) is the
  justification for a constant that has since become a real limit, and
  `core_affinity.cpp:39` says a pipeline is not built that has existed for
  a day. Both are the kind a later reader reasons from.

## 9. Deliverable

- §15d's design question answered in the workplan, before the code.
- `SessionStepClient` on every core, or HS1's dependency reported and the
  row stopped.
- The self-directed stage, emitting in `lo` order across local and remote
  runs.
- `CheckReadAffinity`'s refusal narrowed to what remains unanswerable.
- CS1–CS4 written into `workplan-insert-spreading.md`, and §3's ceiling
  arithmetic updated wherever it is carried — `known-gaps.md` and
  `workplan-insert-spreading.md` §9 both hold it.
- RS5's equivalence gate with its vacuity matrix; the kill −9 matrix re-run.
- S-a … S-d under `bench/v2.6.0/`.
- Each row landing with its worktree name and short commit **inside the
  sentence that makes the claim**, and its `critics-developer` pass with
  findings applied or rejected by name.

**What this does not close.** The aggregate, which becomes runnable and is
the next order's. §3's three ways out, each still with its owner and none of
them binding. `range_size_ids`'s final value, the operator's on R4-M's
curve. The btree subject problem — **the pump is heap-only**, so eighteen of
twenty-four scenario relations never reach `RangeEligible` at all, and that
is per-range trees' and D1's. M5 and R1, and HS1 may hand this order's first
row straight to them.
