# Physical Optimizer — Workplan (Part I: PX01–PX08 · Part II: PHY01–PHY08)

Status: **READY FOR EXECUTION** — spec adopted 2026-08-09; PX01 done.
Spec: `feat-physical-optimizer.md` (normative, decisions R1-R12). Related:
`heap-and-tuple.md` §7 and §3.1a, `waystone-concpets.md` §3.1,
`feat-cabin.md`, `spec-eviction.md` EV1, `spec-pattern-tracking-levels.md`,
`txn.md` §9.

Task prefix is `PX` — the repo already carries three `P`-numbered schemes
(`crosscore.md` `P0`-`P8`, Waystone and protocol `P01`-`P17`), so cite the
file with any bare number, per the standing rule.

Execution order is the numbering order. Each item lists scope, deliverables,
and acceptance. All new code follows the engine rules: explicit `Status`
(no throw), core-local state, deterministic tests, field-wise memcpy page
access, shift/mask only for persisted encodings, no floating point on
statement paths.

**The one rule carried in from `workplan-aggregate-perf.md`: re-measure the
premise before building the fix.** Every benefit number the planner prints
is a prediction; nothing in this plan treats a prediction as a result.

---

## PX01 — Spec adoption and cross-document reconciliation  **[DONE 2026-08-09]**

**Scope.** Make the dangling references true and move the settled decision
out of the open lists. No code.

**Deliverables.**
- `spec-eviction.md` EV1 and `spec-pattern-tracking-levels.md`: point the
  "lazy-decay score (R1)" citations at `feat-physical-optimizer.md`.
- `rule-fixed-length-tuple.md` status line: "the physical-optimizer
  blueprint" → this spec, by name.
- `heap-and-tuple.md`: §3.1a's epoch-storage `[OPEN]` → decided per R4
  (common header `reserved0`, u64, no format bump), pointing here; §7's
  "keep the B+ tree consistent" parenthetical amended per R8 (btree-clustered
  relations only; a heap relation's mover maintains nothing but the epoch).
- `CLAUDE.md`: Open Decisions "per-page epoch counter storage" struck as
  decided-with-pointer; a Core Architecture entry for this feature lands with
  PX08, not here.

**Acceptance.** `grep -rn "blueprint\|lazy-decay" docs/` resolves every hit
to an existing file; no doc names a physical-optimizer decision this spec
does not define.

---

## PX02 — The lazy-decay score library (R1)  **[DONE 2026-08-09]**

**Scope.** `include/kds/stats/decay.hpp`: the `{score, last_bump}` pair,
touch and read over an injected `sched::Clock`, fixed-point per R1's
`[PROPOSED]` representation. Library only — no consumer wiring beyond what
PX05 needs.

**Deliverables.**
- `DecayState` (two words), `Touch(state, clock)`, `ValueAt(state, clock)`;
  named `constexpr` for the fixed-point scale with the derivation comment.
- `decay_half_life` config key threaded to the one construction site;
  unknown-key startup error behavior untouched.
- No-clock degradation: with a null clock the score is a raw counter.

**Acceptance.** Unit tests under `ManualClock`: exact halving at one
half-life, quarter at two; touch-then-read equals read-then-touch minus one;
no-clock counter semantics; zero allocations per touch.

---

## PX03 — The page epoch field (R4)  **[DONE 2026-08-09]**

**Scope.** `page_header.hpp`: `reserved0` becomes `relayout_epoch` with
read/write accessors; every page-format path (`PAGE_INIT`, heap format,
undo, index, cabin-bound) continues writing 0. A bump API exists and is
called by nothing — the eviction sweep's precedent, stated plainly in the
header comment.

**Deliverables.**
- Accessors with the offset `static_assert`s; the R4 pairing rule ("never
  accept on epoch equality alone") stated at the accessor, where a consumer
  will read it.
- No format bump: the claim tested, not asserted.

**Acceptance.** A page image written by the pre-change build mounts and
reads epoch 0; round-trip write/read; checksum discipline unaffected (the
field is inside the checksummed span already).

---

## PX04 — Real epoch reads at the validation sites  **[DONE 2026-08-09]**

**Scope.** Close the two documented vacuous checks. `trail_recorder.hpp`
records the page's current epoch instead of the literal 0
(`trail_store.hpp`'s stated gap); replay compares recorded against current
through the existing per-entry validation; `CabinStore`'s write hook records
it; `exec/tuple_verify.hpp` — the one shared verifier — performs the
comparison for both consumers at one site.

**Deliverables.**
- Recorder/replay and Cabin record-and-compare paths; a mismatch is a
  per-entry miss with the ordinary fall-through, never an error.
- Contract-test extensions: `waystone_contract_test.cpp` and the Cabin suite
  each gain the hand-bumped-epoch case — bump a page's `relayout_epoch`
  directly, then require per-entry miss, heal, and a byte-identical reply.

**Acceptance.** Both suites pass with the new case; all pre-existing
configurations byte-identical; the trail page layout does not change (the
entry's epoch field already exists and was written 0).

---

## PX05 — The planner (R2, R9)  **[DONE 2026-08-09]**

**Scope.** `include/kds/stats/relayout_planner.hpp` (+ `src/stats/`): pure
planning over `sys.access_stats`, the catalog, and — per-relation form
only — a read-only, stoppable, budget-charged chain walk for delete-mark
density and live fill. Produces `RelayoutPlan`s for the three v1 plan kinds
(`compact`, `cluster`, `defrag`), each carrying predicted benefit per R9 and
its blocking gate per §6.

**Deliverables.**
- `RelayoutPlan` with both R9 fields (predicted, measured — the latter
  unpopulated in v1 so the promotion comparison needs no format change).
- Decayed shape weights via PX02; the all-relations form performs no
  relation walk by construction.
- Benefit math as a pure function with unit tests over synthetic stats.

**Acceptance.** Deterministic tests: seeded stats produce the golden plan
set; the walk respects `max_rows_touched`; a page-fetch counter proves the
bare form fetches no relation page.

---

## PX06 — `SHOW RELAYOUT`, config keys, refusals (R3, R12)  **[DONE 2026-08-09]**

**Scope.** The surface: `SHOW RELAYOUT [<relation>]` per spec §5's report
shape; `physical_optimizer = off | shadow` with `on` refused at startup
naming §6's gates; `decay_half_life` validated positive.

**Deliverables.**
- Dispatcher verb + report rendering (client-manual table row included);
  `off` answers the one-line disabled notice.
- Startup refusal text naming all three gates.
- Fingerprint safety: `relayout` stays an ordinary identifier; golden corpus
  gains the new statements and modifies none.

**Acceptance.** E2E tests over a live server: report golden-matched; `on`
refuses at startup with the exact text; corpus diff shows additions only;
`SHOW RELAYOUT` changes no query result (advisory family assertion).

---

## PX07 — Shadow measurement  **[DONE 2026-08-09]**

**Scope.** Run the shadow report against a real workload and record what it
says — the first data for gate 2's eventual decision, and the verification
of the zero-cost claim.

**Deliverables.**
- `bench/results-physical-optimizer-shadow.md`: `tools/scenario0_stockmarket.py`
  (and the freight scenario) with `physical_optimizer=shadow` vs `off` —
  interleaved A/B, Release build, server CPU per the
  `workplan-aggregate-perf.md` measurement rules.
- The report's own output for both scenarios, archived: per-relation plan
  kinds, predicted benefits, decayed weights.

**Acceptance.** The A/B shows the pull-only planner at measurement noise
(the claim is "zero idle cost" — prove it); the results doc carries commit,
full percentile table, and an insight about the engine, per the bench
conventions.

---

## PX08 — Close-out: status and docs  **[DONE 2026-08-09]**

**Scope.** The repo's standing rule: when a decision lands, update
`CLAUDE.md` and the owning spec, and move items out of Open Decisions.

**Deliverables.**
- `CLAUDE.md`: Core Architecture entry for the physical optimizer (shadow
  v1, R-numbered decisions, the three gates, epoch landed); Documents-table
  row; Open Decisions updated (epoch storage decided; mover/cadence/gates
  listed as the open remainder, each naming its owner).
- `feat-physical-optimizer.md` status → adopted, with any `[PROPOSED]`
  amended during build marked as such.
- `overview.md`/`README.md`: while editing for the optimizer's status, fix
  the stale "There is no `CREATE INDEX`" claim and roadmap step 2 —
  falsified since `feat-index.md` shipped. Unrelated to this feature, found
  by this work, cheapest to fix here.

**Acceptance.** Spec review only. `SHOW ACCESS`'s client-manual row and
`heap-and-tuple.md` §7 no longer say "nothing consumes this" without
qualification — the planner consumes it, in shadow.

---

## Where to pick this up

PX01 and PX02 are done (2026-08-09). PX02 built the library as R1 proposed —
`include/kds/stats/decay.hpp`, Q24.8 fixed point, a 16-bucket Q8 fraction
LUT (worst-case overestimate 2^(1/16) − 1 ≈ 4.4%, exact at whole
half-lives), saturating at the top, backwards-clock-proof — with
`decay_half_life` parsed in seconds, validated `1..UINT64_MAX/1e9`, and
defaulted to the `[PROPOSED]` 600 in `Expeditor::Config` (`kds.conf.sample`
and the client manual carry it). Twelve unit tests plus two config tests;
the zero-allocation claim is asserted via `tests/alloc_counter.hpp`, not
stated. PX03 (2026-08-09) renamed the common header's `reserved0` to
`relayout_epoch` in place — same offset 16, same size, so the pre-change
image test writes the old layout by hand and reads epoch 0 through the new
accessor — with `Get`/`Set`/`BumpRelayoutEpoch` beside the LSN accessors,
the pairing rule stated at the declaration, and a test proving the field
sits inside the checksummed span (a bump without a restamp is detected).
`BumpRelayoutEpoch` is called by nothing, as specified. PX04 (2026-08-09)
made the comparisons real: the executor captures the epoch under the row's
span (`exec::TouchedTuple` grew to six integers, reported verbatim as u64 —
each store narrows to its own entry width and owns that decision),
`VerifyTupleAt` gained the `recorded_epoch` parameter and a
`kEpochMismatch` outcome checked before the slot read, `TrailLocation`
carries the entry's epoch to the consult site, and both heal paths stamp
the healed page's *current* epoch (a heal that wrote 0 against a bumped
page would miss and re-heal forever — the subtlety worth carrying). The
Cabin write hook reads the epoch lazily, once per statement that actually
appends. Both contract suites gained the hand-bumped-epoch case —
`ABumpedPageEpochMissesHealsAndChangesNoReply` in each — proving miss,
heal at the new epoch, and byte-identical replies; no entry layout moved.
PX05 (2026-08-09) built the planner: `stats/relayout_planner.hpp/.cpp`,
with **the bare form taking no PageStore parameter at all** — "never walks
a relation" by signature, and additionally proven by a counting-store test.
Sharpenings found while building: `ListTables()` carries objects that are
not resolvable relations, so the bare form skips what `InitTableAccess`
refuses while the *named* form reports it as the caller's error; the
decayed shape weight treats a row's whole `use_count` as at `last_seen`
(stated approximation — a truer weight is a collection change, R2);
`cluster` and `defrag` carry no predicted number because their inputs
(a hot set; sequential-I/O gain) are respectively uncollected and outside
R9's pages metric — stated, not faked. The survey is a census (no MVCC:
`delete_marked` is an upper bound, which is gate 1's whole point), priced
through `exec::Budget` per slot, refusing rather than truncating. Golden
plan set pinned over 400 rows / 300 delete-marks. PX06 (2026-08-09) built
the surface: `SHOW RELAYOUT [<table>]` (`HandleShowRelayout`, rendering in
the SHOW ACCESS style), `physical_optimizer = off|shadow` defaulting to
shadow with `on` refused at startup naming all three gates (pinned by a
test asserting each gate's name in the message), `off` answering the
one-line notice, and the corpus gaining 9 lines - `SHOW RELAYOUT` refusing
as SHOW TABLES does, plus two SELECTs pinning `relayout` as an ordinary
identifier - with zero modified. The dispatcher takes the mode and
half-life through a `set_relayout` setter (`set_aggregate_limits`'s
precedent), so no construction site moved. One jurisdiction finding:
`ListTables` carries `pattern_defs` and `assertions` (the two catalog
relations in user tuple format), and listing gated plans for them would
have been a lie in the hopeful direction - §4 forbids the mover touching
catalog pages, permanently. So the bare form skips system relations, the
named form answers `plans=none reason=catalog-relation-outside-mover-
jurisdiction` and is never surveyed, and `RelationReport.system_relation`
carries the distinction. PX07 is **done** (2026-08-09,
`bench/results-physical-optimizer-shadow.md`): zero idle cost verified at
exact noise — +0.02% TPS on the freight scenario, +0.06% median on the
stockmarket one, both far inside the 2-6% run-to-run spread, as the
mechanism predicted (the two modes differ in one unexercised branch);
the report's own price is ~60 µs server CPU for the bare form and
~60 µs + 24 ns per slot examined for the survey, fitted three independent
ways. The archived reports carry the first gate-2 data, and it points at
scoping before gates: **both real workloads put every hot chain-walk shape
on btree relations (outside R5's mover scope) while the mover-eligible
heap relations are write-only ledgers with shapes=0** — and compact
honestly predicts 0 pages saved everywhere, because nothing deletes and
updates leave no dead heap tuples; the synthetic 33%-delete-mark sweep is
what exercised the arithmetic (1/4/46 pages at 200/1K/10K rows). Named
follow-up: commit the pricing harness as `tools/relayout_price.py`. PX08
(2026-08-09) closed out ahead
of it, since none of its edits depend on PX07's numbers: the CLAUDE.md
Core Architecture entry, Documents row and a Physical-optimizer
Open-Decisions section (the three gates each naming their owner);
`heap-and-tuple.md` §7 now says the shadow planner consumes the
statistics; the spec status reads built-through-PX06 with every
`[PROPOSED]` built as proposed; and `overview.md`/`README.md` dropped the
falsified "there is no CREATE INDEX" claim (three places) plus the stale
"aggregations are on the roadmap" line, and the component table now states
shadow-built/mover-absent honestly.

---

# Part II — the Cabin controller (PHY01–PHY08)

**Merged in at the 2026-08-09 branch merge** (authored as a standalone
"Physical Optimizer v1" workplan on a parallel branch; the umbrella spec's
Part II divider carries the history). Spec: `feat-physical-optimizer.md`
Part II (`§II.1`-`§II.7`, decisions PO1-PO10 — normative). Related:
`feat-cabin.md`, `workplan-eviction.md` (EVT06 scan ring is a hard
dependency of PHY04), `workplan-testing.md`. **PHY01-PHY08 are all built
— the series closed 2026-08-10** (PHY04/PHY06/PHY08 that day, over the
EVT03/EVT06 substrate built for it; the close-out's numbers are
`bench/results-cabin-optimizer.md`). What remains sits outside the
series: the restart posture (managed state and decision log are
memory-resident, re-observation rebuilds them — stated, not owned) and
the tuning follow-ups recorded under PHY08's honest tails. Part I's `stats/decay.hpp` is the R1
implementation PHY01's S1 reuses (it grew the N-point `Accumulate` for
S2's decayed sums); PHY02's pure core sits in `stats/cabin_optimizer.hpp`
with no engine-effect includes, per PO10; PHY07's golden traces are
`tests/testdata/cabin_optimizer_traces.txt`.

Engine rules apply throughout: explicit Status errors, thread-per-core
core-local state, deterministic tests, decide/execute phase separation
(PO10 structural requirement — enforced by construction: the decision core
lives in a module with no engine-effect includes).

## PHY01 — Statistics plumbing (S1–S3)  **[DONE 2026-08-09]**

**Built.** `stats/optimizer_signals.hpp/.cpp` is the collector: S1/S2 as a
per-fingerprint `{executions, pages}` DecayState pair (the decayed mean is
their ratio — division left to PHY02's 16.16 core, so nothing rounds
twice), S3 as per-cabin `{lookups, hint_failures, coverage_misses}`,
bounded maps (`kMaxTrackedFingerprints`/`kMaxTrackedCabins`, 4096
`[PROPOSED]`) evicting the coldest to admit a newcomer — restarting
evidence, never fabricating it. `Snapshot()` is versioned, decay-epoch
stamped, sorted by id for byte-identical determinism, and is the one read
PHY02 will perform. Four things the build decided or found. **S2's counter
had to be built**: `StepStats::pages_fetched` — exact for walks (counted at
the visitor's page transitions), one per keyed access (the VM sees a
descent as a call, not a path; the undercount is the honest direction for
a signal that exists to price scans) — rendered as `pages=` in ANALYZE,
and the fix rode with it that `StepStats::operator+=` had been dropping
the three index counters from `Total()`. **S3's attribution lives in
`CabinStore`**: `NoteHit`/`NoteMiss` forward to the collector and the new
`NoteHint(cabin_id, ok)` is called from the two hint sites (step VM,
fk_check) that already bumped the per-step counters — per-cabin
attribution those counters cannot carry. **The dispatcher's statement
identity widened**: `instance` was derived only when Waystone could use it
(replayable step + recorder/replay on), which excluded exactly the
scan-only shapes the CREATE decision prices; with a collector wired, every
fingerprinted SELECT derives it, and the trail/replay reads still guard on
their own switches. **The ACTIVE-baseline freeze is deferred to PHY03**,
where the lifecycle state it freezes into exists. Wired at the expeditor
(collector before its two feeders, for destruction order), and the
ordinary/aggregated SELECT paths now run with a hoisted `ExecStats` — the
VM always counted internally, so the change is a member reuse, not new
cost. Tests: decayed-mean pair, snapshot immutability/version/sort, cap
eviction, null-clock degradation, and the scripted end-to-end (dispatcher
in, snapshot out, exact counts under no clock).

**Scope.** Make the three input signals collectible into a snapshot.

**Deliverables.**
- S1: per-fingerprint R1-decayed execution frequency exposed to snapshot
  construction (reuses `stats/decay.hpp`; add per-fingerprint accumulation
  if currently only per-relation).
- S2: executor counter "pages scanned" per statement, aggregated per
  fingerprint (decayed mean). Carried baseline: when a Cabin becomes
  ACTIVE, freeze the pre-Cabin `P_scan` into its state (spec §II.4).
- S3: Cabin hint hit/failure and coverage-miss counters (some exist via
  hint-heal accounting; unify and expose).
- `Snapshot` type: immutable, versioned, fixed-point fields, decay epoch
  included; construction is a single home-core step.

**Acceptance.** Unit tests for decayed accumulation; snapshot immutability;
counters correct under scripted workloads.

## PHY02 — Decision core (pure)  **[DONE 2026-08-09]**

**Built.** `stats/cabin_optimizer.hpp/.cpp`: `Decide(Snapshot) → ActionSet`
in unsigned 16.16 (saturating multiply/divide, zero-divide answering the
harmless 0), an ordered managed table so iteration order is part of
determinism, and time taken only from `snapshot.decay_epoch`. The
lifecycle rides `Decide` plus three completion edges (`NoteCreated`/
`NoteBuildFailed`/`NoteDropped`) that PHY04 will call and the tests drive.
It needed one **PHY01 amendment**: §II.4's Σ_i sums fingerprints *served
by candidate c*, and a bare pattern_id names no column - so
`CandidateRef {rel_oid, col_pos, cabin_id}` rides the snapshot, derived by
the dispatcher from the compiled chain (a kCabinProbe step names its
Cabin; else the first kFilterScan's filtered column is what a Cabin would
serve). Three v1 model choices, stated in the header so they are revisited
rather than discovered: P_rel is the largest serving fingerprint's mean
page cost (the observed scan *is* the observed build cost), CREATE's
budget admission uses P_rel/8 `[PROPOSED]` until `NoteCreated` reports the
real size, and EXTEND's marginal pair scales both B and C by the
coverage-miss share. Two rule refinements the tests forced: a managed
candidate whose shape vanished from the snapshot still gets a (zero)
evidence entry, and zero benefit decays an ACTIVE regardless of cost -
otherwise a dead Cabin is forgotten alive. The swap emits its DROP
*before* its CREATE so executing the set in order lands back under
budget. All four acceptance oracles pass: golden ActionSets (exact 16.16
B/C values pinned), hysteresis (100 noisy snapshots inside the θ gap,
zero actions), the budget invariant across admission/refusal/swap, and
bit-identical traces from identical streams.

**Scope.** `CabinOptimizer::Decide(Snapshot) → ActionSet` implementing
spec §II.4 exactly. **No I/O, no clock, no allocation of engine resources,
no float.**

**Deliverables.**
- Fixed-point (16.16) B/C evaluation, rule table (CREATE / EXTEND / HEAL /
  DECAYING / DROP / recover), N_confirm tracking, cooldown tracking,
  budget arbitration with θ_swap replacement selection.
- ActionSet as plain data (target Cabin/candidate id + action + reason
  code + score snapshot) — directly serializable into the decision log.
- Jurisdiction filter: Bound Cabins and Cabins not owned by the optimizer
  excluded at snapshot construction; debug assert in Decide that no such
  id appears.

**Acceptance.** Pure-function unit tests: golden ActionSets for scripted
snapshot sequences; hysteresis test (stationary noisy workload ⇒ zero
create/drop oscillation over long sequences); budget invariant test
(Σ pages ≤ budget after every ActionSet); arithmetic determinism test
(identical snapshots ⇒ bit-identical ActionSets).

## PHY03 — Catalog state and decision log  **[DONE 2026-08-09]**

**Built, sized to what the engine can honestly persist.** Observational
Cabin sets are memory-resident by design (`feat-cabin.md` §9: only the
`sys.cabins` row persists), so "persistence of lifecycle state" means
exactly three things here, and each landed. **The ownership tag costs
nothing**: `sys.cabins.origin` already reserved `kCabinOriginAuto` for "a
future promotion pipeline" - which this controller is - so PO1's tag is
that value, pinned by a restart round-trip test (a second `Catalog` over
the same pages reads the row back, origin intact). **The decision log**
is the controller's own bounded ring (`decision_log_capacity`, 1024
`[PROPOSED]`; the wrap and oldest-first order tested at capacity 2), each
record = the ActionItem plus the `{snapshot version, decay_epoch}` digest
- PO8's "logged with the inputs", memory-resident, loss-on-crash
documented as acceptable because the state machine re-derives from
re-observation. **The frozen P_scan baseline** landed as the load-bearing
piece: frozen at the CREATE decision, it prices an ACTIVE/DECAYING entry's
B and C (live frequency × frozen mean), because an ACTIVE Cabin makes the
very scans it replaced cheap and a live-priced benefit collapses the
moment the Cabin works - the controller would drop its own success and
churn. The harness gained the `served-cheaply` scenario proving it:
post-CREATE the observed page cost collapses to the probe cost and the
trace is **one action, ever**; a dead shape still dies, since zero
frequency zeroes the frozen-priced benefit too. The budget swap's victim
ranking prices incumbents on the same frozen basis it defends them with.
Crash posture: BUILDING is discarded by construction (nothing persists
it); ACTIVE persists as its catalog row and re-derives the rest.

**Scope.** Persistence of managed-Cabin lifecycle state and the decision
log (PO5, PO8).

**Deliverables.**
- Optimizer lifecycle state per managed Cabin in the Cabin catalog area:
  lifecycle state, ownership tag (optimizer-created), frozen `P_scan`
  baseline, evidence accumulators (N_confirm, cooldown timestamps in decay
  epochs).
- Decision log: append-only, bounded (ring of last K decisions, PROPOSED
  K = 1024 per core), each entry = ActionSet element + snapshot digest.
  Not WAL-critical (advisory data; loss on crash is acceptable and
  documented — state machine re-derives from re-observation).
- Crash posture: BUILDING at crash ⇒ discarded on startup (no resume);
  ACTIVE Cabins persist as ordinary Observational Cabins do.

**Acceptance.** Restart tests: ACTIVE survives, BUILDING discarded,
CANDIDATE evidence resets cleanly; log ring wraps correctly.

## PHY04 — Executor (background task)  **[DONE 2026-08-10]**

**Built.** `exec::CabinOptimizerExecutor` (`Apply(ActionSet)` + the
`Tick()` that strings snapshot → Decide → Apply for the expeditor's
cadence, registered beside the checkpointer's at
`cabin_optimizer_snapshot_interval_ms`, 0 = no cadence). What each action
means here, sized to the engine: **CREATE** is the catalog row (origin
`kCabinOriginAuto`) plus the seeded build — empty for a first-time column
*by construction* (no Cabin meant no CabinKey sightings), so §II.5's
"observation-based population" applies literally and traffic fills it via
n=2; a **policy refusal** parks the candidate in BUILDING rather than
resetting it, or a settled "no" would be retried every confirm cycle
forever. **EXTEND** is where the ring earns its keep: the seed is
`SightedUnobservedOf`, and **one complete walk through the scan ring**
(PO4, structural — the build fetches through `OpenScanRing()`) collects
every seed's set at once, committing each only after the walk finished —
empty sets included, the authoritative zero-rows answer. The build judges
by latest settled state and **aborts on a busy row** (counted, its abort
leaves a phantom; skipped, its commit already passed the write hook
unobserved — AST06's argument verbatim), committing nothing; demand
re-nominates. **HEAL** is the batch form of the read path's heal through
the same primitives — `VerifyTupleAt`, `BtreeLookup` + current-epoch
stamp, dangling pks erased on sight (K1), heap sets un-observed.
**DROP** is row → `Forget` → `NoteDropped`, in that order ("late is
fine"). **PO8** is consulted at every boundary — tick, action, and build
*page* — so OFF lands mid-build and discards cleanly, commit being
walk-completion-only. One controller amendment rode in: `NoteCreated`
adopts an entry it no longer tracks rather than dropping the pages from
the budget's accounting. Tests: the full loop (hot filter-scan traffic →
tick → catalog row, origin auto), the seeded build with the empty-set
case and the served `cabin_hits=1` after it, kill-switch mid-build
discard with surviving evidence, the busy-row deferral settling to the
three-row set after COMMIT, heal repairing broken hints and erasing a
planted dangling pk, and drop removing row, sets and controller entry.

**Scope.** `CabinOptimizer::Execute(ActionSet)` on the home core's
background group. **Depends on EVT06 (scan ring).**

**Deliverables.**
- Decision cadence: snapshot → Decide → Execute every
  `kds.po_snapshot_interval`, as a cooperative background task.
- CREATE/EXTEND: observation-seeded build through the scan ring,
  cooperative batches, single-step state transitions at start/finish.
- HEAL: batch hint re-validation walk (reuses the read-path heal
  primitive).
- DROP: page reclamation + catalog removal in a single publish-style step.
- Kill switch semantics: `off` checked at batch boundaries; in-flight
  build discarded, nothing destructive (PO8).

**Acceptance.** Deterministic tests: each action end-to-end on small
relations; interruption mid-build discards cleanly; foreground working-set
protection test (build over large relation does not evict scripted hot
set — reuses the EVT06 oracle); disable-switch harmlessness.

## PHY05 — Configuration surface  **[DONE 2026-08-09]**

**Built.** The switch: `cabin_optimizer` boot key (default **off**,
experimental - the opposite default from Part I's `physical_optimizer`,
because a controller that acts is not a report), `SET CABIN_OPTIMIZER
[=] ON|OFF` at runtime (non-destructive both ways, PO8), `SHOW META`
reporting it. The tuning family, translated to this engine's conventions:
`cabin_optimizer_page_budget`, the five `_theta_*_pct` thresholds as
**percent integers** (the config file parses no decimals; 300 = θ 3.0),
`_confirm_snapshots`, `_snapshot_interval_ms` (0 = no cadence, the
checkpoint-interval precedent) - all parsed and validated at boot, with
the one cross-key rule enforced that the hysteresis stands on:
**θ_drop < 100 < θ_create**, whatever the numbers.
`Config::CabinOptimizerSettings()` assembles them into the decision
core's 16.16 config, sharing R1's `decay_half_life` as T_amort (§II.6's
own default; no separate key until a workload argues for one). Consumed
by PHY04's task when it exists and by nothing until then - stated in the
sample, the manual, and the keys' comments, the V11 precedent. Tests:
defaults + parse + assembly, the broken-gap/zero-budget/zero-confirm
refusals, and the runtime toggle with both spellings.

**Scope.** Spec §II.6 settings wired through boot + runtime (`SET` for the
switch; thresholds boot-time in v1).

**Deliverables.** Settings, validation (theta ordering θ_drop < 1 <
θ_create, budget sanity), introspection visibility.

**Acceptance.** Boot-validation tests; SET on/off runtime toggle test.

## PHY06 — Observability (PO9)  **[DONE 2026-08-10]**

**Built.** The view is **`SHOW CABIN_OPTIMIZER`**, not a `sys.*` SELECT —
`SHOW ASSERTIONS`' rule: the surface renders through the dispatcher, which
holds every source the view needs and the SELECT path does not. The
handler renders and never computes: each number comes from a surface that
exists on its own merits — `CabinOptimizer::ManagedEntries()` (a by-value
projection of the managed table; per entry the state, pages, confirm
streak, and the **last Decide pass's B/C**, stamped every pass so a quiet
entry still shows the numbers keeping it quiet), `DecisionLog()` (the
newest record per candidate renders as `last_action=/reason=/epoch=`),
`CabinOptimizerExecutor::counters()` (**applied**, not decided — ticks,
creates, extends, heals, drops, deferred, failures; the gap against the
decision log is itself the diagnostic), and
`OptimizerSignals::QualityOf()` — a **const, version-silent** S3 peek,
because a SHOW must not advance the version sequence the decision log's
digests are named in. Quality prints as integer percentages (`hint_fail_pct`,
`coverage_miss_pct` — the rates θ_heal and θ_extend compare, not their
complements; Q8 cancels in the ratio). A dispatcher with no controller
answers `CABIN_OPTIMIZER absent (cabins = off)` — SHOW CABINS' no-zeros
rule. ANALYZE marks an optimizer-managed probe with `cabin_optimizer=true`
(`CabinProbe::managed`, origin `kCabinOriginAuto` at compile — not
`!declared`, because a legacy unset origin is neither); a declared Cabin
is deliberately unmarked, pinned by test. **The NoteExtended edge rode in**
(PHY04's recorded gap): a completed EXTEND reports its new page-proxy
**total** — idempotent, never a delta — or PO6's accounting undercounts by
every extend forever; `AnExtendReportsItsNewPageTotalToTheBudget` pins it.

**Scope.** `SHOW CABIN_OPTIMIZER` + counters + ANALYZE flag.

**Deliverables.**
- Per managed Cabin — state, B, C, hint-failure rate, coverage-miss rate,
  pages, last action + reason code + epoch.
- Production counters: actions by type, budget utilization, snapshot
  cadence health (`ticks`).
- ANALYZE: Cabin-hit line gains `cabin_optimizer=true` marking for managed
  Cabins.

**Acceptance.** View snapshot tests under PHY04 scenarios; ANALYZE golden
output.

## PHY07 — Deterministic replay harness (PO10)  **[DONE 2026-08-09]**

**Built.** `tests/cabin_optimizer_replay_test.cpp` +
`tests/testdata/cabin_optimizer_traces.txt` (golden, regenerated via
`KDS_TRACE_REGEN=1` — the parser corpus's arrangement). The harness runs
the **real pipeline**, not a mock: splitmix64-seeded streams (chosen over
`<random>` because the standard's *distributions* are
implementation-defined, and byte-identical traces are the point) feed
PHY01's `OptimizerSignals` under a ManualClock, each period snapshots and
runs PHY02's `Decide`, and a simulated Execute applies the completion
edges with deterministic ids and page counts. Four scenarios (rising
star, fading star, stationary noise, quality collapse), each also
carrying a foreign Cabin with miserable quality for the jurisdiction
oracle. All four oracles wired and green: no-oscillation (sixty jittering
periods inside the θ gap produce **zero** actions), the budget invariant
after every executed set, jurisdiction (the foreign Cabin is never
touched; Bound Cabins cannot appear at all — the snapshot is fed by
Observational counting alone), and PO7's HEAL-before-DROP ordering. One
observation the golden trace surfaced, recorded rather than hidden:
after a quality-collapse DROP, still-hot demand **re-nominates and
re-creates** (steps 12-13 of that scenario) and heals again — PO7's
"demand, if real, re-nominates" working as written, and the churn a
persistent per-key cooldown (PHY03's state) could dampen if measurement
ever says it should.

**Scope.** Seed-driven statistics streams replayed through Decide.

**Deliverables.**
- Stream generator: seeded synthetic fingerprint populations with
  configurable drift (rising star, fading star, stationary noise, bulk-
  update quality collapse).
- Replay runner: stream → snapshots → ActionSet trace; golden traces
  checked in.
- Oracles wired: no-oscillation, budget invariant, jurisdiction (no Bound
  Cabin id ever), HEAL-then-DROP path on quality collapse (PO7).

**Acceptance.** Golden traces stable across runs and platforms
(fixed-point determinism proven here); scenario matrix green in CI.

## PHY08 — End-to-end validation and close-out  **[DONE 2026-08-10]**

**Built.** The E2E is `CabinOptimizerExecTest.TheFullLifecycleObservedThroughTheView`:
one scripted run under a manual clock — hot traffic earns the CREATE
(view: `state=ACTIVE`, `last_action=CREATE reason=sustained-benefit`),
the Cabin serves (`cabin_hits=1`, the managed mark, reply byte-identical
to the pre-optimizer baseline), 50 half-lives of silence move the entry
to DECAYING, the cooldown elapses and the DROP retires the row, the sets
and the controller entry (`managed=0`, `cabins=0`), and the reply still
never moved. **The bench note is `bench/results-cabin-optimizer.md`**
(driver `tools/cabin_optimizer_benchmark.py`, PostgreSQL twin beside it):
overhead with zero eligible candidates is **unmeasurable, confirmed** —
ON−OFF deltas sit inside the same-configuration noise floor at a 10 ms
tick cadence (1,000× default), the tick itself priced at **2–3 µs CPU**
from idle-server accounting, ~0.025 % of a core at 10 ms and sub-ppm at
the default 10 s; the improvement case had the controller create
**exactly 3 ticks after switch-on** (confirm_snapshots working as
declared) and serve at n=2 (`misses=2` exactly, at every size), walk→served
p50 1,439→132 µs = **10.9× client / 19.4× server CPU at 10,000 rows**
(1.97× at 1K, 1.17× at 200 — the index crossover's shape, as expected),
with the served probe 9.4× under PostgreSQL 17's seq-scan on the same
data. Two honest tails recorded in the document: this run's B/C margins
say nothing about where θ_create bites on a *marginal* shape, and the
DECAYING/DROP paths are exercised by the E2E and PHY07's replay, not by
the bench. Docs cross-check done: `feat-cabin.md` §8.1 amended in place
("auto is a name, not a behaviour" — dated, and corrected now that it is
one) with the `kCabinOriginAuto` ownership-tag mention, README carries
the low-key clause (advisory structures, wrong decisions cost performance
only), and the operator manual gained §5a for the view.

**Scope.** System-level pass + docs.

**Deliverables.**
- E2E: workload with a skewed hot predicate ⇒ the cabin optimizer creates
  a Cabin ⇒ measured latency/pages-scanned improvement on the hot
  fingerprint ⇒ workload shifts ⇒ DECAYING ⇒ DROP. Full lifecycle observed
  via `sys.cabin_optimizer` in one scripted run.
- Benchmark note: optimizer-on overhead with zero eligible candidates
  (target: unmeasurable — snapshot cost only); improvement case recorded
  in the perf log.
- Docs cross-check: `feat-cabin.md` gains the ownership-tag mention;
  positioning stays low-key per policy ("experimental self-managing
  advisory structures", no autonomy overclaim).

**Acceptance.** Green E2E in CI; perf log entries; docs consistent.

## Part II dependency graph

```
PHY01 ──► PHY02 ──► PHY03 ──► PHY04 ──► PHY08
              │                 ▲
PHY05 ────────┤                 │ (EVT06 scan ring — hard dep)
PHY06 (needs PHY04) ────────────┘
PHY07 (needs PHY02) ──► PHY08
```

PHY02/PHY07 are pure-code tracks and can run ahead of storage-side work.
PHY04 must wait for EVT06; if the eviction track lags, everything through
PHY03 plus PHY07 can still land and be fully tested. The mover is deliberately absent from this plan —
its prerequisite is a §6 gate opening, and the shadow report (PX07) is what
that decision reads.
