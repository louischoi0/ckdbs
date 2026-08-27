# Test Strategy — Workplan

Work instructions for building the verification layer that sits **above** the
existing unit and subsystem-contract tests. Tasks `SIM01`–`SIM14`.

The 87 files under `tests/` are thick at the unit and subsystem level — device
crash/torn-write contracts, WAL durability classes, B+ tree structural
invariants, MVCC visibility, parser fuzz — and empty above it. Nothing runs a
randomized workload end to end, crashes it at an arbitrary point, restarts,
and checks the whole instance; and nothing checks concurrent-transaction
histories against the isolation levels `docs/spec/txn.md` promises. This workplan
builds both, on one spine: a **seed-driven deterministic simulation harness**.
Every piece of nondeterminism — operation choice, values, crash points, fault
schedule, session interleaving — derives from a single `--seed`, so any
failure replays exactly and every found bug becomes a one-line regression
entry.

Numbering. `SIM##`. The prefix is unused: `P01`–`P17` (Waystone / protocol),
`V##`, `T##`, `CB##`, `AG##`, `TY##`, `FK-M#`, `CC#`/`M#` are all taken and
`CLAUDE.md` already warns about bare numbers. Cite the file, not the number.

Execution rules:
- Do tasks in numeric order unless "Needs" says otherwise.
- Each task ships with its listed tests in the same change; `bash
  scripts/test.sh` green is part of "done".
- Touching an `[OPEN]` item — here or in `CLAUDE.md` — means **stop and
  flag**. The known collisions are tabled at the end.
- From the moment `SIM04`'s loop exists, its committed seed list is
  regression-mandatory: a seed added to `tests/testdata/sim_seeds.txt` is run
  by CI forever, and removing one requires the same justification as deleting
  a test.
- The harness must never contain a workaround for an engine bug. A failing
  seed is either a harness bug (fix the harness) or an engine bug (file it,
  commit the seed, fix the engine). A harness that "knows" about engine
  quirks is measuring itself.

---

## What this is

Three deliverables, in dependency order:

1. **The harness** (`SIM01`–`SIM03`): a standalone binary linking the engine
   as a library, driving it in-process through `CommandDispatcher` — below
   TCP, above the parser — with every simulated device and every random
   choice derived from one seed.
2. **The end-to-end simulation loop** (`SIM04`–`SIM07`): generate schema +
   workload, mirror every operation into an in-memory oracle, inject faults
   and crashes from the seed, restart, and verify — oracle agreement for
   results, an instance-wide integrity sweep for structure.
3. **Concurrent-transaction history checking** (`SIM08`–`SIM11`): a
   multi-session driver with deterministic interleaving, a per-session
   history recorder, and checkers for the exact guarantees the engine
   documents — READ COMMITTED, REPEATABLE READ, first-updater-wins — no
   more, no less. `SERIALIZABLE` is out of scope because the engine refuses
   it; the checker must not demand it.

## What this is not

Hardware-level testing (real-disk power-cut rigs, NVMe pull tests) — out by
the stated constraint; the simulated devices are the stand-in and
`FilePageDevice`/`FileLogDevice` keep their existing unit suites. Performance
regression gates — `bench/` owns numbers; `SIM13` only wires a smoke
threshold. External differential testing against SQLite/PostgreSQL — phase 2
(`SIM14`), deliberately last: it needs a server and a Python client, and it
verifies semantics the oracle already covers, adding value mainly for SQL
edge semantics (NULLs, type coercion) once the grammar grows.

## The two engine facts this is built around

**~~Recovery does not exist~~ — it landed 2026-08-12, and the gate is
armed.** The paragraph below is kept because it is the record of why the
checker was written before the feature; what it describes stopped being
true with `docs/workplan-wal-recovery.md` RV1/RC10. `SimInstance::Boot`
runs analysis / redo / high-water / undo before the first statement, and
`kRecoveryImplemented` in `sim/loop.hpp` is `true`: kCrash's full
durability assertion *fires* rather than counting. The counter it replaced,
`gated_missing_rows`, is still printed and still expected to be zero. The
one assertion the arming did **not** cover is named where it is true —
`unlogged_ddl_lost_tables` — and RV3 closed that too on 2026-08-19.

> **Recovery does not exist.** `CLAUDE.md` is explicit: nothing reads the
> WAL back; a restart is protected only by `PageStore::Sync()` at `SYNC` or
> clean shutdown. So `SIM04`'s crash-restart loop cannot yet assert
> "committed ⇒ survives". It asserts what is *currently promised* — the
> durable prefix is intact and internally consistent — in three modes
> (clean shutdown / crash-after-sync / crash-anywhere), and the full
> assertion is written now but gated off, so that **this harness is the
> acceptance test recovery must pass on the day it lands**. Building the
> checker before the feature is the point: recovery written without an
> adversarial harness waiting for it would be tested by its own author's
> imagination.

**MVCC ships with a known crash gap.** An uncommitted row surviving a crash
reads as committed on the next boot (`docs/spec/txn.md` §8). The integrity sweep
must *detect and report* this state, and the loop must *expect* it in
crash-anywhere mode — a documented-gap counter, not a pass and not a
failure. When recovery closes the gap, the counter's expected value becomes
zero and the gate flips. Same discipline as the FPI and epoch gaps: honest
bookkeeping, no silent tolerance.

---

## Phase S-1 — Harness foundation

### SIM01 — Library split and harness skeleton
The engine builds as a library target (`libckdbs`) and `src/main.cpp` (or
equivalent) becomes a thin executable over it; `tests/` already links most of
the engine, so this is CMake surgery, not a refactor. New binary
`sim/sim_main.cpp` → `ckdbs-sim`, taking `--seed N` (required), `--ops N`,
`--mode {clean|sync-crash|crash|txn}`, `--profile <name>` (workload mix), and
printing a one-line verdict plus the seed on any failure. One
`std::mt19937_64` seeded from `--seed` is the **only** entropy source; every
component that needs randomness takes a sub-generator forked from it by a
fixed label (`fork(seed, "workload")`, `fork(seed, "faults")`, …) so adding a
new consumer does not shift every existing seed's behavior.
**Needs:** nothing. **Tests:** same seed twice ⇒ byte-identical operation
log; different seeds ⇒ different logs; forked streams are label-stable.

### SIM02 — Instance integrity sweep
`sim/integrity.{hpp,cpp}`: `CheckInstance(PageStore&, Catalog&) →
IntegrityReport`, a full sweep over a **quiesced** instance. Checks, each
individually reportable: page header sanity and checksum per `docs/spec/page.md`;
heap-chain order — every page's ids ≥ its `min_key`, each page's ids
entirely below the successor's `min_key` (invariants 2, 3); B+ tree — every
separator equals its child leaf's `min_key`, sibling ordering, leaf/heap page
duality (mirrors `btree_test.cpp`'s per-tree assertions, but over the real
instance); Keystone upper 24 bits zero outside headers (invariant 7);
var-heap — every `kSpilled` cell resolves, spilled bytes' page is
`PageType::kVarHeap`, per-relation root matches `sys.tables.varheap_page_id`;
catalog — chain walkable, oids unique, `owner_core` present, every relation's
root pages allocated in the free map; undo — every nonzero `undo_ptr`
decodes to a `kUndo` page and an in-bounds offset. Plus the documented-gap
detector: a tuple whose `trx_id` exceeds the persisted transaction watermark
(reportable once `next_trx_id` is readable at check time) is counted, not
failed — see "two engine facts".
**Needs:** SIM01. **Tests:** a hand-built valid instance passes; one
deliberately corrupted byte per check category is caught by exactly that
category (mirror the trail-corruption trick `waystone_contract_test.cpp`
uses).

### SIM03 — Workload generator v1 + oracle
`sim/workload.{hpp,cpp}` and `sim/oracle.{hpp,cpp}`. The generator emits SQL
text — the same front door every client uses, so the parser, compiler and
step VM are all inside the tested surface — from a seeded grammar: `CREATE
TABLE` (heap and btree `clustered_type`, int columns plus one varchar to
exercise tagged cells and var-heap spill at both sides of
`inline_cell_width`), `INSERT`, pk-point `SELECT`, pk `BETWEEN` range,
non-pk `FilterScan`. The oracle is the dumbest thing that can be right: per
relation a `std::map<pk, Row>`, updated on every acknowledged write, queried
on every read, compared row-for-row **order-insensitively for scans,
exactly for pk lookups**. Any divergence prints the seed, the op index, and
a minimal repro slice. Value distributions are profile-controlled (uniform /
zipfian / colliding — colliding values are what make FilterScan and later
Cabin paths interesting).
**Needs:** SIM01. **Tests:** 10k-op clean run agrees with the oracle on
every read, on both `clustered_type`s, on ≥ 3 fixed seeds committed as the
first entries of `sim_seeds.txt`.

## Phase S-2 — End-to-end simulation loop

### SIM04 — Crash–restart–verify loop
The centerpiece. One iteration: build an instance on
`MemoryPageDevice`/`MemoryLogDevice`, run seeded workload; at a
seed-chosen op index, `Crash()` both devices (dropping everything unsynced —
the semantics `memory_*_device_test.cpp` already pins); reopen the store over
the surviving image; run `CheckInstance`; reconcile with the oracle. Three
modes with three contracts. **clean**: `SYNC` + shutdown first — everything
the oracle has must be present and integrity must be clean. **sync-crash**:
crash immediately after a `SYNC` — same assertion, restricted to the synced
prefix. **crash** (anywhere): integrity must still be clean *for the durable
image* and no read after restart may return a row the oracle never accepted
(no fabrication); the "every committed-durable row survives" assertion is
written, marked `[GATED: recovery]`, and skipped with a visible count.
Documented-gap states (§8 ghost rows) are counted against expectation, not
failed. The loop runs `--iterations` instances per invocation, each iteration
forking fresh sub-seeds.
**Needs:** SIM02, SIM03. **Tests:** the loop itself, 100 iterations × the
committed seeds, in CI on every run; plus one test proving the `[GATED]`
assertion *fires* when hand-fed a violating image — a gate that cannot fail
is not a gate.

### SIM05 — Fault schedule injection
Widen the failure surface beyond a single crash: a seeded fault schedule
drives `TearNextWrite(n)` and `FailNext*(status)` on both devices during the
workload — torn page writes, torn log appends, failed syncs (with the
`wal_manager_test.cpp` semantics: a failed batch sync leaves committers
waiting, the durable image stays behind), transient read errors. The engine's
obligation under injection is exact: every statement either succeeds or
returns a truthful `Status` — no crash, no wrong answer, no silent
acceptance of a write that did not happen — and the oracle only applies
writes the engine acknowledged. After the fault run: crash, restart, sweep,
reconcile as in SIM04.
**Needs:** SIM04. **Tests:** fault-heavy profile over the seed corpus;
every injected-and-consumed fault is logged with its op index so a failure
names the fault that provoked it.

**BUILT 2026-08-21.** `sim/faults.{hpp,cpp}`, `--faults io --fault-rate N`,
and the loop's absorbing half. What shipped, and the three places it
differs from the paragraph above:

- **Errors only; torn transfers are declined with a reason.** A tear the
  run then *continues past* models a device that reported success for a
  partial transfer and kept working, which leaves a hole in the middle of a
  log whose later records all landed — nothing in `docs/spec/wal.md` is written
  against that, and it was this harness's first false alarm (three of five
  seeds refused the mount with an LSN past the append point, which is the
  hole, not a defect). A tear is what the power cut leaves *in flight*, so
  the realistic image is a partial record at the **tail** with nothing after
  it; expressing that needs a device primitive neither memory device has, a
  crash that promotes a *prefix* of the unsynced overlay. The realistic case
  is already pinned where it belongs — `tests/wal_stream_test.cpp` ("the
  torn record is the end of the stream") and
  `SimIntegrityCorruption.ATornCatalogPageRefusesTheMountInsteadOfServingIt`.
  When `Crash(prefix)` exists the log half asserts and the page half is
  [GATED: FPI].
- **An errored write's outcome is unknown, not absent.** The oracle grew two
  kinds of unknown (`sim/oracle.hpp`): an errored UPDATE/DELETE or an
  abandoned transaction makes those *ids* unchecked, and an errored INSERT
  makes the relation one that may hold **rows the engine never named an id
  for**. Both are permitted and never required in every later comparison.
  This is a model of what a client knows, not a tolerance for an engine bug
  — the harness rule at the top of this file still holds.

  The first cut of that model keyed the errored INSERT on its *content*,
  `(v, name)`, and review killed it: content is not stable. A later
  **acknowledged** `UPDATE ... WHERE v = <x>` moves the ghost off the value
  it was keyed on, and the row then reads as a fabrication — 11 of 15
  corpus cells red. The same key was wrong in the other direction and that
  half was worse, because it was silent: `Reconcile` asks the same question
  of *accepted* rows, so a genuinely lost row whose `(v, name)` merely
  collided with an errored insert's was forgiven. Under `--profile
  colliding`, where `v` ranges over `[0,4]`, that is not a corner case.
  The identity that does hold is the id: ids are issued once and never
  rebound (invariant 11), so the oracle records every id the engine ever
  named and a ghost is exactly a row outside that set.
- **The quiescence probe**, which the task text did not ask for and the
  contract needs: when the schedule is exhausted every injection is
  disarmed and every relation is scanned again. An engine that survives a
  fault run by refusing everything afterwards passes every other check in
  the loop, and fails this one. It is shown to fail
  (`SimFaults.TheQuiescenceProbeFiresOnAnInstanceThatStoppedAnswering`).
- **A `CREATE TABLE` that catches an injection is retried once**, and an
  iteration whose oracle ends up knowing no relation *fails*. Absorbing a
  failed `CREATE` silently is how a fault run passes vacuously: the
  generator keeps naming the relation, the engine keeps refusing, the
  oracle never learned it, and every check in the loop iterates
  `oracle.tables()` — measured at **1291 of 3000 ops thrown away on seed 3,
  printing green**. The retry is what a client does, and
  `ops_on_lost_relation` reports what it could not recover.

  **A retry answering `EXISTS` is adopted, not failed** — and the first cut
  of this had it the other way, which cost nine of ninety-five corpus cells
  until the measurement pass caught it. The injection that kills the
  acknowledgement is a failed commit *sync*, which lands after the catalog
  write, so "it happened" is one of the two legal outcomes: the DDL's
  version of an errored write's unknown outcome, and the retry is how the
  client learns which. **What the harness saw while getting that wrong is
  worth an owner's eye**, because it is not obviously nothing — an
  autocommit `CREATE TABLE` whose commit sync failed answers `ERR` and
  leaves the relation *visible*, which reads oddly beside
  `manual/sql/sql.md` §5's "an autocommit statement is its own transaction
  and unwinds fully". Whether that is a defect or the honest end state of a
  commit that reached its record and lost its fsync belongs to
  `docs/spec/txn.md` §8 and `docs/spec/ddl-transactional.md`, not here. The
  harness accepts both outcomes and asserts the one thing neither reading
  disputes: that the relation must not *half* exist.

Two small additions to the memory devices carry it: `ClearInjections()` and
an `injections_fired` stat, so "this error had an injected cause" is
reportable rather than assumed — and the count is carried per op, because
a failing iteration leaves through its `return` and the counter that says
whether the schedule disturbed anything read zero on exactly the runs it
exists for.

**What it found in the engine: nothing** — one observation for an owner
(the `EXISTS` paragraph above) and no defect. Every fired injection
produces about one errored statement (`armed=120 fired=75 errored_ops=75`
on seed 1 at 1500 ops x 2), and `scripts/sim.sh` over the committed corpus
— 5 seeds x 3 modes x 2 fault profiles x 3 value profiles, plus a pairing
per seed, **95 cells** — passes.

Both numbers in that sentence were wrong before they were measured, and in
both directions: the sweep failed 11 of 15 cells while the oracle keyed a
ghost on its content, and 11 of 95 while the retry called `EXISTS` a
violation. A harness states its own condition as confidently as it states
the engine's, and is wrong the same way.

**What the gate costs**, measured in Release at `dfca583` on a two-core
box: `scripts/sim.sh` over the committed corpus is **11.7 s** (151 s in
Debug — it falls back to `build/` when no release tree exists, so a
Debug-only CI job pays that), and the 17 new tests add **2.60 s** to
`ctest -R '^Sim'` (5.08 -> 7.69 s), five of them accounting for 94% of it.
The engine-side change costs nothing measurable, and not as an estimate:
120 of the 122 engine object files are byte-identical to `aa3e26c`, the
two that differ are the memory devices themselves, and
`file_page_device.cpp.o` / `file_log_device.cpp.o` are unchanged. The
production machine code is bit-identical, so an A/B would be measuring
noise.

### SIM06 — Workload v2: mutations, transactions, features
Grammar grows to `UPDATE` (key-column and non-key-column SETs — the Cabin
append rule and tuple-immobility both care about the difference), `DELETE`,
explicit `BEGIN`/`COMMIT`/`ROLLBACK` with per-transaction durability class
and isolation level (the same three-rung chain the engine exposes), joins
and predicate-position subqueries within the shipped grammar, and feature
toggles per iteration: `waystone_recording`, `cabins` (+ `CREATE
PATTERN`/`CREATE CABIN` ops), `access_statistics`. The oracle learns
transactions as a pending write-set applied on commit, dropped on rollback
— which is exactly enough for single-session correctness; multi-session
semantics are S-3's job, not a smarter map. **The invariant this phase
exists to hammer**: toggling any advisory feature (Waystone, Cabin, access
stats) may never change a result — every iteration runs with a
seed-chosen toggle set, and the oracle does not know the toggles exist.
**Needs:** SIM04. **Tests:** seed corpus extended with mutation-heavy and
toggle-varied profiles; a paired-run mode (same seed, toggles on vs off)
asserting byte-identical result streams — `waystone_contract_test.cpp`'s
five-way comparison, generalized.

**BUILT 2026-08-21.** The grammar grew to `UPDATE` and `DELETE` — each by
pk and by a non-key predicate, each assigning either the column a Cabin and
an index are keyed on or the varchar that may spill — `BEGIN`/`COMMIT`/
`ROLLBACK` with the isolation level drawn per transaction, and `CREATE
CABIN` / `CREATE PATTERN`. The oracle learned transactions as a pending
write-set, and the loop compares the engine's own `UPDATED <n>` /
`DELETED <n>` against the oracle's count of the matching rows, which is
the sharpest single assertion in the harness.

Three divergences from the paragraph above, each named rather than
silently skipped:

- **Per-transaction durability class is not generated, because no spelling
  selects one.** `manual/sql/sql.md` §5 is explicit: the class is the
  instance-wide `durability` config key, and the per-transaction field is a
  KWP/1 feature that is not wired (parser-v2 V12 is open). The class stays
  instance-wide here; when V12 lands this is where it gets drawn.
- **Joins and predicate-position subqueries are not generated yet.** The
  oracle would need a join model to have an opinion about them, and that is
  a bigger change than this task; the shapes are reachable and unclaimed.
- **The pairing compares *answers*, in three tiers.** Byte-for-byte for
  every query and mutation; the assigned id but not the placement for an
  INSERT (the advisory features keep state in `sys.*` relations whose pages
  come out of the same free map, so a trail shifts the next user page —
  invariant 8 promises the state cannot change what a query answers, never
  that the free map allocates identically); acceptance only for a
  declaration, because `CREATE PATTERN` answers CREATED or **ADOPTED**
  depending on whether the recorder had already auto-registered the shape
  and `CREATE CABIN` appends a WARN when the access statistics hold no
  filter on the column. Both of those were found by the pairing, and both
  are replies reporting advisory state on purpose.

**What it found in the engine: one wrong answer**, and it was fixed the
next day. A Cabin entry set banked inside a transaction outlived the
ROLLBACK that restored the row and was then served as authoritative
(`docs/spec/cabin.md` §6a, `docs/inflight/known-gaps.md`). Chasing the fix found the
other half of the same rule — a set banked while *another* session's
transaction is in flight loses the rows that transaction commits — which
is what ruled out un-observing on rollback as a repair. Both halves are
pinned in `tests/sim_loop_test.cpp`; the corpus cell that produced it runs
clean.

**The count assertion is checkable per predicate, not per relation**
(`Oracle::CountCheckable`). A relation with an unknown in it cannot check
a count over a *value* predicate — any row the predicate might match could
be the unknown one — but a **pk** predicate names one row and stays
checkable while that row is known. The generator emits pk predicates 70% of
the time, and the difference is the assertion running on 6% of mutations
under faults instead of most of them.

### SIM07 — Minimizer and corpus discipline
A failing seed at op 80,000 is a fact, not a diagnosis. `--minimize
<seed>` replays with delta-debugging over the operation log (drop a chunk,
replay, keep the failure) until no single removal preserves it, then emits
the trimmed op list as a standalone `.sim` file replayable without the
generator. Corpus discipline in CI: every run executes (a) all committed
seeds, (b) `N` fresh seeds from the date, so the corpus explores forward
while never losing a past failure. New failures auto-append seed + verdict
to an artifacts file for triage.
**Needs:** SIM04. **Tests:** a planted engine bug behind a feature flag is
found by a fresh-seed sweep and minimized to < 50 ops.

**BUILT 2026-08-21.** The seam is `SimPlan` (`sim/loop.hpp`): one
iteration's whole input — the ops, the faults armed before each of them,
and the feature toggles — drawn before the first statement runs, because
the generator never reads a reply. `BuildPlan` is the only place the seed is
consulted and `RunPlan` the only place the engine is, which makes an
iteration replayable without the generator and shrinkable by deleting
entries. **Faults travel with their op** rather than being keyed on an op
index, or every removal would re-aim the whole schedule.

`sim/minimize.{hpp,cpp}` adds delta debugging over the entry list, the
`.sim` case file, and `--minimize [--out FILE] [--max-replays N]` /
`--replay FILE`. The predicate is a normalized **failure signature** — digit
runs collapse to `#`, bracketed SQL and the fault trace are cut — not "it
failed", because dropping ops produces different failures easily and a
minimizer without that check wanders to another bug instead of shrinking
this one.

Two limits, stated rather than discovered: the signature can still conflate
two failures that read alike, and a plan whose failure needs a *specific* op
count (a page fill, a segment roll) shrinks badly. The output is a lead, not
a verdict.

**Its first real use is the record**: the Cabin divergence above, 1200 ops
→ **9** in 933 replays, from which the six-statement case in
`docs/inflight/known-gaps.md` was read straight off. The planted-bug test the
paragraph above asks for is served instead by a fault the harness already
owns — `--skip-recovery` loses acknowledged rows on purpose (RC10) — and
the test asserts the shrunk case still fails *the same way*, which is the
property that makes a minimized case worth reading.

**The case file carries the run-level gates**, which is not paperwork: a
case minimized under `--skip-recovery` — the harness's own planted bug, and
the one the minimizer is demonstrated on — replayed *green* without them,
so SIM07's primary artifact was a file that claimed a failure and did not
reproduce one. Round-tripping it through the file is now part of the
minimizer's test.

**The corpus half is `scripts/sim.sh`**: every committed seed forever, plus
N seeds derived from today's date so the corpus explores forward (date-
derived rather than random, so a failure found today reproduces for anyone
running the same day), with failures appended to an artifacts file for
triage. The sanitizer matrix and the wall-clock smoke threshold stay
SIM13's and are deliberately not in it.

## Phase S-3 — Concurrent-transaction history checking

### SIM08 — Multi-session deterministic driver
Sessions are the concurrency unit the engine actually has: `Session` state,
autocommit vs explicit transactions, `failed-txn` poisoning. The driver owns
K sessions over one dispatcher and interleaves them **statement-at-a-time by
seeded choice** — legitimate because the engine's own contract is one
statement in flight per connection and cooperative execution per core, so
statement-level interleaving is the real interleaving, not a simplification.
v1 is single-core (`cores = 1`); the multi-core generalization is `[GATED:
crosscore pipeline]` and its acceptance criteria are written here — remote
reads observe the owning core's latest committed snapshot, RR weakening
across cores is *expected* and asserted as documented, not excused.
**Needs:** SIM06. **Tests:** same seed ⇒ identical interleaving; a session
poisoned mid-transaction accepts exactly the whitelist.

### SIM09 — History recorder
`sim/history.{hpp,cpp}`: an append-only event log — `{session, txn_ordinal,
event}` where event is `begin(level, class)` / `invoke(stmt)` /
`ok(result-digest)` / `fail(status)` / `commit-ok` / `commit-fail` /
`rollback` — capturing exactly what an external observer knows: what was
asked, what came back, in what per-session order. Row values in read results
are recorded (digested for scans, exact for point reads); the checker never
peeks at engine internals, because a checker that reads the implementation
verifies the implementation against itself.
**Needs:** SIM08. **Tests:** recorder round-trips through serialization;
replaying a recorded history through the oracle is deterministic.

### SIM10 — Isolation checkers
`sim/checkers/`, one checker per documented promise, each a pure function of
a history. Workload shape for this phase: register ops (pk point read /
point overwrite with unique values per write, so every read names its
writer) plus per-relation increment counters for lost-update detection —
the Elle insight (make every write self-identifying, recover the
version graph from values) without importing Elle. Checkers:
- **No dirty reads** (both levels): a value read was written by a
  transaction that committed, or by the reader itself.
- **No lost updates**: first-updater-wins means a conflicting second write
  gets `kTxnConflict` — so for any register, committed writes form a single
  chain; a fork is a checker failure. The conflict *behavior* is also
  asserted: the loser's error is the retryable spelling, and a retried loser
  in a fresh transaction succeeds.
- **READ COMMITTED**: each statement's reads are consistent with *some*
  single committed prefix at statement start (per-statement re-snapshot is
  observable: two statements in one RC transaction may legally disagree).
- **REPEATABLE READ**: two reads of the same register inside one RR
  transaction return the same value unless the transaction itself wrote in
  between; and an RR transaction's whole read set is consistent with one
  committed prefix.
- **Own-writes visibility** and **rollback completeness**: a rolled-back
  transaction's writes are visible to no later read, including delete-marks
  cleared and overwritten bytes restored (the `txn_manager_test.cpp`
  guarantees, now checked under generated concurrency instead of authored
  scenarios).
Write-skew is **deliberately not checked**: the engine promises snapshot-ish
RR with first-updater-wins, not serializability, and a checker demanding
more than the contract is a false alarm generator.
**Needs:** SIM09. **Tests:** each checker is validated against hand-written
histories — one passing, one violating per rule — before it ever judges the
engine; then the driver runs the corpus and every violation prints seed +
minimized history.

### SIM11 — Crash meets concurrency
Compose S-2 and S-3: the multi-session driver runs under the fault schedule,
crash at a seeded point with transactions in flight, restart, sweep, and
check the *surviving* history — in-flight transactions must be wholly
invisible after restart (`[GATED: recovery]`, same discipline as SIM04, with
the §8 ghost-row counter expected nonzero until then), committed-durable
ones intact per their durability class, and the relaxed-class loss window
bounded as `wal_manager_test.cpp` states it. This task is mostly wiring; its
value is that the two hardest subsystems are finally tested *against each
other*.
**Needs:** SIM05, SIM10.

## Phase S-4 — Widening (after the spine holds)

### SIM12 — Grammar-aware statement fuzzing
`parser_fuzz_test.cpp` proves byte noise cannot crash the parser; this adds
the layer it deliberately skips — seeded *well-formed-ish* SQL (mutated from
the generator's grammar: wrong types, unknown columns, over-depth nesting,
40-bit-overflowing pks, aggregate/HAVING forms the engine refuses) asserting
the full dispatch path answers every one with a truthful `Status` and the
instance stays integrity-clean afterward. Every `Unsupported` in the spec
corpus gets a generator that produces it.
**Needs:** SIM03.

### SIM13 — CI wiring and sanitizer matrix
`scripts/test.sh` stays the unit gate; a new `scripts/sim.sh` runs the seed
corpus + fresh seeds with a time budget. Nightly: long sweep, ASan/UBSan
build of the sim binary, TSan build of the real-thread configuration
(`cores > 1` smoke — the sim loop itself is single-threaded by design and
TSan on it proves nothing). A wall-clock smoke threshold (generous, e.g.
3× median) catches accidental quadratic blowups without becoming a flaky
perf gate; real numbers stay in `bench/`.
**Needs:** SIM04; SIM07 for corpus mechanics.

### SIM14 — External differential layer (phase 2)
Python, over a running server through `tools/ckdbs_cli.py`'s transport: the
same seeded workload mirrored to SQLite (embedded, cheap) with results
compared modulo documented divergences (a maintained, *short* exception
list — every entry cites the spec section that licenses it). Value: SQL
semantics the C++ oracle is too simple to have opinions about, and a
protocol-level end-to-end that exercises `TcpServer` and session teardown.
Explicitly after KWP/1 lands or against the text protocol as-is — either
way it must not block S-1..S-3.
**Needs:** SIM06.

---

## Open items and known collisions

- `[OPEN: sim clock]` Nothing in the loop needs simulated time yet — the
  engine's time uses are `last_seen` stats and the checkpointer cadence.
  The moment a checker wants to reason about the relaxed-class loss
  *interval* (SIM11) rather than its boundedness, a seeded clock behind a
  seam must land first. Do not reach for wall clock inside the harness in
  the meantime; assert on event ordering only.
- `[OPEN: harness home]` `sim/` at the repo root vs `tests/sim/`. The doc
  assumes `sim/`; either is fine, but the sim binary must not link gtest —
  a framework's fixture lifecycle fights crash-restart iteration (the
  reason this is a standalone binary at all).
- `[OPEN: torn injection]` SIM05 injects device *errors* only. A torn
  transfer that the run continues past is not the failure it looks like
  (`sim/faults.hpp` carries the argument); the realistic image needs a
  device primitive neither memory device has — a crash that promotes a
  **prefix** of the unsynced overlay rather than dropping all of it. When
  `Crash(prefix)` lands the log half asserts and the page half is
  `[GATED: FPI]`.
- `[OPEN: generated joins]` SIM06's grammar stops short of joins and
  predicate-position subqueries: the oracle would need a join model to
  have an opinion about them. The shapes are reachable and unclaimed.
- ~~`[GATED: recovery]` SIM04/SIM11's full durability assertions~~ —
  **the gate flipped 2026-08-12**, when WAL replay landed (RV1/RC10).
  SIM04's assertion is armed; SIM11 is unbuilt and inherits it armed.
- `[GATED: crosscore pipeline]` SIM08's multi-core driver. Blocked on the
  same open decision `docs/inflight/in-progress/workplan-crosscore.md` P6 names (relation vs
  page ownership); the acceptance criteria are written above so the
  pipeline work inherits its test.
- **Collision watch:** SIM02's sweep and any future `CHECK`/`ANALYZE
  INTEGRITY` statement should share one implementation — if a user-facing
  command is wanted later, it wraps `CheckInstance`, never re-implements
  it. Same rule as `exec/tuple_verify.hpp`: one verifier.
