# Work order BP — what the per-core engine left: per-core structures the shared engine still pays for

Written 2026-10-12 **in the main checkout on `main` at `fe248d94`**
(`git describe` not computed: the session's clone is shallow and reaches no
tag), with no worktree. It is a document-only change, pushed to `main`
directly under the standing practice for documents written in a claude.ai
session. It follows the operator's words of 2026-10-12, recorded in English
(`raft-marks-2026-10-12.md` §8-§9):

- **W0:** *"Sync the current repo and find what is left over from the
  shared-nothing era, and the overhead those leftovers cause."* CLA reported
  a survey in four groups. Group A is cost `cores = 1` pays. Group B is
  structure that costs above one core. Group C is dead code. Group D is
  comment and naming residue. §1 is that survey, re-read for this order.
- **W1:** *"Write a work order for that issue: hypothesis, verification,
  code change, measurement, and re-exploration or rejection where needed."*

**Opened 2026-10-12** on `worktree-bp-per-core-residue` from `cf889b63`, on
the operator's word W2, recorded in English (`raft-marks-2026-10-12.md`
§13): *"Open BP, adopt CLA's proposals, S2 first, and remove every citation
of the shared-nothing concept (CLAUDE.md and other documents)."*
- **Marked by W2, as CLA proposed:** BP-Q0..Q3, Q6, Q8..Q13 and every
  ruling in §2. BP-Q4, Q5 and Q7 are marked "decided by BP-S1": their
  option is taken from the census, on CLA's proposal, under the operator's
  standing go-ahead of 2026-10-12 (go-ahead-achieving-milestone).
- **BP-Q14**, the reach of W2's citation sweep, was written after W2 and
  adopted on the same standing go-ahead (§5).
- The order was renamed from `workorder-bp-shared-nothing-residue.md` under
  W2; its W0 quotation above is the operator's words and stays as said.

**The `critics-developer` review owed at BP-S0** ran at the opening (§7).

**Claim tags:**
- `[source-read]`, with `path:line` at `fe248d94`: read, not run.
- `[measured]`, with the results file that carries the invocation. BP-S0
  ran nothing; its one `[measured]` number is quoted from BI's results
  file (§1.B B2).
- `[design]`, for CLA's reasoning about code not yet written.
- `[quiet-wrong]`, for an item where a wrong choice turns a correct answer
  into a wrong one with no error.
- `[provisional]`, for a threshold set before any measurement. It is
  replaced by the measured value.

The operator's words, and a rule that restates them, are decisions rather
than claims, so they carry no tag.

**The letter.** `[source-read]` BO is the last letter under
`instructions/v3.0.0/` (`workorder-bo-sys-statistics-show-sugar.md`). No file
under `instructions/`, `docs/`, `src/` or `include/` uses BP. The operator
named no letter, so BP is CLA's choice, and BP-Q12 puts it to the operator.

**BP against other orders:**
- **AT, M3 Uniformity** (`workorder-at-m3-uniformity.md`,
  `workorder-at-close-ar0-5-left-open.md`). AT retired ownership and the
  ring. BP is not AT reopened. BP takes what AT's retirement left standing
  because it still had a consumer, and asks whether that consumer still
  needs it.
- **AX, the in-flight publication** (`workorder-ax-inflight-publication.md`,
  closed). AX built the per-core in-flight tables. BP-H9 measures them and
  changes them only on a mark (BP-Q7).
- **BF, DROP TABLE page reclamation** (closed). BF built the statement
  epoch. BP-H1 asks whether its fence is needed at `cores = 1`.
- **BI, the checkpoint off the reactor** (closed in code at `ec827a19`). BI
  moved each core's checkpoint run off its reactor. BP-H7 measures how many
  runs there are. It changes neither BI's pacing nor its sync thread.
- **BL, the session balancer** (written, not opened). BL-R1 keeps a
  transaction on its core, partly because its in-flight slot and its `live_`
  entry are per core (BL's `[source-read]` *"A transaction is pinned"*). BP
  changes neither, unless BP-Q7 or BP-Q6 is marked to. Any such mark would
  bear on BL-Q1 (b), and §5 says so.
- **BM, the scan row** (written, not opened). Both orders touch
  `command_dispatcher.cpp`, and no ruling of either depends on the other.
  BP-S2 and BP-S3 touch the statement head and the mint. BM touches the
  walk.
- Not part of AR0 §8's chain.

---

## 0. What BP is

**The intent, in one sentence:** every structure the engine keeps because
work used to be partitioned by core is put to one test, run as one loop. Is
it still needed? Is it paid for where it should cost nothing? And is the
cost material where it is allowed to cost something? Each structure then
ends cut, re-explored, or rejected with the number that rejected it.

**The loop, per hypothesis** (BP-R0, W1's five words):
1. **Hypothesis.** A falsifiable sentence about one structure: what it
   costs, and why the engine no longer needs it in its present form.
2. **Verification.** The premise is checked before any code is written: in
   source where source settles it, by instrument where it does not. A
   failed premise rejects the hypothesis at this step.
3. **Code change.** In its own stage, with cells that pin the behaviour and
   mutations each cell must kill.
4. **Measurement.** On the cells §6 names, in `build-release`, interleaved
   A/B.
5. **Re-explore or reject.** If the measurement or the verification
   contradicts the hypothesis, CLA writes the next hypothesis as `BP-Hn.k`
   (a re-exploration), or records the hypothesis as rejected with the
   number or citation that rejected it. A rejection is a result, not a
   failure of the order.

**Three goals:**
- **BP-G1 — `cores = 1` pays nothing for a structure that is not shared.**
  This is the standing rule `base/latch.hpp:9-15` calls G2 (this order
  says BP-G1, so the two numbers do not collide), restored where
  §1.A shows it broken.
- **BP-G2 — above one core, a per-core structure stays only if it earns its
  cost.** It earns it either by avoiding a shared hot spot that §6
  measures, or by a ratified decision the operator keeps (BP-R3).
- **BP-G3 — the tree says what is true.** Dead code goes. A comment that
  states something false of the code at `fe248d94` is corrected.

**Non-goals, stated so no stage grows into them:**
- **No on-disk format change.** The superblock stays at version 22
  (`superblock.hpp:274`). The 64 anchor slots, `sys.tables.owner_core`'s
  four bytes and the page stream stamp's two bytes stay where they are on
  disk. BP may stop *writing* a value. It never moves or removes a field.
- **No change to a ratified structure without a mark** (BP-R3): AT-S8's
  per-core checkpoint, AT-S10b/AT-R4's per-core id carve, and AX's per-core
  in-flight table. For those, BP verifies and measures. A change needs the
  operator's ruling and, if ruled, its own order.
- **Not BL.** Sessions and transactions stay on their core.
- **No new cross-core primitive.** Write-then-kick stays the only one
  (`crosscore.md`).

---

## 1. Survey at `fe248d94`

Every line here is `[source-read]` at `fe248d94` unless tagged otherwise.
The survey ran over `src/` and `include/` in two passes:
- three independent readers, one each for `txn`, for WAL/superblock, and
  for server/catalog/stats;
- CLA's own re-read of every line this order cites.

### 1.A Cost `cores = 1` pays

**A1 — the statement epoch's fence.**
- `StatementEpochs::Enter` stores `kEntering` and issues
  `std::atomic_thread_fence(std::memory_order_seq_cst)`
  (`include/kds/server/statement_epoch.hpp:64-67`).
- The outermost statement scope calls it on every statement
  (`src/server/command_dispatcher.cpp:887-904`).
- The epochs are built unconditionally, sized by `cores`
  (`src/server/expeditor.cpp:1207`).
- Their one reader is the reclaimer: `AllAtLeast`, with its own fence
  (`statement_epoch.hpp:101-107`), on core 0's 50 ms reclaim tick
  (`expeditor.cpp:2088-2096`).
- The epoch exists because a catalog memo is revalidated only at a
  statement's head (`statement_epoch.hpp:3-13`). That is A1's tie to B1's
  per-core memo.

**A2 — the window latch.**
- `InstanceVisibility::window_latch_` is a `Latch` member, not a `Latch*`
  (`include/kds/txn/instance_visibility.hpp:636`).
- `LookupCommit` (`src/txn/instance_visibility.cpp:316-325`) and
  `BeginCommit` (`:73-96`) take it through `LatchGuard(&window_latch_)`,
  which is never null, so the mutex is taken at every core count.
- `LookupCommit` is how a reader decides a writer above the floor.
- The commit marker's `open_bounds` push, find and erase run per commit
  (`:90-109`).

**A3 — the Cabin store's latches.**
- `CabinStore::Partition::latch` (`include/kds/stats/cabin_store.hpp:559`)
  and `stats_latch_` (`:608`) are `Latch` members.
- `CabinSet::At` takes the partition latch per entry
  (`src/stats/cabin_store.cpp:89-96`).
- `Find` and `NoteHit` take the partition latch, and `NoteHit` also the
  stats latch (`:112-140`).
- Every other instance-shared structure built since AT is armed only above
  one core:
  - the assertion registry, `emplace(/*shared=*/cores > 1)`
    (`expeditor.cpp:1120`);
  - the optimizer signals (`:1217-1219`);
  - the Cabin view latch (`:1309`).
- The store became one for the instance at AT-S7, which is why the latch
  exists. It is a residue of the store's merger rather than of ownership.
  It is in BP because it is the same break of `latch.hpp`'s G2.

**A4 — the mint walks this core's `live_`.**
- `TransactionManager::MintCheckView` walks `live_` for any active
  transaction other than its own (`src/txn/manager.cpp:110-115`).
- It then asks `visibility_->AnyUnresolved()` only if the walk found none
  (`:122`).
- The comment calls the walk "sharper" because it can exclude the view's
  own transaction (`:116-121`). The two answers are joined by OR, so the
  walk can only add a `true`, never remove one.
- The walk is a residue of the per-core Cabin store
  (`instance_visibility.hpp:345-356`: the question "is the instance's now").

**A5 — the page stream stamp.**
- Every logged page mutation writes `StreamStampFor(CurrentCore())` into
  the page header (`src/storage/device_page_store.cpp:1722-1725`), and redo
  writes it again (`src/wal/redo.cpp:93-126`).
- The header says it "decides nothing" and that no branch reads it
  (`include/kds/storage/page_header.hpp:143-149`). Its readers are
  `SHOW PAGE` (`command_dispatcher.cpp:2206`, `:2240`).
- It recorded which per-core stream a `page_lsn` was an offset into.
- `superblock.cpp:74-77` states that this build "neither writes nor reads"
  the stamp. That is false at `fe248d94`.

### 1.B Structure that costs above one core

**B1 — a catalog per core, and one word for every catalog change.**
- Each core holds its own `Catalog` and `CatalogCache`. `Revalidate` loads
  the instance's schema word at a statement's head and, if it moved, drops
  the **whole** memo (`src/catalog/catalog.cpp:961-971`).
- The word moves on every catalog version bump (`catalog.cpp:931-944`), on
  a bare `BumpWord` for a root move, and when a transaction that wrote
  catalog rows resolves (`src/txn/manager.cpp:296-300`). Among the movers:
  - a Cabin's create or drop (`:3113`, `:3140`), which CABIN AUTO issues on
    its own (`src/exec/cabin_optimizer_exec.cpp:267`, `:294`, `:311`,
    `:439`);
  - a clustered tree growing a level (`catalog.cpp:2692`, a `BumpWord`
    with no version bump);
  - an index root moving (`:3626`, likewise, "every other core re-reads;
    this one keeps its entry").
- So a level growth on any relation empties every other core's memo. Each
  of those cores then re-resolves every relation it touches next.
- The first `IX` of a write statement re-runs the statement when the word
  moved after the statement's head
  (`src/server/command_dispatcher.cpp:9368-9384`, "Conservative: any
  catalog write anywhere moves the word").
- **A stale root is a refusal, not a wrong answer.** A lookup that lands on
  a leaf no longer covering its key re-descends, and after
  `kMaxDescentRestarts` it is refused `TxnConflict`, naming "a root that
  has since grown a level" (`src/storage/btree/btree.cpp:1185-1207`). A
  write descent refuses on coverage too (`btree.cpp:203-207`), the
  grown-over mark refuses a second root (`:477-486`), and the catalog's own
  comment names the descent's retryable refusal as what covers the window
  (`catalog.cpp:2689-2691`). The bump on a root move buys liveness: a core
  that never refreshes its root would be refused for good on keys outside
  the old subtree.
- **What the word guards for correctness is the rest of the catalog.** A
  statement that resolved before a `CREATE INDEX` or `CREATE ASSERTION`
  must not write rows under the relation without them
  (`command_dispatcher.cpp:9368-9379`, the AT-S5e review's C2).
- No counter records how often a memo is dropped or a statement re-run.

**B2 — one checkpointer per core over one pool.**
- Each core builds a checkpointer over the instance's store
  (`src/server/core_runtime.cpp:439-441`), on a cadence offset by
  `period / core_count * core_id` (`:545-555`).
- Each run snapshots the core's own active transactions and the
  **instance's** dirty table (`src/wal/checkpointer.cpp:157-158`). It
  writes back the instance's dirty pages (`:499`) and parks on the data
  sync twice (`:515`, `:527`).
- One run at a time for the instance (`include/kds/wal/checkpointer.hpp:266-276`),
  and the gate rotates over cores (`:340-355`).
- The anchor folds every core's latest run
  (`src/server/superblock_checkpoint_anchor.cpp:44-60`), so it follows the
  core whose run is oldest.
- **Ratified:** AT-S8 declined an instance-wide active table
  (`include/kds/server/superblock_checkpoint_anchor.hpp:85-93`). Per period
  the instance writes back its pool up to `cores` times and syncs data up to
  `2 * cores` times. `[design]`, from the code's shape.
- `[measured]` BI's close counted the runs: 120 per ten minutes at
  `cores = 1` and 384-476 at `cores = 4` under `group`
  (`bench/v3.0.0/results-bi-s5-checkpoint-stall-v2.7.0-784-gec827a19.md`,
  item (iv)). That is about 3.2-4x the runs for the same cadence. BI did
  not report bytes written back or data syncs per run by core count, so
  the I/O multiplier is BP-S1's to measure.

**B3 — a transaction-id window per core.**
- Every core carves its own block of `kTrxIdBlockSize = 4096` ids from the
  one ceiling (`include/kds/txn/trx_id.hpp:46-57`, `:75`). Ids therefore
  stop following issue order across cores. That is the reason for:
  - the issue cursor in the floor
    (`src/txn/instance_visibility.cpp:179-194`);
  - `PinsFloor` (`:207-213`);
  - the idle-block burn (`src/txn/manager.cpp:657-702`).
- Peers burn on their drain tick (`core_runtime.cpp:525-530`), and core 0
  on its writeback tick.
- **Ratified:** AT-S10b and AT-R4.

**B4 — an in-flight table per core.**
- 1,024 slots per core (`instance_visibility.hpp:306`), one table per
  attached core (`:621-622`).
- `InFlight` scans every attached core's table and returns at the first
  hit (`instance_visibility.cpp:265-277`). A miss reads every table.
- Its callers:
  - `PURGE`'s per-row test (`command_dispatcher.cpp:10128`);
  - a named key's duplicate check (`:5913`);
  - `UndecidedInsert` (`:5644`);
  - the catalog's unfiltered `ScanAll` read for DT9 (`catalog.cpp:128`);
  - the poll predicates of a row wait with no slot (`command_dispatcher.cpp:408`,
    `:801`, `:9209`).
- A core's 1,025th concurrent `Begin` is refused `ResourceExhausted`
  whatever the other cores hold (`manager.cpp:144-154`).
- **Ratified:** AX.

**B5 — lock partitions sized `64 * cores`.**
- The size is set at `src/txn/lock_table.cpp:59-63`, with
  `kLockPartitionsPerCore = 64` at `include/kds/txn/lock_table.hpp:396`.
- `FenceCoversKey` takes every partition's latch in turn
  (`lock_table.cpp:606-631`). It is reached only while the relation's fence
  counter is nonzero (AO-R3, `:609-612`).
- The sizing's evidence is AO-S7. CLAUDE.md's Transactions row says that
  measurement "no longer describes the engine": it ran when a relation's
  writes ran on its owner core.

### 1.C Function, not overhead

**C1 — only core 0's DDL triggers the delete-mark purge.**
- The one call to `PurgeSettledDeleteMarks` is behind
  `if (core_id_ == catalog::kSystemCore)` at the end of a DDL scope
  (`command_dispatcher.cpp:6697-6698`).
- The mark counter is the instance's, so that the sweeping core sees a
  peer's mark (`include/kds/catalog/catalog.hpp:356-366`, "or a peer's
  `DROP` would wait for the next mount").
- The sweep itself runs only when core 0 ends a DDL scope.
- `[design]`: under a workload whose DDL all runs on peers, a mark waits
  for the next mount. No cell asserts otherwise.

### 1.D Dead code

**D1 — the two-phase and handoff recovery arms.**
- TXN_PREPARE (`RecordType::kTxnPrepare = 27`, `include/kds/wal/record.hpp:218`)
  is decoded and resolved in several places:
  - `src/wal/analysis.cpp:193-221`;
  - `src/wal/recovery.cpp:49-102`;
  - `src/wal/redo.cpp:56-60`;
  - `EncodeTxnPrepare` and its decoder (`src/wal/payload.cpp:174-210`).
- PAGE_HANDOFF is checked at `analysis.cpp:100-105` and `:139`, and at
  `redo.cpp:52`. Its emitter `LogPageHandoff`
  (`include/kds/wal/log_page_handoff.hpp:34`) and its payload codec are
  still compiled, with no caller.
- No engine path emits either record: grep finds `EncodeTxnPrepare` called
  only from `tests/`.
- A mountable volume is version 22 alone (`src/server/superblock.cpp:68-83`).
  2PC retired at AT-S6 and its last emitter at AT-S18. The last handoff
  writer retired at AT-S9.
- The comments' justification, "a pre-AT volume still needs it", cannot
  hold behind that gate. `SHOW META`'s `recovery_prepared*` lines print
  only when the count is nonzero (`command_dispatcher.cpp:1846-1851`), so
  they never print.

**D2 — dead symbols:**
- `TableAccess::AnyCabin()` (`include/kds/catalog/schema.hpp:282`) has no
  caller.
- `Catalog::catalog_version()` (`catalog.hpp:1214`) has no reader in
  `src/`; about six test cells read it (`tests/catalog_test.cpp:603`,
  `:608`, `:797`, `:982`, `:1011`, `:1349`, `tests/txn_session_test.cpp:964`),
  and `:608` pins "a root move must not drop the cache". By BP-H13's own R
  it stays.
- `Expeditor::BroadcastShutdown`'s scheduler argument is unused
  (`expeditor.cpp:1346`).
- `SuperBlockFields::wal_anchor_count` is written
  (`superblock.cpp:237-239`) and read by no branch. It is an on-disk field,
  so it stays written (non-goal).
- `CoreRuntime::Config`'s `cabins`, `cabin_limits` and
  `buffer_pool_frames` are unread by a server peer
  (`include/kds/server/core_runtime.hpp:122-139` says so). `anchor`
  (`:203`) is read by the fixture path (`core_runtime.cpp:432`) and belongs
  with D3.

**D3 — fixture-only arms.**
- A runtime handed no instance structure builds its own:
  - a lock table (`core_runtime.cpp:317-322`);
  - a Cabin store (`:333-335`);
  - an assertion resume (`:421-434`);
  - an oid counter (`catalog.cpp:909-928`).
- No server path reaches them. They are the shape of the per-core engine,
  kept for tests.

### 1.E Names and prose

- **`kMaxWalCores = 64`** (`include/kds/server/superblock.hpp:408`) caps the
  core count (`superblock.cpp:18-23`, "the number of superblock WAL anchor
  slots"). Only slot 0 is published (`superblock.cpp:226-235`). The cap is
  still real, because these are sized 64:
  - the published mask (`superblock_checkpoint_anchor.hpp:226`);
  - `CheckpointGate`'s slots (`checkpointer.hpp:340-355`);
  - the in-flight and visibility arrays (`instance_visibility.hpp:615-622`);
  - the page latch's owner field.

  The *name* and the *message* name a reason that is gone.
- **Statements false at `fe248d94`:**
  - `superblock.cpp:74-77` (A5);
  - `include/kds/catalog/catalog_cache.hpp:47-52` ("only safe while a
    single instance owns DDL");
  - `include/kds/txn/manager.hpp:49-51`: "No cross-core protocol" and its
    `wal.md` §3 pointer (S0 review: "a transaction spanning cores is not
    representable" is still true, BL-R1);
  - `catalog_cache.hpp`'s "there is no DROP TABLE to retire one" and "DDL
    cannot interleave with a statement because the reactor is
    single-threaded", which a yielded `SELECT` has broken since BA-S15
    (S0 review);
  - `src/txn/lock_table.cpp:379` ("bounded by the live transactions on this
    core": the wait-for graph is the instance's).
- **Tombstones.** `[source-read]` 281 comment lines in `src/` and 311 in
  `include/` name retired machinery. The invocation is in §7. Their
  densest homes:
  - `command_dispatcher.cpp` (95);
  - `expeditor.cpp` (40);
  - `command_dispatcher.hpp` (30).

  That count is noisy in both directions (S0 review): a tombstone block
  runs past its keyword line, and `AT-S5`, `coordinator` and `participant`
  also match live text.

---

## 2. Rulings — CLA's proposals, marked by W2

### BP-R0 — One loop per hypothesis, and a rejection is recorded

Every BP-H item runs §0's five steps in order.
- **Verification precedes code.** A stage writes no line of a code change
  until the hypothesis's verification is written into §7, as
  `[source-read]` or `[measured]`.
- **What a rejection records:**
  - the step that rejected the hypothesis;
  - the citation or number that did it;
  - whether a re-exploration `BP-Hn.k` replaced it.
- A re-exploration is a new hypothesis. It runs the loop from step 1.
- **At most one re-exploration per original hypothesis** without the
  operator's word (BP-Q2). This keeps the loop from becoming open-ended.

### BP-R1 — A BP-G1 break is cut on proof, not on size

Group A items are breaks of a standing rule (BP-G1, `base/latch.hpp:9-15`).
Each is cut when its verification proves that nothing at `cores = 1`
shares the structure. The measurement reports the delta and **gates
nothing**: a break of a rule is not kept because it is cheap.
- **What a verification must show:** every thread that can touch the
  structure at `cores = 1`, by its call sites. Besides the reactor, the
  instance runs three threads, each named and shown not to touch it or to
  go through a path that still synchronizes:
  - the WAL writer (`src/wal/writer.cpp`);
  - the log preparer (`src/wal/file_log_device.cpp:311`);
  - the data syncer (`src/storage/data_syncer.cpp`).
- **`[quiet-wrong]`** for A1, A2, A3: a missed thread is a data race that
  no single-core test sees.
- **What a test can and cannot pin.** The tree has no sanitizer build:
  `scripts/sim.sh:22-24` leaves the sanitizer matrix to SIM13. Most
  two-core rig tests drive both cores from the test thread, without
  starting the reactors (`tests/inflight_publication_rig_test.cpp:12-15`,
  `tests/in_run_reclaim_test.cpp:9-10`). A removed fence or latch above
  one core is therefore not something a cell can kill. What a cell can pin
  is the **arming**: the structure is armed at `cores > 1` and unarmed at
  `cores = 1`. The rest is the call-site census, written into §7 as
  `[source-read]`. That census is the proof BP-R1 asks for, and BP says
  so rather than presenting the arming cell as more than it is.

### BP-R2 — A Group B change is made only if material

A Group B item is changed only if §6's census shows it material. The
threshold is `[provisional]` until BP-S1:
- **3 %** of a statement's server CPU on any §6.1 cell; or
- **5 %** of throughput, or of p99, on a multi-core cell, against the same
  cell with the structure's cost removed by instrument.

An item under the bar is rejected with its number, and its structure stays.

### BP-R3 — A ratified structure is measured, not changed

B2 (AT-S8), B3 (AT-S10b, AT-R4) and B4 (AX) are decisions the operator
made. BP-S1 measures each, and §7 puts the number beside the ruling that
made it. Code changes only on a mark (BP-Q5, BP-Q6, BP-Q7). A marked
change gets its own order if it is larger than one stage.

### BP-R4 — Dead code is deleted whole, and a retired record is refused

A D-item is deleted with its tests, its counters and its comments. A
retired WAL record kind is never ignored.
- **The rule for a retired kind.** If analysis meets
  TXN_PREPARE (27) or PAGE_HANDOFF, it refuses `Corruption`, naming the
  kind as retired at a superblock version below 22. The kind's number
  stays reserved in `record.hpp`.
- **Why `[quiet-wrong]` if inverted.** A skipped record on a volume that
  should not exist would mount a log BP cannot reason about.

### BP-R5 — Prose is corrected where false, and pruned only where marked

- **Always corrected:** §1.E's false statements and A5's line.
- **Tombstones are pruned only as BP-Q10 marks.** A tombstone is history.
  The tree's rule is that history lives in git (CLAUDE.md, "a plan's
  history lives in git"), but deleting a tombstone also deletes a reader's
  warning not to rebuild the thing. That trade is the operator's.

### BP-R6 — Measure first; the close measures once

- **BP-S1 measures every B premise** before any B stage.
- **The whole change is measured once, at the close** (Session Workflow
  step 3), A at BP's opening commit against B at its close.
- **A BP-G1 stage reports its own A/B** at that stage, because BP-R1 says its
  measurement gates nothing. The report keeps the number honest, it does
  not decide.

---

## 3. The hypotheses

Each card is §0's loop. `V` is verification, `C` the code change, `M` the
measurement, and `R` the re-exploration or rejection.

### Group A — `cores = 1` (BP-R1)

**BP-H1 — the epoch's fence is not needed at `cores = 1`.** `[quiet-wrong]`
- **H.** At `cores = 1` the statement and the reclaimer run on one reactor
  thread. Program order already gives the publish-before-read the fence
  exists for (`statement_epoch.hpp:14-20`). The fence is a full barrier per
  statement with no second thread to order against.
- **V.** The call-site census of BP-R1:
  - every caller of `Enter`, `Publish`, `Leave` and `AllAtLeast`, by
    thread: the statement scope (`command_dispatcher.cpp:887-904`), the
    reclaim tick (`expeditor.cpp:2088-2096`), any reclaim driven at mount,
    any test seam;
  - a yielding `SELECT` (BA-S15) holds its slot across a yield, and the
    reclaim tick reads it on the same thread.
- **C.** `StatementEpochs` takes a `shared` flag at construction (`cores >
  1`). `Enter` and `AllAtLeast` issue their fences only when shared.
- **Cells:**
  - **arming:** `shared()` is true at `cores = 2` and false at `cores = 1`;
  - **behaviour, `cores = 1`:** a `DROP TABLE` committed while a yielded
    `SELECT` over the relation holds its slot is not reclaimed until the
    `SELECT` returns;
  - the BF reclaim suite unchanged.
- **Mutations, each repeated:**
  - `shared` built false at `cores = 2` (killed by the arming cell);
  - the slot left idle across a yield (killed by the behaviour cell).

  The fence's own necessity above one core is argued, not tested
  (`statement_epoch.hpp:14-20`): a StoreLoad reorder between atomics is
  neither a data race a sanitizer reports nor an interleaving a
  test-thread rig produces.
- **M.** §6.1's BP-G1 cells, reported (BP-R1).
- **R.**
  - If V finds an off-reactor reader, H1 is rejected with that call site.
  - **H1.1:** whether the epoch can go entirely, once B1 is settled. If
    BP-Q4 keeps the whole-memo drop, H1.1 is rejected with BP-Q4's mark.

**BP-H2 — the window latch is not shared at `cores = 1`.** `[quiet-wrong]`
- **H.** At `cores = 1` every caller that takes the window latch runs on
  the reactor: `LookupCommit`, `BeginCommit`, `PublishCommit`, `Reclaim`
  and `window_size`. (`EndCommit` takes none,
  `instance_visibility.cpp:99-109`.) The latch is a `std::mutex` taken per
  above-floor visibility decision and per commit, with no contender.
- **What the review found in source** (§7; to be re-read as V):
  - `PublishCommit`'s one caller is `FinishCommitBody`
    (`src/txn/manager.cpp:324`). It is reached from `Commit` (`:414`), on
    the reactor, and from `FinishDeferredCommit` (`:379`).
  - A deferred commit needs `KicksDurableWaiters()`
    (`command_dispatcher.cpp:1089-1092`), which nothing sets at one core
    (`include/kds/wal/manager.hpp:340-347`). Above one core the deferred
    publish still runs on the reactor after its wait
    (`command_dispatcher.cpp:869-873`).
  - At one core a `group` sync runs on the calling thread
    (`src/wal/manager.cpp:186-188`).
  - The WAL writer syncs and kicks and does not touch `InstanceVisibility`
    (`src/wal/writer.cpp:115-143`).
  - `Reclaim()`'s callers are `PublishCommit`
    (`instance_visibility.cpp:312`), the idle burn (`manager.cpp:699`, on a
    tick), the manager's attach (`manager.hpp:327`) and the undo horizon
    source (`manager.hpp:345-347`, `undo_log.cpp:100`).
- **V.** The census above, re-read at the stage's commit and extended to
  every caller of `LookupCommit` and `window_size`.
- **C.** `window_latch_` becomes armed-or-null, like the lock table's
  latches (`lock_table.cpp:64`), **armed by default**: the Expeditor
  disarms it at `cores = 1`. A default-constructed `InstanceVisibility`
  stays armed because `tests/instance_visibility_test.cpp:980`
  (`AReclaimedWinnerIsNeverAnsweredUncommitted`) runs `LookupCommit`
  against `Reclaim` on two threads over one. `WindowLatchForTest()`
  returns `Latch&` (`instance_visibility.hpp:608`, used at
  `tests/contention_counters_test.cpp:328`) and changes with the seam. The
  commit marker's `open_bounds` stays: it is per-commit state, not
  synchronization.
- **Cells:**
  - **arming:** armed at `cores = 2`, null at `cores = 1`;
  - the visibility, commit and recovery suites green at both core counts;
  - `inflight_publication_rig_test.cpp` unchanged.
- **Mutation:** the latch null at `cores = 2`, killed by the arming cell,
  and probabilistically by the two-thread cell above (S0 review).
- **M.** §6.1's BP-G1 cells, plus the `group` and `strict` commit loops at
  `cores = 1`.
- **R.** If V finds an off-reactor caller, H2 is rejected with its call
  site. No re-exploration is proposed.

**BP-H3 — the Cabin store's latches are not shared at `cores = 1`.** `[quiet-wrong]`
- **H.** At `cores = 1` the store's readers and writers share one thread:
  - the walk's serve (`step_vm.cpp`);
  - the FK check's serve (`fk_check.cpp`);
  - the write hook;
  - the CABIN AUTO executor on core 0's tick.

  The partition and stats latches guard nothing there.
- **V.** Every caller of `CabinStore` and `CabinSet` named by thread. In
  particular, show that the Cabin optimizer's executor runs on core 0's
  reactor (`expeditor.cpp`'s tick) and not on a helper thread.
- **C.** `CabinStore` takes `shared` at construction. Its latches become
  armed-or-null, as `expeditor.cpp:1120` already does for the assertion
  registry.
- **Cells:** the Cabin contract suite byte-identical at `cores = 1` and
  `cores = 2`; arming, armed at `cores = 2` and null at `cores = 1`.
- **Mutation:** the latches null at `cores = 2`, killed by the arming cell.
  The race the latch prevents, one core serving a set while another heals
  an entry of it (`cabin_store.cpp:91-93`), is argued, not tested
  (BP-R1).
- **M.** §6.1's Cabin-served cell at `cores = 1`.
- **R.** Rejected if V finds an off-reactor caller.

**BP-H4 — the mint's `live_` walk adds nothing `AnyUnresolved` does not.** `[quiet-wrong]`
- **H.** `in_flight_at_mint` is `walk || AnyUnresolved()`
  (`manager.cpp:110-122`). The walk is true only when another active
  transaction is on this core. That transaction's `Begin` already ran
  `PublishCoreBounds()` synchronously (`manager.cpp:183`, to `:726`), which
  sets this core's `oldest_unresolved` and so raises
  `cores_with_unresolved_` (`instance_visibility.cpp:44-50`). Then
  `AnyUnresolved()` is already true.
- **V.**
  - Every path that sets `Transaction::active_` true, shown to run
    `PublishCoreBounds()` before it returns.
  - Every path that lowers `oldest_unresolved` to "none", shown to run
    only when no active transaction is left in `live_`.
  - The comment at `manager.cpp:99-102` ("the one reason this still walks
    `live_`") and `:116-121` (the walk is "sharper") read against the OR.
- **C.** The walk is deleted, and the two comments are corrected.
- **Cells** (direct, on `TransactionManager`, because both banking
  consumers ask `AnyUnresolved()` again, `step_vm.cpp:955`,
  `cabin_optimizer_exec.cpp:123`, and a banking cell would not see a
  mint-side defect):
  - after `Begin` of transaction A on core 0, a view minted for B on core
    0 has `in_flight_at_mint` true;
  - after A commits and is released, a new view has it false when no other
    core has an unresolved transaction.
- **Mutation:** `Begin`'s `PublishCoreBounds()` removed, so the bounds
  publish lazily. It is killed by the first cell once the walk
  is gone.
- **M.** §6.1's point `SELECT` and point `UPDATE` at `cores = 1` (one mint
  per statement).
- **R.** **H4.1**, a re-exploration needing BP-Q2's word: whether the
  `in_flight_at_mint` field is itself redundant, given that both consumers
  re-ask at banking. It is `[quiet-wrong]`: the mint-time fact and the
  bank-time fact differ for a transaction that resolves in between.

**BP-H5 — the page stream stamp is a write nothing needs.**
- **H.** Nothing reads the stamp but `SHOW PAGE` (§1.A A5). Writing it
  costs one thread-local read and a two-byte store per logged mutation, and
  it ties a page's bytes to the core that last wrote it.
- **V.** `[source-read]` already: the readers grep to `SHOW PAGE`, and the
  header says no branch reads the stamp. Verification adds the tests that
  assert the stamp through `GetPageStreamStamp`, not by its printed name:
  `tests/device_page_store_test.cpp:103-106`, `:981`, `:1008` and
  `tests/wal_redo_test.cpp:388`, and (S0 review) `page_header_test.cpp:41-50`,
  `wal_redo_test.cpp:402`, `:421-435` and `wal_checkpointer_test.cpp:445`.
  Under BP-Q3 (a) they change in C. **The envelope's logging-core byte
  dies with the stamp**: its one reader is `LoggingCoreOf`
  (`redo.cpp:93`), and it is written at `checkpointer.cpp:28-29` and
  `log_page_init.hpp:40`; `payload.hpp:526-540`'s "a dirty table belongs
  to one core's pool" is false since AM-S2 step 3.
- **C.** Per BP-Q3:
  - (a) stop writing it: the field stays, written 0 by a new page, and
    `SHOW PAGE` drops it;
  - (b) keep it as a diagnostic, and correct only `superblock.cpp:74-77`.
- **M.** §6.1's every-row `UPDATE` cell. It is reported, not gated.
- **R.** No re-exploration. Under (b), H5 is closed as "kept by the
  operator's mark".

### Group B — above one core (BP-R2, BP-R3)

**BP-H6 — whole-memo drops for root moves are a material share of multi-core time.** `[quiet-wrong]` for any change
- **H.** B1's word moves on structural events, not only on catalog
  changes:
  - a level growth;
  - an index root move;
  - CABIN AUTO's create and drop;
  - a catalog-writing transaction's resolution.

  So on an insert-heavy multi-core workload, every other core repeatedly
  re-resolves every relation it touches, and write statements re-run at
  their first `IX`. A root move needs no word bump for correctness: a
  stale root is refused, not answered wrongly (§1.B B1). The bump buys
  only the refresh.
- **V.**
  - `[measured]` at BP-S1, by **one word-move counter per cause** in
    `SHOW META` (S0 review: a root move happens only on a level growth,
    about three or four times in a relation's life at an internal fanout
    near 600, so the frequent movers are CABIN AUTO's create and drop and
    catalog-writing resolutions): `catalog_word_moves_root`,
    `catalog_word_moves_cabin`, `catalog_word_moves_ddl`. The premise holds
    if §6.1's CABIN AUTO or growth cell shows the moves, and the
    resolution time the sampler attributes to them, above BP-R2's bar.
    Since `fe248d94` the two `BumpWord` sites merged into `AdoptRoot`
    (`catalog.cpp:2677-2699`).
  - `[source-read]` for options (a) and (d): every read and write descent
    that can start from a memoized root, clustered and index, shown either
    to cover the whole key space from a stale root or to refuse. The known
    cases are a lookup (`btree.cpp:1185-1207`), a write
    (`btree.cpp:203-207`), an index seek that lands left and walks right
    (`src/storage/index/index_tree.cpp:553-561`) and a full scan along the
    leaf chain. A descent that answers from a subtree without refusing
    rejects (a) and (d).
- **C** (only on BP-Q4's mark). The options:
  - (a) **Struck at the S0 review**: a ring of recent moves with a
    `[quiet-wrong]` classifier, aimed at the rarest mover.
  - (b) **No memoized roots.** A bind reads the root from the relation's
    anchor page, one page access per relation per statement, and root
    moves stop bumping the word.
  - (c) **Keep it.**
  - (d) **Root moves stop bumping the word.** A structural refusal
    refreshes that one relation's root from its anchor before the
    statement's retry. **The index tree needs the same hook** (S0 review):
    `NoteStructuralRefusal` is called only from `btree.cpp:235`, `:455`,
    `:482`, `:1201`; the index tree's refusals (`index_tree.cpp:197`, `:257`)
    rely on the word bump (`index_tree.cpp:192-194`), and an index refusal
    reaches the client rather than re-running. Without an index-side
    refresh, (d) refuses a stale index root forever.
- **Why `[quiet-wrong]`.** The danger is not the stale root. It is a word
  that stops moving, or a move classified as structural that was not.
  Either way a memo survives a `CREATE INDEX` or `CREATE ASSERTION`, and a
  write lands without the index entry or the assertion check
  (`command_dispatcher.cpp:9368-9379`). The cells aim there:
  - **(d):** after core 1 commits `CREATE INDEX i ON t(v)`, core 0's
    next `INSERT INTO t` writes an entry into `i`; likewise for
    `CREATE ASSERTION`, which core 0's next write checks;
  - **(d):** after core 1 grows a level, core 0's next `SELECT` of a
    key the old root does not reach returns it, within the retry budget
    and without a client-visible `TxnConflict`;
  - **(d):** after core 1 grows an index root, core 0's next `INSERT`
    succeeds.
- **Mutations, each repeated:**
  - **(d):** the refresh skipped on a structural refusal (killed by the
    level cell, as a refusal reaching the client);
  - **(d):** the index-side refresh skipped (killed by the index-root
    cell).
- **M.** §6.1's growth cell and CABIN AUTO cell, A/B.
- **R.**
  - Under the bar, rejected with its counters.
  - **H6.1** (`[quiet-wrong]`), if the re-runs dominate and the drops do
    not: the first-intention test compares the relation's own entry rather
    than the word. It needs BP-Q2's word, and the C2 cells above are its
    gate.

**BP-H7 — N checkpoint runs per period cost what one would not.** Ratified (AT-S8); measure only
- **H.** Above one core the instance writes back its pool, and syncs data,
  up to `cores` times per period (§1.B B2). The anchor trails by about one
  period. One instance-wide run at period `P / cores` would bound the
  anchor's age equally at `1/cores` of the I/O.
- **V.** `[measured]` at BP-S1 with `tools/bi_checkpoint_stall.py` at
  `cores = 1, 2, 4`:
  - runs and data syncs per period;
  - bytes written back per period;
  - the anchor's age, sampled;
  - p99 of a concurrent OLTP cell.
- **C.** None in BP. §7 sets the number beside AT-S8's ruling and puts
  BP-Q5's options. One option is an instance-wide active set gathered from
  the per-core in-flight tables AX already publishes. Those tables carry
  ids, not undo heads, so that option needs the heads published too, which
  is a `[design]` question for its own order.
- **R.** If the I/O multiplier is under BP-R2's bar, rejected, and BP-Q5
  is withdrawn.

**BP-H8 — the per-core id window's machinery is material.** Ratified (AT-S10b, AT-R4); measure only
- **H.** The issue cursor, `PinsFloor` and the burn exist only because ids
  are carved per core (§1.B B3). An idle core can hold the floor back by up
  to a block (4,096 ids) until it burns. While it does, every visibility
  decision above the floor goes through `LookupCommit` (A2's latch), and
  `Reclaim` walks a larger window.
- **V.** `[measured]` at BP-S1 from counters that already exist, with no
  new instrumentation (S0 review: BP-Q6 is report only, so a counter built
  here has no outlet): the window's size and burns, if `SHOW META` carries
  them, in the skew cell (§6.1). Otherwise BP-S7 reports H8 as unmeasured
  and leaves it to the later order BP-Q6 names.
- **C.** None in BP. BP-Q6 puts the number beside AT-R4.
- **R.** Under the bar, rejected.

**BP-H9 — the per-core in-flight table's miss scan, and its per-core cap, are material.** Ratified (AX); measure only
- **H.** A miss in `InFlight` reads every attached core's table (§1.B B4)
  on paths that ask per row:
  - `PURGE`;
  - the named-key duplicate check;
  - a row wait's poll.

  And a core's 1,025th `Begin` is refused while other cores have room.
- **V.** `[measured]` at BP-S1:
  - `InFlight` calls and misses, and the slots read per miss, by counter;
  - the `PURGE` cell and the named-key cell at `cores = 1, 4`;
  - for the cap: whether any §6.1 cell, or BL's session-skew premise
    (`workorder-bl-session-balancer-priority-affinity.md` §5.1), reaches
    1,024 concurrent transactions on one core.
- **C.** None in BP. BP-Q7's options:
  - (a) keep;
  - (b) an instance pool of slots, which also bears on BL-Q1 (b);
  - (c) a per-core summary word (min and max live id) that answers most
    misses without the scan.

  (c) is the one CLA would build first, as `BP-H9.1`, if the scan is
  material and the operator marks it.
- **R.** Under the bar, rejected.

**BP-H10 — `64 * cores` lock partitions are sized by evidence that no longer holds.**
- **H.** The partition count scales with cores on AO-S7's evidence, taken
  under ownership (§1.B B5). `FenceCoversKey` latches every partition, so
  a fence-holding relation pays `64 * cores` latch acquisitions per tuple
  ask.
- **V.** `[measured]` at BP-S1 with `tools/lock_contention_benchmark.py`
  at `cores = 1, 4, 8`:
  - the partition latch's contention counter (`base/contention.hpp`);
  - in a fence cell, the time in `FenceCoversKey`.
- **C** (only if material): a fixed partition count measured at BP-S1, or
  a per-relation fence list that `FenceCoversKey` reads instead of every
  partition. At `cores = 1` the partition latches are null
  (`lock_table.cpp:64`), so the single-core cost is the walk over 64
  partitions, not latching.
- **M.** The same cells, A/B.
- **R.** Under the bar, rejected. AO-S7's sizing then stands on BP-S1's
  number instead of its own.

### Group C — function

**BP-H11 — a peer's `DROP` leaves its delete-marks to the next mount.**
- **H.** Only core 0's DDL scope end calls `PurgeSettledDeleteMarks`
  (§1.C C1). With every DDL on peers, marks accumulate until a mount.
- **V.** A two-core rig cell:
  - core 1 runs `CREATE TABLE t` then `DROP TABLE t`;
  - then nothing runs on core 0;
  - then the cell waits past every reader's settlement;
  - and reads core 0's `SHOW META` `catalog_marks_purged` and the mark
    counter.

  H11 holds if the mark is still there after a bound `[provisional]` of
  1 s. A second V cell at `cores = 1` (S0 review): core 0's own last DDL
  leaves its marks until the next DDL whenever readers had not settled at
  its scope end.
- **C.** The purge moves from "core 0's DDL scope end" to core 0's
  existing 50 ms `system` tick. **It runs only on progress** (S0 review): a
  sweep is a write fetch of every catalog page even when it retires nothing
  (`catalog.cpp:1063-1065`), so a mark held back by a long reader would
  otherwise dirty the whole catalog twenty times a second. The tick sweeps
  when the instance mark counter is nonzero (`catalog.hpp:356-366`) and the
  read horizon has moved since the last sweep that left a mark. It stays
  one sweeping core and a placement, as the comment at
  `command_dispatcher.cpp:6690-6696` requires.
- **Cells:**
  - the V cell, now purged within the bound;
  - a mark whose deleter is unresolved is not purged.
- **Mutation:** the tick's progress gate removed, killed by a cell holding
  a mark back under an open reader and reading `catalog_purge_sweeps` (a
  new `SHOW META` counter), which must not rise while the horizon stands
  still.
- **M.** The idle tick cost at `cores = 1`, by the reclaim tick's existing
  shape (a predicate and a return).
- **R.** If V shows a purge that this survey missed, rejected with its call
  site.

### Group D — dead code and prose (BP-R4, BP-R5)

**BP-H12 — the TXN_PREPARE and PAGE_HANDOFF arms are unreachable on any mountable volume.** `[quiet-wrong]` if inverted
- **H.** No engine path emits either record, and the superblock refuses
  every volume that could hold one (§1.D D1).
- **V.**
  - Grep for emitters across `src/`, shown empty.
  - Show every superblock version ≥ 22 postdates AT-S18. That means
    reading BK's bump (`superblock.hpp:267-274`) against AT-S18's date in
    `workorder-at-close-ar0-5-left-open.md`.
- **C.** BP-R4's deletion:
  - both kinds stay assigned, so no later kind reuses their numbers and
    a cell can still encode one (the pins at
    `tests/wal_payload_test.cpp:538-563` stay), and analysis refuses both
    `Corruption`, naming them retired;
  - `LogPageHandoff` (`log_page_handoff.hpp:34`) and the handoff payload
    codec go;
  - the resolution loop, `prepared_txns`, `PreparedTxn`, the payload
    codec, `recovery_prepared*` in `SHOW META`, and
    `tests/prepared_recovery_test.cpp` go;
  - `wal_payload_test.cpp`, `wal_analysis_test.cpp`, `wal_redo_test.cpp`,
    `wal_recovery_test.cpp` and `mount_recovery_test.cpp` lose their
    prepare and handoff cases and gain one refusal cell per kind.
- **Mutation:** the refusal turned into a skip, killed by the refusal
  cells.
- **M.** None (cold, mount only). `tools/mount_cost_benchmark.py` is run
  once, to say so.
- **R.** If V finds a mountable volume that could carry either kind,
  rejected, and the arm's comment is corrected to name that volume.

**BP-H13 — the dead symbols and fixture arms can go with no behaviour change.**
- **H.** §1.D D2 and D3 have no server caller.
- **V.** A grep per symbol across `src/`, `include/`, `tests/`, `tools/`
  and `sim/`.
- **C.**
  - Delete D2's symbols. `wal_anchor_count` stays written (non-goal), its
    comment saying it is read by nothing.
  - D3's fixture arms are deleted only if the suite builds the instance
    structures for those fixtures through the rig's existing wiring
    (`workorder-av-two-core-rig.md`). Otherwise they stay, and the comment
    names them fixture-only.
- **M.** None. The suite green is the gate.
- **R.** A symbol with a caller is dropped from the list.

**BP-H14 — names and prose that state a retired fact.**
- **H.** §1.E's names and statements mislead a reader about the engine at
  `fe248d94`.
- **V.** Each is re-read at the stage's commit.
- **C.**
  - `kMaxWalCores` is renamed to say what it now bounds. CLA proposes
    `kMaxCores`. BP-Q11 names it.
  - Its refusal message is corrected.
  - §1.E's four statements are corrected.
  - Tombstones are pruned as BP-Q10 marks.
- **M.** None.
- **R.** None.

---

## 4. Stages

| stage | what | done when | size |
|---|---|---|---|
| BP-S0 | **The order** | <ul><li>This file</li><li>Its review (§7)</li><li>The index row</li><li>`raft-marks-2026-10-12.md` §8-§9</li></ul> | S |
| BP-S1 | **The census** (BP-R6): every Group B premise, and the BP-G1 cells' baseline | <ul><li>**Code: instrumentation only, no behaviour change:**<ul><li>BP-H6's word-move counters, one per cause;</li><li>BP-H9's `InFlight` calls, misses and slots read;</li><li>checkpoint runs and data syncs per core in `SHOW META`'s checkpoint block.</li></ul></li><li>**The drivers:** §6.1's cells in `tools/ba_census.py` and `tools/multicore_benchmark.py`.</li><li>**The measurement**, into `bench/v3.0.0/results-bp-s1-census-<describe>.md`.</li><li>**Each Group B hypothesis** marked held or rejected against BP-R2's bar in §6.</li><li>**The suite green.**</li></ul> | M |
| BP-S2 | **Dead code and prose** (BP-H12, H13, H14) | <ul><li>Each hypothesis's V written into §7.</li><li>The deletions.</li><li>BP-R4's refusal cells.</li><li>The suite and the sim corpus green.</li><li>Independent of BP-S1: may run first.</li></ul> | M |
| BP-S3 | **BP-G1** (BP-H1..H5), one commit per hypothesis | <ul><li>Per hypothesis:<ul><li>V in §7;</li><li>C with its cells and mutations, each mutation repeated;</li><li>the BP-G1 cells' A/B, reported;</li><li>a rejected hypothesis recorded with its call site.</li></ul></li><li>An arming cell for every fence or latch made conditional, and its call-site census in §7 (BP-R1).</li><li>The suite green.</li></ul> | M |
| BP-S4 | **The purge's trigger** (BP-H11) | V's rig cell first; then C, `catalog_purge_sweeps`, cells, mutation; the suite green | S |
| BP-S5 | **The memo** (BP-H6), **only if BP-S1 holds it and BP-Q4 takes (b) or (d)** | C with the descent cells and mutations; §6.1's growth and CABIN AUTO cells A/B; the sim corpus | M |
| BP-S6 | **Locks** (BP-H10), **only if BP-S1 holds it** | C; the lock contention cells A/B; the deadlock and fence suites | S |
| BP-S7 | **The ratified three, reported** (BP-H7, H8, H9) | §7 carries each number beside its ruling; BP-Q5..Q7 put to the operator; no code | S |
| BP-S8 | **The close** | <ul><li>§6.2's measurement.</li><li>**Text:**<ul><li>`statement_epoch.hpp`, `instance_visibility.hpp`, `cabin_store.hpp` and `latch.hpp` for what is armed where;</li><li>`wal.md` for the retired kinds;</li><li>`catalog.md` CT2 if BP-S5 landed;</li><li>CLAUDE.md's rows: WAL, Transactions, Cabin, Transactional DDL;</li><li>`known-gaps.md` for each rejected hypothesis whose cost stays.</li></ul></li></ul> | S |

---

## 5. Items for the operator

Marked 2026-10-12 by W2 (`raft-marks-2026-10-12.md` §13). Every proposal is `[design]` unless a cell says otherwise.

| item | question | kind | CLA's proposal | mark |
|---|---|---|---|---|
| BP-Q0 | **Open BP**, with BP-S0..S8 and BP-R0..R6 as written | process | Yes, sequenced as BP-Q13 marks | W2, as proposed (S2 first, per BP-Q13) |
| BP-Q1 | **BP-R1: a BP-G1 break is cut on proof, its measurement gates nothing** | process | As written. BP-G1 is a standing rule (ways of working: zero overhead for single-core configurations). Keeping a break because it is cheap would make the rule a threshold | W2, as proposed |
| BP-Q2 | **Re-exploration's bound.** One `BP-Hn.k` per hypothesis without a word; a second needs the operator | process | As written | W2, as proposed |
| BP-Q3 | **The page stream stamp** (BP-H5). (a) Stop writing it; `SHOW PAGE` drops it. (b) Keep it as a diagnostic | user-visible (`SHOW PAGE`) | (a). It names a stream that no longer exists. A diagnostic that answers a retired question is noise in every page dump | W2: (a) |
| BP-Q4 | **The memo** (BP-H6), only if BP-S1 holds it. (a) ~~Per-entry invalidation through a ring of recent moves~~, struck at the S0 review (§7). (b) No memoized roots: a bind reads the anchor page. (c) Keep. (d) Root moves stop bumping the word; a structural refusal refreshes that relation's root | design, **[quiet-wrong] under (a), (b) and (d)** | Decided by BP-S1's counters and BP-H6's descent census. CLA leans (d), with the index-side refresh the S0 review found missing (BP-H6), if the census shows every descent from a stale root either complete or refused: it removes the root-move bumps without adding a classifier, and a missed refresh costs a refusal, not a row. (b) adds a page access to every statement on every core, which is a cost at `cores = 1` that BP-G1 forbids unless armed | W2: decided by BP-S1; the option is adopted from the census on the standing go-ahead |
| BP-Q5 | **The checkpoint** (BP-H7), against AT-S8. (a) Keep. (b) One instance-wide run, with undo heads published beside AX's in-flight ids, in an order of its own | design, ratified, **[quiet-wrong] under (b)** | Decided by BP-S1. Recovery needs every live transaction's undo head inside the replay range, so (b) is a recovery-correctness change | W2: decided by BP-S1; the option is adopted from the census on the standing go-ahead |
| BP-Q6 | **The id window** (BP-H8), against AT-R4. Report only; any change is a later order | design, ratified | Report only | W2: report only |
| BP-Q7 | **The in-flight table** (BP-H9), against AX. (a) Keep. (b) An instance pool of slots. (c) A per-core min/max summary word in front of the scan | design, ratified, **[quiet-wrong] under (b) and (c)** | Decided by BP-S1. If material, (c) first. (b) bears on BL-Q1 (b), a transaction moving between cores, and is BL's question as much as BP's | W2: decided by BP-S1; the option is adopted from the census on the standing go-ahead |
| BP-Q8 | **The purge's trigger** (BP-H11): core 0's 50 ms `system` tick, gated on the instance mark counter | design | As written. It keeps one sweeping core, a placement and not an authority, as `command_dispatcher.cpp:6690-6696` states | W2, as proposed |
| BP-Q9 | **Retired WAL kinds** (BP-R4): refused `Corruption`, numbers reserved | design, **[quiet-wrong] if skipped** | As written | W2, as proposed |
| BP-Q10 | **Tombstones.** (a) Correct false statements only. (b) Also prune tombstones of machinery retired before AT closed, keeping one line per retired mechanism that names the commit to read. (c) Prune all | scope | (b). A one-line pointer keeps the warning and the history's address, and drops the essay | W2: (b) |
| BP-Q11 | **`kMaxWalCores`'s new name** | naming | `kMaxCores` | W2: `kMaxCores` |
| BP-Q12 | **The letter** BP, chosen by CLA | naming | BP | W2: BP |
| BP-Q13 | **Sequencing.** (a) BP opens now in its own worktree; BP-S2 first, since it needs no census; BP-S1 then gates S5 and S6. (b) BP waits for BM's close | process | (a). BP-S2 and BP-S3 touch the statement head, the mint and `wal/`; BM touches the walk. The two meet only in `command_dispatcher.cpp`, and in different functions | W2: (a) |
| BP-Q14 | **The reach of W2's citation sweep.** (a) Live documents only: `CLAUDE.md`, `overview.md`, `docs/`, `manual/`, and source comments as BP-Q10 marks; dated records under `instructions/` and `bench/` stay as written, except this order's own title and file name. (b) Also rewrite the dated records | scope | (a). A work order, a raft-marks entry and a results file are records of what was decided and measured when; rewriting them falsifies the record, and the operator's quoted words stay as said. A live document states the current contract, where a citation of the retired model is the noise W2 removes. `docs/spec/cross-owner-txn.md`, a retired spec of a retired mechanism, is deleted | adopted on the operator's standing go-ahead of 2026-10-12 (go-ahead-achieving-milestone): (a) |

---

## 6. Measurement

### 6.1 BP-S1

`[design]` Where every cell is run:
- In `build-release`, rebuilt at BP-S1's instrumented commit and named by
  `git describe --tags`. `bench/README.md`'s five rules apply.
- On one host, with server and driver pinned to separate CPUs as BA's
  census pinned them.
- Each cell on a fresh server and data file, interleaved, eight runs.

**BP-G1 cells** (`cores = 1`; reported against BP-S3's stages, gating nothing):
- the point `SELECT`;
- the point `UPDATE`;
- the every-row `UPDATE` over 10,000 rows;
- a Cabin-served `SELECT`;
- a `group` commit loop and a `strict` commit loop.

**Group B cells** (`cores = 1, 2, 4`, and 8 where the host has the CPUs):
- **Growth** (BP-H6): every core inserting into its own btree relation from
  an empty start, so levels grow throughout, while one core runs point
  `SELECT`s over a fifth relation. Reported:
  - word moves per cause;
  - the reader's p99 against the same cell with no inserting cores.
- **CABIN AUTO** (BP-H6): `cabin_optimizer = on` over scenario 4's
  shape (`tools/scenario4_cabinopt_days.py`), with the same counters.
- **Checkpoint** (BP-H7): `tools/bi_checkpoint_stall.py`'s cell. Reported:
  - runs and data syncs per period;
  - bytes written back per period;
  - the anchor's age;
  - the concurrent OLTP p99.
- **Skew** (BP-H8): `cores = 4`, sessions on three cores, the fourth idle.
  Reported:
  - the window's size over time;
  - burns;
  - `LookupCommit` per statement.
- **In-flight** (BP-H9):
  - a `PURGE` of 100,000 delete-marked rows;
  - a named-key `INSERT` loop.

  Both report `InFlight` misses and slots read per miss.
- **Locks** (BP-H10): `tools/lock_contention_benchmark.py`, plus a cell
  holding one range fence on the written relation.

**Profile.** `tools/kds_sampler.c` on the growth, checkpoint and skew
cells.

**Host.** `/proc/loadavg`, the competing-process list and `df -T`, per cell.

### 6.2 BP-S8

`[design]` The close follows the Session Workflow's step 3:
- A is BP's opening commit and B its close, interleaved, over §6.1's cells.
- The BP-G1 cells carry BP-S3's reported deltas.
- Group B cells are reported for the stages that landed. A rejected
  hypothesis's cell is run once more at B, to show that nothing moved it.

---

## 7. Row status

### BP-S0 — written 2026-10-12

Written in the main checkout on `main` at `fe248d94`, with no worktree,
as a document-only change:
- No file under `src/`, `include/` or `tests/` moved.
- No build ran and no suite ran.
- The pre-push hook is not enabled in this clone (`core.hooksPath` unset).
  A documentation-only push is the case its own text names as legitimate
  for `--no-verify` (`scripts/githooks/pre-push`).

The survey of §1 was first reported to the operator at `fe248d94` (W0).
For this order, CLA re-read every cited line at the same commit. Before
the push `origin/main` moved to `79d3148f`, a run of `CLAUDE.md`-only
commits (the operator's abbreviation table). No cited file changed, so
every `[source-read]` citation holds at `79d3148f`, read and not run.

**The tombstone count's invocation** (§1.E), run at `fe248d94` from the
repository root:

```
PAT='owner_core|2PC|ring kind|cross-owner|shipp|AT-S5|AT-S6|AT-S9|AT-S10|AM-S4\(d\)|AW-S1b|fan-in|split relation|per-core stream|kPerCoreStreams|statement shipping|participant|coordinator'
grep -rhE --include='*.cpp' --include='*.hpp' --include='*.h' "^\s*//.*($PAT)" src | wc -l       # 281
grep -rhE --include='*.hpp' --include='*.h' "^\s*//.*($PAT)" include | wc -l                     # 311
```

With it:
- `instructions/v3.0.0/index.md` gains BP's row;
- `raft-marks-2026-10-12.md` records W0 and W1 (§8-§9).

**Review before the push.** An independent agent that had not seen the
drafting checked the order against `fe248d94`:
- every `[source-read]` citation;
- the load-bearing claims without a tag;
- the `[quiet-wrong]` arguments of BP-H1..H4, H6 and H12;
- the cells against the mutations they claim to kill;
- the cross-references.

It found two blockers, seven should-fix and nine nits. **All were applied**,
and CLA re-read each cited line before applying it.
- **Blocker: the mutations of H1-H3 could not be killed.** The tree has no
  sanitizer build (`scripts/sim.sh:22-24`), and the two-core rigs the
  draft named run on the test thread. The cells are now arming cells, and
  the fence's and latches' necessity above one core is stated as argued,
  not tested (BP-R1).
- **Blocker: H6 named the wrong danger.** The draft said a stale root
  returns a wrong answer. It is refused (`btree.cpp:1185-1207`), so the
  draft's cells could only find a liveness failure. H6 now aims at the
  word's real job, a memo surviving a `CREATE INDEX` or `CREATE ASSERTION`
  (C2). It adds option (d), and H6.1 carries `[quiet-wrong]`.
- **H2's premise, found to hold in source.** At one core every window
  publish and reclaim runs on the reactor. The draft's re-exploration
  assumed a writer-thread publish that does not exist, and is withdrawn.
  `EndCommit` takes no latch.
- **H4's mechanism.** It is `PublishCoreBounds` in `Begin`, not
  `PublishInFlight`. The draft's mutation was inexpressible, and a banking
  cell could not kill a mint-side defect, so the cells are now direct.
- **H5.** It missed four test assertions on the stamp.
- **H12.** It missed `LogPageHandoff`, the handoff codec and
  `analysis.cpp:139`, and left open where the refusal happens.
- **Labels and references:**
  - `catalog.cpp:128` is DT9's `ScanAll`, not the sweep;
  - `anchor` is read by the fixture path;
  - two `§5.1` references;
  - the missing index row and raft-marks sections;
  - this review's own record.
- **Nits:**
  - `expeditor.cpp:1207` and `lock_table.cpp:379`;
  - `recovery_prepared*` absent, not zero;
  - root moves bump the word without a version;
  - `manager.cpp:296-300` moves the word too;
  - `catalog_version()`'s two test readers;
  - H10's single-core cost is the walk, not latches;
  - H11's counter;
  - the G1/G2 collision with `latch.hpp`.

After the review, CLA found BI's measured run counts (§1.B B2) and
replaced a `[design]` sentence with them.

**The `critics-developer` review, run at the opening** (2026-10-12, on
`worktree-bp-per-core-residue` at `cf889b63`, read-only): no blocker;
every load-bearing `[source-read]` claim held, and B1's lines drifted (the
two `BumpWord` sites merged into `AdoptRoot`). It confirmed H12's V by
ancestry: the prepare emitter left at AT-S6 `ac4bd646` (leftovers at
AT-S18 `854aefe3`), the handoff at AT-S9 `94b3b96e`, and both are ancestors
of BK's bump to 22, `efa6adba`. **Applied:**
- H6 (d) gains the index-side refresh and its cell; option (a) struck;
  V becomes one word-move counter per cause (should-fix 1, 2);
- H11's tick runs only on progress, with a `cores = 1` V cell (3);
- H2's latch stays armed by default, the Expeditor disarming it (4);
- H5's inventory gains the logging-core byte and four tests (5);
- §1.E corrected: `manager.hpp`'s spanning claim is true, and
  `catalog_cache.hpp` carries two more false statements (6);
- D2: `catalog_version()` stays (its test readers), and the config fields
  are D3's (7);
- H12's list names the state that dies with the arms (8; already in C);
- the nits: H3's doubled bullet, H4's mutation wording, the count's noise.

**Applied in part:** H8 keeps its report but builds no counter (BP-Q6 is
report only, so a counter here has no outlet). **Rejected:** cutting H7's
measurement, because BI counted runs but not bytes written back per run,
which is H7's premise; and citing `walk_slice_rig_test.cpp` in place of
H1's behaviour cell, which stays a `cores = 1` cell where those rigs run
two cores.

### BP-S2 — dead code and prose

Worked on `worktree-bp-per-core-residue` from `cf889b63`, first of BP's
stages per BP-Q13 (a).

**BP-H12, V** `[source-read]` at `cf889b63`: no file under `src/` appends
kind 25 or 27 - `LogPageHandoff` had no caller and `EncodeTxnPrepare` was
called only from `tests/`; `Decode` mounts version 22 alone; 22 is BK's
bump at `efa6adba` (2026-10-11), and AT-S6 `ac4bd646`, AT-S9 `94b3b96e`
and AT-S18 `854aefe3` (2026-09-26) are its ancestors. No `sim/` or
`tools/` path writes a WAL record. **Held.** **C:** analysis refuses both
kinds `Corruption`, naming the kind and "retired", before it notes a
transaction; both numbers stay assigned and named (`record.hpp`,
`wal_payload_test.cpp`'s pins). Deleted: `log_page_handoff.hpp`, both
payload codecs, `TxnOutcome::kPrepared`, `PreparedTxn`, `prepared_txns`,
the resolution phase in `RecoverCore`, the `prepared*` counts in
`AnalysisResult`, `RecoveryReport` and `MountRecovery`, the mount log's
clause, `SHOW META`'s `recovery_prepared*`, redo's two `TouchesNoPage`
arms (redo refuses a retired kind too, through the same
`IsRetiredRecordType` and `RetiredRecordRefusal`, after the S2 review
found TXN_PREPARE would otherwise pass redo as a no-page record),
`tests/prepared_recovery_test.cpp`, and the prepare and handoff cases of
`wal_analysis_test.cpp`, `wal_payload_test.cpp`, `wal_redo_test.cpp`,
`wal_recovery_test.cpp` and `mount_recovery_test.cpp`. **Cells:**
`RetiredKindTest.ARetiredKindIsCorruptionNamingIt/{TxnPrepare,PageHandoff}`
and `MountRecoveryTest.ARetiredPrepareRefusesTheMount`. **Mutation:** the
refusal turned into a skip.

**BP-H13, V** `[source-read]`: `TableAccess::AnyCabin()` has no caller -
deleted; `BroadcastShutdown`'s scheduler argument was `(void)` - removed;
`wal_anchor_count` stays written, its field comment saying no branch reads
it. **Dropped from the list by R:** `Catalog::catalog_version()` (six test
cells read it, one pinning "a root move must not drop the cache"), and
`CoreRuntime::Config`'s `cabins`, `cabin_limits`, `buffer_pool_frames` and
`anchor`, read on the fixture path (`core_runtime.cpp:186`, `:333-334`,
`:432`). **D3's fixture arms stay**: the suite builds those fixtures
through `CoreRuntime::Open` with no instance structure, and the rig's
wiring does not cover them; their comments already name them
fixture-only.

**BP-H14:** `kMaxWalCores` is `kMaxCores` (BP-Q11), its comment naming
what it bounds and its refusal saying "the most cores an instance runs";
two test names that said "anchor table" follow. Corrected:
`superblock.cpp`'s stamp sentence, `catalog_cache.hpp`'s three
statements, `manager.hpp`'s "No cross-core protocol" paragraph,
`lock_table.cpp`'s walk bound, and `expeditor.cpp`'s "each core's log is
its own" at shutdown. Every literal "shared-nothing" left the code's
comments (W2). Tombstones pruned per BP-Q10 (b).

**Review** (`critics-developer`, on the uncommitted S2 diff from
`cf889b63`): no blocker and no correctness bug; the refusal's placement,
every replay path and every switch checked. **Applied:** one
`IsRetiredRecordType` and `RetiredRecordRefusal`, used by analysis and by
redo (TXN_PREPARE would otherwise pass redo as a no-page record); the false
"the scanner hands them here" sentence; `RecoverCore`'s and `Analyze`'s
refusal lists; the `CREATE/DROP INDEX` hold's reason, which the prune had
lost; `device_page_store.hpp`'s unlatched-writeback and future-tense
claims; `kwp_session.hpp`'s "no walk parks"; `static_assert`s tying
`kContentionSlots`, `CheckpointCensus::kSlots` and `CoreLoad::kSlots` to
`kMaxCores`; the mount cell's assertion that redo never ran; the test
name lambda; the nits (a dead comment, a pointer's form, the error
prefix). **Rejected:** `WalWriter::kWaitSlots`' assert, because `wal/`
sits below `server/` and the assert would need the include the layering
forbids; reflowing eight comment lines past 100 columns, which no rule
or tool in the tree enforces.

**Sim corpus:** 190 runs, 9 failures, all fresh seed `20261012003` with
`--faults io`, and identical at a base build of `cf889b63`: not S2's.
Recorded in `docs/inflight/bugs/a-ddl-rollback-compensates-logged-catalog-pages-unlogged.md`
with its seven-op minimization. Committed seeds all pass.

**Suite:** 3604/3604 (`ctest -j8`, Debug), before and after the review's
fixes. **Mutation:** the refusal turned into a skip fails all three
retired-kind cells, three runs of three. Overhead not measured; measured
at the milestone's close.

### BP-S3 — BP-G1: what `cores = 1` no longer pays

Worked on `worktree-bp-per-core-residue` from `1592007e`. **Deviation from
the stage row:** H1, H2 and H3 land in one commit, because they share one
arming cell (`ExpeditorTest.TheSharedStructuresAreArmedAboveOneCoreOnly`)
and the `Expeditor` lines that arm them; H4 and H5 land one commit each.

**The call-site census BP-R1 asks for** `[source-read]` at `1592007e`. The
instance spawns three threads besides its reactors - the WAL writer
(`src/wal/writer.cpp`), the log preparer (`src/wal/file_log_device.cpp`)
and the data syncer (`src/storage/data_syncer.cpp`) - and none of them
reaches `StatementEpochs`, `InstanceVisibility`'s window or `CabinStore`.
Every arming site reads `config_.cores`, which `CheckCoreCount` validates
before `Open`.

**BP-H1** (`StatementEpochs`' two `seq_cst` fences). **V:** at `cores = 1`
every publish and every read run on the one reactor thread - the statement
scope, the KWP load endpoint, core 0's reclaim tick, and a test's
`ReclaimStep()` on the dispatching thread - so program order already puts
the publish before the read. **Held. C:** the fences are taken only when
`cores > 1` (`shared()`); a cell in `in_run_reclaim_test.cpp`.

**BP-H2** (the window latch). **V:** its callers - `LookupCommit`,
`BeginCommit`, `PublishCommit`, `Reclaim`, `window_size` - all run on the
reactor at `cores = 1`. A default-constructed instance stays armed:
embedded callers and rigs drive one from several threads
(`instance_visibility_test.cpp`'s reclaim race). **Held. C:**
`InstanceVisibility(bool shared = true)`; `Expeditor` passes `cores > 1`.

**BP-H3** (`CabinStore`'s partition and stats latches). **V:** the walk's
serve, the FK check's serve, the write hook and `CABIN AUTO`'s executor on
core 0's tick are its only callers, all on the reactor at `cores = 1`.
**Held. C:** `CabinStore(limits, bool shared = true)`, `Armed()` handing
`LatchGuard` a null latch when unshared; `Expeditor` passes `cores > 1`.

**BP-H4** (`MintCheckView`'s walk of `live_`). **V:** `Begin` sets
`active_` (`manager.cpp:148`) and publishes its core's bounds (`:170`)
before it returns; a commit drops `active_` and republishes (`:313-314`),
an abort likewise (`:617`, `:626`); `Release` refuses an active
transaction (`:786`). So `AnyUnresolved()` is true whenever any
transaction on this core is active, and the walk was OR'd with it: the
walk added nothing. **Held. C:** the walk deleted; a cell in
`txn_manager_test.cpp`.

**BP-H5** (the page stream stamp, BP-Q3 (a)). **V:** the stamp's one
reader was redo's diagnostic, retired by AM-S4(d); `kPageFlagsOffset` is
read only by `Read/WritePageHeader`, and `record.cpp` loads the envelope's
`flags` byte without validating it. **Held. C:** nothing writes either
byte any more - `DevicePageStore::StampPageLsn` and its suppression, the
mount's `StampSuppression`, redo's `ApplyPageInit` stamp,
`LoggingCoreOf`, the checkpoint records' and `PAGE_INIT`'s core byte,
`Get/SetPageStreamStamp`, `SHOW PAGE`'s `stream_stamp=`; the core-id guard
`CheckLoggableCoreId` and its cell, which existed only because a
checkpoint record named its core in that byte. **No superblock bump**:
neither byte has a reader in either direction - a new build ignores an old
volume's stamps and core bytes, and an old build reads 0.
`page.md` §2b and `wal.md` §3 say so.

**Review** (`critics-developer`, on the uncommitted diff): no correctness
bug; H4's comment verified against every `active_` write. **Applied:**
H5's leftover - `CheckLoggableCoreId`, its two calls and its cell deleted;
false prose in `redo.hpp` and `page_mgr.cpp`, `page.md`'s offset-2 row,
`wal.md`'s "is 0" made "written 0"; H2 collapsed onto the constructor form
the other two use (the post-construction `DisarmWindowLatch()` and its
"call once" caveat gone, `WindowLatchForTest()` returning null when
unarmed); the census sentence repeated in three headers cut to a pointer
here; `CabinStore::shared_`'s duplicated default; a dead include in
`mount_recovery.cpp`. **Rejected:** none. Also fixed before the review:
`Checkpointer::AppendEnd` still wrote the core byte (the suite's red
`APeersCheckpointNamesItsCoreInTheAnchorAndNotInTheRecord`), and a
`-Wreorder` in `CabinStore`'s initializer list.

**Suite:** 3603/3603 (`ctest -j8`, Debug) at the stack's head; the four commits were not built one by one. **Mutations** (`~/bench-runs/bp-s1/mut_s3.sh`, each built and run three times): H1 (fences forced off), H2 (window latch forced off), H3 (Cabin latches forced off), H4 (`Begin`'s publication removed) and H5 (redo restamping a formatted page) each killed 3/3.
**BP-G1 cells' A/B:** reported at the close per BP-R6 (BP-R1: it gates
nothing). Overhead not measured; measured at the milestone's close.
