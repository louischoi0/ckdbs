# Cabin — Workplan

Work instructions, companion to `feat-cabin.md`. Tasks `CB01`-`CB11`.

**Status: v1 is complete and measured.** Every task below is done; what
remains is in "What v1 is not". Numbers in `bench/results-cabin.md`, which was **corrected on
2026-08-04**: the first pass was run on tmpfs and reported as a block
device, which made the reporting scan look like the bottleneck it is not.
On disk at 10,000 users the feature is a **wash** - TPS +3.2%, the targeted
query's mean -20% at a 23.9% hit rate - because the WAL-logged inserts are
311s of a 380s run and the scan is 63s. The write hook is at or below noise.

Execution rules:
- Do tasks in numeric order unless "needs" says otherwise.
- Each task ships with its listed tests in the same change.
- If a task turns out to touch an `[OPEN]` item in the spec — stop, flag,
  do not decide. `CB04`'s caps are the one place this workplan deliberately
  builds *behind* an open item rather than around it, and the rule there is
  that every option §8 lists stays viable.
- The cabin contract test (`CB10`) is regression-mandatory from the moment
  it exists.

---

## What v1 is

`Cabin(relation, non-pk column)` with per-value entry sets, memory-resident,
demand-driven, `n = 2`. The catalog stores that a Cabin **exists**; the
observed sets live in a core-local store and do not survive a restart — §9
already declares that a crash leaves every Cabin fully unobserved, which is
invariant-preserving by C1's own terms. Persisting them (`PageType::kCabin`)
is phase 2 and is deliberately not designed here.

## What v1 is not

`PageType::kCabin` and entry-set persistence · background pruning (§5 fixes
the gates; the cadence belongs to the background-group policy) · the
promotion pipeline that would auto-create cabins from patterns and Waystone
heat (§7) · density levels 1-5 (§8) · expression, multi-column or range
cabins (C3) · `DELETE` (no such statement exists).

## The two engine gaps this is built on top of

**There is no per-page epoch.** `CabinEntry::page_epoch` is written 0 and
its check passes trivially — the same gap `stats/trail_store.hpp` and
`exec/trail_replay.hpp` already document. Sound only while a tuple's address
is stable for life (invariant 13, plus nothing relayouts), which means the
epoch must land with relayout, whichever comes first. Cabin is now the
**second** subsystem waiting on it.

**There is no transaction manager.** "Per snapshot" degenerates to "the
tuple exists and still matches". **Superseded 2026-08-04**: `docs/txn.md`
is built, and the read path now applies the visibility predicate at
`ChainRunner::AcceptTupleAt` — which every Cabin resolve funnels through.
"Per snapshot" is literal now, and §1's surplus is subtracted by MVCC as
well as by the key re-check. The append-only write rule did not change,
because it was built for the snapshot world from the start. The append-only write rule is built for the
snapshot world anyway, because retrofitting removal-is-forbidden after the
fact is how a structure ends up with a hot-path delete it cannot take back.

---

## Phase 1 — the catalog object

**CB01 — `sys.cabins`, the row, the format version.** — **done.**
Files: `docs/cabin-workplan.md`, `include/kds/catalog/well_known.hpp`,
`include/kds/catalog/rows.hpp`, `src/catalog/rows.cpp`,
`src/catalog/catalog.cpp`, `include/kds/server/superblock.hpp`.
`kSysCabinsTable = 131` on `kCatalogPageCabins = 12`, an eighth bootstrap
relation; `SysCabinRow` as a fixed-offset typed row (28 bytes); superblock
format version **7 → 8**.
Tests: `SysCabinRow` round-trip, short-buffer refusal (`CB10`).
Needs: nothing.

Decisions worth carrying forward:

- **0 is reserved in both `origin` and `status`.** The third repeat of the
  collision `StoredStatementClass` and `StoredAccessKind` were each taught
  to avoid: a zeroed row decodes to 0, so a 0 that also named a real value
  would make empty page bytes read as an active user-declared Cabin.
- **`observed_ct` is stored but unwritten in v1.** The live count belongs to
  the runtime store. The field exists so that persisting the sets does not
  need a row format change — which, by the rule the version comment states,
  is a format-version event.
- **A new bootstrap relation is a format-version event**, and the 7 → 8
  comment now says so once for all three occurrences (5 → 6, 6 → 7, 7 → 8):
  the file mounts cleanly and then fails on the first statement reaching for
  a table that does not exist, which is worse than a size mismatch, not
  better.
- **`cabin_id` comes from `AllocateRowId(kSysCabinsTable)`**, the persistent
  sequence in sys.cabins' own sys.tables row — the same repurposing
  `RegisterPattern()` records, and for the same reason: `GenerateUserOid()`
  restarts at `kUserOidStart` every boot and this row is persisted.

**CB02 — Catalog API and `TableAccess::cabin_mask`.** — **done.**
Files: `include/kds/catalog/catalog.hpp`, `src/catalog/catalog.cpp`,
`include/kds/catalog/schema.hpp`, `src/catalog/catalog_cache.cpp`.
`CreateCabin` / `DropCabin` / `ListCabins` / `FindCabinOnColumn`, both
mutators through `BumpVersion()`. `TableAccess` gains `cabin_mask` (bit per
`col_pos`) and the cabin ids, filled by one `sys.cabins` scan in
`InitTableAccess()`.
Tests: create/drop/list, duplicate refused, pk column refused, the mask
appearing and disappearing across DDL.
Needs: CB01.

Why a mask on `TableAccess` rather than a per-step catalog probe: "does
`(rel, col)` have a Cabin" is asked once per step per compile, it is a
**positive** fact about a relation (so `catalog_cache.hpp`'s "absences are
never cached" rule does not bite), and `CREATE CABIN` is DDL, so it
invalidates with the entry it rides on.

**CB03 — `CREATE CABIN` / `DROP CABIN` / `SHOW CABINS`.** — **done.**
Files: `include/kds/parser/ast.hpp`, `src/parser/parser.cpp`,
`include/kds/exec/cabin_ddl.hpp`, `src/exec/cabin_ddl.cpp`,
`src/server/command_dispatcher.cpp`.
Grammar `CREATE CABIN ON <table>(<column>)` and `DROP CABIN ON
<table>(<column>)`, modelled on `ParseCreatePattern`/`ParseDropPattern`.
The checks follow `pattern_ddl.cpp` §6's error/warning line: **error** for a
declaration that could never do what it says (the pk column, an unknown
column, a duplicate, a type the row codec cannot compare), **warning** for
one that works and will disappoint.
Tests: each refusal by message and position; `SHOW CABINS` before and after.
Needs: CB02.

## Phase 2 — the runtime store

**CB04 — `stats::CabinStore`.** — **done.**
Files: `include/kds/stats/cabin_store.hpp`, `src/stats/cabin_store.cpp`.
Core-local, no synchronization, one per core, and it **never fails a
statement** — every policy path returns void or a bool, exactly as
`TrailRecorder` does. Holds: the 24 B `CabinEntry` of §3, the observed set
(`value → entries`), a capped sighting table for unobserved values, and the
`n = 2` policy.
Tests: `n = 2`, sighting-table overflow restarting the count, append-only
semantics, the `v → v′ → v` duplicate, caps refusing to observe,
`Unobserve`.
Needs: CB01.

Two rules that are correctness, not policy:

- **A cap refuses to observe; it never truncates a set.** A truncated entry
  set marked observed is a wrong answer — it is exactly the "missing a
  qualifying pk" §1 forbids. So the legal responses to a cap are "do not
  observe" and "un-observe", and there is no third.
- **A value becomes observed only when its recording walk *completes*** —
  not on `VisitControl::kStop`, not on error. This is the point §6's
  soundness argument attaches to: under core-ownership dispatch, scan +
  record + mark-observed is atomic against every other statement, and a
  partial walk marked observed would be a set that was never complete even
  for an instant.

**CB05 — extract the shared tuple verifier.** — **done.**
Files: `include/kds/exec/tuple_verify.hpp`, `src/exec/tuple_verify.cpp`,
`src/exec/step_vm.cpp`.
`VerifyTupleAt(store, page_id, slot, expected_pk)` returning
kOk / kPageGone / kSlotGone / kPkMismatch, and `ChainRunner::TryReplay()`
rewritten onto it. C6 requires the hint check to run "under the same rules
as waystone entries (shared validation code, not a parallel implementation —
two verifiers would be where the bugs live)"; this makes that structural.
Tests: the existing Waystone contract suite, unchanged and green — including
its deliberately corrupted trail, which is what proves the check is still
load-bearing after the move.
Needs: nothing.

## Phase 3 — the access path

**CB06 — `AccessKind::kCabinProbe`.** — **done.**
Files: `include/kds/exec/step_chain.hpp`, `src/exec/step_compiler.cpp`,
`src/exec/plan_printer.cpp`, `src/server/command_dispatcher.cpp`.
A new kind between `kRange` and `kFilterScan`, `Step::cabin` carrying the
resolved probe, `CabinOnColumn()` beside `HasUnindexedEqualityFilter()`,
stored value **6**, and the printed name in the plan and in `SHOW ACCESS`.
Tests: the kind is emitted for a cabined column and not for an uncabined
one; the pk kinds still win; `sys.access_stats` records the cabin column.
Needs: CB02.

**`IsTrailReplayable()` does not change.** Cabin is authoritative and a
trail is not, but the line invariant 9 draws is *lookup versus search*, not
*authoritative versus advisory* — a cabin probe is not a pk lookup, so a
trail may not replace it. Adding an authoritative-but-not-pk kind still
cannot move that line, and the fact that it does not is the check that the
two trust models stayed separate.

**The cabin equality stays in `Step::residual`**, exactly as a lookup's key
and a range's bounds do. Downgrading a `kCabinProbe` to a plain `kScan` must
not change the result — and that is not a nicety here, it is what licenses
the append-only maintenance model, because the read-time key re-check is
precisely what subtracts §1's surplus.

**CB07 — the read path.** — **done.**
Files: `include/kds/exec/step_vm.hpp`, `src/exec/step_vm.cpp`.
`RunCabinStep()`, and `Execute()` gains a `stats::CabinStore*` beside the
collector and the replay index, with the same contract those two carry:
**passing it cannot change what `Execute` returns.**
Tests: hit, miss, recording, and every fallback (`CB10`).
Needs: CB04, CB05, CB06.

- **Hit** → serve authoritatively, entries sorted by `page_id` (§3's
  serve-time batching). Per entry: verify the hint, and on failure resolve
  the pk. Then the same `AcceptTupleAt()` a descent feeds, so visibility,
  the residual, sub-chains and the next step are **inherited, not
  reimplemented** — the same argument the probe memo and trail replay both
  rest on. A pk absent from the tree is a **skip**, never an error;
  duplicate pks are served once through a seen-set, because duplicates are
  *expected* under append-only maintenance and not a sign of damage.
- **Miss** → the ordinary walk, with a recording sink attached.
- **The heap fallback differs.** A heap relation has no pk descent, so a
  failed hint cannot be resolved per entry: the first failure abandons cabin
  serving for that step, runs the authoritative walk, and re-records the
  value's set from it — one bulk heal instead of one scan per entry.
- R1 (`docs/parser-v2.md` I15): the page must be re-fetched per entry.
  `AcceptTupleAt` descends into the next step, and anything below it may
  fetch, so a page view held across entries is exactly the span the rule
  forbids.

**CB08 — the write path: the witness.** — **done.**
Files: `src/server/command_dispatcher.cpp`.
One shared `NoteWrite()` called from `HandleInsert` — after the placement
succeeds and **before** `LogInsert()` — and from `HandleUpdate`'s `apply`.
Per §5's table: INSERT appends if the new value is observed, UPDATE v→v′
appends to v′ and **leaves v untouched**, and nothing ever removes.
Tests: an insert of an observed value appears in the next read; an update
away from an observed value stops appearing; the surviving surplus entry is
subtracted by verification and not by the hook (`CB10`).
Needs: CB04.

- **Ordering is the point.** A WAL failure after the page mutation leaves a
  row in the page. A row in the page that no Cabin witnessed is exactly the
  completeness break C1 forbids, so the hook runs before the log, not after.
- **Any failure in the hook un-observes the value.** §1's corollary — un-
  observing is always legal — is what makes that the correct response rather
  than a lossy one: the value falls back to the authoritative scan path, a
  performance event.
- **Removal on the hot path is *incorrect*, not merely unnecessary.** An
  older snapshot may still be entitled to match through the undo chain. That
  there are no snapshots yet is not a reason to write the eager version now.

## Phase 4 — wiring and proof

**CB09 — config, expeditor, docs.** — **done.**
Files: `include/kds/server/expeditor.hpp`, `src/server/expeditor.cpp`,
`kds.conf.sample`, `docs/client-manual.md`, `CLAUDE.md`.
`cabins` (default on — it does nothing until a `CREATE CABIN`),
`cabin_max_values` and `cabin_max_entries_per_value`, both `[PROPOSED]`
and both named in §8's open budget item so the decision stays open.
Needs: CB07, CB08.

**CB11 — the per-column cabin policy (C7).** — **done.**
Files: `include/kds/catalog/rows.hpp`, `src/catalog/rows.cpp`,
`include/kds/parser/ast.hpp`, `src/parser/parser.cpp`,
`src/catalog/catalog.cpp`, `src/server/command_dispatcher.cpp`,
`docs/feat-cabin.md` §8.1.
`<col> <type> [CABIN | CABIN AUTO | NO CABIN]`, stored on the `sys.columns`
row as `cabin_policy`, reported by `DESCRIBE`.
Needs: CB02, CB03.

Decisions worth carrying forward:

- **The axis is authority, not on/off.** Three answers to "who may decide
  this column carries a Cabin": the operator, the engine, nobody.
- **`enabled` implies n=1, not just creation** - the same rule a declared
  pattern gets, on the same argument: a declaration *is* the evidence
  waiting exists to gather.
- **`auto` is a name, not a behaviour.** Nothing creates a Cabin on it; the
  value is stored so the decision exists before the pipeline that consumes
  it, and a column declared auto behaves exactly as an undeclared one.
- **Enforced at two doors, and `Catalog::CreateCabin` is the load-bearing
  one** - every Cabin comes through it, so a future auto-creator cannot
  forget the `NO CABIN` check by being written somewhere else.
- The policy was **dropped in transit** on the first attempt:
  `InsertColumnRow()` did not take it, so `NO CABIN` parsed, stored nothing
  and enforced nothing. Caught by the end-to-end check, not by a unit test -
  which is what `CB10`'s DDL cases are for.

**CB10 — the contract suite.** — **done.**
Files: `tests/cabin_contract_test.cpp`, `tests/cabin_store_test.cpp`,
`tests/catalog_row_test.cpp`, `tests/exec_chain_test.cpp`.
Six configurations compared **byte for byte** over the same statements, on
**both** clustered types: no cabin; cabin created but unobserved; value
observed; observed then written through the hook; observed with corrupted
hints (wrong page, wrong slot, pk mismatch); observed with dangling pks.
Plus the **authoritative-empty** case — an observed value with no matching
rows returns zero rows with nothing decoded (`examined=0`), which is the one
claim no advisory structure can make and which leaves no trace other than
the work not done.
Needs: CB07, CB08.

Decisions worth carrying forward:

- **Two tests exist to stop the feature being correct and useless.**
  `ServingStopsReadingTheWholeRelation` and the authoritative-empty case
  both assert *work not done*; every byte-for-byte test in the file would
  still pass if serving silently degraded to a scan.
- **`RecordingIgnoresTheStatementsOtherConjuncts` is the subtlest case.**
  The set recorded for a value must be the rows whose key column equals it,
  never the rows the recording statement wanted - otherwise a
  `WHERE sym = 'aaa' AND qty > 30` execution would publish a narrowed set as
  authoritative, and every row a later `WHERE sym = 'aaa'` returned would be
  real. Nothing else in the suite catches it, because the failure looks
  exactly like a small table.
- The suite's Cabins are **declared**, so they record at n=1. A test written
  against n=2 fails on the *second* execution rather than the third, which
  is a confusing way to learn the policy: it is stated at the call site.

## Phase 5 — the correlated probe

**CB12 — `kCabinProbe` keyed by an earlier step's row — built 2026-08-19.**
Spec §4a. `CorrelatedCabinProbeOf` in the step compiler, reached after both
index forms and the literal Cabin declined; `CabinProbe::key_from` carries
the outer column, and `RunCabinStep` reads the probed value from the frame
per outer row - stable for the walk's duration because the outer row is
fixed while the inner step runs. The probe value threads through
`ServeFromCabin` / `FallBackAndReRecord` / `WalkAndRecord`, so the miss,
serve and re-record paths are the literal form's unchanged. ANALYZE prints
`key=` for the deferred form where the literal prints `value=`.

Found while building it: the serve emitted in **entry order**, which stops
being pk order the moment the write hook appends an earlier pk to an
observed value (an UPDATE moving a row into the set) - a reply-reordering
bug against I12's within-step contract, latent since v1 because the
original contract queries always filtered the exposed set to one row. The
serve now sorts to the walk's order before emission - pk for ASSIGNED,
page-and-slot for EXPLICIT, per heap-and-tuple.md §4.1 (IX8a's rule, key
modes respected; the unconditional pk sort was itself a reordering on
EXPLICIT, caught in review). The §4a join queries and the two focused
pins in `tests/cabin_contract_test.cpp` are what caught it and hold it.

**CB13 — the correlated EXISTS converges — built 2026-08-19.** Spec §4a's
closed paragraph. `ChainRunner::RecordThroughStops()`, set only by
`EvaluateSubChain` on the runner it builds: in a sub-chain every stop is
the sub-chain's own short-circuit (V09 refuses LIMIT at subquery depth),
so a walk carrying a live cabin recording runs on through the stop -
`AcceptTupleAt` short-circuits to the recording block once stopped, so
nothing emits twice - and `WalkAndRecord` commits the whole set. A
top-level runner never sets the mode, which is what keeps the quota's
bounded-work property (pagination_exec_test) intact.
`ACorrelatedExistsConvergesToObservedSets` pins convergence, the whole-set
commit, and reply identity; it fails without the mode.

**CB13a — `correlated_scans` counts walks, not kinds — built 2026-08-19.**
The counter moved from `EvaluateSubChain`'s compile-time kind test into
`RunWalkStep`, gated on sub-chain mode and the driving step: a
`kFilterScan` driver (never counted before) and a cabined driver's miss
both count, a served probe counts nothing, and ANALYZE's `corr_scans=`
goes quiet when a cabined sub-chain converges. Pinned by the counter
tests in `tests/cabin_contract_test.cpp`.
