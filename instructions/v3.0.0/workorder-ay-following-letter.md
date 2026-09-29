# Work order AY — the following letter: D9(a), D7's last gate, and what AT and AX left

Opened 2026-09-29 on `worktree-ay-s0-order` from `58198cb`
(`v2.7.0-481-g58198cb`), on the operator's *"start AY-S0"*, after the
letter was opened as AY (`raft-marks-2026-09-29.md` §7). It is AT-0
item 6's letter (`workorder-at-m3-uniformity.md`): M3's D7/FK half, which
AT-9 carried, and the items the 2026-09-29 marks placed beside it. The
v3.0.0 tag waits on AR0 §8's chain through M4 (`raft-marks-2026-09-26.md`
§4, Q3), so AY cuts no tag.

## 0. The marks this order serves

Every item below is marked; this order proposes how to build them, and §4
lists what the marks do not decide.

| mark | what it asks of AY | recorded (`raft-marks-2026-09-29.md` unless named) |
|---|---|---|
| **D9(a)**, ratified as a design | the child's forward check takes `IS` on the parent relation and `S` on the parent tuple **at the hoist, held to its decide**; no escalation, no persisted bit; the cross-core wait AX's | `raft-marks-2026-09-28.md` §6, AR0-M4 |
| **D8 as revised** | write skew closed by named units - the assertion group by the registry's reservation, the FK parent row by D9(a)'s `S`; no gap locks | `raft-marks-2026-09-28.md` §5 |
| **AR0 D7** | `RefuseAuxiliaryOnSplitRelation` lifted here, **not before its lock protection exists** - the assertion's since AT-S5d, the FK's D9(a) - each of index, assertion and FK admitted only once a cell shows its build and maintenance cover every chain of a relation split before AT-S9 | §3, AR0-M8 |
| **AR2 E3** | retires once D9(a)'s own two-core cells pass: a parent `DELETE` on one core against a child write on another | §4 |
| **The FK Cabin's "no children" answer** | restored as its own stage beside D9(a), two-core cells first, and a mutation showing the walk fallback is what the cells catch | §6 |
| **`AllocateCatalogPage`** | fixed as its own stage | §8 |
| **The chunked assertion snapshot** | fixed by a chunk count carried in the snapshot; `assertion.md` §7 changes | §9 |
| **R8.3's lost lock-family cells** | AY's first stage, ahead of D9(a); AT-S14's SQL-level cells in the stage that touches `fk_check` - **AY-S4** | §12 |
| **The containment wake** | a write refused at a declared range or a range fence parks on a slot the release flips | §12 |
| **B6** | `NoteBlockingWriter` records the block whenever the refusing unit's wake names the holder, even once it has left the in-flight table; the repeatable-read guard ahead of it (placed here by CLA) | §13 |
| **AO-0 items 9, 22, 25, 27** | decided; 9 is D9(a) itself, the others need nothing built | §10 |

**Open under these marks**: whether AR1's AQ and AR are AY's stages or
their own letters (§7, *Does not settle*), and whether E10 - AY's by §11 -
retires. AY-Q10, AY-Q11.

## 1. Survey at `58198cb`

Three read-only surveys, one per area, every claim cited to a line at
`58198cb` (identical at `2b0a744` in every `src/` and `tests/` file cited;
the two differ in KWP-session files and docs only). Each stage in §3 names
its cells.

### 1.1 The lock family

- **R8.3's cells need one core and a reactor, nothing of 2PC.** The five
  fixtures (`git show ac4bd64^:tests/txn_2pc_protocol_test.cpp`) derive from
  `Txn2pcBlockedWriterTest` over `Txn2pcParticipantTest`, and of its
  machinery only `ShippedStatementExecutor` (constructed, never used by
  these cells) and `set_in_doubt_ceiling_ns` are gone. **53 cells**
  (`known-gaps.md`, Testing, inventories them). **Seven more were lost and
  are in no inventory** - `Txn2pcBlockedWriterTest`'s own non-2PC cells
  (old `:3833-4122`) - and they ran with **no lock table** (`LockCap()`
  is `nullopt`, old `:73`), an arm no production assembly builds
  (`expeditor.cpp:1017`, `core_runtime.cpp:353` always call `set_locks`);
  two have table-backed successors (`fk_cross_core_rig_test.cpp:237`,
  `:263`). R8.3's mutant M1 (`step_compiler.cpp:1375`, the bind declaring
  nothing) is still there and the restored join/subquery cells kill it; M2
  (`NoteWaitFor`) is killed since AX-S3.
- **The containment wake is missing for a structural reason.** A
  cross-unit conflict is found in the verify arm (`lock_table.cpp:322-366`)
  on another key's entry, in another partition; `WakeWaiters` flips only
  the released key's own waiters, and the scans register nothing, so the
  ask returns with no slot. It was promised to AO-S6c as "C3" and not built
  (`workorder-ao-m2-lock-family.md`). The fix is a registration on the
  **blocking** entry, inside the scan under the partition latch that saw the
  holder, and an `AcquireResult` that carries that entry's key. The release
  side needs nothing. Sites that poll today: the `UPDATE`/`DELETE` declared
  range, the sorted fill, an `INSERT` or per-row write under a fence - and
  **the FK forward check's tuple `S` under a parent's range fence does not
  wait at all** (`WaitForParentRowWriter` returns on a null wake).
- **B6 is one condition.** `NoteBlockingWriter`'s `!IsInFlight(trx)` early
  return (`command_dispatcher.cpp:8088`) comes after `blocking_wake_` is set
  at every refused tuple or range ask; bypassing it when that wake names
  `trx` falls through to the no-table and repeatable-read guards. The window
  is not observable on one core in production (`Commit` retires and
  releases with no suspension between); the cell is `LateRelease`'s
  reordered (`row_wait_wake_rig_test.cpp`).

### 1.2 Foreign keys

- **Nothing is held between the check and the write.** The hoist
  (`ResolveForeignKeyParents`, `command_dispatcher.cpp:3323-3406`)
  deduplicates parents and descends each through the held leaf; only a
  busy verdict asks the lock table (`WaitForParentRowWriter`, tuple `S`,
  released at once if granted, no `IS`, the cap deliberately ignored at
  `:3462-3467`). D9(a)'s `IS` and `S` belong in `scope.txn->borrows()`,
  released at commit and abort; `BorrowChain` hard-codes `IX`/`X` and
  needs an `IS`/`S` arm.
- **The parent `DELETE`'s side is already shaped for it.** A point `DELETE`
  takes the tuple `X` per row before its conflict and reverse checks, and a
  refusal naming a blocker that is not the row's writer is a wait worth
  taking at every level - it parks mid-walk with a wait-for edge. A
  multi-key `DELETE` takes a range `X`, which meets a tuple `S` only in the
  verify arm - **so D9(a)'s two directions both need the containment wake**.
- **What the mark does not say**, cited here and put as AY-Q1..Q3: the
  tuple `S` against every parent `UPDATE`'s `X`; the order of `S` and the
  descent; the self-referencing arm, not hoisted (`:3351`) and checked per
  row with no `S` (`:3507-3517`).
- **The Cabin's clearing return needs D9(a) first.** A set's entry count is
  fixed at `Find`, and the reverse check reads latest state, so a child
  appended after `Find` leaves a drained set answering "no children"; and
  there is a gap between the row's placement and its hook. Both close only
  if every path that makes a row reference P holds `S(P)`. The existing
  `ADrainedCabinSet...` cell (`fk_cross_core_rig_test.cpp:298-330`) has not
  reached the drained path since AT-S7. The return itself restores the arm
  `f247c52` removed.
- **AT-S14's SQL-level cells fit one dispatcher over `DivideOnRefetchStore`**
  (`btree_test.cpp:1204-1236`); the two-core rig cannot take a forwarding
  store. **E3's cells fit the existing rig** (`fk_cross_core_rig_test.cpp`),
  the window itself driven through `SetBeforeInsertLogForTest` on a
  two-row `INSERT` - no new seam.
- **Record corrections**: `FkPendingDeleteTable` went at AT-S5f
  (`f247c52`) and AT-0 item 6 carried it (corrected with this order);
  `foreign-keys.md` says the reverse check's view is writerless, and the
  code mints it with the deleter's id (`:8845`, `:3292`) - AY-S5's to
  correct.

### 1.3 D7's gate, the catalog page, the assertion snapshot

- **Of the three auxiliaries D7's gate refuses, only the assertion has a
  build that misses a chain.** A split relation is always heap (CC8). An
  **index** on a heap relation is refused by IX3, the check after the gate
  (`catalog.cpp:3553`, `:3559`), so lifting the index arm changes the answer
  from `NotImplemented` to `InvalidArgument`. An **FK** is declared only at
  `CREATE TABLE` on an empty child and its parent must be btree (F1); the
  **child** arm is reachable through the catalog door and its reverse walk
  already covers every chain (`foreign_key_test.cpp:702-735`, `SplitChild`).
  **An assertion's build walks `desc_page_id` alone**
  (`assertion_build.cpp:186-196`) - on a split heap relation, the first
  chain - and would under-count and admit what it forbids: SB3's Cabin
  defect in the same shape (`71f92f6`). Its maintenance is per row and
  chain-blind. No test pins the gate's refusal.
- **SB3's own cell has probably pinned nothing since SUS-1**
  (`cabin_optimizer_exec_test.cpp:185`): it creates its relation without
  `HEAP`, so its rows go to a btree. Read, not run.
- **`AllocateCatalogPage` is an unlatched write, not a use-after-free in a
  production assembly.** The new overflow page's `PageRef` dies at the
  function's return and the rows are written through the span
  (`catalog.cpp:355-367`, `:423-462`); every catalog page is under
  `SetResidentLimit(kFirstUserPageId)`, never swept, so the frame is not
  reused - but the exclusive latch is gone, and another core's writeback can
  copy a `page_lsn` 0 image ahead of its `PAGE_INIT`. AT-S21's shape fixes it.
- **The chunked snapshot has two defects beside the entry's.** Recovery
  closes every open base at the first non-snapshot record
  (`assertion_recover.cpp:230-236`); it also closes a crash-torn run at
  scan end as a base (`:292-296`, a second quiet under-count), and an
  `OutOfSpace` mid-run leaves a partial run in a live log
  (`checkpointer.cpp:55-62`). `AssertSnapshotPayload` has a `u32 reserved`,
  written 0 and never checked - room for the count with no format bump.
- **E10 has no subject.** `range_eligible` went at AT-S9 (`94b3b96`),
  nothing splits, and a pre-AT split relation's schema cannot become
  spillable (ALTER is catalog-only).

## 2. Rulings - CLA's proposals under the marks

**AY-R1 - One-core fixtures for R8.3.** The 53 cells return on a new
one-core fixture copying the old base without its two retired members;
what the seven add is AY-Q9's. One direct `NoteWaitFor` cycle cell joins
`lock_table_test.cpp`. Comments naming the 11 s net or a ship are
rewritten, not carried; `fk_cross_core_rig_test.cpp:26`'s citation of
`Txn2pcBlockedWriterTest` as a live pin is corrected.

**AY-R2 - The containment wake registers on the blocking entry.** Inside
the verify scan, under the partition latch that observed the holder, a
waiter is pushed on that entry, and `AcquireResult` carries the entry's
key, which `WriteBlock::key` and `DropWake` then use. A registration is
either before the holder's removal (flipped) or after it (never saw the
holder) - the in-latch arm's rule, one latch at a time (AO-R2). Mutual
refusal of two overlapping asks stays a deadlock victim, pinned by a cell.

**AY-R3 - B6 as marked**: the early return is bypassed when
`blocking_wake_` names `trx`, and nothing else moves.

**AY-R4 - D9(a) in this order**: `IS` on the parent relation, then `S` on
the parent tuple, **then** the descent (AY-Q2); both into the transaction's
borrows, counted against the cap and refused `ResourceExhausted` past it -
the mark's "no escalation" under AO-R10, reversing the cap-ignoring at
`command_dispatcher.cpp:3462-3467`; `WaitForParentRowWriter` folds into the
ask - a refused `S` is the wait. The self-referencing arm takes the same
pair per row (AY-Q3).

**AY-R5 - The clearing return behind D9(a)**: returned only when the set's
loop ran to its end and no hint failed, with `SHOW ACCESS` recording a
Cabin probe for it - the arm `f247c52` removed, restored.

**AY-R6 - `AllocateCatalogPage` returns its `PageRef`**, held in
`InsertRow` until after `LogCatInsert`'s stamp, AT-S21's shape; the bug
entry's severity restated.

**AY-R7 - The chunk count in the reserved word**: `chunk_index:u16 |
chunk_count:u16`, so index 0 opens a run and a partial run is discarded; a
snapshot needing more than 65,535 chunks is refused `OutOfSpace`, as
`LogAssertionSnapshot` refuses a too-large group; a pre-AY snapshot is
AY-Q6's and a run torn at scan end AY-Q7's.

**AY-R8 - D7's lift in two stages**: the assertion's build walks
`WalkHeads()` and both its gates go, behind cells with rows in both chains
(AY-S9); the index arm and the FK arms go in AY-S10, and with its last
caller `RefuseAuxiliaryOnSplitRelation` is deleted.

## 3. Stages

Each stage waits for the operator's word, as AT's did.

| stage | what | exit | size |
|---|---|---|---|
| AY-S0 | This order; the index row | the files at the commit | S |
| AY-S1 | **R8.3's cells restored** (AY-R1): tests only | the 53 cells green (and what AY-Q9 marks); M1 killed by a restored cell; one `NoteWaitFor` unit cell | M |
| AY-S2 | **The containment wake** (AY-R2) | a declared-range `UPDATE` on core 1 against core 0's open row write proceeds at the release kick (red first); the FK `S` under a parent's fence waits rather than refusing; mutual refusal pinned; mutation: no registration in the scan | M |
| AY-S3 | **B6** (AY-R3) | `LateRelease` reordered - the holder decided, its `X` held - parks and ends `UPDATED 1` (red first); the repeatable-read refusal unchanged; mutation: the early return restored | S |
| AY-S4 | **The FK cells, red first**: E3's (i)-(v) - a child's open `S` parks a parent `DELETE`, the window through `SetBeforeInsertLogForTest`, a range `DELETE` against the `S`, self-reference, the `S`/`S` upgrade deadlock - AY-Q8's parent `DELETE` against a child writer holding no `S(P)`, and AT-S14's SQL-level cells for `step_vm` and `fk_check` over `DivideOnRefetchStore` | each cell's red named at the commit; AT-S14's cells green (their window is closed) with a mutant re-fetching inside `BtreeLookup` | S |
| AY-S5 | **D9(a)** (AY-R4) | S4's cells green; mutants - `S` taken after the descent, `S` released at statement end - each killed by the window cell; E3 retired in AR2's row; `foreign-keys.md` §3a, its writerless-view sentence and `known-gaps.md` (Foreign keys) corrected; the costs of AY-Q1 stated | L |
| AY-S6 | **The Cabin's clearing return** (AY-R5) | three two-core cells: a drained set with a new child open on the other core, a set banked while a child insert is open, a skipped hook; mutation: the walk fallback removed (an exhausted set clears unconditionally), killed by the cells; the stale drained-set cell rewritten | M |
| AY-S7 | **`AllocateCatalogPage`** (AY-R6) | red first: a cell through the undo-hook seam in which another core's flush of the new page waits for its record | S |
| AY-S8 | **The chunk count** (AY-R7) | a foreign record between chunks, a pre-AY stream, a torn tail and a partial run each read as marked; the "no continuation flag" comments (`payload.hpp:805-810`, `checkpointer.cpp:37-39`) rewritten; `assertion.md` §7 | M |
| AY-S9 | **D7: the assertion** (AY-R8) | `HEAP` cells: rows in both chains at build, an upper-range write past the bound refused; both gates gone | M |
| AY-S10 | **D7: index and FK**; `RefuseAuxiliaryOnSplitRelation` deleted; the stale text - `catalog.hpp`, `cabin.md`, `cabin_optimizer_exec.cpp`; SB3's cell given `HEAP` | an FK on a split child through `SplitChild` with a row in each chain; cells pinning IX3 and F1 as the answers on a split relation | S |
| AY-S11 | **AY's close** | a row per stage; what AY carries | S |

## 4. Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| AY-Q1 | **What D9(a)'s tuple `S` costs**: it blocks every `UPDATE` of the parent row while a child writer is open, not only its `DELETE`; two transactions that each write a child of P and then update P deadlock and one is refused (today they serialise) - "insert a trade, then update its account" is that shape; a transaction referencing more than `max_locks_per_txn` distinct parents is refused (no escalation, AO-R10) | user-visible; a cost | build D9(a) as ratified and state all three in `foreign-keys.md`; an *existence* unit only `DELETE` takes is an AR2 unit change, put forward as its own item if the cost is measured to matter |
| AY-Q2 | **`S` before the descent** - the mark says "at the hoist" and not the order | `[quiet-wrong]` if reversed | `S` first, then the descent: after a passing check, a whole `DELETE` fits before the grant |
| AY-Q3 | **The self-referencing arm** is not hoisted | `[quiet-wrong]` if skipped | `IS` + `S` in the per-row arm, the wait recorded as the insert's own tuple borrow records its |
| AY-Q4 | **DDL on the parent waits for open child writers** - `CREATE`/`DROP INDEX`, `CREATE ASSERTION` meet the held `IS` | user-visible | accept and state it; a steady stream can refuse the DDL at the 1 s net, as AO-0 item 25 accepts for `DROP TABLE` |
| AY-Q5 | **D7's cell condition for the index and the FK parent cannot be met literally** - no SQL reaches either on a split relation; the FK child arm can be, and is AY-S10's cell | spec | lift both behind cells pinning the refusal that answers instead - IX3 (`NotImplemented` becomes `InvalidArgument`) and F1; if marked, AY-S10 need not follow AY-S5, the FK child's reverse walk being chain-complete already; an auxiliary that fails its cell stays refused and does not block the letter (§3 of the 2026-09-29 marks, *Does not settle*) |
| AY-Q6 | **A pre-AY snapshot** has no count | `[quiet-wrong]` either way it is read wrong | `reserved == 0` is pre-AY, read by today's rule - which keeps today's two under-counts for the first mount of an old binary's log; the alternative, not a base, fails closed and leaves every such assertion unenforcing until its next checkpoint |
| AY-Q7 | **A snapshot run torn at scan end** is a base today | `[quiet-wrong]` today | not a base: the assertion comes up unenforcing - fails closed, a behaviour change |
| AY-Q8 | **A parent `DELETE` meeting a child row whose writer holds no `S(P)`** - a child `DELETE`, or an `UPDATE` moving the fk column - is refused, not waited | user-visible | carry the child's transaction id out of the check and park mid-walk, inside AY-S5, its cell in AY-S4 |
| AY-Q9 | **The seven uninventoried cells** ran with no lock table, an arm no production assembly builds | scope | port only the premises that still hold onto AY-S1's table fixture; two have table-backed successors already. **Marked as proposed 2026-09-29** (`raft-marks-2026-09-29.md` §17); built with AY-S3, §6 |
| AY-Q10 | **E10** - put back by the 2026-09-29 marks (§11) | spec | retire it: nothing splits since AT-S9 and a pre-AT split relation's schema cannot change |
| AY-Q11 | **AR1's AQ/AR** - AT-0 item 6 lists them; AR1 §14 names them letters behind AP | scope | their own letters, after AP's order is settled |
| AY-Q12 | **A zero-row `UPDATE`** takes the `S` at the hoist | cost | accept |

## 5. Sequencing

**S1 → S2 → S3 → S4 → S5 → S6**, in that order: S1 pins the detector and
the cap before `S` waits add edges (the 2026-09-29 marks, §12); S2 before
D9(a), because D9(a)'s `S` meets a parent `DELETE`'s declared range in the
verify arm in both directions; S3 after S2 so its cells can cover a retired
fence holder; S4's cells before S5 so the fence lands against red; S6 after
S5, because the clearing return is sound only once every referencing path
holds `S(P)`. **S7 and S8 are independent** of that chain and of each
other; **S9** of everything but S0; **S10 follows S5** unless AY-Q5 is
marked as proposed (D7's rule: nothing removed before its lock protection
exists). S11 last.

## 6. Row status

### AY-S0 — written 2026-09-29

On `worktree-ay-s0-order` from `58198cb`, as this file and its index row,
after the second batch's review was applied (`14cfdfa`). Three read-only
surveys (lock family; foreign keys; D7's gate, the catalog page and the
assertion snapshot) were run against `58198cb`/`2b0a744` and are
summarised in §1. No code, no spec edit and no test (the review's spec
edits are `14cfdfa`'s).

**The review** (`critics-developer`, one pass) found §0 and §5 misstating
three marks - AQ/AR and E10 called "not AY's", D7's lock-protection rule
dropped with the FK arm untied from D9(a), S6's mutation left out - AY-Q1
missing the upgrade deadlock and the cap refusal, AY-Q6 untagged, one line
cite wrong, stage exits without their mutants, and the seven cells' lack of
a lock table unstated. All taken, with its cuts.

**Suite**: 3019/3019 in Debug on two full runs (one pre-existing disabled
cell); a first run failed
`RowWaitWakeRigTest.AWriterParkedOnAnotherCoresRowProceedsAtTheReleaseKick`
with its message not captured, and the cell then passed 20/20 alone, 40/40
across eight parallel copies and 40/40 under a concurrent full suite
(`known-gaps.md`, Testing). Overhead not measured.

### AY-S1 — built 2026-09-29

On `worktree-ay-s1-lock-family-cells` from `69a1f76`, on the operator's
*"start AY milestone"*, read as AY-S1's word (`raft-marks-2026-09-29.md`
§15). Tests only; no source file changes.

**The 53 cells** are back in `tests/lock_family_test.cpp` on
`LockFamilyTest`, the old participant base without the shipped-statement
executor and the in-doubt ceiling: one core, a WAL on core 0, a scheduler,
and one lock table wired into both the manager and the dispatcher. All 53
passed unchanged; the ports are comment-only, and each comment naming the
11 s net, a ship or a remote step is rewritten. Beside them are three new cells:
`ADeleteDeclaresItsRelationAtResolveToo`, because `DeleteInner`'s resolve
borrow had no cell; `ARowWaitAtTheFaultNetIsRefusedRetryablyAndNamesTheNet`,
the premise of an uncounted 2PC-shaped cell that no cell pinned (the review's
find); and `LockTableTest.AnEdgeThatClosesACycleIsRefusedAndRecordsNothing`,
the direct `NoteWaitFor` cell. The citations in `fk_cross_core_rig_test.cpp`,
`read_borrow_rig_test.cpp` and `row_wait_wake_rig_test.cpp` are corrected.
AY-Q9's seven are not ported.

**Mutants**, each run against the file's cells:

- **M1** (the bind's `declare->Position` skipped) is killed by the join and
  subquery cells. The single-relation read cell does not kill it, because
  the walk reports its own first position, and its comment now says so.
- **`NoteWaitFor` never finding a cycle** is killed by six cells.
- **Each write verb's resolve `ReadBorrow` given no table** is killed by that
  verb's own cell: INSERT, UPDATE, and DELETE.
- **The net's wording changed** is killed by the fault-net cell.

**The review** (`critics-developer`, one pass) found no defect in the port.
Taken:

- a false comment in the new unit cell, which claimed a victim's stray edge
  is never cleared;
- the M1 comment, which still left the DELETE cell out;
- the read cell's comment, which overclaimed what it pins;
- three wording fixes and an indentation fix;
- `RunAsync` and `Start`/`Pump`, which were duplicated, merged into the base;
- the lost-cell range, now `:3833-4156`;
- six uncounted 2PC-shaped cells, now named in `known-gaps.md` with the
  fault-net cell above.

Rejected, as outside the stage: `ParentWriter`'s stale doc in
`fk_cross_core_rig_test.cpp`, which describes a child-relation fence that no
remaining cell uses. It belongs to AY-S4, the stage that writes the FK cells.

**Suite**: 3074/3074 in Debug (`ctest -LE heap-suspended -j8`) at
`ffdcfb1`, and 3075/3075 at `ac6baae`, after the review's fault-net cell;
`origin/main` had not moved, so there was nothing to merge. Overhead not
measured.

### AY-S2 — built 2026-09-29

On `worktree-ay-s2-containment-wake` from `d0e39e2`, on the operator's
*"merge and push, then start AY-S2"* (`raft-marks-2026-09-29.md` §16).

**Red first**, at `a3db595` against `d0e39e2`:
`RowWaitWakeRigTest.ADeclaredRangeOnCore1ProceedsAtTheReleaseKickOfARowCore0Holds`
- core 1's `WHERE id >= 1` meets core 0's row `X` in the verify arm; there was
no registration on the row, and no kick to core 1 at the commit - and
`LockDeadlockTest.AChildWhoseParentIsUnderARangeFenceWaitsForTheFence` - the
child's tuple `S` under a range `DELETE`'s fence, answered at once with the
busy verdict.

**Built** (AY-R2), at `7d90ca7`. `FenceCoversKey` and `ConflictingOverlap`,
given a `LockWake`, push the waiter onto the entry whose holder refused the
ask, under the latch that observed that holder. `TryAcquire`'s wake is a
`LockWake{key, slot}`, and so is `AcquireResult::wake`, so no caller can drop a
registration on the asked key when the blocking one differs. The four
dispatcher sites take the key from it. Mutual refusal is pinned at the table
through `SetVerifyHookForTest`: both asks are refused, each is woken by the
other's unwind, and the second edge closes a cycle. The release side needed
nothing, as the order said.

**Mutant**: with the scans registering nothing, both red cells fail again and
three table cells fail. The FK cell crashed on a null slot until the site
tested the slot. The threaded cell passed 200/200 and the rig cell 10/10.

**The review** (`critics-developer`, one pass) found no lost wakeup and no
leak. It traced the scan against the release, the holder's own unwind, a shared
`S` fence, `DropWake` by slot, and every drop site. Taken:

- `AcquireResult`'s `slot`/`slot_key` folded into one `LockWake`;
- the same-key arm's push folded into `RegisterWake`;
- two false comments in `lock_table.hpp`, "no slot is handed back" and the
  mutual-refusal claim that ignored repeated autocommit rounds;
- the `LockWait` doc;
- the hook's threading contract stated;
- a bounded wait in the threaded cell, so a regression fails rather than
  hangs;
- the closed gap deleted from `known-gaps.md` (Locks) rather than struck
  through.

Rejected: removing the FK site's null-slot test as dead. Under the mutant that
test turned a crash into the pre-AY-S2 refusal, and its comment now says so.

**Suite**: 3080/3080 in Debug (`ctest -LE heap-suspended -j8`) at
`7d90ca7`. At `33b9433`, after the review, one run failed
`ExpeditorTest.AtOneCoreTheDispatcherHoldsTheInstancesLockTable` at
`Start()`. That path is untouched here; the cell did not reproduce alone or
in 40 parallel runs, and it is recorded in `known-gaps.md` (Testing). The
next full run was 3080/3080. Overhead not measured.

### AY-Q9 and AY-S3 — built 2026-09-29

On `worktree-ay-s3-b6-and-q9-cells` from `e187b2b`, on the operator's
*"follow CLA proposal for AY-Q9, AY-S3"* (`raft-marks-2026-09-29.md` §17).

**AY-Q9, as proposed** (`0fdb0e5`). Four of the seven cells from the old base
fixture have premises that still hold with a lock table, and are ported onto
`LockDeadlockTest`:

- `ARepeatableReadWriterIsRefusedRatherThanOfferedANarrowerWait`
- `ARepeatableReadChildWaitsOutItsParentBecauseTheCheckViewIsFresh`
- `AnAutocommitWriterWaitsOutALocalHolderAndThenSeesItsValue`
- `AWriterWaitingOutAHolderThatRollsBackWritesOverThePriorVersion`

The other three are not ported:

- `ATransactionThatAlreadyWroteIsRefusedRatherThanWaited` has the same shape
  as `WithoutATableTheNarrowGuardIsWhatKeepsTheStageSafe`.
- The FK child's commit and rollback cells are covered by the two-core rig's
  first two cells.

One of AY-S1's six uncounted cells, `ThePathThatCannotWaitPoisonsExactlyAsItAlwaysDid`,
had no surviving cell and is ported too.

Mutants, each killed by its cell:

- `NoteBlockingWriter`'s repeatable-read guard removed;
- a level test added to `WaitForParentRowWriter`'s table path, which does not
  pass through that guard (the old cell's `kFutile` mutant named the no-table
  arm);
- `NoteBlockingWriter` recording without `may_park_`.

**AY-S3, B6** (AY-R3). Red first at `27ee3e3` against `0fdb0e5`, with the
`LateRelease` cell reordered: the holder commits with its `X` still held
before core 1's UPDATE first meets the row, and the UPDATE was answered
`TXN_CONFLICT … held by transaction 4`. At `b201ed3`, `NoteBlockingWriter`'s
`!IsInFlight` return is bypassed when `blocking_wake_` names the holder, and
nothing else moves. The mutant "early return restored" is the code at
`0fdb0e5`, where the cell was red.

**The review** (`critics-developer`, one pass) found B6 correct:

- a leftover wake is impossible, since `blocking_wake_` lives one statement;
- a holder released before the ask means the ask was granted, with nothing
  registered;
- an edge to a decided holder closes no cycle;
- the repeatable-read guard is unchanged beneath the bypass.

It also found the Q9 decisions sound. **One real finding**: B6 made AX-S2b's
mutant (a), a slot wait that polls `IsInFlight`, survive. That mutant's re-run
inside the window used to be refused; it now waits again on a predicate already
true, spins through the window, and still ends `UPDATED 1`. The sibling cell
now asserts that core 1 sleeps inside the window, and the mutant is killed 3/3
by both late-release cells.

Also taken:

- stale comments in the dispatcher, its header, the RR comment and
  `txn.md` §5's new sentence, which now excepts the repeatable-read refusal;
- the B6 cell's early `ASSERT` made an `EXPECT`;
- the late-release setup folded into `HoldRowLate`;
- the Testing entry in `known-gaps.md` deleted, since it held only closed
  work.

Rejected:

- merging the commit and rollback cells: they keep their original names, one
  per decide;
- a fixture helper for the five-line row setup: the file's cells read
  standalone.

The sibling rig cells' early-`ASSERT` teardown hazard, which predates this
stage, is left as it is.

**Suite**: 3092/3092 in Debug (`ctest -LE heap-suspended -j8`) at `b201ed3`
and at `58e78c7` after the review. `origin/main` had not moved, so there was
nothing to merge. The rig file passed 20 repeats alone and 8 × 5 in parallel.
Overhead not measured.

### AY-S7 — built 2026-09-29

On `worktree-ay-s7-allocate-catalog-page` from `267f955`, on the operator's
*"go ahead for AY-S7 (allocateCatalogPage)"* (`raft-marks-2026-09-29.md`
§18). S7 is independent of the S1-S6 chain (§5).

**The severity, restated (AY-R6).** The bug entry called the new page a
use-after-free. In a production assembly it is not one: every catalog page
lies under `SetResidentLimit(kFirstUserPageId)` and is never swept, so the
frame is not reused. It is an unlatched write and a flush-before-log.

**Red first**, at `09b2924` against `267f955`:
`InsertLogCrashRigTest.AnotherCoresFlushOfANewCatalogPageWaitsForItsRecord`.
The cell grows a catalog chain onto `kCatalogOverflowFirst` through direct
`CreateTable` calls. From inside the DDL undo hook, in the gap between the
row's placement and its `HEAP_INSERT`, core 1 calls `WriteBack` on the new
page. Unfixed, that writeback finished inside `kGive` and the snapshot file
held the page with its row under `page_lsn` 0. It failed 3/3.

The first version of the cell asserted the file's `page_lsn` equal to the
page's final one. That is wrong: the same `CREATE` goes on writing
`sys.columns` rows onto the page, so it was corrected to `!= 0` before the
fix landed, and the red was re-proved against `267f955`'s `catalog.cpp`.

**Built** at `68d8b18`. `AllocateCatalogPage` returns the new page's
`PageRef`, taken exclusive by `CreateAt`. `InsertRow`'s new-page arm formats,
logs `PAGE_INIT`, places the row, fires the hook and stamps under that hold,
then releases it before the tail's link edit. This is AT-S21's shape. The
cell passed 20/20. The mutant, the span return, is `267f955`'s code, where
the cell was red. The row leaves `records-appended-after-their-page-is-released.md`.

**The review** (`critics-developer`, one pass) found no correctness defect:

- the production hook's undo append takes only undo pages, the WAL and
  `StampPageLsn`, and never waits on a writeback, so the hold adds one more
  pair of the (old tail, undo page) kind already held across the hook;
- every early return drops the page by RAII, leaving what it left before;
- releasing before the link edit is right.

Taken:

- a guard that the page is unallocated before the loop, since a tail-arm
  event on it would pass with or without the fix;
- `<array>`;
- the file header, which said every cell uses the dispatcher's seam and
  mounts;
- the probe loop's two checks folded into one.

Rejected:

- **Folding the two arms' place-hook-log sequence into one helper.** It fixes
  a separate pre-existing defect: the tail arm sets `*where` only after its
  log succeeds, so a failed hook or append leaves a placed row that the
  rollback never retires. That is a behaviour change with no cell, outside
  this stage. It is recorded as
  `a-catalog-row-placed-on-a-chains-tail-is-not-reported-when-its-logging-fails.md`.
- **Dropping the tail re-fetch as unneeded.** `heap_chain.cpp` rests on the
  same premise, and the two should change together.
- **A shared two-column schema helper.** The file's cells read standalone.

**Suite**: 3093/3093 in Debug (`ctest -LE heap-suspended -j8`, one
pre-existing disabled cell) at `68d8b18`, and again with the review's changes
applied. The rig file passed 5 repeats after the review. Overhead not
measured.
