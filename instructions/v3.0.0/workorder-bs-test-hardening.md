# Work order BS — test hardening: the verification methods a production engine is expected to carry, measured against SQLite's

**Written 2026-10-13** on `wo-test-hardening` at `ec0c07ef`, on the
operator's words W0-W1. **Opened 2026-10-13 for S0 and S1 only** (BS-Q0
(b), §6); S2-S8 waited for BR's close, which came on 2026-10-20
(`instructions/v3.2.0/raft-marks-2026-10-20.md` §3). **Every question in §5
marked by the operator on 2026-10-13, each on CLA's proposal** (§6). **Every
test BS adds stays out of the push gate** (the operator's ruling of
2026-10-13, §6.1). **S0 done 2026-10-13** (`bs-s0-census.md`, measured at
`691beee3`). **Resumed 2026-10-21 on the operator's standing go-ahead**
(`instructions/v3.2.0/raft-marks-2026-10-21.md` §1) in the worktree
`bs-test-hardening`: the rules BS-R1..R4 and every item that surfaced since
are adopted as CLA proposed, not marked by the operator (§7, BS-A1 onward).
The stage log is §8.

The operator's words, translated:

- **W0** (a question): *"Compared with SQLite, what is behind in our test
  methodology and test case implementation, if we are to stand as a
  commercial DB?"* - answered in the session from a read of the main
  checkout on `main` at `dc117a5d`, re-read for this order at `ec0c07ef`
  (§1).
- **W1**: *"Organize it as a work order draft."*

BS was checked against every file name under `instructions/`, `docs/` and
`CLAUDE.md` for a `BS-` token or a `workorder-bs` name at `ec0c07ef`: no use
found. The last letter assigned is BR.

## 0. What BS is

### 0.1 Subject

The suite is large - about 220 test sources, about 104k lines, about 3,640
`TEST`/`TEST_F`/`TEST_P` cells under `tests/` at `ec0c07ef`, and a
deterministic simulation harness under `sim/` - and it is written cell by
cell against the claims of each spec. What it lacks is the set of methods
that find what nobody wrote a cell for. W0 named SQLite; its published
testing practice (sqlite.org/testing.html) is the reference because it is
the best documented example of those methods, not because KDS shares its
architecture:

| SQLite practice | What it finds | BS stage |
|---|---|---|
| Branch / MC-DC coverage measured every release | code no test reaches | S2 |
| Memory and UB sanitizers; assert-on and assert-off builds | memory and UB defects a passing cell hides | S1 |
| OOM and I/O-error tests failing the 1st, 2nd, ... Nth operation in turn | error paths reached once in production and never in a test | S3 |
| Crash tests persisting an arbitrary subset, order and tear of unsynced writes | durability defects that need a realistic power cut | S4 |
| Continuous coverage-guided fuzzing of SQL and of database files | input-handling defects in parsers and the mount path | S5 |
| sqllogictest: queries compared against other engines | wrong answers in the query engine | S6 |
| A regression test per fixed bug, kept forever | the same defect twice | S8 |

KDS needs one thing SQLite does not: SQLite has one writer, KDS has
multi-core MVCC and a lock family, so **isolation is a property only a
multi-session history checker can test** (F9, S7).

### 0.2 Out of scope

Performance benchmarking (`bench/`, `ck-tester`), black-box QA sessions
(`ckdbs-qa-explorer`), formal verification, and the full-page-image cadence
itself, which no order owns (F7).

## 1. What the tree already gives - `[source-read]` at `ec0c07ef`

| # | Fact | Source |
|---|---|---|
| F1 | No sanitizer flag, no coverage instrumentation and no `CMakePresets.json`; the sanitizer matrix is named as unbuilt. Two recorded gaps already wait on a sanitizer build: a teardown mutant killed only under ASan, and BP's fence no cell can kill without one | `CMakeLists.txt`, `cmake/`; `scripts/sim.sh:23-24`; `docs/inflight/known-gaps.md:954-957`; `workorder-bp-per-core-residue.md:436-444` |
| F2 | No CI: no `.github/`, no other runner configuration. The only gate is the local pre-push hook | `scripts/githooks/pre-push`; `CLAUDE.md` Working Rules |
| F3 | The simulation harness drives a whole instance on crashable memory devices, crashes at a seed-chosen op, restarts, and reconciles against an oracle; a failing run shrinks to a `.sim` case and replays | `sim/loop.hpp`, `sim/minimize.hpp`, `scripts/sim.sh` |
| F4 | Its regression corpus is **6 seeds** (`1`-`5`, `20260826003`; the file's other 13 lines are comments `sim.sh` strips); a default sweep adds **4** date-derived fresh seeds | `tests/testdata/sim_seeds.txt`; `scripts/sim.sh:40`, `:46` |
| F5 | Fault injection is **seeded random**, six kinds: page read/write/sync/grow failure, log write/sync failure. No N-th-operation sweep exists | `sim/faults.hpp:66-73` |
| F6 | An allocation failure inside a statement fail-stops the process; five death cells pin it, none sweeps allocation sites | `tests/bad_alloc_death_test.cpp`; `docs/spec/sched.md` §3 |
| F7 | A crash drops every unsynced byte of both devices; **the log side alone** can be cut inside its unsynced run (`CrashWithLogPrefix`, H2). The page side is never torn, partially persisted or reordered. The per-checkpoint FPI cadence is not built and no order owns it (BR-Q1 was adopted as (c), no window images), and even an available image does not heal an allocated checksum-failed page today. A torn page is refused `Corruption` at its first read, which need not be the mount | `sim/instance.hpp:133-153`; `include/kds/wal/memory_log_device.hpp:131`; `docs/spec/wal.md` §10 (line 139); `docs/inflight/known-gaps.md:1179-1183`; `workorder-br-backup-restore.md:438`; `docs/inflight/bugs/a-full-page-image-cannot-heal-a-checksum-failed-allocated-page.md` |
| F8 | The sim workload is CREATE TABLE, INSERT (issued and named key), SELECT by pk and range, a filter scan, UPDATE, DELETE, BEGIN/COMMIT/ROLLBACK, SYNC, CREATE CABIN, DROP TABLE, PURGE. **No join, aggregate, subquery, ORDER BY, secondary index, foreign key or assertion** reaches the oracle | `sim/workload.hpp:61-103` |
| F9 | The oracle is a `std::map<pk, Row>` per relation for **a single session**; multi-session semantics are named as wanting "a history checker, not a smarter map" | `sim/oracle.hpp:14-20` |
| F10 | The only fuzzer is one gtest file of byte-mutation cells over the lexer, parser and fingerprint; it is not coverage-guided, and its seed corpus is the V04-V07 grammar - its header still calls `ORDER BY` and `DELETE` unbuilt, and SET expressions, PURGE and the scalar functions are not seeded | `tests/parser_fuzz_test.cpp:15-36` |
| F11 | KWP's frame decoder has byte-at-a-time and truncate-at-every-offset sweeps, and the mount path has hand-written corruption cells; neither has a coverage-guided fuzzer, and the handshake and SCRAM state machine - unauthenticated input - has none of either | `tests/kwp_frame_test.cpp:128`, `:145`; `tests/superblock_test.cpp`, `tests/page_header_test.cpp`, `tests/mount_recovery_test.cpp` |
| F12 | `docs/spec/wal.md` §16 already states the crash matrix (item 3: partial segment write) and torn-write injection (item 4: checksum detects at load, FPI restores) as testing requirements | `docs/spec/wal.md` §16 (line 218) |
| F13 | A PostgreSQL 18 twin exists for benchmarks and can serve as a differential reference | `/home/cdkbs/pg-bench-ba`, port 15434, as used by `bench/v3.0.0/results-bn-close-v2.7.0-787-g8679367e.md` |
| F14 | AR0's D7, D8 and D9 are open and `[quiet-wrong]` - a wrong choice turns a refusal into a wrong answer - and BR carries `[quiet-wrong]` rules (BR-R4, BR-R5, H3, H8). No harness can observe a wrong answer under concurrency | `CLAUDE.md` Open Decisions; `workorder-br-backup-restore.md`; F9 |

## 2. Rules BS proposes (`BS-Rn`)

- **BS-R1 - A method is a gate only once it runs unattended.** A sanitizer
  build, a fuzzer or a sweep that runs only when someone remembers is a
  tool, not a gate; each stage lands with the place it runs (S1).
- **BS-R2 - BS changes no engine behaviour; a finding is a bug entry.** A
  defect a method finds is filed under `docs/inflight/bugs/` and fixed on
  its own branch, reviewed as a fix. No sanitizer suppression, fuzzer
  exclusion or skipped seed lands without that entry, and it is deleted
  with the fix. The harness adds test seams only where a seam pattern
  exists (`sim/`, the memory devices, `tests/alloc_counter.hpp`); a new seam
  in engine code is a question to the operator.
- **BS-R3 - Every method is deterministic on replay.** A fuzzer crash is
  committed as a corpus file, a sim failure as a seed and a `.sim` case, a
  history-checker anomaly as the recorded history: the reproducer outlives
  the run (F3's discipline, extended).
- **BS-R4 - A differential mismatch is triaged before it is believed.** It
  is a KDS defect, a documented semantic difference (`NULL` ordering, the
  `NOT NULL` default, refused types), or a generator defect. The second
  kind is recorded once, in the generator, with the spec line that states
  it; the generator starts narrow and widens only as triage keeps up.

## 3. Stages

Ordered by cost against what each finds. `∥` marks stages that can run in
parallel once their gate is met.

### BS-S0 - Census (no code)

Measure before building: the suite's wall time under each sanitizer (one
subsystem's cells are enough for an estimate), the allocation and I/O
operation counts of a typical sim run, and the semantic differences between
KDS and the reference engine a generator must avoid or normalize (BS-Q3).
The counts bound S3, whose allocation sweep needs **one forked process per
failure point**, since a fail-stop ends the in-process sim. Output:
`bs-s0-census.md` beside this order.

### BS-S1 - Sanitizer builds and the unattended runner

- Build configurations `asan` (ASan + UBSan) and `tsan`, each running the
  full suite and the sim corpus; plus an assert-off Release run of the suite.
- A nightly runner (BS-Q1) that builds `main`, runs the suite in those three
  configurations, `scripts/sim.sh` with a wider fresh-seed count, and S5's
  fuzzers for a fixed budget; it reports into a file the operator reads.
  The expensive configurations live here, not in the push gate.
- Known-noisy cells
  (`CoreRuntimeTest.APeerListenerServesAReadAndAWriteWithNothingGrantedAndRoutesStop`'s
  SIGPIPE, `IdAllocationAcrossCores`'s timeout) are named in the report,
  never silently retried.
- TSan may not see the synchronization of the coroutines and custom
  latches and report false races; an annotation is an engine change and
  each one is a question (BS-R2).

Gate: the suite green under `asan`, which closes F1's two recorded gaps;
`tsan`'s findings filed. Every configuration and every cell this stage adds
runs from the nightly runner, never from the push hook (§6.1).

### BS-S2 - Coverage measurement ∥ (after S1)

A `coverage` configuration (BS-Q2), a per-directory line and branch report
under `src/`, and a per-subsystem floor recorded at the stage's commit. The
floor is a ratchet: it may only rise (BS-Q4 sets whether it gates).

### BS-S3 - N-th-failure sweeps ∥ (after S1)

- **I/O sweep:** for a fixed workload, fail the 1st, 2nd, ... Nth page
  read, write, sync and grow, and log write and sync, one per run, and
  assert F5's obligation each time: a truthful `Status`, no wrong answer,
  the oracle's indeterminate set honoured.
- **Allocation sweep:** fail the Nth allocation inside a statement, in a
  forked child, and assert `sched.md` §3's fail-stop - the statement never
  completes `OK` and the next mount recovers to the oracle.
- The full sweep runs nightly, and so does any sample of it; none of it
  runs in the push gate (§6.1).

### BS-S4 - Realistic power cut: `wal.md` §16-3 and §16-4

Delivers the two requirements F12 names, and records in `known-gaps.md`
where the tree still falls short of them.

- **Log half (buildable now):** draw a `CrashWithLogPrefix` cut per seed in
  every sim run, composed with F5's faults, not only in H2's cells.
- **Page half:** a page-device crash that persists an arbitrary subset of
  unsynced page writes, in any order, with any one torn at a sector
  boundary. With no FPI cadence (F7) the obligation is the loud one: **every
  read of a torn page is refused `Corruption`, never served**, at whatever
  point it is read. Healing is asserted only when an owner builds the
  cadence and the allocated-page bug is fixed (BS-Q5).

### BS-S5 - Coverage-guided fuzzing ∥ (after S1)

Three targets, each a standalone fuzz binary under `fuzz/` (BS-Q2), its
corpus committed (BS-R3), each seeded from the cells F10 and F11 name:

1. **SQL text**: lexer, parser and compile, seeded from
   `tests/testdata/parser_corpus.txt` plus every grammar form after V07.
2. **KWP**: frame decode, handshake and the SCRAM state machine, from
   `tests/testdata/kwp_golden.txt`.
3. **Volume and log images**: a superblock, a page of each class and a WAL
   segment, mutated and mounted; the obligation is a refusal naming
   `Corruption`, never a crash and never a mount that serves a wrong row.

### BS-S6 - Wider sim workload and differential query testing

- Extend `sim/workload` with secondary indexes, joins, aggregates,
  subqueries, `ORDER BY`/`LIMIT`, foreign keys and assertions, each with its
  oracle extension (F8). An index, a Cabin or an inner build must not change
  an answer - the premise of the index, cabin and inner-build contract
  suites - so the oracle compares each query with and without them.
- A random-query generator in the sqllogictest manner, run against KDS and
  the reference engine (BS-Q3) over the same data, under BS-R4.

### BS-S7 - Multi-session history checker

Several sessions on several cores run a randomized transactional workload;
every operation's invocation, response and observed values are recorded,
and a checker (Elle-style cycle detection over the dependency graph) decides
whether the history is admissible under the isolation `docs/spec/txn.md`
promises. S4's crash points compose with it. This is the stage that can
observe what F14's `[quiet-wrong]` decisions would get wrong.

### BS-S8 - Regression discipline and close

- Every bug fixed since BS opened has a regression cell or seed (BS-R3);
  the sim corpus grows with every seed a sweep found.
- Mutation testing applied to one subsystem per milestone as a check on the
  suite's teeth, not a gate. Under concurrency one mutant run proves
  nothing: each is repeated, and the interleaving forced with a barrier.
- The close report states each method's last nightly result and the
  coverage floor, with the commit they were true of.

## 4. The bar BS closes on

At one commit: `asan` green; `tsan` green or every finding filed; the
nightly runner has reported on seven consecutive nights; a coverage floor
recorded per subsystem; S3's sweeps green; S4's log half in every sim run
and its page half asserting the refusal; three fuzz targets each run for
their budget with no open crash; S6's workload covering every row of F8's
"no" list; S7's checker run over the lock family's shapes with no
unexplained anomaly.

## 5. Questions for the operator (`BS-Qn`)

| # | Question | Options | CLA's proposal |
|---|---|---|---|
| BS-Q0 | Open BS? | (a) open now beside BR; (b) S0 and S1 only now, the rest after BR closes; (c) not now | **(b)**: S1 is cheap, needs no engine change and finds defects in work already landed; the larger stages compete with BR for the same review attention |
| BS-Q1 | Where does the unattended runner run? | (a) GitHub Actions on the remote; (b) a cron job on the benchmark host; (c) both | **(b)** first: the suite already runs there, and a run must never overlap a benchmark on that host, so the slot is outside measurement windows; (a) once a hosted runner's budget is known. Sending the tree to a hosted runner is outward-facing and waits for the word |
| BS-Q2 | Toolchain for coverage and fuzzing | (a) clang: libFuzzer + llvm-cov; (b) gcc: AFL++ + gcov; (c) both | **(a)**: one compiler gives sanitizers, coverage and in-process fuzzing. The toolchain is proposed, never decided, by CLA (`CLAUDE.md`) |
| BS-Q3 | Differential reference engine | (a) the PostgreSQL 18 twin; (b) SQLite; (c) both | **(a)**: its typed comparisons and three-valued logic are closer to KDS's than SQLite's dynamic typing, and it is installed (F13) |
| BS-Q4 | Does the coverage floor gate? | (a) record only; (b) a drop below the floor fails the nightly; (c) a drop fails the push | **(b)**: visible every day without making a push wait on an instrumented build |
| BS-Q5 | S4's page half, with no owner for the FPI cadence | (a) build the device primitive now and assert only the refusal; (b) defer the page half until an order owns the cadence | **(a)**: the primitive is test-only, and "a torn page is never served" is worth pinning before any heal exists; whether the cadence gets an owner is a separate question this order does not answer |

## 6. Marks

The operator's marks of 2026-10-13, given in one line as
`BS-Q0 $!b, Q1 $!b, Q2 $!a, Q3 $!a, Q4 $!b, Q5 $!a` - each the option CLA
proposed in §5:

| # | Mark | What it obliges |
|---|---|---|
| BS-Q0 | **(b)** | BS is open for S0 and S1 only; S2-S8 open after BR closes |
| BS-Q1 | **(b)** | The nightly runner is a cron job on the benchmark host, scheduled outside measurement windows; a hosted runner is a later question, and sending the tree to one still waits for the word |
| BS-Q2 | **(a)** | clang: sanitizers, libFuzzer and llvm-cov from one compiler |
| BS-Q3 | **(a)** | The differential reference is the PostgreSQL 18 twin |
| BS-Q4 | **(b)** | A coverage drop below the recorded floor fails the nightly; it does not block a push |
| BS-Q5 | **(a)** | S4's page half is built now as a test-only device primitive and asserts only that a torn page is refused `Corruption` at its read, never served |

### 6.1 The operator's ruling of 2026-10-13: BS's tests stay out of the push gate

The operator's words, translated: *"Do not include the tests newly added in
this milestone in the suites the hook runs on every push."*

What it obliges:

- **Every test BS adds runs outside `scripts/githooks/pre-push`.** A gtest
  cell BS adds carries a `ctest` label that `scripts/test.sh` excludes with
  `-LE`, as `heap-suspended` is excluded (AS-R4); a sweep, fuzzer, sanitizer
  configuration or checker BS adds is a target or script the hook never
  builds or runs. The label's name is S1's to choose and is stated where it
  is registered in `tests/CMakeLists.txt`. Two mechanics follow from the
  tree as it is: `scripts/test.sh` excludes with one `-LE`, which since BZ
  deleted `heap-suspended` (AS-R4's label) names `bs-nightly` alone (S1);
  and `scripts/build.sh` builds every default target, so a sweep or
  fuzz target is `EXCLUDE_FROM_ALL` or behind an option that defaults off.
- **S3's "strided sample in the suite" is withdrawn**: the sample runs in
  the nightly with the full sweep, never in the gate.
- **The nightly runner (S1) is where these tests run.** Until it reports,
  a BS test runs only when a stage runs it by hand, and a stage report says
  so - AS-Q4's warning (excluding a cell from the only thing that runs it
  deletes its coverage) is the reason the runner lands in the same stage as
  the first labelled cell.
- **Not covered by the ruling, and left where it is**: a fix to an existing
  test file a BS method exposes (S0 found `tests/alloc_counter.cpp` replacing
  `operator new` but not its `nothrow` form, which ASan reports as an
  alloc-dealloc mismatch) changes no gate membership; and a regression cell
  for an engine bug a BS method finds is written on the fix's own branch
  (BS-R2), where the fix's reviewer decides its gate membership.

## 7. Items adopted on the standing go-ahead of 2026-10-21

BS resumed on the operator's go-ahead (`instructions/v3.2.0/raft-marks-2026-10-21.md`
§1). Each item below is **CLA's proposal, adopted on the operator's standing
go-ahead of 2026-10-21 (go-ahead-achieving-milestone)**, not marked by the
operator item by item.

| # | Item | Options | Adopted, and why |
|---|---|---|---|
| BS-A1 | The rules BS-R1..R4, unmarked since 2026-10-13 | (a) adopt as written; (b) adopt with BS-R2's bug-entry clause read against `CLAUDE.md` | **(b)**: `CLAUDE.md` says a defect fixed in the session that found it gets no `docs/inflight/bugs/` entry, and it outranks an order's proposal. A defect a BS method finds and this session fixes is recorded in §8's stage log and in its fix commit, on a branch of its own; one left unfixed gets the entry BS-R2 names |
| BS-A2 | BS-Q2 (a) names clang, which census T1 found absent and needing `sudo` | (a) stop for the install; (b) clang 18 from Ubuntu's own packages, fetched with `apt-get download` and unpacked with `dpkg -x` under `/home/cdkbs/opt/llvm18`, no `sudo` | **(b)**: it meets BS-Q2 (a) as marked - one compiler for sanitizers, libFuzzer and llvm-cov - and `scripts/bs/install-llvm18.sh` makes it again on any host with the same packages. No system package is installed |
| BS-A3 | Where the nightly runs before BS reaches `main` | (a) install it after the push; (b) follow the work branch in the local repository until the push, then `main` | **(b)**: §4's bar counts seven consecutive nights, and nights on the branch are nights the methods ran; following a local branch publishes nothing. The report names the ref it ran |
| BS-A4 | TSan and the test binary's replaced `operator new` (`tests/alloc_counter.cpp`) | (a) replace every form, aligned ones included; (b) link TSan's runtime shared | **(b)**: clang links TSan's static C++ runtime whole (`--whole-archive`), so any replacement is a duplicate definition, whatever forms it covers; a shared runtime is interposed over instead (`scripts/bs/config.sh`) |
| BS-A5 | S3's allocation sweep: census §3 found ~1.26 M allocation points per sim iteration, which no night holds at one forked child each | (a) a short plan swept exhaustively; (b) a fixed budget of points drawn per statement kind; (c) both | **(b)**: ten statement kinds, each in a file-backed instance of its own, its allocations counted in a reference run and 4 + 4 + 24 points swept - the first and last four, where setup and teardown paths live, and an even spread between. A point past the child's own count (the counts differ by an allocation or two) is counted as unreached, never as a statement that survived; at most four may be (`tests/bs/alloc_sweep_test.cpp`) |
| BS-A6 | S3's I/O sweep: how far N goes for each device kind | (a) a probe run counts each kind's operations first; (b) N rises until a point's fault no longer fires | **(b)**: the sim reports `fired=` for every run, so the end of a kind is observed rather than predicted, and a probe that miscounted a kind would leave points unswept with nothing to say so. Every committed seed, `clean` and `crash`, 1,500 ops a night (`scripts/bs/io_sweep.py`) |
| BS-A7 | S3's I/O sweep reaches almost no page read: S3's review found the first read never failing at seed 1, and at 1,500 ops in `crash` mode one seed in six reading a page under a statement at all - a mount is where pages are read | (a) state the window and leave reads to the census; (b) a second phase, the Nth operation counted from the reboot's start, with the restart's obligation | **(b)**: `KIND:N@mount` (`sim/faults.hpp`). A mount the fault fails is followed by a power cut and a mount with nothing armed, which must recover what the first would have and pass the run's whole contract; a mount the fault does not fail is held to it as it stands. The workload then runs fault-free and is held to the whole contract too. It is the property a restart after a failed mount depends on, and no cell asked it before |

## 8. Stage log

| Stage | State |
|---|---|
| BS-S0 | **Done 2026-10-13** (`bs-s0-census.md`), measured at `691beee3` |
| BS-S1 | **Built 2026-10-21** on `bs-test-hardening`. **The configurations** (`scripts/bs/config.sh <config> build suite sim`): `asan` (Debug, ASan + UBSan, `-fno-sanitize-recover=undefined`), `tsan` (Debug, its runtime linked shared, BS-A4) and `noassert` (Release), each in `build-bs-<config>/`, through the CMake cache entry `KDS_SANITIZE` (empty by default, so the hook's build is unchanged); clang 18 is the user-space install of `scripts/bs/install-llvm18.sh` (BS-A2), whose TSan, unlike census T2's gcc 13, starts on this kernel without `setarch -R`. **The runner**: `scripts/bs/nightly.sh` runs each configuration's build, the hook's range of the suite and `scripts/sim.sh` with 16 fresh seeds, and writes `<day>.md`, `latest.md` and one line in `index.md` (§4's seven nights are counted there); a release `kds_server` running skips the night and says so; the two known-noisy cells are named under their own heading and never retried. `scripts/bs/install-nightly.sh` installs it as a cron entry at 19:30 UTC following a ref of a repository: the work branch in the local repository until BS lands (BS-A3), then `main`. **The gate's mechanics** (§6.1): `scripts/test.sh` excludes the label `bs-nightly`, named in `tests/CMakeLists.txt`. **Census T3 fixed**: `tests/alloc_counter.cpp` replaces the `nothrow` forms too. **What the first runs found** - ASan + UBSan failed 163 of 4,189 cells at `e0dd4d04`, and TSan 14 of 4,190 at `8ef3cd98`: **four engine defects**, each fixed on a branch of its own (BS-R2, BS-A1) - `CREATE CABIN` reading a column name through a `TableAccess` the write's version bump had freed (110 cells), `DROP COLUMN` dereferencing a cached `SysColumnRow*` after a catalog write (2), `SelectBody` reading the fingerprint of the empty parser a query-layer walk carries (UBSan, 48), all three on `fix-asan-s1-findings` at `8ef3cd98`; and `DevicePageStore`'s chunk vector reallocated under an unlatched `SlotPage` (TSan, 4), made a fixed array sized at `Open` on `fix-tsan-s1-findings` at `3099051b`; **test defects** - `tests/visibility_test.cpp` handing `memcpy` a null (UBSan, 3), rig cells submitting to or dispatching on a running reactor against `two_core_rig.hpp`'s own rule (TSan, 8; `TwoCoreRig::Post` added, or the rig stopped before the test thread reads), `checkpoint_data_sync_test.cpp` reading a count the syncer's thread writes after it publishes (TSan, 1); and `ExpeditorTest.TwoCoresComeUpOnOneLogAndEachHoldsTheVolumesOwnImage` dispatching on a running peer with no seam to avoid it, **filed** (`docs/inflight/bugs/expeditor-test-dispatches-on-a-running-peer.md`). **At `0e9ea02e` with S1's files**: `asan` 4190/4190 (904 s at `-j8`), `tsan` 4189/4190 - the filed cell - (944 s), `noassert` 4190/4190 (426 s), builds excluded; the Debug suite, the hook's own range, 4189/4190 at `-j8` under a host load of ~16, its one failure `SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement` timing out at 60 s and passing 5 runs in 5 alone (1.4-6.9 s), now named among the runner's known-noisy cells. **F1's two gaps**: the teardown mutant (`Scheduler::DiscardTasks`'s call removed) dies under `asan` 5 runs in 5 (`known-gaps.md`, updated); BP's fence (`workorder-bp-per-core-residue.md` H1-H3) still cannot be killed, because its rig cells drive both cores from the test thread - TSan exists now, the cell that would give it two threads does not. Overhead not measured; measured at the milestone's close |
| BS-S3 | **Built 2026-10-21** on `bs-test-hardening` from `c3abee3a`. **The I/O sweep** (BS-A6, BS-A7): the memory devices' one-shot injections take a skip count (`include/kds/base/injected_failure.hpp`, one type for both devices); the sim fails the Nth operation of a kind, counted from the first op - `ckdbs-sim --fail-nth KIND:N`, which turns the loop's fault contract on as `--faults` does - or from the reboot's start - `KIND:N@mount`, where a refused mount must be recovered by the next one, fault-free; a case file shrunk under either keeps it. `scripts/bs/io_sweep.py` raises N for each of the six kinds until a point's fault no longer fires, over every committed seed, `clean` and `crash`, workload and mount: at 1,500 ops, **5,772 runs, 5,261 of them points that fired, 0 failures, 2.2 min** on 6 jobs (noassert build). The mount phase is what reaches page reads: 13 to 28 points of them a seed, where the workload reaches 0 to 2. **The allocation sweep** (BS-A5): `tests/bs/alloc_sweep_test.cpp`, ten statement kinds - insert, multi-row insert, update by key, an update that spills, delete by predicate, an insert under an index, a child insert under a foreign key, `CREATE INDEX`, `CREATE TABLE`, a join with an aggregate - each point a forked child that must stop at its allocation, its stderr naming the fail-stop, the parent then mounting what it left and reading every check of the kind as it stood before the statement or every one as it stood after - an index probe beside a walk, a catalog view beside a read. Its first run found the harness's own fault, not the engine's: at the last points the child's allocation count fell short of the reference run's, so the Nth never came and the statement returned - now counted as unreached, at most four a kind; the child also reads the checks the reference read, so both count over one history. The new binary `kds_bs_tests` carries the label `bs-nightly`, so the hook never runs it; the nightly runs it under `asan` (`config.sh asan bs-cells`) and the I/O sweep on noassert's `ckdbs-sim`. Overhead not measured; measured at the milestone's close |
