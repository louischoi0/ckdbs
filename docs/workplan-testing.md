# Test Strategy — Workplan

Work instructions for building the verification layer that sits **above** the
existing unit and subsystem-contract tests. Tasks `SIM01`–`SIM14`.

The 87 files under `tests/` are thick at the unit and subsystem level — device
crash/torn-write contracts, WAL durability classes, B+ tree structural
invariants, MVCC visibility, parser fuzz — and empty above it. Nothing runs a
randomized workload end to end, crashes it at an arbitrary point, restarts,
and checks the whole instance; and nothing checks concurrent-transaction
histories against the isolation levels `docs/txn.md` promises. This workplan
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

**Recovery does not exist.** `CLAUDE.md` is explicit: nothing reads the WAL
back; a restart is protected only by `PageStore::Sync()` at `SYNC` or clean
shutdown. So `SIM04`'s crash-restart loop cannot yet assert "committed ⇒
survives". It asserts what is *currently promised* — the durable prefix is
intact and internally consistent — in three modes (clean shutdown /
crash-after-sync / crash-anywhere), and the full assertion is written now but
gated off, so that **this harness is the acceptance test recovery must pass
on the day it lands**. Building the checker before the feature is the point:
recovery written without an adversarial harness waiting for it would be
tested by its own author's imagination.

**MVCC ships with a known crash gap.** An uncommitted row surviving a crash
reads as committed on the next boot (`docs/txn.md` §8). The integrity sweep
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
individually reportable: page header sanity and checksum per `docs/page.md`;
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
- `[GATED: recovery]` SIM04/SIM11's full durability assertions. The gate
  flips when WAL replay lands; no partial mitigation (per `docs/txn.md`
  §8's own instruction).
- `[GATED: crosscore pipeline]` SIM08's multi-core driver. Blocked on the
  same open decision `docs/workplan-crosscore.md` P6 names (relation vs
  page ownership); the acceptance criteria are written above so the
  pipeline work inherits its test.
- **Collision watch:** SIM02's sweep and any future `CHECK`/`ANALYZE
  INTEGRITY` statement should share one implementation — if a user-facing
  command is wanted later, it wraps `CheckInstance`, never re-implements
  it. Same rule as `exec/tuple_verify.hpp`: one verifier.
