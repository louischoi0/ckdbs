# Work order AZ — what AY carried: seven items, one letter

Written 2026-09-30 on `worktree-az-ay-carry-forward-order` from `a59da9c`
(`v2.7.0-533-ga59da9c`), on the operator's *"미해결 항목 7건을 모두
커버하는 새로운 작업 지시서를 작성해줘"*: a work order covering every item
AY's close carried forward (`workorder-ay-following-letter.md` §7, *What AY
carries forward*). **The letter is CLA's proposal** (AZ-Q0): a letter is
opened on the operator's word, as AY was (`raft-marks-2026-09-29.md` §7),
and until then this order licenses no stage. It cuts no tag; the v3.0.0 tag
waits on AR0 §8's chain through M4 (`raft-marks-2026-09-26.md` §4).

## 0. The items this order serves

Numbered as AY §7 numbers them. "Covered" means differently per item, and
the column says how: three are built by a stage whatever is marked, three
need a mark before a stage exists, and one cannot be built here at all.

| AY §7 | item | owner at AY's close | covered here by |
|---|---|---|---|
| 1 | A peer's `CREATE ASSERTION` can miss every snapshot in the mount's scan (`docs/inflight/bugs/a-peers-create-assertion-can-miss-every-snapshot-in-the-mounts-scan.md`) | none | AZ-S2, on AZ-Q1 |
| 2 | A catalog row placed on a chain's tail is not reported when its logging fails (`docs/inflight/bugs/a-catalog-row-placed-on-a-chains-tail-is-not-reported-when-its-logging-fails.md`) | none | AZ-S1 |
| 3 | A failed child check inside an explicit transaction keeps its `S` on the absent parent key until the rollback (`known-gaps.md`, Foreign keys) | the operator | AZ-Q3; AZ-S5 only if it marks a release |
| 4 | A large transaction's decide is quadratic in its borrows, and D9(a) doubles them (`known-gaps.md`, Foreign keys) | none | AZ-Q4; AZ-S6 only if it marks a build |
| 5 | AR1's AQ and AR | their own letters, after AP's order is settled (AY-Q11) | AZ-Q5 - **no stage**, see below |
| 6 | Two one-off cell failures under `-j8` that did not reproduce (`known-gaps.md`, Testing) | none | AZ-S4 |
| 7 | A cabin needing more than 65,535 snapshot chunks is refused at every checkpoint | none | AZ-S3, on AZ-Q2 |

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
run is logged inside `CreateAssertion` (`src/exec/assertion_catalog.cpp:576`),
ahead of the catalog row (`:609`); the assertion enters the registry's
`live_` only at `enforcer_->Adopt` (`src/server/command_dispatcher.cpp:2834`,
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

Not established: whether the adoption can move inside the build's
relation `X` (AT-S5e) without a writer on another core meeting an assertion
whose row is not yet committed. AZ-S2's first cell answers it.

### 1.2 Item 2 - the catalog row on a chain's tail

The bug entry (verified at `68d8b18`) still describes the tree.
`InsertRow` (`src/catalog/catalog.cpp:384`) sets `*where` in the tail arm
only after `LogCatInsert` succeeds (`:411`, after the hook at `:399-405`
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
cell 3).

### 1.5 Item 5 - AQ and AR

AR1 §14 orders AP, then AQ, then AR
(`ar1-architecture-revision-cabin-function.md:401-403`). AR1-V2 (`:471-487`)
finds §14's reason for AP first gone and names what can still argue it: AP
is the only one of the three with no dependency. The ratification took
AR1-V's corrections and left AP's order unsettled (status line, `:3`).

### 1.6 Item 6 - the two unreproduced failures

- `ExpeditorTest.AtOneCoreTheDispatcherHoldsTheInstancesLockTable` failed at
  `ASSERT_TRUE(db.Start().ok())` (`tests/expeditor_test.cpp:377`) with no
  status printed. The file carries **ten** such bare `Start()` assertions.
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
`Checkpointer::Start` after `CHECKPOINT_BEGIN` is logged (`:135`, `:143`).
By source read, what then happens:

- the periodic checkpoint's status is discarded - `(void)Checkpoint()` at
  `src/server/core_runtime.cpp:529` and `src/server/expeditor.cpp:1878` -
  so **no checkpoint completes again** on that volume while the assertion
  lives, and the anchor stops moving;
- the shutdown checkpoint logs an error and continues
  (`expeditor.cpp:2050`, `:2116`);
- the mount's completion checkpoint returns the refusal
  (`src/server/mount_recovery.cpp:368`), so **the next mount fails**.

AY §7 recorded this as "a refusal there is the rule". Read this way it
stops checkpoints for good and then leaves the volume unmountable. That is
not a wrong answer, but it is not the bounded refusal the close described.
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

## 2. Rulings - CLA's proposals

**AZ-R1 - one placement helper in `InsertRow`.** Both arms place, set
`*where`, fire the hook and log through one helper. Once the row is on a
page, `*where` names it whatever fails after. The duplicated
place-hook-log sequence goes with it.

**AZ-R2 - adopt before the publish run** (AZ-Q1). `CreateAssertion` adopts
into the registry before `LogAssertionSnapshot` at
`assertion_catalog.cpp:576`. Every failure after the adoption, the same
paths that emit `ASSERT_DROP` today, evicts it again (`Evict`,
`assertion_check.cpp:196`). The adoption at `command_dispatcher.cpp:2834`
moves in, and it does not run twice. Any core-0 checkpoint that begins
after the publish run then snapshots the assertion. `assertion.md` §7 and
§8.1 restated.

**AZ-R3 - refuse at admission, and never fail the checkpoint** (AZ-Q2).
Two parts.

- An assertion write that would create a group whose key no record can
  carry, or would take the cabin's run past 0xFFFF chunks, is refused at
  admission, so the refusal reaches the writer that caused it. The cabin
  keeps a running total of its header bytes to answer the second check
  without a walk.
- A checkpoint that meets such a cabin anyway, for example on a volume
  written before this stage, marks that assertion unenforceable
  (`NoteUnenforceable`) and completes. That fails closed: the relation's
  writes are refused `CannotEnforce` until `DROP` and `CREATE`, and the
  volume keeps checkpointing and mounting.

The writer's refusal stays as a last line. The stage also ports the cell
from `266db2e` and corrects the two "version 17" comments.

**AZ-R4 - instrument, do not repair** (item 6).

- Every bare `ASSERT_TRUE(db.Start().ok())` in `tests/expeditor_test.cpp`
  gets its status message.
- `row_wait_wake_rig_test.cpp`'s `Within` assertions get what each waited
  for and what it saw.
- Neither bound is widened and the port probe is not changed: a cause
  nobody has observed is not fixed.

`known-gaps.md` keeps both entries, each saying the next failure will now
print its cause.

**AZ-R5 - release only the ask's own `S`** (AZ-Q3, if marked to release).
On `FK_VIOLATION`, the hoisted arm and the self-referencing arm release the
`S` and the `IS` above it only when the transaction did not hold them before
the ask - the `Holds(unit)` test `command_dispatcher.cpp:3597-3602` already
applies. An `S` held from an earlier statement is never released here.

**AZ-R6 - no build** (AZ-Q4), as AY's close proposed. If the operator marks
a build, the cut is a keyed partition: each partition's entries indexed by
`LockKey`, so `ReleaseHeld` and `WakeWaiters` find their entry without a
walk. The all-partition verify scan still iterates. A hashed holdings set in
the ledger does not touch the measured cost (`known-gaps.md`, the `Holds`
entry).

## 3. Stages

Each stage waits for the operator's word.

| stage | what | exit | size |
|---|---|---|---|
| AZ-S0 | This order; the index row | the files at the commit | S |
| AZ-S1 | **The catalog tail arm** (item 2, AZ-R1) | red first: a DDL undo hook failing on its first event with the target's tail holding room leaves `written` (and `CREATE INDEX`'s `created_row`) empty and the row live after the rollback; green with the helper; mutation: `*where` set after the log again, killed by the cell; the bug entry deleted | S |
| AZ-S2 | **The peer's `CREATE ASSERTION`** (item 1, AZ-R2) | red first on the two-core rig: a peer's create paused between its publish run and its adoption, a core-0 checkpoint completing past the publish run, a crash, a mount - the assertion comes up unenforcing today; green with adoption first; cells for a create that fails after adoption (evicted, no enforcement left) and for a writer on another core during the window (the question §1.1 leaves open); mutation: adoption moved back after the publish, killed; the bug entry deleted | M |
| AZ-S3 | **The unsnapshottable cabin** (item 7, AZ-R3) | first the survey question: whether SQL reaches a group key past one record; a cell per door reached - an admission refused with its position, a checkpoint that meets such a cabin completing and the assertion unenforcing at the next mount, the mount succeeding; the chunk-count door at its threshold through a test seam, not 4 GB; `266db2e`'s writer cell ported; the two "version 17" comments corrected; mutation: the checkpoint's fail-closed arm returning the refusal again, killed | M |
| AZ-S4 | **The unreproduced failures instrumented** (item 6, AZ-R4) | every `Start()` assertion and every rig `Within` carries its cause; the suite green; `known-gaps.md`'s two Testing entries restated | S |
| AZ-S5 | **The failed check's `S`** (item 3, AZ-R5) - only if AZ-Q3 marks a release | red first: a failed child `INSERT` inside `BEGIN`, then a parent `INSERT` of that key on the other core, which waits and is refused `TxnConflict` at the 1 s net today, proceeds; a cell where the `S` was held from an earlier statement and survives the violation; the self-referencing arm's pair; mutation: the `Holds` guard removed, killed; `foreign-keys.md` §2c and the `known-gaps.md` entry | S |
| AZ-S6 | **The keyed lock partition** (item 4, AZ-R6) - only if AZ-Q4 marks a build | `lock_table_test.cpp` and `lock_family_test.cpp` green unchanged; AY-S11 cell 3's shape re-run at the close, K up to 16,384, `58198cb`'s and `0552d55`'s numbers as the controls | M |
| AZ-S7 | **AZ's close** | a row per stage; what AZ carries; the overhead measured over the whole change | S |

## 4. Items for the operator

| # | item | class | CLA proposal |
|---|---|---|---|
| AZ-Q0 | **The letter** - whether AY §7's seven items open as one letter, and as AZ | scope | yes: every item is small, none depends on another, and three need nothing but the word |
| AZ-Q1 | **Item 1's shape**: adopt before the publish run, or hold the checkpoint gate from the publish to the adoption | user-visible | adopt first (AZ-R2). It extends the over-enforcing choice `command_dispatcher.cpp:2836-2840` already made, and keeps `CheckpointGate` - which orders checkpoints against each other, not against DDL - out of a DDL's path |
| AZ-Q2 | **Item 7's door**: today a checkpoint that meets the cabin fails, and by §1.7's read no checkpoint completes after it and the next mount fails | user-visible | refuse at admission, and let the checkpoint complete with the assertion unenforcing (AZ-R3): fail closed, keep the volume mountable |
| AZ-Q3 | **Item 3**: release a failed check's `S` at the violation, or keep it to the rollback | user-visible | release the ask's own `S` only (AZ-R5): PostgreSQL's `FOR KEY SHARE` locks nothing for a missing row, and the poisoned transaction can write nothing that `S` protects. Keeping it is also sound; its cost is a refusal bounded by the client's rollback |
| AZ-Q4 | **Item 4**: build the keyed partition, or accept the priced cost | cost | accept, no stage (AZ-R6): ~6 % at 16,384 parents in one transaction, unresolved at 1,024. Revisit when a workload holds thousands |
| AZ-Q5 | **Item 5**: AP's order, which blocks AQ and AR opening as their own letters (AY-Q11) | scope | AP first, argued on AR1-V2's remaining ground: it is the only one of the three with no dependency. Settling it opens nothing here |

## 5. Sequencing

**AZ-S1 and AZ-S4 are startable on the word alone**; neither waits on an
item. **AZ-S2 waits on AZ-Q1, AZ-S3 on AZ-Q2**, and they land in that
order, because both restate `assertion.md` §7 and S3's fail-closed arm is
cleaner written against S2's adoption. **AZ-S5 and AZ-S6 exist only if
their item is marked** to build, and are otherwise struck in §6. **AZ-S7
last**, with the milestone's overhead measured once over its whole code
change (`raft-marks-2026-09-30.md` §8), from `a59da9c` to the commit that
closes it.

## 6. Row status

### AZ-S0 — written 2026-09-30

On `worktree-az-ay-carry-forward-order` from `a59da9c`, as this file and
its index row. §1's survey was read against `a59da9c`, not run. No code,
spec or test is changed. The letter itself is AZ-Q0's.
