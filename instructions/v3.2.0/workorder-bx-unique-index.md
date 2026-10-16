# Work order BX — `UNIQUE`: the unique index and the unique constraint

Written 2026-10-15 on `worktree-bx-unique-index` from `87d2893c`
(`v3.2.0-58-g87d2893c`), on the operator's word, recorded in English:

**CLOSED 2026-10-16 on the operator's word** (`raft-marks-2026-10-16.md`
§1), pushed to `main` at `0479cb29`; §7 is the stage log.

- **W0**: "Compose the milestone for the UNIQUE constraint and index."
- **W2** (2026-10-15): *"Q6 $!b, go ahead"* - BX-Q6 marked (b), BX-S6
  opened (`raft-marks-2026-10-15.md` §7).
- **W1** (2026-10-15): *"BX-Q1..Q5 $!a, go ahead"* - every question
  marked (a) by the operator, and BX opened. **BX-Q1 (a) is the operator's
  choice against CLA's proposal (b)**: `CREATE UNIQUE INDEX` is the one
  surface, `UNIQUE` in `CREATE TABLE` stays refused, and BX-S4 is cut.
  Q2..Q5 (a) are CLA's proposals (`raft-marks-2026-10-15.md` §6).

**Claim tags**: `[source-read]` with `path:line` at `87d2893c`; `[design]`
for reasoning about code not yet written; `[quiet-wrong]` where a wrong
choice turns a correct answer into a wrong one with no error.

**The letter.** `[source-read]` BW is the last work order under
`instructions/v3.2.0/`; BY was rejected as a letter at BN's writing
(`instructions/v3.0.0/index.md`, BN's row). BX is unused. CLA's choice.

**BX against other orders.** BX replaces IX11's refusal
(`docs/spec/index.md` §11) and leaves every other §11 refusal standing:
heap relations, nullable key columns, the tuple key, `float`, the column
caps, `ORDER BY` from an index, expression, partial and descending
indexes. Not part of AR0 §8's chain.

---

## 0. What BX is

A secondary index on a btree relation that **refuses a write which would
give two live rows the same key value**, declared by `CREATE UNIQUE INDEX`
and, if BX-Q1 says so, by `UNIQUE` in `CREATE TABLE`.

The reason it was refused is the whole of the design problem, and IX11
names it: uniqueness makes the index a **constraint**, which needs

1. a visibility rule for the check - which rows count as "holding" a key;
2. a second write-conflict path - two writers of one key on two cores,
   neither of which has appended yet when the other looks;
3. an answer to the index's own shape - entries are a **superset**
   (IX1, IX2): an index never removes an entry, so a key's run names rows
   that have since moved off it, been deleted, been rolled back or been
   purged.

BX answers each with machinery the engine already has, and adds one lock
unit. It changes nothing on the read path.

---

## 1. The tree as read `[source-read]` at `87d2893c`

- **The refusal is in two places.** The parser refuses `UNIQUE` after
  `CREATE` with its byte, `NotImplemented` (`src/parser/parser.cpp:3412-3421`);
  the catalog refuses the flag again (`src/catalog/catalog.cpp:3597`).
  `kIndexFlagUnique = 0x1` is defined and never written
  (`include/kds/catalog/rows.hpp:440`).
- **One write hook, two callers, and every write path reaches it.**
  `exec::MaintainIndexes` (`src/exec/index_maintain.cpp:206`) loops over
  the relation's indexes and appends; a relation with no index pays one
  `empty()` test (`:213`). It is called by INSERT after the row is placed
  and the Cabin witnessed (`src/server/command_dispatcher.cpp:5942`) and by
  UPDATE after the in-place overwrite (`:8677`). The backfill calls
  `AppendIndexEntry` directly (`src/exec/index_ddl.cpp:114`). The KWP load
  stream goes through `ExecuteInsert` (`src/server/kwp_load_server.cpp:526`);
  the sorted-fill fast path is heap-only and requires a relation with no
  index (`command_dispatcher.cpp:5254-5255`). **The row is in its page
  before its entry is in the index**, on both write paths. An UPDATE
  appends when a key **or a covered** column moved (`:8673-8675`).
- **A wait is a re-run, and only a statement that wrote nothing re-runs.**
  `EndWrite` re-runs a refused statement after a yield only when it wrote
  nothing (`command_dispatcher.cpp:9684-9688`); UPDATE and DELETE can also
  park mid-walk on an undecided holder (`:8825-8837`, `:10472`), and
  `ExecuteInsert` re-runs nothing (`:9701`). So an INSERT can wait only
  before its first row is written.
- **A failed append fails the statement** and inside a transaction fails
  the transaction (`docs/spec/index.md` §2.1). The INSERT path takes the
  placement back on that failure (`command_dispatcher.cpp:5952`,
  `take_back`).
- **The latest-state check exists.** `txn::CheckVisibility`
  (`include/kds/txn/visibility.hpp:144`) against a view minted at check time
  (`MintCheckView`) is what the foreign-key check uses
  (`docs/spec/foreign-keys.md` §4): visible and live, visible and
  delete-marked, or in flight.
- **The lock family has four units** - relation, range, slice, tuple
  (`include/kds/txn/lock_table.hpp:195`), in a nesting order the enum's
  order states (`:193`); a unit's identity is `(rel_oid, unit, lo, hi)`
  (`:266`). None names a key value. The compatibility table is by mode
  only (`:229`). **`LockKey::Overlaps` compares `rel_oid` and the interval
  and not the unit** (`:~305`), and a fence scan uses it.
- **An index oid and a relation oid are different number spaces.** An
  index's oid is `sys.indexes`' row id (`src/catalog/catalog.cpp:3700`,
  `src/exec/index_ddl.cpp:292`); a relation's comes from `GenerateUserOid`,
  from 4000 (`include/kds/catalog/well_known.hpp:264`). One number can name
  both.
- **A writer meeting an undecided holder waits for its decide** (AX-S1,
  AY-S2); a named tuple key meeting an undecided insert of the same key
  waits (BD-Q9). Waits enter the wait-for graph and the deadlock detector.
- **The index tree's coverage check rests on "no index entry is removed"**
  (`docs/spec/index.md` §12.2, IX14). BX removes none, so the premise holds.
- **The superblock is 26** (`include/kds/server/superblock.hpp:159`) and
  refuses every other version.
- **The equal-sort-keys bug is open**
  (`docs/inflight/bugs/a-run-of-equal-index-sort-keys-promotes-one-separator-twice.md`):
  about 600 entries with one `(key, tuple key)` refuse that row's later
  appends for good. It is fail-closed and reaches a unique index exactly
  as it reaches any other. **Fixed since `32beda89`** (merged at
  `228fc689`, after BX closed) and its entry deleted; see BX-R11.

---

## 2. Rulings proposed `[design]`

**BX-R1 — uniqueness is a property of the latest state, never of a
snapshot.** A key value is *held* by a row when that row's version on the
page, read under a freshly minted check view, carries the value and is
not delete-marked. This is `foreign-keys.md` §4's rule, used unchanged:

| the row's version on the page, against a fresh check view | verdict |
|---|---|
| writer visible, not delete-marked, carries the key | **held** - the write is refused |
| writer visible, delete-marked | free |
| writer visible, carries another key | free (a surplus entry) |
| no row at the tuple key (rolled back, purged, never placed) | free |
| writer not visible (in flight) | **busy** - BX-Q3 |

The transaction's own pending versions are visible to its own check by the
ordinary rule, so `DELETE` then `INSERT` of one key in one transaction
passes, and the second row of a multi-row `INSERT` naming the first row's
key is refused.

**BX-R2 — the check walks the key's whole run and re-reads every row.**
An equality probe for the encoded key returns a superset: surplus entries
(IX1), and with a string column longer than `kIndexStringKeyBytes`, other
values sharing the prefix (IX6). The check deduplicates by tuple key,
resolves each, and compares the **full** column values of the resolved
version against the written ones. A match on the encoded key alone is
never a violation `[quiet-wrong]` - it would refuse two distinct long
strings.

**BX-R3 — two writers of one key are serialized by a key unit**
(BX-Q2 decides how long it is held). A new unit, `LockUnit::kIndexKey`,
named under the **relation's** oid - so the writer's `IX` on the relation
is its intention, and an index oid, a different number space (§1), never
stands where a relation oid is read - with an interval that is a hash of
the index's oid and the **encoded** key, masked to 63 bits so
`hi = lo + 1` cannot wrap. A writer that writes a key of a
unique index takes it `X` **before its check** and holds it at least until
its entry is appended. A hash collision costs a wait, never an answer: the
check still compares values. The unit draws wait-for edges, the deadlock
detector sees it, and nothing of it is persisted.

The unit sits outside the relation-range-slice-tuple nesting: it is a
descendant of none, takes no intention on anything, and **a fence never
overlaps it**. A key unit shares its relation's oid with every fence on
that relation, so `Overlaps` must compare the unit as well, or a
whole-range `X` fence `[0, 2^48)` meets every key unit whose hash falls
inside it - spurious waits and deadlocks, never a wrong answer.

**BX-R4 — the check runs before the row is placed.** A refused write has
written nothing: no placement to take back, no entry, no log record. The
order on both write paths becomes key unit, check, placement, append.

**BX-R5 — only a write that moves a key column of a unique index is
checked.** The gate is the key columns, not the append: an UPDATE that
moves only a covered column still appends (§1) and is neither checked nor
given a key unit. DELETE checks nothing. A relation with no unique index
runs exactly the code it runs today: the branch is one test of the index's
flag inside the existing loop.

**BX-R6 — a violation is `AlreadyExists`**, naming the index, the key's
columns and values, the conflicting tuple key, and the byte of the value
that carried it (for a multi-row `INSERT`, the row's ordinal). Not
retryable. Inside a transaction it fails the transaction, as every other
statement failure does (`docs/spec/txn.md`).

**BX-R7 — `CREATE UNIQUE INDEX` verifies while it builds.** The backfill
already runs under the relation's `X` (AT-S5e), so no writer is open: each
row's latest version is its committed one. The build appends every version
(IX10a, unchanged) and, **as built in BX-S2**, asks `CheckUniqueKey` of each
row's latest version once that row's entries are in - so of every pair the
later row finds the earlier one, with memory one run, never a set of every
key. That is one index descent per row, and O(n²) entry visits for `n` rows
whose string keys share their 32-byte prefix (IX6); the single key-order
walk this ruling first proposed would cost one pass, and was set aside for
the reuse. A duplicate refuses the create `AlreadyExists` naming both tuple
keys, the tree unreachable and no `sys.indexes` row written - IX10a's
"complete or absent" covers it with no new rule. The unreachable tree's
pages are freed since BX-S6 (BX-Q6 (b)).

**BX-R8 — the format moves.** The flag is written, so a binary that
ignores it would admit duplicates into a volume that promises none
`[quiet-wrong]`. Superblock 26 → **27**, older volumes refused, per the
standing order of 2026-10-07 (no backward compatibility). `SysIndexRow`'s
size does not change.

**BX-R9 — recovery checks nothing.** Redo replays committed state, which
was unique when it committed; losers are rolled back at mount with no
writer open. The key unit is memory-resident like every other.

**BX-R10 — the read path does not move.** An equality on every key column
of a unique index stays `kIndexProbe`, search-class, sorted to tuple key
order, its run walked whole: the run still holds surplus entries, so the
probe still returns a set, and IX8's argument is about the set.
`IsTrailReplayable`, Waystone, Cabins and the inner build are untouched.

**BX-R11 — the equal-sort-keys bug is not a gate.** It is fail-closed (a
refusal, never a duplicate admitted), it reaches every index today, and BX
does not make it more reachable. Its entry gains one line: a unique index
inherits it. **Moot since `32beda89`** (merged to `main` at `228fc689`, after
BX closed): an index entry's sort key is the whole entry, so a run of one
`(key, tuple key)` divides cleanly, the bug entry is deleted, and no index -
unique or not - refuses that row's writes.

---

## 3. What does not move

- **IX2.** Maintenance stays append-only, so IX14's coverage premise stands.
- **The tuple key and the foreign key.** The clustered tree's uniqueness is
  the descent's; a foreign key still references the parent's tuple key
  only.
- **`kFingerprintVersion`.** `UNIQUE` already parses as a word; DDL is not
  fingerprinted.

---

## 4. Questions — every one marked (a) by the operator (W1)

**BX-Q1 — the surface.**
- (a) `CREATE UNIQUE INDEX <name> ON <t> (<cols>) [COVERING (...)]` only.
- (b) (a), plus `UNIQUE` as a column constraint and `UNIQUE (<cols>)` as a
  table constraint in `CREATE TABLE`, each creating a unique index in the
  same transaction under a derived name (`<table>_<col>[_<col>]_key`,
  refused `AlreadyExists` if the name is taken).
- (c) (b), plus `ALTER TABLE ... ADD CONSTRAINT ... UNIQUE`.

The tension: the manual refuses a second spelling of something that
already has one (`manual/sql/sql.md`, "No cross-dialect shims"), and (b)'s
`UNIQUE` is a second spelling of (a). Against it: (b) is the standard's
own spelling, not another dialect's, and W0 names the constraint beside the
index. **CLA proposes (b)**, built as its own stage (BX-S4) so it can be
cut without touching the rest; (c) is refused `NotImplemented` naming
`CREATE UNIQUE INDEX`. **Marked (a) by the operator (W1)**: BX-S4 is cut,
and `UNIQUE` in `CREATE TABLE` keeps its refusal.

**BX-Q2 — how long the key unit is held** `[quiet-wrong]`.
- (a) **Released once the row's entry is appended.** By then the row and
  its entry both exist, so a second writer's check finds the row in flight
  and takes BX-Q3's path - whichever of placement and append ran first.
  A writer holds one key unit at a time.
- (b) **Held to the decide.** A second writer waits on the unit itself.
  Every unique-key write keeps an entry against `max_locks_per_txn`
  (65,536) until commit, so a transaction writing more than 65,536 keys of
  a unique index is refused `ResourceExhausted` - half that with two unique
  indexes - where an omitted-key load meets no such ceiling today.
- Not offered: a check under the index leaf's exclusive hold with no unit.
  A key's run can straddle leaves (IX4b), so two writers can hold two
  leaves of one run.

**CLA proposes (a)**: as sound as (b), with no new ceiling.

**BX-Q3 — what a busy verdict does.** A busy verdict - an in-flight row
holding the key, or a refused key unit - can be waited out only where the
engine can wait (§1).
- (a) **Use the waits that exist.** UPDATE parks mid-walk on the holder and
  checks again at its decide. A statement that has written nothing is
  re-run after a yield (`EndWrite`), which waits for a single-row INSERT
  and for row 1 of a multi-row one. **Row 2 onward of a multi-row INSERT,
  and every KWP load row past a chunk's first, is refused `TxnConflict`
  (retryable)**, failing the transaction.
- (b) (a), plus INSERT resume: an INSERT parks mid-statement on the holder
  as UPDATE does. A change to `ExecuteInsert` that reaches every INSERT,
  not only those on a uniquely indexed relation.
- (c) Refuse every busy verdict `TxnConflict`.

**CLA proposes (a)**, the refusal stated in the spec and the manual. (b) is
its own work and can follow BX; (c) refuses writes that would pass once the
holder rolls back, where the engine has turned refusals into waits since AO.

**BX-Q4 — when the check runs.**
- (a) **Per row, immediately** - PostgreSQL's default for a non-deferrable
  constraint.
- (b) At the statement's end.
- (c) Deferrable to commit.

**CLA proposes (a)**. It is the only one the write hook can do without a
second pass, and it refuses statements the standard admits:
`UPDATE t SET k = k + 1` over consecutive `k` fails on the first row whose
new value another row still holds. The spec and the manual say so.

**BX-Q5 — NULL in a unique key.**
- (a) Keep the refusal: a nullable key column is refused at
  `CREATE UNIQUE INDEX` as at `CREATE INDEX` (`docs/spec/null.md`).
- (b) Admit it, NULLs distinct from one another (the standard's default).

**CLA proposes (a)**. (b) is a change to the index's key encoding (§5's
discriminator byte) and to the null spec, and is its own decision.

**BX-Q6 — a refused unique build's pages** (raised at BX-S2's review;
**marked (b) by the operator on 2026-10-15**, W2).
A failed `CREATE INDEX` leaves its partial tree allocated and named by
nothing (`index.md` §12); before BX only a device or structural failure got
there, and a refused `CREATE UNIQUE INDEX` over duplicates now does on every
attempt.
- (a) Leave it, stated in `known-gaps.md`: the leak is bounded by the
  relation's size per attempt.
- (b) Free the partial tree on a failed build: its pages were never logged
  and nothing names them, so the BF free primitive (`page.md` §5) could
  take each - which is a reading of BF's rules nobody has ruled on.
- (c) Check before building: sort every live row's key in memory first,
  allocating nothing on a refusal, at the cost of memory proportional to
  the relation.

**CLA proposed (b)**, after the operator's ruling that freeing never-logged
pages is within BF. **Marked (b) (W2)**: built in BX-S6.

---

## 5. Stages

Each stage is reviewed by `critics-developer` and gated by the full suite.
The overhead is measured once, at BX-S5, within the time budget of
2026-10-13 (about one hour, building included).

**BX-S1 — the key unit and the check, no SQL surface.**
`LockUnit::kIndexKey`, its `LockKey` constructor (63-bit hash) and its
`LockUnitName`; `Overlaps` and the fence scans compare the unit (BX-R3);
the nesting comment states the unit sits outside the hierarchy.
`exec::CheckUniqueKey`: probe the encoded key, deduplicate by tuple key,
resolve each under a minted check view, compare full values, answer
held / free / busy with the holder. Cells: every row of BX-R1's table,
a string pair sharing a 32-byte prefix (free), a surplus entry after an
UPDATE moved the key (free), a rolled-back and a purged key (free), a
relation's whole-range `X` fence and a key unit of that relation whose hash
falls inside it (no conflict). A hash collision is a wait at the
dispatcher, so its cell is BX-S2's.

**BX-S2 — `CREATE UNIQUE INDEX`, one core.** The parser's and the
catalog's refusals replaced; the flag written; superblock 27; `sys.indexes`
(and so `SHOW INDEXES`) prints `unique`. The backfill's duplicate check
(BX-R7). INSERT and UPDATE take the unit, check before placement, refuse
(BX-R4, BX-R6), gated on key columns (BX-R5); the KWP load stream is
covered through `ExecuteInsert` (§1). Cells: the refusals, the multi-row
`INSERT` ordinal, `DELETE` then `INSERT` in one transaction, the
`k = k + 1` refusal of BX-Q4, a covered-only UPDATE taking no unit, an
explicit transaction failed by a violation, a KWP load chunk carrying a
duplicate, the index contract suite's byte-for-byte reads extended with a
unique index (reads identical with and without it).

**BX-S3 — across cores, and across a crash.** Two-core cells: two
sessions inserting one key (one commits, the other is refused after its
wait; one rolls back, the other passes), row 2 of a multi-row INSERT
meeting an in-flight holder (`TxnConflict`, BX-Q3), a delete on one core and an insert
of the key on the other, a deadlock between two writers each holding the
other's key (one refused naming deadlock). Each cell forced with a barrier
and repeated, never a single run (`mutation-testing-concurrency`). **As
built**: a crash-and-mount cell on the file-backed rig in place of the
simulation harness - the sim's oracle answers every errored `INSERT` as an
unnamed row it then permits, so unique refusals would weaken its checks
rather than test BX - and a KWP load chunk carrying one key twice.

**BX-S4 — cut** (BX-Q1 (a), W1). `UNIQUE` in `CREATE TABLE` keeps its
refusal.

**BX-S6 — a failed build's tree freed** (BX-Q6 (b), W2). On a failed
backfill - before `LogBuiltTree`, so no record names a page and no row
names the tree - `FreeUnbuiltTree` walks the partial tree from the root the
backfill grew it to, through the walk `LogBuiltTree` shares (only index
pages carrying the index's oid, none twice), and frees each page through
`FreePage`. Best-effort, and a leak where it falls short. Cell: a refused unique build over 2,000 rows, three times, leaves
the allocated page count where it was; a mutant without the free fails
it.

**BX-S5 — the close.** `docs/spec/index.md` IX11 rewritten as the rule BX
built, §8's "there is no unique index" sentence and §13's list updated;
`manual/sql/sql.md` (the refusal list, the grammar, the per-row check of
BX-Q4, the mid-INSERT `TxnConflict` of BX-Q3); `docs/spec/client-manual.md`;
`docs/spec/txn.md` §5 (the fifth unit); the bug entry's line (BX-R11);
`CLAUDE.md`'s secondary-index row.
The overhead, interleaved A/B in `build-release` from `87d2893c` to the
close: an INSERT/UPDATE mix on a relation with a non-unique index (the
claim is no cost) and on one with a unique index (the cost of the unit and
the run walk, stated).

---

## 6. Where to start

Every question is marked (W1). BX-S1 is the first stage.

---

## 7. Stage log

**BX-S1 - landed 2026-10-15** on `worktree-bx-unique-index`.
`LockUnit::kIndexKey` (`LockKey::IndexKey`, 63-bit mask; `Overlaps` excludes
it), and `exec::EncodeUniqueKey` / `UniqueKeyUnit` / `CheckUniqueKey` /
`UniqueViolation` in `index_maintain.{hpp,cpp}`, the key encode shared with
`AppendIndexEntry` through one helper. 17 cells in
`tests/index_unique_test.cpp`; a mutant of `Overlaps` fails both key-unit
cells. `critics-developer`: no S1 bug; accepted - the encode loop and the
moved test merged into one helper (`UniqueKeyMoved` deleted), the redundant
finalizer cut, BX-R3's text corrected to the code's naming, a purged-key and
a DATE-key cell added. Rejected - a shared `IndexLayoutOf` across
`step_vm.cpp` and `index_ddl.cpp` (outside BX; the helper stays local to
`index_maintain.cpp`), reusing another file's FNV-1a (each is file-local).
Deferred to S2 - `BorrowOrWait`'s refusal text for a key unit, the
hash-collision cell. Suite: 3951/3951 before the review fixes; after them
3952/3953, the one failure
`DropTableReclaimServingTest.ATwoCoreMountReclaimsOnceEveryCoreHasCheckpointedWhileServing`,
the load flake `known-gaps.md` records, 10/10 alone. Overhead not measured;
measured at the milestone's close.

**BX-S2 - landed 2026-10-15** on `worktree-bx-unique-index`. `CREATE UNIQUE
INDEX` parses (`UNIQUE` before anything but `INDEX` refused at its byte);
the catalog admits `kIndexFlagUnique` and refuses any other bit;
`IndexRef` carries `unique` and the name; superblock 27; `sys.indexes`
prints `unique`. The build checks each row's latest version once its
entries are in (BX-R7 as built). The dispatcher's `UniqueKeysOf` /
`CheckUniqueKeys` / `UniqueKeyHolds`: an INSERT checks before its
admission, holding each key unit until the row's function returns; an
UPDATE stops its walk or its tuple-key probe at a row that moves a unique
key, checks with no page held, and resumes at the row (BX-R4). 14 SQL cells
in `index_unique_test.cpp`; the index contract suite gains a unique column
(`code`) in every indexed configuration, five probes on it, all
byte-identical to the plain baseline; the golden corpus pins
`CREATE UNIQUE INDEX` as `ok`. Mutants disabling the INSERT check (7 cells
fail) and the UPDATE check (2 cells fail) are caught. `critics-developer`:
no wrong-answer bug; accepted - the backfill decodes each row once, the
dead `held_before` deleted, `UniqueKeysOfRow` an alias, the parser's two
branches merged, a stale comment and the `AlreadyExists` byte rule
commented, BX-R7 amended to what was built. Recorded rather than fixed -
a refused unique build leaks its partial tree on every attempt (BX-Q6,
open; `known-gaps.md`), since freeing never-logged pages needs a ruling on
BF's rules. Rejected - a hash-collision cell (no seam forces one; a
collision is a wait the check then passes). Moved to BX-S3 - the KWP load
cell and the mid-walk park. Suite 3967/3967. Overhead not measured;
measured at the milestone's close.

**BX-S3 - landed 2026-10-15** on `worktree-bx-unique-index`.
`tests/index_unique_rig_test.cpp`: on the two-core rig, an insert parked on
an undecided insert, move-off and delete of its key, each answered by a
commit and by a rollback; a multi-row `INSERT`'s row 2 refused
`TxnConflict` inside the fault net; an `UPDATE` parked mid-walk resumed at
the holder's rollback (exact rows) and refused at its commit (row 1 taken
back); the placement-to-append window, writer A paused by
`SetAfterPlacementForTest` - **without the key unit it fails 3 of 3**, B
admitted as a duplicate; a two-writer deadlock refused naming it; a
crash-and-mount cell (flag, built tree and committed keys survive, a
loser's key does not). The KWP refusal cell gains a unique index and a
duplicate row, its other refusals keeping their codes. Every cell 3 runs
green. **One engine change**: a key that does not encode is not checked,
so a NULL or mistyped key keeps the row's own refusal, wording and byte
(`AKeyThatDoesNotEncodeKeepsTheRefusalTheRowAlwaysHad`).
`critics-developer`: accepted - the window cell's B key was one the seed
held (the mutant's failures came from a tuple-key duplicate, not the
unique one) and its flags outlived the hook; the mid-walk and multi-row
checks made exact; a positive park sign; the delete and rollback cells;
one refusal matcher; the KWP cell folded into the existing one; the
trivial `sys.indexes` check cut. A first positive sign, a wait-for edge,
was wrong - this park records none - and its failed assertion left core
0's hook unreleased, which hung a run for about two hours before it was
stopped; the sign is now "started and not ended in 150 ms" and a guard
releases the hook on every exit. Rejected - a KWP busy-row cell (the same
`ExecuteInsert` path the multi-row cell pins). Suite 3980/3980. Overhead
not measured; measured at the milestone's close.

**BX-S5 - the close, 2026-10-15** on `worktree-bx-unique-index`. Docs:
`index.md` §10.2 (IX11 as built) and its §8/§10/§11/§12/§13 lines;
`txn.md` §5 (the fifth unit); the SQL manual; `client-manual.md`,
`protocol.md`, `assertion.md` and CN-8 no longer call unique indexes
unbuilt; the equal-sort-keys bug entry; `CLAUDE.md`'s row.
`critics-developer` on the docs: accepted - `SHOW INDEXES` has a `unique`
column, not `unique=1`; the refusal is at the next token's byte; the bug
line names its commit; four stale "no unique index" statements; §13's
NULL line; the manual's 600-update defect; padding cut and the `CLAUDE.md`
row trimmed to a link. Left - CN-3's quotation, a dated concept note.
**Overhead** (`bench/v3.2.0/results-bx-close-v3.2.0-62-g9bf559e2.md`,
`v3.2.0-58-g87d2893c` against `v3.2.0-62-g9bf559e2`, `relaxed`, one core):
an ordinary index costs about +0.3 µs per statement (about 1 %, every
cell inside the A-A' control's spread); a unique index adds +5.0 to
+5.5 µs per `INSERT` (+19 to 22 %) and +6.6 to +7.6 µs per `UPDATE` of its
column (+23 to 27 %), flat from 200 to 10,000 rows, and nothing measurable
to an `UPDATE` of another column. Cut, as the file states: multi-row
`INSERT`, `strict`/`group` durability, two cores, the build, covering and
composite keys, a sampler split, the PostgreSQL floor. BX-Q6 stayed open,
and was then marked (b) (BX-S6).

**BX-S6 - landed 2026-10-15** on `worktree-bx-unique-index`, after the
merge of `origin/main` at `e1a5133e`. A failed backfill frees its partial
tree (`FreeUnbuiltTree`); `CollectTreePages` is the one walk it and
`LogBuiltTree` share. Cell
`ARefusedUniqueBuildGivesItsPartialTreeBackEveryTime`: three refused
builds over 2,001 rows leave `allocated_pages` unchanged; a mutant without
the free fails all three. `critics-developer`: the precondition holds
(nothing logs or names a tree page before `LogBuiltTree`; a failed divide
never marks a root). Accepted - the walk refuses a page that is not an
index page carrying the index's oid, or one reached twice, so `FreePage`
is never handed another relation's page or an id reissued in between; one
walk for logging and freeing; no retry loop on `kDeferred` (a deferral is
the documented leak). **And an older bug the review found in the backfill,
fixed here**: a failed append of an undo-chain version stopped the walk
with `OK`, so the build succeeded with no entry for that version - a row an
older snapshot would miss `[quiet-wrong]`; the failure now fails the
build. No cell forces it: an append fails only on a full pool, the
equal-sort-keys refusal or a failed divide. The softened claim - `FormatRoot`
failing would leak the root - is moot: `CheckIndexLayout` refuses that
layout first. Overhead not measured: the change runs only on a failed
build.

**BX closed - 2026-10-16** on the operator's word (`raft-marks-2026-10-16.md`
§1). Pushed to `main` at `0479cb29`, the hook skipped: the full suite ran
green on the build inputs pushed (4055/4055 at `7ea52998`; the last merge
brought `instructions/` only). Left open, each recorded where it lives:
`ON CONFLICT` against a unique index (`known-gaps.md`), INSERT resume
(BX-Q3 (b)), NULLs in a unique key, `UNIQUE` in `CREATE TABLE`, a foreign
key referencing a unique column. No tag, no version.
