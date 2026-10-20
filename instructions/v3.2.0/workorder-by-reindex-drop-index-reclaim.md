# Work order BY — `REINDEX`, and a dropped index's pages reclaimed

**CLOSED 2026-10-20 on the operator's word** (`instructions/v3.2.0/raft-marks-2026-10-20.md` §3).

Written 2026-10-16 on `worktree-reindex-drop-index-reclaim` from `5f610082`
(`v3.2.0-115-g5f610082`), on the operator's words, recorded in English:

- **W0**: "Then write the milestone document for implementing the REINDEX
  feature and the DROP INDEX page reclaim feature."
- **W1** (2026-10-16): *"BY-Q1..Q6 $!a, go ahead"* - every question marked
  (a) and BY opened (`raft-marks-2026-10-16.md` §2). **BY-Q3 (a) and BY-Q4
  (a) are the operator's choices against CLA's proposals (b)**: a settled
  anchor slot is removed by a new record kind, and `REINDEX` runs in
  autocommit only.

W0 followed CLA's account (same session, after BX closed) that a secondary
index is append-only and has no purge, that a rebuild is the reclaim that
fits KDS's design - the backfill already builds exactly the entries some
live snapshot can still reach (IX10a) - and that a rebuild reclaims nothing
today because `DROP INDEX` frees no page.

**Claim tags**: `[source-read]` with `path:line` at `5f610082`; `[design]`
for reasoning about code not yet written; `[quiet-wrong]` where a wrong
choice turns a correct answer into a wrong one with no error.

**The letter.** `[source-read]` BX is the last work order under
`instructions/v3.2.0/`. BY was named once, for the read/update order on
2026-10-11, and withdrawn the same day for BN
(`instructions/v3.0.0/raft-marks-2026-10-11-bn-paths.md`); nothing has used
it since. CLA's choice.

**BY against other orders.** BY takes up what `docs/spec/index.md` §12
declined at BF-Q8 (b) - a dropped index's tree freed before its relation is
dropped - and adds the rebuild IX2's append-only maintenance needs. It
changes nothing on the read or write path of a live index. Not part of
AR0 §8's chain.

---

## 0. What BY is

Two features, the second built on the first:

1. **A dropped index's tree is reclaimed.** Today a dropped index's pages,
   and a rolled-back `CREATE INDEX`'s logged tree, stay allocated until the
   relation is dropped. After BY, once nothing can replay or read the tree,
   its pages go back to the allocator - BF's reclaim, aimed at one index -
   and its anchor slot is removed.
2. **`REINDEX INDEX <name>`** builds the index again from the relation under
   a new oid and the same name, and the old tree is reclaimed through (1).
   The new tree holds an entry for each row's current version and every
   older one a reader can still reach (IX10a's backfill, which since BY-S4's
   merge of the backfill fix stops where the oldest reader would) - so the
   surplus entries of rows that left the relation (rolled back, purged) and
   of versions no reader reaches are gone from it. (Corrected twice: the
   first wording, "every surplus entry", overclaimed against a backfill that
   walked every chain to its end; that walk read recycled undo and is fixed.)

Neither is a purge of a live tree: BY removes no entry from a tree anyone
reads, so IX14's premise ("no index entry is removed") stands untouched.

---

## 1. The tree as read `[source-read]` at `5f610082`

- **`DROP INDEX` retires (autocommit at the bootstrap id) or delete-marks
  (in a transaction) the `sys.indexes` row and nothing else**
  (`src/catalog/catalog.cpp:3774-3840`, `src/exec/index_ddl.cpp:519`). It
  frees no page, by decision (`docs/spec/index.md:721`, BF-Q8 (b)): the
  anchor slot still names the tree, reclaimed with its relation (DT1).
  `index.md:720` says the drop retires rather than delete-marks, which
  disagrees with the code; BY-S5 corrects it.
- **The anchor slot is a tree's only durable name once its row is gone.** A
  relation's anchor page holds `{index_oid, root}` per index ever created on
  it, dropped and rolled-back ones included; **no slot is ever removed**
  (`include/kds/storage/anchor_page.hpp:57-60`, `:93-98`).
- **The slot cap refuses `CREATE INDEX`.** `kAnchorMaxIndexEntries` (about
  679). Since PW2-3 the slot is written at create (`catalog.cpp:3747-3758`),
  so at the cap `CREATE INDEX` itself is refused `ResourceExhausted`
  (`catalog.cpp:3682`); a root split only updates its slot. The header's
  claim that "the next index's first root split fails an ordinary INSERT"
  (`anchor_page.hpp:85-90`) is out of date; BY-S5 corrects it.
- **Why removal was left out** (`anchor_page.hpp:93-98`): a removal is a
  mutation the log cannot describe - redo of an older `ANCHOR_UPDATE` would
  resurrect the entry - and `DROP INDEX` is transactional, so "the removal
  belongs at DDL *resolution* ... with its own record".
- **`ANCHOR_UPDATE`** (`include/kds/wal/record.hpp:158`, kind 26) carries
  `{index_oid u64, root u32}` (`include/kds/wal/payload.hpp:83-91`); redo
  calls `SetAnchorIndexRoot` (`src/wal/redo.cpp:265`), which sets the slot
  or adds it.
- **The `sys.indexes` row's root is the creation root**
  (`include/kds/catalog/rows.hpp:448-453`); a root split moves the slot.
  `InitTableAccess` falls back to the row's root when the slot holds
  `kInvalidPageId` (`catalog.cpp:2616-2622`).
- **A rolled-back `CREATE INDEX` leaves a logged tree and a slot.** The tree
  is logged as images before the row (`LogBuiltTree`,
  `index_ddl.cpp:139-151`, `:456`) and the slot written at create; a
  rollback keeps the slot, and `EndDdlScopeById` forgets anything that did
  not commit (`src/server/command_dispatcher.cpp:6776-6790`). BX-S6's free
  runs only when the backfill fails (`index_ddl.cpp:443-448`), not on a
  failure after `LogBuiltTree`.
- **BF's reclaim** (`src/server/page_reclaim.cpp`) is built around a
  relation: a job starts at the anchor (`:23`, `:43`), walks the clustered
  root and every slot's tree as `Expect::kIndex` with the slot's oid as
  owner (`:203-206`), checks each page's type and owner while walking
  (`:165-301`), frees its plan (`:327-356`, `NotFound` accepted), frees the
  anchor last and ends in `ClearPendingRoots`. It is gated by the durable
  redo start passing the drop's commit (BF-R4, `:72`) and by every core's
  statement epoch passing the drop's schema word (BF-R9, `:79-83`). A
  refused job waits for the next mount; tombstones are re-read there
  (`CollectAtMount`, `:23`). One reclaimer, on core 0, runs every job.
- **Reads of `sys.indexes`.** `ScanAll` with no view (`catalog.cpp:67-118`)
  counts a committed delete-mark as gone and an uncommitted insert as
  present. `FindIndexByName` returns the first match in scan order
  (`:3875-3882`); `CheckIndexNameFree` answers `AlreadyExists` to the
  transaction's own mark (`:3916-3920`); a drop of a row another open
  transaction marked is refused (`:3843-3848`). A transaction's own writes
  still maintain an index it delete-marked (`catalog.cpp:56-58`).
- **`CREATE INDEX` and `DROP INDEX` hold the relation `X`** (AT-S5e,
  `ddl-transactional.md` §5e); `DROP INDEX` is admitted inside a
  transaction (`ddl-transactional.md:140`).
- **The superblock is 28** (`include/kds/server/superblock.hpp:171`).

---

## 2. Rulings `[design]`

**BY-R1 — a tree is owed through its anchor slot.** The slot survives the
row and names the current root, so it is the durable record of what is
owed. A slot is **owed** when its `index_oid` names no live `sys.indexes`
row and its root is not `kInvalidPageId`. **That predicate is read only at
mount, unfiltered (`ScanAll` with no view), after undo** `[quiet-wrong]`:
a view-filtered read would call an open `CREATE INDEX`'s slot owed and free
a tree being built. While the instance runs, an owed tree is known only
from the DDL that made it (BY-R2).

**BY-R2 — every ending that orphans a tree queues it.** A committed
`DROP INDEX`, a rolled-back `CREATE INDEX`, and the old tree of a committed
`REINDEX` each queue an **index job**
`{relation oid, anchor page, index oid}` at the decide, with the schema
word the decide moved to (BF-R9) and a gate past the decide's record
(BF-R4). The job reads the root **from the slot when it walks**, after its
gates - not at the drop `[quiet-wrong]`: the dropper's own writes still
maintain the index until commit and can split its root, and a root taken at
drop time reaches only part of the tree. **A `CREATE INDEX` that fails
after its tree is logged** queues nothing - the failed statement reports
no oid - and is reclaimed at the next mount (BY-R1); a failure before its
slot went down seeds the slot first, so the tree is owed from it rather than
named by nothing (BY-S2's review).

**BY-R3 — one job owns a tree** `[quiet-wrong]`. A relation's drop job walks
every slot (§1), so a `DROP TABLE` after a `DROP INDEX` could plan the same
pages as the index job; the first free returns an id to the free list,
another relation takes it, and the second free frees a live page. So: the
reclaimer never holds two plans that cover one anchor. A relation job for
`R` cancels every index job for `R` before it walks (a page the index job
already freed is `NotFound` or another owner's to the relation's walk, both
skipped); an index job does not start while a relation job for its
relation exists; and the mount's owed-slot scan skips a relation whose drop
is already owed.

**BY-R4 — the index job.** A new job kind: check the anchor's type and owner
before each step; walk from the slot's root as `Expect::kIndex` with the
index's oid as owner; free children first, the map synced per level; then
**remove the slot** (BY-R5) under the anchor's latch, its record logged
under that hold. A refused job stays owed in the slot, so the next mount's
scan (BY-R1) re-drives it.

**BY-R5 — the slot is removed by its own record** (BY-Q3 (a)).
`ANCHOR_SLOT_REMOVE {index_oid}`, a new WAL record kind, written once every
free of the tree is durable; the slot table is compacted in place under the
anchor's latch. Replay is safe against older records: redo is gated by the
page LSN, and every `ANCHOR_UPDATE` for the oid precedes the removal in the
log. A removed slot frees its place for a later `CREATE INDEX`, which is
what lifts the cap. **Superblock 28 → 29**, older volumes refused (the
standing order of 2026-10-07): a binary that does not know the kind cannot
replay a log that carries it.

**BY-R6 — `REINDEX INDEX <name>` is a create and a drop in one autocommit
statement** (BY-Q1 (a), BY-Q2 (a), BY-Q4 (a)). Under the relation `X`, it
builds a tree for the index's definition (key and covered columns,
`kIndexFlagUnique`) under a **new** `index_oid`, with BX-R7's duplicate
check for a unique index; writes the new `sys.indexes` row under the same
name and its anchor slot; and drops the old row as `DROP INDEX` does. At
commit the old tree is queued (BY-R2). Inside `BEGIN` it is refused
`NotImplemented` with its byte.

**BY-R7 — a refused or failed `REINDEX` leaves the old index as it was.**
Its new tree is freed by BX-S6 when the backfill fails, and queued as an
orphan (BY-R2) when the failure follows `LogBuiltTree`; the old row, slot
and tree are untouched.

**BY-R8 — the name during the swap** (BY-Q5 (a)). `CheckIndexNameFree`
admits a name whose only holder is a row this transaction delete-marked,
and `FindIndexByName` returns, of the rows holding a name, the one with the
largest `index_oid` - the later row, since `sys.indexes`' row-id sequence
only rises, where scan order says nothing (a row may land in a retired slot
of an earlier page). Its own mark is still in flight to the asker, so the
unfiltered read returns both; the larger oid is the row the transaction
wrote last.
Scope, stated: this also admits `DROP INDEX x; CREATE INDEX x` inside one
explicit transaction, which is refused today.

**BY-R9 — what a reader sees.** Under the relation `X` no statement holds
the relation while the swap happens; a later statement resolves the new row
through the schema word (CT2), and BF-R9's epoch keeps the old tree's pages
until every statement older than the swap has ended. Results are identical
before and after (IX1, IX10a).

**BY-R10 — a settled slot with a live row is `Corruption`.** Once slots can
be removed, `InitTableAccess`'s fallback to the row's creation root
(`catalog.cpp:2616-2622`) must not apply to an index whose slot is gone: it
would name a freed page `[quiet-wrong]`. A live row with no slot is refused
`Corruption`.

---

## 3. What does not move

- **IX2 and IX14.** No entry leaves a live tree.
- **The read path, the write hook and BX's unique check.**
- **A relation drop's reclaim**, beyond BY-R3's cancel and its walk
  skipping a removed slot (there is none to walk).
- **`kFingerprintVersion`.** `REINDEX` is DDL and is not fingerprinted;
  the word is not reserved.
- **Assertions, foreign keys, Cabins.** None reads an index tree.

---

## 4. Questions — every one marked (a) by the operator (W1)

**BY-Q1 — the surface.** (a) `REINDEX INDEX <name>` only; (b) also
`REINDEX TABLE <t>`. CLA proposed (a). **Marked (a).**

**BY-Q2 — the rebuilt index's oid** `[quiet-wrong]`. (a) A new
`index_oid`, the name kept; (b) the same oid, its slot's root swapped. CLA
proposed (a): under (b) the old and the new tree share one owner stamp, so
a re-driven walk of the old tree could free a page the new tree took from
the free list, and once the slot is settled `InitTableAccess` would fall
back to a freed creation root (BY-R10). **Marked (a).**

**BY-Q3 — the anchor slot cap.** (a) Remove a settled slot by a new record
kind; (b) settle and reuse the slot in place; (c) leave the cap. CLA
proposed (b) - no new kind, though it changes how kind 26 is replayed and
so needs a bump too. **Marked (a), the operator's choice against (b)**:
BY-R5. At the cap `CREATE INDEX` is refused (§1), so the cap was a
refusal, never a failed `INSERT`.

**BY-Q4 — inside an explicit transaction.** (a) `REINDEX` in autocommit
only, refused `NotImplemented` inside `BEGIN`; (b) admitted, as
`DROP INDEX` is. CLA proposed (b). **Marked (a), the operator's choice
against (b)**: BY-R6.

**BY-Q5 — the name during the swap.** (a) The name check admits a name
whose holder this transaction delete-marked; (b) a temporary name. CLA
proposed (a). **Marked (a)**: BY-R8, with its scope.

**BY-Q6 — the measurement.** The close measures, on a relation whose
indexed column was churned, the index's entry count, page count and a
probe's server CPU before and after `REINDEX`, the pages `DROP INDEX`
returns, and the A/B claim that an ordinary `INSERT`/`UPDATE` on an
indexed relation is unchanged, within the hour's budget. **Marked (a).**

---

## 5. Stages

Each stage is reviewed by `critics-developer` and gated by the full suite.

**BY-S1 — the slot is removable and owed slots are found.**
`ANCHOR_SLOT_REMOVE` (kind, payload, redo, page-LSN gate), the slot table
compacted under the anchor's latch; superblock 29. The mount's owed-slot
scan (BY-R1: unfiltered, after undo) and BY-R10's refusal. Cells: remove
and replay, a removed slot's place taken by a later create, a crash after
the removal mounts without the slot, the owed predicate at mount for a
live, a committed-dropped, a rolled-back create's and an open (then
rolled-back) slot.

**BY-S2 — the index job.** BY-R2's queueing at commit and at a rolled-back
or failed-after-log create; BY-R3's one-owner rule; BY-R4's job, ending in
BY-R5's removal; the mount's re-drive. Cells: the pages return
(`allocated_pages`, `pages_freed`) only after the redo start passes the
decide and every older statement ends; a rolled-back drop owes nothing; a
rolled-back create's tree is reclaimed; the dropper's own inserts split the
root before commit and the whole tree is still reclaimed; a `DROP TABLE`
after a `DROP INDEX` whose job has not run frees each page once (a mutant
without BY-R3 fails it); crash cuts before, during and after the frees;
680 create/drop cycles on one relation never reach the cap.

**BY-S3 — `REINDEX INDEX`.** Grammar (the word not reserved), BY-R6's build
and swap, BY-R7, BY-R8's name rule and its scope. Cells: a churned index's
entry count falls to the live set's and every query answers
byte-identically before and after (the index contract suite gains a rebuilt
configuration); a unique index rebuilt and BX's checks hold after; a
refused rebuild changes nothing and frees its new tree; `REINDEX` inside
`BEGIN` refused; `DROP INDEX x; CREATE INDEX x` inside a transaction; two
cores, a reader and a writer arriving during the rebuild wait for the `X`
and then use the new tree; a crash between the swap's records mounts one
index or the other, never neither.

**BY-S4 — the close.** `docs/spec/index.md` §12 (BF-Q8 reversed, the slot
rules, `:720`'s retire/delete-mark), §10.1 (rebuild), §13; `drop-table.md`
DT1; `ddl-transactional.md`; `wal.md` (the new kind); the SQL manual
(`REINDEX`, `DROP INDEX` returning pages); `known-gaps.md`'s "Pages no root
reaches" list; `anchor_page.hpp`'s header; `CLAUDE.md`'s secondary-index
and superblock rows. The measurement of BY-Q6.

---

## 6. Where to start

Every question is marked (W1). BY-S1 is the first stage.

---

## 7. Stage log

**BY-S1 - landed 2026-10-16** on `worktree-reindex-drop-index-reclaim`.
`storage::RemoveAnchorIndexSlot` (the table compacted); `ANCHOR_SLOT_REMOVE`
(kind 33: payload, emitter, strict redo); superblock 29;
`Catalog::OwedIndexTrees` (BY-R1: unfiltered, one `sys.tables` scan, a
dropped relation's slots excluded, a rootless slot skipped) and
`Catalog::RemoveIndexSlot` (latched, logged, stamped, the anchor's type and
owner checked); BY-R10's `Corruption` for a live index with no slot.
**And `CreateIndex` writes the slot before the row** (the review's finding
1): BY-R10 read a row written before its slot - another core opening the
relation in that window - as `Corruption`; a slot with no row is read by
nothing while the instance runs and is owed after a crash. Cells: the slot
table at its cap, compacted and refilled; the payload through the envelope,
oid 0 refused at the writer and the reader; redo of the removal after the
updates that named the slot, and of an absent slot (`Corruption`); the owed
predicate for a live, a committed-dropped, a rolled-back-drop, a
rolled-back-create, a removed and a dropped relation's slot; a removed
slot's place taken through `CREATE INDEX`; BY-R10; two crash cells - a
removal replayed at mount, and a create open at the crash owed after undo.
`critics-developer`: accepted - the slot-first order, the root clause, the
writer's oid-0 refusal, one `CheckAnchorOf` for the five anchor readers'
structural check (three existing, two new), one `sys.tables` scan,
`memmove` compaction, the duplicate include, the two missing cells. Not
done - a cell forcing the row-before-slot window (no seam between the two
writes; the order makes it unreachable), and the redo tests' repeated anchor
`PAGE_INIT` setup. Suite: 4068/4068 before the review's fixes, 4070/4070
after. Overhead not measured; measured at the milestone's close.

**BY-S2 - landed 2026-10-16** on `worktree-reindex-drop-index-reclaim`.
`PageReclaimer` gains the index job (`Job::index_oid`): its anchor visit
reads the slot's root and takes nothing else, its clear removes the slot
(`NotFound` read as done), one `MakeJob`/`Enqueue` for every job kind;
`CollectAtMount` loads `Catalog::OwedIndexTrees`; BY-R3's
`DropIndexJobsOfRelationJobs` runs after every inbox drain, before any walk.
`CommandDispatcher` records a drop's and a create's tree at the statement
(`NoteOwedIndex`) and queues it at the decide (`EndDdlScopeById`): a drop
at its commit, gate past the commit; a create at any other ending, gate the
log's end. `exec::CreateIndex` seeds the slot when the catalog refuses
after the tree is logged (the review's C2). Cells
(`tests/index_reclaim_job_test.cpp`): nothing freed before the redo start
passes the drop, then the whole tree and the slot; a rolled-back drop owes
nothing; a rolled-back create's tree reclaimed after one checkpoint; the
dropper's own inserts grow the tree and the whole tree is reclaimed; the
relation's job ahead with the later gate owns the tree - a mutant without
BY-R3 fails it (its index job ends refused on the reused anchor; the
live-page check did not trip in that run); `DROP INDEX` then `DROP TABLE`
frees every page once; 699 create/drop cycles never reach the slot cap; a
job cut anywhere is finished by the next mount. Found while building: a
rollback's gate was one past the log's end, so the job waited a checkpoint
longer than it had to. `critics-developer`: accepted - C1 (the cell that
could not fail without BY-R3, rewritten), C2 (the failed-after-log create's
unnamed tree), `MakeJob`/`Enqueue`, the gate passed directly, one `Clear`,
the header's contract text. Rejected - reading a reused anchor as a finished
clear (it would also read a damaged anchor as one). Not done - moving the
test helpers into `reclaim_census.hpp`, an epoch-gate cell (one core's
dispatch holds no memo past a statement). Suite: 4077/4077 before the
review's fixes, 4078/4078 after. Overhead not measured; measured at the milestone's close.

**BY-S3 - landed 2026-10-16** on `worktree-reindex-drop-index-reclaim`.
`REINDEX INDEX <name>` parsed into `IndexStmt.reindex`, routed to
`HandleIndex`, on the archive's fail-closed list, refused `NotImplemented`
inside `BEGIN` before a scope opens (BY-Q4 (a)), and run under one
`DdlScope` and the relation `X`. `exec::ReindexIndex` delete-marks the old
row, then builds the definition from that row's own positions, widths and
flag - never names re-resolved, so a `RENAME COLUMN`, which holds no `X`,
cannot move it - checks it under the transaction's id, issues a new oid and
goes through `BuildAndWriteIndex`, the one door it now shares with
`CREATE INDEX` (split out of `CreateIndex(stmt)`). BY-R8 as built:
`CheckIndexNameFree` skips the asker's own mark, and `FindIndexByName`
returns the largest `index_oid` holding the name (the text of BY-R8 above
was aligned to the code). **`exec::DropIndex` and `ReindexIndex` act only
on the relation the `X` was taken for**: no relation is `NotFound`, an
index the name now finds elsewhere is `TxnConflict`. Cells
(`tests/index_reindex_test.cpp`, `index_reclaim_job_test.cpp`,
`ddl_fence_rig_test.cpp`, the contract suite's `LoadRebuilt`
configuration): a rolled-back and purged churn's entries gone, a new oid,
every query identical; a unique index rebuilt unique; refused inside
`BEGIN` with the transaction going on; an unknown name and `REINDEX TABLE`
refused; `DROP INDEX x; CREATE INDEX x` in one transaction, committed and
rolled back; another transaction's open drop still holding the name; at the
slot cap a rebuild refused `ResourceExhausted` with the old index, its
entries and the page count unchanged; the old tree reclaimed and the new
serving; a crash after the statement; **a crash at every record the
statement wrote**, over the data file as it was before it: every cut before
the commit mounts the old index, every cut past it the new; two cores, a
`REINDEX` on core 0 parking on a writer's `IX` on core 1, then holding its
row and serving core 1's probe and insert. **BY-R8's scope changed one
existing cell**: `CatalogNameRigTest`'s own-drop cell expected
`CREATE INDEX` after `DROP INDEX` in one transaction to be refused, and now
expects it admitted (renamed
`ItsOwnOpenDropAdmitsAnIndexAndRefusesARenameAsNotRetryable`; its rename
half stands); `catalog.md` CT7 says so. `critics-developer`, two passes.
First: accepted - B1 (a name that took no hold rebuilt), B2 (the rebuild
re-spelled by names, racing `RENAME COLUMN`), B3 (the "every surplus entry"
wording in the header and the contract cell), the BY-R8 text, the dispatcher
helpers `NoteIndexRowDropped`/`NoteIndexRowWritten`, `ExpectKeyword`, one
message-wrapping rule (`DropNamedIndex`), `PrepareIndexDef`'s unused
defaults, and the missing cells. Second: accepted - B1's hole in the
`DROP INDEX` arm (an unresolved name dropped under no hold and an empty
`std::optional` dereferenced; one `FindNamedIndex(stmt, relation)` now
serves both arms), `replaced_oid` in `IndexDdlResult` for the out-parameter,
`REINDEX` in the parser's unknown-keyword list, and the crash cell rebuilt
on record boundaries over a data file copied before the statement (the first
version cut at sixteenths and could not show a loser holding the new row).
Rejected - passing the row-change pointers unconditionally (the guard is the
create and drop arms' existing shape and changes nothing the helpers do),
moving `PrepareIndexDef` out of the header (it is one of the three seams
the header documents `CreateIndex` as). Not done - a cell for the name
moving between the dispatcher's lookup and the statement's (no seam between
two catalog reads on one reactor), and a `REINDEX` whose backfill fails
(`known-gaps.md`, Page reclamation). Suite: 4085/4086 before the reviews
(the own-drop cell), 4089/4089 after the first review's fixes, 4089/4089
after the second's. Overhead not measured; measured at the milestone's close.

**BY-S4 - the close, with the backfill fix merged, 2026-10-16.** The docs
(`index.md` §10.1/§12/§13, `drop-table.md` DT1, `ddl-transactional.md`,
`wal.md`, the SQL manual's `REINDEX` section, `known-gaps.md`, the
`CLAUDE.md` rows) under a `critics-developer` docs review: accepted - seven
corrections it made (`DropIndex` delete-marks under the implicit transaction;
§13 and the manual overclaiming what a rebuild sheds; the known-gaps commit
ids; the server manual's superblock 29; "queued", not "reclaimed", at the
decide; the `CLAUDE.md` wording), the backfill-failure overclaim, §10.1's
duplicate of §12, the manual's "past the next checkpoint", and the manual's
lock fault net (11 s, which is 1 s). Rejected - cutting the slot-cap limit
to one copy (the spec, the gap list and the manual have three readers).
**The review found the backfill walking every undo chain to its end**, into
undo the purge had recycled - refused `undo record at N has type 0` after
two `UPDATE` rounds, on `5f610082` and `9da66f0e` alike, which the close
measurement reproduced independently. On the operator's word ("merge into
BY"), `worktree-fix-backfill-walk-horizon` (`796ecf49`, unpushed since
2026-10-13) was merged: the walk stops where the oldest reader would
(`ResolvedForEveryReader`). Resolved by hand against BY and BX: every row
still unique-checked (the fix's early `continue` would have skipped BX's
check), an append failure kept apart from a read failure (the fix's excuse
would have swallowed one), the tree by reference for BX-S6's free,
`tuple_key` naming, `txns` threaded through `BuildAndWriteIndex` to both
`CREATE INDEX` and `REINDEX`. A `REINDEX` now sheds an `UPDATE`'s superseded
entries too: cell `ABuildOverRecycledUndoStopsWhereNoReaderReachesAndARebuildShedsUpdates`
(5,000 updates then a build of 1 entry; 100 more, 101 entries, rebuilt to
1) - its mutant (`unreachable_below` false) refused `has type 0`, 1 in 1.
`critics-developer` on the merge: no must-fix; accepted - the contract cell
whose refusal branch IX4b made unreachable, rewritten as a 1,500-version
chain under an open snapshot indexed whole; the in-visitor excuse folded
into the post-walk one; `CreateIndex`'s dead defaults; a duplicated comment;
`undo_log.hpp`'s "every version"; the spec's "never indexed", softened and a
known gap filed (a recycled record of another relation's same tuple key -
a refusal or a surplus entry, never a wrong answer). Suite: 4095/4095 on the
merge before the review's edits and after. Close measurement:
`bench/v3.2.0/results-by-close-v3.2.0-119-g9da66f0e.md`, on `9da66f0e`,
before the merge; the merge changes no `INSERT`/`UPDATE` path.
