# Work order AZ — what AY carried: seven items, one letter

Written 2026-09-30 on `worktree-az-ay-carry-forward-order` from `a59da9c`
(`v2.7.0-533-ga59da9c`), on the operator's *"미해결 항목 7건을 모두
커버하는 새로운 작업 지시서를 작성해줘"*: a work order covering every item
AY's close carried forward (`workorder-ay-following-letter.md` §7, *What AY
carries forward*). **Opened as AZ on 2026-09-30, with every §4 item marked
as proposed** (`raft-marks-2026-09-30.md` §16). Each stage still waits for
its own word. It cuts no tag; the v3.0.0 tag waits on AR0 §8's chain
through M4 (`raft-marks-2026-09-26.md` §4).

## 0. The items this order serves

Numbered as AY §7 numbers them. "Covered" means differently per item, and
the column says how, as §16 of the 2026-09-30 marks left it: five have a
stage, each waiting for its own word; one is accepted as priced; and one
cannot be built here at all.

| AY §7 | item | owner at AY's close | covered here by |
|---|---|---|---|
| 1 | A peer's `CREATE ASSERTION` can miss every snapshot in the mount's scan (`docs/inflight/bugs/a-peers-create-assertion-can-miss-every-snapshot-in-the-mounts-scan.md`) | none | AZ-S2 (AZ-Q1 marked adopt first) |
| 2 | A catalog row placed on a chain's tail is not reported when its logging fails (`docs/inflight/bugs/a-catalog-row-placed-on-a-chains-tail-is-not-reported-when-its-logging-fails.md`) | none | AZ-S1 |
| 3 | A failed child check inside an explicit transaction keeps its `S` on the absent parent key until the rollback (`known-gaps.md`, Foreign keys) | the operator | AZ-S5 (AZ-Q3 marked a release) |
| 4 | A large transaction's decide is quadratic in its borrows, and D9(a) doubles them (`known-gaps.md`, Foreign keys) | none | AZ-Q4, accepted as priced; AZ-S6 struck |
| 5 | AR1's AQ and AR | their own letters, after AP's order is settled (AY-Q11) | AZ-Q5 (AP first) - **no stage**, see below |
| 6 | Two one-off cell failures under `-j8` that did not reproduce (`known-gaps.md`, Testing) | none | AZ-S4 |
| 7 | A cabin needing more than 65,535 snapshot chunks is refused at every checkpoint | none | AZ-S3 (AZ-Q2 marked refusal at admission) |

**Item 5 is not built by this order.** AY-Q11 is marked (`raft-marks-2026-09-30.md`
§15): AQ and AR are their own letters, after AP's order is settled.
Putting their stages here would undo that mark. What this order can carry is
the question that blocks them - AP's order, which AR1's ratification left
open (`ar1-architecture-revision-cabin-function.md`, status line and
AR1-V2) - and it does, as AZ-Q5.

## 1. Survey at `a59da9c`

Read, not run. Every citation is to `a59da9c`.

### 1.1 Item 1 - the peer's `CREATE ASSERTION`

The bug entry (verified at `5574a7e`) still describes the tree. The publish
run is logged inside `BuildAssertionCabin`, which `CreateAssertion` calls
(`src/exec/assertion_catalog.cpp:576`), ahead of the catalog row (`:609`);
the assertion enters the registry's `live_` only at `enforcer_->Adopt` (`src/server/command_dispatcher.cpp:2834`,
`src/exec/assertion_check.cpp:160`), after `CreateAssertion` returns; core
0's checkpoint snapshots only `live_` (`VisitSnapshots`,
`assertion_check.cpp:153`, called from `Checkpointer::Start` at
`src/wal/checkpointer.cpp:143`). The window between `:576` and `:2834` is
the defect.

**The existing design already prefers over-enforcing.** The comment after
the `Adopt` (`command_dispatcher.cpp:2836-2840`) records that a sync failure
answers `ERR` with the registry still enforcing what the log holds - chosen
over a durably created constraint left unenforced. Adopting earlier extends
that choice; it does not introduce one. A base for an assertion whose
publish then fails is already possible and harmless: a mount folds only
what `ListAssertions` returns (`assertion_catalog.cpp:560-564`).

The adoption already runs under the build's relation `X` (AT-S5e): the
`BuildLock` that holds it (`command_dispatcher.cpp:2782-2812`) is released
only when the handler returns, after `:2834`. Moving the adoption earlier
keeps it under the same `X`, so no writer of the relation, on any core,
can meet the assertion before its row is written. A dispatcher with no
lock table (`locks_ == nullptr`, `:2791`) takes no `X` today either.

### 1.2 Item 2 - the catalog row on a chain's tail

The bug entry (verified at `68d8b18`) still describes the tree.
`InsertRow` (`src/catalog/catalog.cpp:384`) sets `*where` in the tail arm
only after `LogCatInsert` succeeds (`:411`, after the hook at `:400-406`
and the append at `:407-410`), and in the new-page arm right after
placement (`:440-442`), before its hook. A failed hook or append in the
tail arm leaves the row on the page and `*where` unset, so the DDL's
rollback never retires it - a quiet wrong answer, since catalog reads hide
aborted work by compensation (`ddl-transactional.md` §2). The fix the entry
names is mechanical.

### 1.3 Item 3 - the `S` a failed check keeps

D9(a)'s hoist takes `IS`, then `S`, then descends
(`src/server/command_dispatcher.cpp:3335`). On `FK_VIOLATION` nothing
releases the `S`, so it stays to the rollback. Releasing one borrow is
already an operation the table has (`LockTable::ReleaseOne`,
`src/txn/lock_table.cpp:570`), used for a granted-then-given-back `S` at
`command_dispatcher.cpp:3608`, which is also where the one hazard is
written down: an `S` the transaction held **before** the ask must not be
the one released (`:3597-3602`, the `Holds(unit)` guard).

### 1.4 Item 4 - the decide

`LockTable::Release` (`lock_table.cpp:587`) calls `ReleaseHeld` per borrow.
`ReleaseHeld` (`:529`) walks its partition's vector to find the entry and
erases it; `WakeWaiters` (`:434`) walks the same partition again. With
`kLockPartitionsPerCore = 64` (`include/kds/txn/lock_table.hpp:396`) a
decide is O(n²/64) in the transaction's borrows. AY's close measured it at
~2.1 µs of a +3.43 µs row cost at K = 16,384 distinct parents, unresolved
at K ≤ 1,024 (`bench/v3.0.0/results-ay-s11-overhead-v2.7.0-530-g0552d55.md`,
cell 3; the file left the tree with the bench rebaseline `fc2d343` and is read with `git show a59da9c:<path>`).

### 1.5 Item 5 - AQ and AR

AR1 §14 orders AP, then AQ, then AR
(`ar1-architecture-revision-cabin-function.md:401-403`). AR1-V2 (`:471-487`)
finds §14's reason for AP first gone and names what can still argue it: AP
is the only one of the three with no dependency. The ratification took
AR1-V's corrections and left AP's order unsettled (status line, `:3`).

### 1.6 Item 6 - the two unreproduced failures

- `ExpeditorTest.AtOneCoreTheDispatcherHoldsTheInstancesLockTable` failed at
  `ASSERT_TRUE(db.Start().ok())` (`tests/expeditor_test.cpp:377`) with no
  status printed. The file carries **ten** such bare `db.Start()`
  assertions, and four more on `opened.value()->Start()`.
  The fixture's ports come from `TwoFreeLoopbackPorts()` (`:75-95`), which
  closes its probe sockets before returning, so another process can bind a
  port before the instance does; that reading is still unconfirmed.
- `RowWaitWakeRigTest.AWriterParkedOnAnotherCoresRowProceedsAtTheReleaseKick`
  is bounded by wall clock (`tests/row_wait_wake_rig_test.cpp:99`, 2000 ms;
  `:119`, 1000 ms). Its failure message was not kept.

Neither can be attributed from what was recorded.

### 1.7 Item 7 - the unsnapshottable cabin

**The consequence is larger than AY's close recorded.** `LogAssertionSnapshot`
refuses a run of more than `kMaxAssertSnapshotChunks` (0xFFFF,
`include/kds/wal/payload.hpp:833`) chunks at `checkpointer.cpp:71-77`, and a
group no record can carry at `:55-64`. Either refusal returns out of
`Checkpointer::Start` after `CHECKPOINT_BEGIN` is logged (`:136`, `:143`).
Only core 0's checkpoints reach it: a peer sharing the instance's registry
gets no snapshot source (`src/server/core_runtime.cpp:438-440`). By source
read, what then happens:

- core 0's periodic checkpoint fails every tick: `RunGated` logs the error
  (`checkpointer.cpp:314-318`) and the cadence discards the status -
  `(void)Checkpoint()` at `src/server/expeditor.cpp:1878` - so **no core-0
  checkpoint completes again** while the assertion lives, and the fold's
  redo start, bounded by core 0's last completion, stops moving;
- core 0's shutdown checkpoint logs an error and continues
  (`expeditor.cpp:2116`);
- the mount's completion checkpoint returns the refusal
  (`src/server/mount_recovery.cpp:368`), so **the next mount fails**.

AY §7 recorded this as "a refusal there is the rule". Read this way it
stops core 0's checkpoints for good and then leaves the volume unmountable.
That is not a wrong answer, but it is not the bounded refusal the close described.
Not run: no cell reaches 65,535 chunks (about 4 GB of group headers).
**Not established**: whether any SQL path yields one group key larger than a
record's usable payload (`WalManager::usable_payload_bytes`,
`include/kds/wal/manager.hpp:228`). AZ-S3 answers that before choosing a
cell shape.

Two leftovers of AY-S8's superseded attempt belong here. That attempt's
branch, `worktree-ay-s8-assertion-snapshot-chunk-count` at `266db2e`
(local, unpushed), holds the only cell that pins the writer's refusal of a
too-large group before any chunk is written
(`AGroupNoRecordCanCarryIsRefusedBeforeAnyChunkIsWritten`); on `a59da9c` no
cell reaches `checkpointer.cpp:55-64`. And two comments still say the
superblock is refused unless it is version 17, where it is 19:
`include/kds/bootstrap/bootstrap.hpp:79`, `tests/core_runtime_test.cpp:110`.

## 2. Rulings - CLA's proposals, marked as proposed 2026-09-30

**AZ-R1 - one placement helper in `InsertRow`, and a failure retires
what nothing logged.** Both arms place, set `*where`, fire the hook and log
through one helper. Once the row is on a page, `*where` names it whatever
fails after. The duplicated place-hook-log sequence goes with it.

**The rollback must not log a retire for an insert that was never logged**
(the review of `98ad783`). With `*where` set, the rollback retires the slot.
On a catalog page at or above `kCatalogOverflowLimit` (128) it logs a `SLOT_RETIRE`
(`src/txn/manager.cpp:491-505`), and after a crash before that page's
writeback, redo meets a slot the on-disk page does not have:
`ApplySlotRetire` returns `NotFound` (`src/wal/redo.cpp:180-183`), and the
mount fails. The new-page arm has that shape today when its hook fails,
with only `PAGE_INIT` logged. So a failure before the row's `HEAP_INSERT`
is appended retires the slot in place and unlogged, inside the helper, and
leaves `*where` unset. A failure after the append leaves `*where` set, and
the rollback's logged retire has a record to follow. The compensations on
pages below 128 are a separate, recorded defect
(`docs/inflight/bugs/a-ddl-rollback-compensates-logged-catalog-pages-unlogged.md`),
which this ruling neither relies on nor fixes.

**AZ-R2 - adopt and log the publish run under one hold of the registry
latch** (AZ-Q1). The adoption moves from `command_dispatcher.cpp:2834` to
the publish: under the registry's latch, taken the way `VisitSnapshots`
takes it (`assertion_check.cpp:153`, directory then stream), the assertion
is adopted into `live_` and its publish run is logged, and then the latch
is released. The log call leaves `BuildAssertionCabin`
(`assertion_catalog.cpp:576`) for this step, or the registry is passed down
to it.

**One hold, not two steps**, because a core-0 checkpoint logs its run for
every id in `live_` under the same latch, one `Append` per chunk. Adopted
first and logged after, the two runs of one id can interleave. Recovery
keys runs by id alone (`src/exec/assertion_recover.cpp:245-253`), so with
three or more chunks the order C0 C1 P0 C2 P1 P2 discards both runs, and the
assertion comes up unenforcing: this bug, reached another way.

Every failure after the adoption evicts again (`Evict`,
`assertion_check.cpp:196`). That covers a refused publish run itself
(`assertion_catalog.cpp:576-578`, which emits no `ASSERT_DROP`) and every
path that emits `ASSERT_DROP` today. The adoption does not run twice. The
build's relation `X` is held across all of it (§1.1). `assertion.md` §7 and
§8.1 restated.

**AZ-R3 - refuse at admission, and never fail the checkpoint** (AZ-Q2).
Two parts.

- An assertion write that would create a group whose key no record can
  carry, or would take the cabin's run past 0xFFFF chunks, is refused at
  admission, so the refusal reaches the writer that caused it. The code is
  AZ-Q2's. The chunk bound must be conservative: the writer cuts chunks
  greedily, so a run's count is not its header bytes over the budget. The
  cabin keeps a running total of its header bytes and admits against
  `bytes ≤ 0xFFFE × (budget − kAssertSnapshotFixedSize − the largest group)`: every chunk but the last closes holding more than that bracket, so no greedy cut can
  pass 0xFFFF chunks under it.
- A checkpoint that meets such a cabin anyway, for example on a volume
  written before this stage, evicts the assertion and marks it
  unenforceable **in one latched step inside the snapshot's hold**, then
  completes. `NoteUnenforceable` cannot be called there as it stands: it
  returns early for an id in `live_` (`assertion_check.cpp:188`), and it
  takes the latch `VisitSnapshots` already holds. That fails closed: the
  relation's writes are refused `CannotEnforce` until `DROP` and `CREATE`,
  and the volume keeps checkpointing and mounting.

The writer's refusal stays as a last line. The stage also ports the cell
from `266db2e` and corrects the two "version 17" comments.

**AZ-R4 - instrument, do not repair** (item 6).

- Every bare `Start()` assertion in `tests/expeditor_test.cpp` gets its
  status message: ten on `db`, four on `opened.value()`.
- `row_wait_wake_rig_test.cpp`'s `Within` assertions already name what they
  waited for (`:100`, `:120`). They get what they saw.
- Neither bound is widened and the port probe is not changed: a cause
  nobody has observed is not fixed.

`known-gaps.md` keeps both entries, each saying the next failure will now
print its cause.

**AZ-R5 - release only the ask's own `S`** (AZ-Q3, marked to release).
On `FK_VIOLATION`, the hoisted arm and the self-referencing arm release the
tuple `S` only when the transaction did not hold it before the ask. The
answer is taken before `BorrowOrWait` (`command_dispatcher.cpp:3344`) and
carried to the violation. The guard at `:3597-3602` only skips an ask, so it
is a partial analogue and not the test itself. An `S` held from an earlier
statement is never released here.

**The `IS` is never released.** It is shared by every parent key of that
relation in the statement (`BorrowChain`). With rows `(p = 99, absent)` and
`(p = 10, present)`, releasing it with `S(99)` would leave `S(10)` held
under no intention, and a relation `X` on the parent - `DROP TABLE`,
`CREATE INDEX` - could be granted over it.

**AZ-R6 - no build** (AZ-Q4, marked), as AY's close proposed. If a later
word marks a build, the cut is a keyed partition: each partition's entries indexed by
`LockKey`, so `ReleaseHeld` and `WakeWaiters` find their entry without a
walk. The all-partition verify scan still iterates. A hashed holdings set in
the ledger does not touch the measured cost (`known-gaps.md`, the `Holds`
entry).

## 3. Stages

Each stage waits for the operator's word.

| stage | what | exit | size |
|---|---|---|---|
| AZ-S0 | This order; the index row | the files at the commit | S |
| AZ-S1 | **The catalog tail arm** (item 2, AZ-R1) | red first: a DDL undo hook failing on its first event with the target's tail holding room leaves `written` (and `CREATE INDEX`'s `created_row`) empty and the row live after the rollback; green with the helper; a second cell failing the append instead, then a crash before writeback and a mount - the mount succeeds and the row is gone (the unlogged in-place retire); the same pair through the new-page arm; mutation: `*where` set after the log again, killed by the first cell, and the in-place retire removed, killed by the second; the bug entry deleted | S |
| AZ-S2 | **The peer's `CREATE ASSERTION`** (item 1, AZ-R2) | red first on the two-core rig: a peer's create paused between its publish run and its adoption, a core-0 checkpoint completing past the publish run, a crash, a mount - the assertion comes up unenforcing today; green with adoption first; cells for a create that fails after adoption (evicted, no enforcement left) and for a writer on another core during the window (waits on the relation `X`, §1.1); a cell interleaving a core-0 checkpoint run with a three-chunk publish run of the same id, which recovers only if both are under one hold; mutation: adoption moved back after the publish, killed, and the latch dropped between adopt and log, killed by the interleaving cell; the bug entry deleted | M |
| AZ-S3 | **The unsnapshottable cabin** (item 7, AZ-R3) | first the survey question: whether SQL reaches a group key past one record; a cell per door reached - an admission refused with its position, a checkpoint that meets such a cabin completing and the assertion unenforcing at the next mount, the mount succeeding; the chunk-count door at its threshold through a test seam, not 4 GB; `266db2e`'s writer cell ported; the two "version 17" comments corrected; mutation: the checkpoint's fail-closed arm returning the refusal again, killed | M |
| AZ-S4 | **The unreproduced failures instrumented** (item 6, AZ-R4) | all fourteen `Start()` assertions in `tests/expeditor_test.cpp` (ten on `db`, four on `opened.value()`) carry their status, and every rig `Within` what it saw; the suite green; `known-gaps.md`'s two Testing entries restated | S |
| AZ-S5 | **The failed check's `S`** (item 3, AZ-R5) - AZ-Q3 marked the release | red first: a failed child `INSERT` inside `BEGIN`, then a parent `INSERT` of that key on the other core, which waits and is refused `TxnConflict` at the 1 s net today, proceeds; a cell where the `S` was held from an earlier statement and survives the violation; the self-referencing arm's pair; a statement with an absent and a present parent key, whose `IS` survives the violation and holds off a parent `DROP TABLE`; mutation: the `Holds` answer removed, killed, and the `IS` released too, killed; `foreign-keys.md` §2c and the `known-gaps.md` entry | S |
| AZ-S6 | ~~**The keyed lock partition**~~ (item 4, AZ-R6) - **struck 2026-09-30**: AZ-Q4 accepted the cost as priced (`raft-marks-2026-09-30.md` §16); what follows is the stage a later build would run | `lock_table_test.cpp` and `lock_family_test.cpp` green unchanged; AY-S11 cell 3's shape re-run at the close, K up to 16,384, `58198cb`'s and `0552d55`'s numbers as the controls | M |
| AZ-S7 | **AZ's close** | a row per stage; what AZ carries; the overhead measured over the whole change | S |

## 4. Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| AZ-Q0 | **The letter** - whether AY §7's seven items open as one letter, and as AZ | scope | yes: every item is small, none depends on another, and two need nothing but the word. **Marked as proposed 2026-09-30** (`raft-marks-2026-09-30.md` §16) |
| AZ-Q1 | **Item 1's shape**: adopt before the publish run, or hold the checkpoint gate from the publish to the adoption | user-visible | adopt first, and log the publish run under the same hold (AZ-R2). §1.1 gives the reason; it also keeps `CheckpointGate` - which orders checkpoints against each other, not against DDL - out of a DDL's path. **Marked as proposed 2026-09-30** (`raft-marks-2026-09-30.md` §16) |
| AZ-Q2 | **Item 7's door**: today a checkpoint that meets the cabin fails, and by §1.7's read no core-0 checkpoint completes after it and the next mount fails | user-visible | refuse at admission, and let the checkpoint complete with the assertion unenforcing (AZ-R3): fail closed, keep the volume mountable. **The admission refusal's code** is also the operator's, since it is a wire surface (`protocol.md` §11). CLA proposes `NotImplemented`: the bound is the snapshot format's (a `u16` count, one record per group), and a later release can widen it, so *this release* is what is true (`status.hpp`). `ResourceExhausted` would need a third wire detail, and `OutOfSpace` says storage is full, which it is not. **Marked as proposed 2026-09-30** (`raft-marks-2026-09-30.md` §16) |
| AZ-Q3 | **Item 3**: release a failed check's `S` at the violation, or keep it to the rollback | user-visible | release the ask's own `S` only (AZ-R5): PostgreSQL's `FOR KEY SHARE` locks nothing for a missing row, and the poisoned transaction can write nothing that `S` protects. Keeping it is also sound; its cost is a refusal bounded by the client's rollback. **Marked as proposed 2026-09-30** (`raft-marks-2026-09-30.md` §16) |
| AZ-Q4 | **Item 4**: build the keyed partition, or accept the priced cost | cost | accept, no stage (AZ-R6): ~6 % at 16,384 parents in one transaction, unresolved at 1,024. Revisit when a workload holds thousands. **Marked as proposed 2026-09-30** (`raft-marks-2026-09-30.md` §16) |
| AZ-Q5 | **Item 5**: AP's order, which blocks AQ and AR opening as their own letters (AY-Q11) | scope | AP first, argued on AR1-V2's remaining ground: it is the only one of the three with no dependency. Settling it opens nothing here. **Marked as proposed 2026-09-30** (`raft-marks-2026-09-30.md` §16) |

## 5. Sequencing

§0 and §3 give each stage's gate. The one order among them: **AZ-S2 lands
before AZ-S3**. Both restate `assertion.md` §7, and both take the registry
latch inside the snapshot's hold, so S3's evict-and-mark step is written
against S2's hold. **AZ-S7 is last**, and the milestone's overhead is
measured once over its whole code change (`raft-marks-2026-09-30.md` §8),
from `a59da9c` to the commit that closes it.

## 6. Row status

### AZ-S0 — written 2026-09-30

On `worktree-az-ay-carry-forward-order` from `a59da9c`, as this file and
its index row. §1's survey was read against `a59da9c`, not run. No code,
spec or test is changed. The letter itself is AZ-Q0's.

**The review** (`critics-developer`, on `6b32c92`) did two things.

- **It corrected the survey's citations and counts itself** (`98ad783`).
  It confirmed §1.7 by source read and narrowed it: only core 0 checkpoints
  an assertion, and a failed tick is logged, not silent.
- **It found four hazards in the rulings, all applied.**
  - AZ-R2 as first written would let a checkpoint's run and the publish
    run of one id interleave, which recovery discards whole. Adopt and log
    are now one hold.
  - AZ-R5 released the `IS`, which the statement's other parent keys still
    need. Only the `S` is released now.
  - AZ-R1's rollback would log a `SLOT_RETIRE` for an insert never logged,
    and a crash would then fail the mount. A failure before the append now
    retires in place.
  - AZ-R3 relied on `NoteUnenforceable`, which does nothing for a live id
    and would deadlock under the snapshot's latch. Its chunk bound was also
    not conservative.
- **Its lead outside the order** (compensations on catalog pages below
  128) is already a bug entry. AZ-R1 cites it and does not take it up.

**Rejected: none.** The review's trims were taken: the over-enforcing
argument now lives in §1.1 only, and §5 keeps only the S2 → S3 order.

### AZ opened, and §4 marked - 2026-09-30

On *"CLA 제안대로 진행하고 main에 push해줘"*, every §4 item was marked as
proposed and the order landed (`raft-marks-2026-09-30.md` §16).

- **Open**: AZ-S1, S2, S3, S4, S5 and S7, each on its own word.
- **Struck**: AZ-S6, since AZ-Q4 accepted the cost as priced.
- **Settled**: AR1's AP order, AP first, recorded in AR1's status line.

No stage has started.

### AZ-S1 — built 2026-09-30

On `worktree-az-s1-catalog-tail-arm` from `f2f1ee7`, on *"AZ-S1 진행해줘"*.
The reproduction (`aa9c3ef`) was red at `f2f1ee7` in all three cells:

- **the hook refused on the `sys.objects` tail**: the failed create's row
  stayed live, and `FindTableOidByName` found the relation - the quiet
  wrong answer item 2 named;
- **the hook refused on a new page**: the row stayed on it;
- **the row's `HEAP_INSERT` refused after the hook succeeded**: the row
  stayed live and was found. The cell fills the ring and makes its drain
  fail, so the refusal is the one a real append gives.

**The fix is one helper, `ReportPlacedRow`**, which both arms of `InsertRow`
call. It fires the hook, then logs, and on a failure removes the row it
placed. The arms set `where` only once the row is logged.

**What changed from AZ-R1 as written, and why**:

- **The hook's failure takes the row back whole** (`PageView::UnInsertTuple`,
  new); it does not retire it in place. A dead slot no record describes
  would sit before any later logged insert on the page, and redo onto an
  image older than that slot refuses the later record (the dense-slot rule,
  `heap_page.cpp`). Taking the row back is exact because the page is held
  exclusive across the insert and slots are only ever appended.
- **The log's failure retires the slot, as AZ-R1 said.** The undo record
  the hook appended names the slot, and a retired slot is never handed out
  again. A taken-back slot could be reused, and recovery's undo would then
  meet another row there.
- **AZ-R1's premise was wrong for catalog rows.** No catalog page is at or
  above `kCatalogOverflowLimit`: `AllocateCatalogPage` allocates below it.
  So `Compensate` never logs a `SLOT_RETIRE` for a catalog row, and the
  mount failure the review predicted cannot arise here. The live defect
  in that area is the reverse one,
  `docs/inflight/bugs/a-ddl-rollback-compensates-logged-catalog-pages-unlogged.md`,
  which this stage does not take up. The crash-and-mount cells the exit
  named were not written. For the arm that takes the row back, a mount has
  nothing of it to replay. For the retire arm, recovery's undo answers a
  dead or missing slot as "nothing to retire" (`recovery_undo.cpp`), but
  redo does not - see the review below.

**The cells.**

- **The new-page cell runs through `CreateIndex`.** `CreateTable` records
  a row only after that row's insert succeeds, so a `where` set too early
  does not show through it. `CREATE INDEX`'s `created_row` is read even
  when the create fails.
- **Mutants: four, all killed.**
  - the take-back removed: two cells;
  - the take-back replaced by a retire: two cells;
  - the retire on a log failure removed: one cell;
  - `where` set before the report again: the new-page cell.
- **`heap_page_test.cpp` pins `UnInsertTuple`**: only the last insert, the
  directory and free space restored, and a retired slot refused.

**Also**:

- `ddl-transactional.md` §2 states the rule.
- The bug entry is deleted.
- Overhead not measured; it is measured at AZ's close.

**The review** (`critics-developer`, on `55a5aab`) found one defect, and
it is not fixed here.

- **The retire arm leaves the dead slot no record describes, which this
  row had rejected for the hook arm.** A later DDL's logged insert on the
  same tail page, then a crash before writeback, makes redo refuse the
  mount (the dense-slot rule).
  - It predates the stage: the same arm left a live, unlogged row with the
    same redo shape.
  - Taking the row back instead fails another way: recovery's identity
    check meets a reused slot.
  - The cure is a design call - stop on the append failure, hold the page
    until a record describes the slot, or accept it. It is recorded in
    `known-gaps.md` (WAL), and is **the operator's**, not assumed.
- **The docs claimed it closed.** Corrected: `ddl-transactional.md` §2,
  `ReportPlacedRow`'s comment, and this row.
- **Also taken:**
  - `InsertRow`'s comment says `where` is set once the row is logged;
  - `ReportPlacedRow` takes the page by reference, as its sibling helpers
    do;
  - a refused report on a new page spending one reserved catalog page is
    recorded in `known-gaps.md`.
- **Rejected:**
  - dropping the unreachable take-back and retire failure contexts - kept,
    since they cost nothing and a failure there would otherwise hide the
    original error;
  - removing `UnInsertTuple`'s redundant `length == 0` check - kept, for
    symmetry with the page's other slot tests.

### AZ-S5 — built 2026-09-30

On `worktree-az-s5-failed-check-share` from `cd433ea`, on *"start
AZ-S*"*. The reproduction (`f882e75`) was red at `cd433ea`: a failed child
`INSERT` inside `BEGIN`, then another session's `INSERT` of that parent,
refused `TXN_CONFLICT` "row id=99 is held by transaction 4".

**The fix is AZ-R5 as written** (`92e14c7`).

- On `FK_VIOLATION`, the parent row's `S` is given back with
  `LockTable::ReleaseOne`, which wakes the key's waiters.
- It is given back only when this statement's ask took it:
  - the hoist tests `HoldsRow` before `BorrowOrWait` and records its own
    asks (`FkParentVerdicts::NoteAsked`/`Asked`);
  - the self-referencing arm tests the same.
- The relation's `IS` is never given back.

**The cells** (`fk_parent_hold_test.cpp`: one core, the lock table, two
sessions, synchronous dispatch).

- **The release**: red, then green.
- **An `S` held from an earlier statement survives.** The earlier statement
  is a zero-row `UPDATE` that named the absent key.
- **The `IS` survives.** One statement names an absent and a present parent.
  After the violation, a `CREATE INDEX` on the parent is still refused, and
  the absent key's parent `INSERT` goes through.
- **Mutants: three, all killed.**
  - the held-before answer ignored: the held-before cell;
  - the `IS` given back with the `S`: the intention cell;
  - no release: the release and intention cells.

**Not as the exit wrote it.**

- **No self-referencing cell.** A self-referencing key cannot be declared
  (`ForeignKeyCheckTest.ASelfReferencingForeignKeyCannotBeDeclared`), so the
  arm's release is written and nothing reaches it. The reproduction's
  message called that pair red; it was red only because its `CREATE TABLE`
  was refused (corrected in `92e14c7`).
- **One core, synchronously.** The cells do not run "on the other core ...
  at the 1 s net": a refused hold is the refusal there, and the release's
  wake is `ReleaseHeld`'s.
- **`CREATE INDEX` stands in for `DROP TABLE`.** The parent of a declared
  foreign key cannot be dropped (RESTRICT).

**Also**: `foreign-keys.md` §2c, §2a and §5, the `known-gaps.md` entry and
the CLAUDE.md Foreign keys row restated. The suite at `92e14c7`: 3062/3062
under `-j8`. Overhead not measured; it is measured at AZ's close.

**The review** (`critics-developer`, on `92e14c7`) found the change safe:
it never gives back an `S` anything relies on. It also found a gap, and it
is recorded here, not closed.

- **A statement that parked after its hoist keeps the `S`.** It runs again
  whole, and its first run's `S` is still in the ledger, so the re-run reads
  it as held before. An example: a violation after a wait on a child row
  another open transaction holds. Inside `BEGIN` the `S` then stays to the
  rollback, as before AZ-S5. That is a refusal, not a wrong answer, and the
  synchronous fixture cannot reach it.
- **The review's proposal is the operator's**, because it changes AZ-R5:
  give back the `S` on any failed check, whoever held it. An `S` on a key
  the check reads as absent protects no row - while any `S` is held no
  other transaction can take the key's `X`, so absent means absent at every
  grant, or deleted by this transaction under its own `X`. That closes the
  gap, deletes `HoldsRow` and the asked set, and flips the held-before cell.
  It is recorded in §2c and `known-gaps.md`, and not assumed.
- **Taken:**
  - §5's "an absent parent is held by the same `S`";
  - §2a now says the self-referencing arm is unreachable;
  - `ReleaseOne`'s "two callers";
  - the dispatcher header's comments sat above the wrong declarations;
  - the test helper is a fixture method.
- **Rejected:**
  - folding the asked bit into the verdict map - `Find`'s callers would all
    change for one flag;
  - leaving the self-referencing arm unchanged - AZ-R5 names both arms, and
    the arm should not diverge silently if a self-referencing key ever
    becomes declarable.
