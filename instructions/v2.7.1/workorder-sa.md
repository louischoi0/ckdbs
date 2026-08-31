# Work order SA — auxiliaries under split and 2PC (Cabin, FK, Index)

Drafted 2026-08-31 against `main` at `04403a1` (`v2.7.0-22-g04403a1`).
Governed by `docs/spec/crosscore.md` (CC8-CC13, §6a), `docs/spec/cabin.md`
(§12's class boundary; `assertion.md` §5 wins on conflict),
`docs/spec/foreign-keys.md`, `docs/spec/index.md`,
`docs/spec/cross-owner-txn.md`, and `docs/spec/page-lsn-cross-stream.md` §9.

**Objective.** A relation that carries a secondary index, an Observational
Cabin, or a foreign key today loses either the feature or the split: §6a
gates it from splitting, and `RefuseAuxiliaryOnSplitRelation` refuses the
feature forever once it has split. This order makes the three features and
range-granular ownership coexist — reads and writes correct under 2PC,
serving from every core, with the gates narrowed to what genuinely cannot
be admitted yet (UNIQUE, Bound-Cabin split).

## Standing instruction

The operator's direction for this order: **where a decision is required,
CLA's proposal is accepted.** Each such decision is recorded below as
SA-R*n* with CLA's reasoning attached, so a later reader can see it was
proposed-and-accepted rather than measured. **Constants are not covered**
— no threshold, cap or interval is decided here; each is a measured item
with its cell named in §Measurement.

## Ratifications — SA-R1..SA-R8

**SA-R1 — Secondary indexes are local per range.** Ratifies `index.md`
§13's on-record reading. Reasoning: a global index is one structure
written by many cores (breaks the single-writer rule) or makes every
indexed write cross-core (a 2PC per row); local keeps index maintenance
core-local and unpriced, and moves the cost to read-time fan-out, which
M2's router and M5's fan-in bound. Uniqueness enforcement is **excluded
and stays gated** (SA-R2).

**SA-R2 — The index gate narrows; the UNIQUE gate remains.**
`RangeEligible`'s "an indexed relation does not split" becomes "a relation
carrying a UNIQUE secondary index does not split." IX11 is unbuilt, so
today the narrowed gate admits every indexed relation. Recorded so that
IX11, when built, arrives already knowing its relations cannot split until
cross-range uniqueness is designed.

**SA-R3 — An Observational Cabin is discarded on split and on migration.**
Grounds: `cabin.md` §1's corollary (un-observing is always legal) and
§12.1 (failure response is fall-back to scan — a performance event).
Discard is whole-cabin per `(relation, column)`, not range-scoped:
simpler, and the re-observation cost is the measured item (SA-M4). No
correctness argument is required — that absence is the argument. The
invariant-8 precedent (a deleted trail is performance, never a result)
extends to this class and is written into the rules with this order.

**SA-R4 — The Cabin gate becomes class-conditional.** §6a's "a cabined
relation does not split or migrate" applies to the **Bound** class alone.
Observational-cabined relations split and migrate under SA-R3's discard.
Bound split stays gated (group columns need not include the pk, so groups
straddle boundaries — the Finding 2 failure); Bound **migration** is
opened as a designed task (SA-T7), not by this ratification, on the
durable-load argument: §12.1 makes the Bound class "enforceable at restart
with no rebuild scan," so an incoming owner can load the directory the way
a mount does, ordered before the CC10 grant. SA-T7 verifies that premise
from source before building.

**SA-R5 — The ship-time downgrade is replaced by structure re-derivation,
and kept as the backstop.** A shipped or fan-in stage on a relation the
executing core can see in its own catalog re-derives the index or Cabin
and runs the structure-served step; the descriptor's refusal (and the
downgrade to the walk) stays for any caller outside the sanctioned route.
This is `known-gaps.md:1713-1735`'s recorded improvement, promoted to a
task (SA-T1).

**SA-R6 — F5 is relaxed from "same core" to "2PC-reachable owner."**
Colocation stops being a correctness requirement and becomes what it
always was operationally: the placement optimization (D3 keeps the FK
graph as an input). The correctness path is SA-T4/T5's protocol. The heap
parent refusal (`Unsupported`) and every non-crosscore FK rule (F1-F4, F6)
are untouched.

**SA-R7 — `kConstraintBusy` is ratified** as a distinct status from
`kFkViolation`, retryable, carried on the wire in the `TXN_CONFLICT`
spelling. It was `[PROPOSED]` in F3 since FK-M1; the cross-core check
makes the retry/wrong distinction load-bearing (a busy from a foreign
intent must not read as a violation), so it lands with SA-T4.

**SA-R8 — Cross-core DDL on a split relation is autocommit-only.**
`CREATE INDEX` on a multi-owner relation (SA-T3) follows CB5's rule
unchanged: inside an explicit transaction it refuses and poisons. The
cross-core commit oracle (DT9) is not built by this order; autocommit
scope is what keeps it unneeded, exactly as it did for CB4-CB6.

## Background — the issue register this order covers

Each row is an issue named in the 2026-08-31 survey; the Task column is
where this order answers it. `[source-read]` sites are as of `04403a1`.

| # | Issue | Site | Task |
|---|---|---|---|
| B1 | §6a gates: indexed / cabined / FK relations do not split; Cabin and Bound block migrate too | `crosscore.md` §6a | SA-T2 (narrow), SA-T7 (Bound migrate) |
| B2 | `RefuseAuxiliaryOnSplitRelation` is permanent: CREATE INDEX/CABIN/FK on a 2+-range relation refused forever, auto-Cabin path included | `known-gaps.md:923-932`, `src/catalog/catalog.cpp` | SA-T3 (index), SA-T2 (cabin), SA-T6 (fk) |
| B3 | Ship-time downgrade voids index/Cabin on peer-owned relations (`kIndexProbe`/`kCabinProbe` → `kScan`) | `known-gaps.md:1713-1735`, `step_descriptor.cpp` | SA-T1 |
| B4 | Cross-core FK refused `NotImplemented` (F5); 2PC §8 lists it out of protocol | `foreign-keys.md` F5/§7, `cross-owner-txn.md` §8 | SA-T4, SA-T5 |
| B5 | FK validation-to-commit window sound only against local latest-committed state | `foreign-keys.md` §7 | SA-T4 (intent) |
| B6 | Read-only participant pays a durable prepare + full decide (7-30× the read) — prices every FK forward check | `cross-owner-txn.md` §1a/§8, XD6 | SA-T0 (prerequisite) |
| B7 | Per-range local vs global undecided; uniqueness bound to it | `index.md` §13 | SA-R1/R2 |
| B8 | Observational/Bound conflated in the gate text | `crosscore.md` §6a vs `cabin.md` §12 | SA-R4, SA-T2 |
| B9 | Var-heap gate: a spilling schema does not split (one kVarHeap page serves rows on both sides of a boundary) | `crosscore.md` §6a | SA-T8 |
| B10 | `SHOW INDEXES` on core 0 reads a maintenance-moved peer root as a subtree (diagnostic) | `known-gaps.md:1855-1863` | SA-T9 |
| B11 | Index build window can expire before a late core-0 commit (unreachable at shipped timeouts) | `ddl-transactional.md` §5e | recorded; close stays "commit-leg bound or DT9" — out of scope (SA-R8 keeps DT9 unneeded here) |
| B12 | Bound split: straddling groups, empty registry admits unchecked (Finding 2) | `crosscore.md` §6a, `bench/v2.2.0/results-shipping-part-a-*` | stays gated (SA-R4) — named non-goal |
| B13 | `kConstraintBusy` `[PROPOSED]` only | `foreign-keys.md` F3 | SA-R7, SA-T4 |
| B14 | Cabin banking refused in-transaction; enrolled reads cannot bank | `cabin.md` §6a | unchanged by decision — banking stays outside transactions; re-observation after SA-R3 discard runs on the ordinary autocommit path |

## Conclusions the design commits to

1. **The routing thesis carries all three features.** An FK value is a
   parent pk; a Cabin entry holds a pk; an index entry is `key‖pk‖covered`
   — each answer names its range, each range names its owner
   (blueprint §2). Nothing here invents a discovery mechanism.
2. **Writes stay core-local; coordination is read-shaped.** Local indexes
   are owner-maintained; Cabin observes per owner; the only cross-core
   *write*-coupled obligation left is FK's, and it rides the existing 2PC
   as a read participant plus an intent record — no new commit protocol.
3. **Correct-but-slower always exists as the fallback.** The downgrade
   walk (B3), the FK busy-retry (F3), the Cabin discard (SA-R3): every
   path degrades to a priced, correct form rather than a refusal, except
   where refusal is the correctness statement (UNIQUE, Bound split).

## Hypotheses

- H1. Re-derived structure service (SA-T1) recovers ≥ the local
  structure-vs-walk gap on a peer stage (Cabin serve ~67 µs vs per-build
  ~560 µs is the local ratio; the shipped ratio is the measured cell).
- H2. Cabin-routed single-stage probes (SA-T5a) beat the k-stage fan-in on
  observed keys by ~(k-1)/k of stage cost at the measured stage price.
- H3. FK forward check under SA-T4 costs one enrolment + one intent write
  on top of the probe, and after SA-T0 the parent leg's commit adds no
  waited sync on the read-only path.
- H4. SA-R3's discard costs re-observation only: post-split serve rate
  recovers to pre-split levels within the n=2 witness discipline, with no
  correctness deltas in the contract suites.
- H5. A per-owner index build (SA-T3) is each owner's PW1c-6b build over
  its own ranges, coordinated but not serialized by core 0, and its
  elapsed time approaches max(owner build times), not their sum.

## Scope

**In.** SA-T0..SA-T9 below; the spec amendments in §Where-this-lands; the
gate rewrites in `RangeEligible` / `RefuseAuxiliaryOnSplitRelation`; the
measured cells in §Measurement.

**Out.** UNIQUE secondary indexes (IX11) and cross-range uniqueness. Bound
Cabin **split**. CASCADE/SET NULL. Composite FKs. Multi-range transactions
(R6) and the mover (R5) themselves — this order widens what they will be
allowed to touch, and builds none of them. DT9. Index-only scans.

## Tasks

Dependency order; a task names its gate. Every refusal this order touches
keeps byte-position discipline and the two-code rule (`Unsupported` vs
`NotImplemented`).

**SA-T0 — Read-only participant optimisation.** The XD6 lever, built
first because SA-T4 is priced on it: a participant whose enrolment saw
only reads (and, after SA-T4, intent records — decide whether an intent
is a "write" for this purpose from the recovery argument, not
convenience; CLA's accepted proposal: **an intent is prepare-relevant** —
it must survive to be visible to a reverse check racing the decide — so
the optimisation applies to participants with no writes *and* no
intents, and an FK-only enrolment keeps the durable prepare) skips the
sync legs the decision record does not need. Measured against
`bench/v2.5.0/results-rr-read-half-*`, which prices exactly this and
nothing else. Gate: none.

**SA-T1 — Structure re-derivation on the executing stage (SA-R5).** A
self-directed or peer stage whose core can resolve the relation in its
own catalog compiles the structure-served step (`kIndexProbe`,
`kCabinProbe`) instead of the downgraded walk; the descriptor refusal
stays as the backstop. Invalidation rides the existing
`kCatalogInvalidate` path — note the `catalog_version()` caveat
(`known-gaps.md:1864-1877`): re-derive per stage open, cache nothing
across a suspension. Gate: none. Unblocks value on today's unsplit
peer-owned relations before any gate moves.

**SA-T2 — Gate rewrite, cabin class split (SA-R2/R3/R4).**
`RangeEligible`: indexed → UNIQUE-indexed; cabined → Bound-cabined (the
enforcer answers the class, not `TableAccess` — the assertion gate's own
precedent). Split and CC10 migration perform the Observational discard
(SA-R3) as a logged engine decision with a `SHOW META` counter.
`RefuseAuxiliaryOnSplitRelation`: `CREATE CABIN` (manual and optimizer
path) is admitted on a split relation — observation is per-owner and
needs nothing global. Gate: SA-T1 (a cabin a peer stage cannot serve is
observation without a consumer).

**SA-T3 — `CREATE INDEX` on a split relation: per-owner builds.** Each
range owner runs the PW1c-6b owner-build over its own ranges; core 0
coordinates the catalog transaction and the single publishing event
(one `sys.indexes` row; per-range subtree roots recorded the way the
range directory records per-range entry pages — CC9's shape).
Autocommit-only (SA-R8). A build failure on any owner rolls the DDL back
losers-at-mount style; orphaned subtrees orphan as dropped-index pages
do today. `RefuseAuxiliaryOnSplitRelation` drops its `CREATE INDEX` arm
when this lands. Gate: SA-T1, SA-T2.

**SA-T4 — FK forward check cross-core: enrolment + row-scoped reference
intent (SA-R6/R7).** The child-side probe resolves the parent pk through
the range directory to one owner; if foreign, the statement's
transaction enrols that owner as a participant (autocommit wraps as the
shipped forms already do) and the probe leaves a row-scoped intent keyed
`(parent pk, enrolment)`. A parent-side DELETE's reverse check meeting a
live intent answers `kConstraintBusy` (retryable, wire-spelled
`TXN_CONFLICT`) — fail-fast preserved, no blocking. Intents resolve with
the decide; recovery resolves them with the transaction (they live in
the participant's prepare footprint — SA-T0's clause). Gate: SA-T0.

**SA-T5 — FK reverse check and the router.**
(a) *Router:* a probe whose key the session core's own Observational
Cabin has observed opens one stage at the entry's range instead of the
fan-in — miss falls through to (b). (b) *Fan-out:* reverse check fans a
boolean probe to child-range owners over M5's fan-in (AG3 pipeline;
one-bit replies, `kMaxFanInUpstreams` bounds apply); each owner answers
from its local child-fk-column Cabin (F6's nomination, now per-range) or
its walk, busy on live intent. Gate: SA-T1, SA-T4.

**SA-T6 — FK declaration gates rewritten.** `CREATE`-time: the
cross-core pair stops refusing `NotImplemented` and admits under SA-R6;
the split-parent/child gate in `RangeEligible` lifts once SA-T4/T5 are
in (the window argument is answered by intents, the reverse check by
T5). `RefuseAuxiliaryOnSplitRelation` drops its FK arm. Heap-parent
`Unsupported` unchanged. Gate: SA-T4, SA-T5.

**SA-T7 — Bound Cabin migration: durable-load handover.** Verify from
source (`assertion.md`, AST04 build) that the restart path loads the
directory from durable pages with no rebuild scan; if it does, add one
step to CC10's sequence — incoming owner completes the load between
step 3 (durable directory row) and step 4 (grant), so no window exists
where an empty registry can admit. If the premise fails at the source,
stop and report; do not build a second loader. Gate: SA-T2; independent
of T3-T6.

**SA-T8 — Var-heap under a split: page-scoped read grants, no
relocation.** CLA's accepted proposal: spilled values never move
(invariant 14 stands); a split grants the incoming owner read rights
over exactly the pages the moved rows' spills name — the
`CatalogSpillPages` / CB2 mechanism generalized from catalog relations
to a split's moved row set — and **new** spills land in the writing
owner's own pages. No format change, no relocation, one asymmetry:
pre-split spills remain owned (write/free-wise) by the original core,
which reclamation work inherits, stated not hidden. The §6a var-heap
gate lifts for read-correctness; the gate's page-*ownership* half is
satisfied by the grant being read-only. Gate: SA-T2.

**SA-T9 — `SHOW INDEXES` cross-core probe fix.** The diagnostic reads a
maintenance-moved peer root as a subtree; answer it through the same
re-derivation SA-T1 builds (resolve the root from the executing core's
catalog) rather than a special case. Gate: SA-T1. Small; bundled here so
B10 has an owner.

## Measurement

Discipline: `build-release` only, `git describe --tags` naming, ≥3 runs
with spread, interleaved A/B arms, per-statement fixed costs reported as
server CPU. Cells:

- **SA-M0** (T0): commit p50/p99 for a read-only-participant transaction,
  arms pre/post, serial and 8-coordinator, against
  `results-rr-read-half-*` and `results-xe-ack-at-append-*` baselines.
- **SA-M1** (T1): structure-served vs downgraded-walk stage, per shape
  (point probe, range probe, join inner), on unsplit peer-owned and on
  split relations; H1's cell.
- **SA-M2** (T5a): routed single-stage vs k-stage fan-in at k ∈ {2,4,8},
  observed vs unobserved keys; H2's cell.
- **SA-M3** (T4): FK'd INSERT local vs cross-core, before/after SA-T0;
  intent write cost isolated; H3's cell. Busy-rate under a racing
  parent-delete workload reported beside it.
- **SA-M4** (T2): serve-rate recovery after a split's discard (H4), and
  the discard's own cost at the split.
- **SA-M5** (T3): per-owner index build elapsed vs single-owner baseline
  at 2/4/8 owners (H5).
- **SA-M6** (T8): spilled-read latency across a boundary (granted page)
  vs owner-local, and grant-set size distribution at the split.

Every cell lands in `bench/<version>/` with its commit in the filename;
no result is diffed against a newline-protocol-era number.

## Where this lands

- `docs/spec/index.md` — §13 loses "local vs global" (SA-R1), gains the
  per-range subtree/root record and the UNIQUE gate note (SA-R2, T3).
- `docs/spec/cabin.md` — §11 closes "Cabin under a split" for the
  Observational class (SA-R3/R4, T2); §12 notes the class-conditional
  gate; the router lands in §7's serve rules (T5a).
- `docs/spec/foreign-keys.md` — F5 rewritten (SA-R6), F3's
  `kConstraintBusy` de-proposed (SA-R7), §7's cross-core and split
  bullets closed by T4-T6; the intent record specified in a new §.
- `docs/spec/cross-owner-txn.md` — §8 drops "cross-core foreign keys";
  §1a amended by SA-T0 with the intent clause.
- `docs/spec/crosscore.md` — §6a rewritten per SA-T2/T6/T8; CC10 gains
  T7's load-before-grant step; §9's auxiliary-placement open items close
  to the residue (UNIQUE, Bound split).
- `docs/rules/rules.md` — the invariant-8 extension to Observational
  discard (SA-R3).
- `docs/inflight/known-gaps.md` — B2, B3, B10 entries amended; B12 and
  B11 restated with their named non-goals.
- `src`: `catalog.cpp` (both gates), `step_descriptor.cpp` (T1),
  `command_dispatcher.cpp` (T5), 2PC services (T0, T4),
  `status.hpp` (`kConstraintBusy`).

## What this order does not claim

It does not build the mover (R5) or multi-range transactions (R6) — it
widens the relation set they may legally touch. It does not decide any
constant (routing thresholds, intent caps, fan-in sizing stay measured
items). It does not lift the UNIQUE or Bound-split gates, and it does not
touch the DDL build-window close (B11) beyond keeping DT9 unnecessary via
SA-R8. Reclamation debts it creates (orphaned pre-split spill ownership,
discarded-cabin re-observation traffic) are named where they arise and
inherited by the reclamation work, not resolved here.

---

## Row status (CLA, appended as rows land)

| row | status |
|---|---|
| SA-T0 | **Built 2026-08-31** at `1beda80`. A participant whose enrolment wrote nothing writes no `TXN_PREPARE` and takes no sync for it; the intent clause is written into the predicate's site ahead of SA-T4. Spec: `cross-owner-txn.md` §1a. Cell SA-M0 not run |
| SA-T1 | **Built 2026-08-31.** `exec::RestructureForExecutingCore`, wired at both remote shapes — the leaf stage (literal arms) and the consuming stage (**correlated** arms, the join inner), the compiler's own ladder rather than a second one. `ShippedStructureTest` asserts the *kind*, because the pipeline suite's byte-identity cannot fail either way. Cell SA-M1 not run |
| SA-T2..T9 | — |

**Amendment riding with this order** (2026-08-31, operator):
`instructions/v2.7.1/amendment-spreading-per-relation.md` makes insert
spreading a per-relation option shipping **off**. It changes SA's urgency
and none of its content — every gate SA narrows is still reachable the
moment a relation asks to spread.
