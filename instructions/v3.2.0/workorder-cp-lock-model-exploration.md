# Work order CP — the lock model re-examined: modes and units against what KDS contends on

Written 2026-11-07 on `worktree-cp-lock-model-exploration` from `6c4b9a6d`
(`v3.4.7`), on the operator's word, recorded in English
(`raft-marks-2026-11-07.md` §1):

- **W0**: *"A milestone opens here. First we re-explore the relations among
  the locks - `IS`, `S`, `IX`, `U` and the like. It is an exploratory,
  experimental milestone. The gist: not the conventional lock system, but
  one that is new, fits the KDS system better, is optimized, and keeps
  correctness - that is what we research."*

**Written 2026-11-07; opened 2026-11-08** on the operator's marks
(`raft-marks-2026-11-08.md` §1, W1: *"CP-Q0..Q9 `$!a`, proceed with S0"*):
**CP-Q0..CP-Q9 are all marked (a)**, CLA's proposal in each (§4), and CP-S0
started the same day from `cc54a296` (`v3.4.7-1-gcc54a296`, build inputs
`6c4b9a6d`'s); **CP-S0 landed at `c2937a32` and CP-S1 on 2026-11-08 on the
tree after `d7b75977`** (§7, §8), S1 carrying three items to the operator
before S2 (CP-Q10..CP-Q12, §8.10). Reviewed on 2026-11-07 by the `critics-developer` pass (§7,
the opening review): the census corrected, two missed risks written into C2
and C4, one candidate (release before durability) withdrawn because the
tree contradicts its premise. **Exploratory on W0**: CP-S0 (the census and
the premise measurement) and CP-S1 (the model) decide which candidates of
§3 are built; a candidate not taken is recorded with the number that
refused it, not built.

**Claim tags**: `[operator]` for W0; `[source-read]` with `path:line` at
`6c4b9a6d`; `[measured]` with its results file or, where the file left the
tree, the order that keeps the number; `[design]` for reasoning about code
not written; `[quiet-wrong]` where a wrong choice turns a refusal or a
wait into a wrong answer with no error; `[loud]` where a wrong choice
turns a wait into a retryable refusal the client sees.

**The letter.** `[source-read]` CJ, CK and CL are on `origin/main` at
`6c4b9a6d`; CM and CO are taken by orders and worktrees of other sessions
(`git worktree list`); CN is the concept notes' prefix (`CN-4`). CP is the
first free letter. CLA's choice; the operator may reletter it.

**CP against other orders.** AO (`instructions/v3.0.0/workorder-ao-m2-lock-family.md`,
M2) built the family to AR2's design
(`instructions/v3.0.0/ar2-architecture-revision-borrow-model.md`), and
`docs/spec/txn.md` §5 owns the contract. CP re-reads both against the
engine as it is now - every write on the session's core (AT-S5), a
foreign key's `S` (AY-S5), a unique index's key unit (BX), an online index
build under `IS` (CI), wait events and ASH (CJ) - and amends them where it
lands. The consumers whose contracts CP keeps or re-states by name:
`docs/spec/foreign-keys.md` §2a-§3a, `docs/spec/index.md` §10.2 and
§10.3, `docs/spec/ddl-transactional.md` §5e-§5f, `docs/spec/drop-table.md`
DT7, `docs/spec/heap-and-tuple.md` §4.1 (BD-R6). Not part of AR0 §8's
chain.

---

## 0. What CP is

The lock family has four modes and five units, and the question AO's close
left standing (`workorder-ao-m2-lock-family.md` §AO-8, carried item 5) is
**which of those cells has a contender on this engine at all**. CP answers
it from the tree and from measurement, then asks of every cell that has
one: *what does it protect that nothing else protects* - the MVCC header
under the page latch, catalog MVCC and the statement epoch, the page latch
itself - and *what is the cheapest shape that protects exactly that*.
What the answer licenses is a model, not a tuning: cells with no
contender go, cells the engine pays for and nothing reads go, a cell a
measured contention needs and the matrix lacks is added - the operator's
`U` is one such question - and the structure that holds the cells is
re-shaped where the cells left make a different structure cheaper.

**The method is CF's**: measure first, inside the standing budget; build
only what the measurement says; land a candidate only if it improves the
shape it names and costs nothing outside the spread elsewhere (CP-Q7).
**The bar is correctness unchanged**: every candidate lands behind the
family's existing interface with every result byte-identical - the four
contract suites, every `lock_*`, `*_wake_rig_*` and `fk_cross_core_rig`
cell green or amended with its reason stated - and **no wait becomes a
refusal** unless the operator marks that change by name (CP-Q4). The
two-source model (`txn.md` §5: the header names the row's last writer,
the table holds what has no header) is either kept or replaced by an
argument as explicit as `lock_table.hpp`'s publish-then-verify one.

---

## 1. The tree as read `[source-read]` at `6c4b9a6d`

### 1.1 Modes, units, tables

- Four modes, `IS`, `IX`, `S`, `X` (`include/kds/txn/lock_table.hpp:214`);
  five units, relation, range, slice, tuple, index key (`:206`), the
  first four nested, the fifth outside the nesting (BX-R3). No `SIX`, no
  update mode: AR2's E1, **ratified by the operator** (AR2-A, 2026-09-03).
- The compatibility and covering matrices are `constexpr` tables
  (`:241`, `:254`; C2's table below carries the 4×4 as its first four
  rows). **Three sites filter by mode name before the matrix is read**:
  `IsFence` counts an interval unit as a fence only in `S` or `X`
  (`src/txn/lock_table.cpp:26`), `FenceCoversKey` skips any holder not in
  `S` or `X` (`:673`), and `ConflictingOverlap` skips `IS` and `IX`
  holders (`:733-736`); `AcquireInner`'s verify gate (`:339-353`) reads
  `IsFence`'s answer and the unit, not a mode name (corrected at S1,
  §8.11). A mode added to the enum compiles past
  all three - and past the two tables as well: `kLockModeCount` sizes
  them and nothing asserts on them, so an unfilled row compiles and reads
  `false` (found at S1, §8.3 A; the `static_assert` block S2b adds is the
  guard).
- One table for the instance, 64 partitions at every core count keyed by
  `(rel_oid, unit, lo)` (`lock_table.hpp:339`, BP-S6), a `std::mutex`
  latch per partition null at `cores = 1`, entries as vectors scanned
  under the latch; a striped fence counter per relation beside them
  (AO-R3's probe gate); the wait-for graph under one instance-wide
  `wait_latch_`, gated by an atomic edge count (`lock_table.cpp:378-434`).
- The engine's ask is `TryAcquire`, non-queueing: a refusal names one
  holder and leaves a wake registration; the statement parks outside
  every page span and **re-runs** (`lock_table.hpp`, "The engine's ask").
  A wake is not a grant, the re-check is mandatory (AO-R4). `Acquire`,
  the queueing ask, has no engine caller.
- Nothing persisted (AO-R3, the operator's decision of 2026-09-03): no
  page bit, no record kind; a crash releases every lock by rolling the
  loser back.
- The fault net defaults to 1 s (`lock_table.hpp:375`,
  `lock_wait_fault_net_ms`), restarted by a registered build's progress
  (`holder_progress.hpp`); it is a defect's end, never a wait's. The cap
  defaults to 65,536 entries per transaction (`:333`,
  `max_locks_per_txn`), refused `ResourceExhausted`, never escalated.
- **A commit releases its borrows before its sync under `group`**:
  `WalManager::Commit` returns after the append for `kGroup`
  (`src/wal/manager.cpp:441-445`), `TransactionManager::Commit` then runs
  `FinishCommitBody` (`src/txn/manager.cpp:398`; `:363` is the deferred
  commit's call), which publishes, retires, moves the schema word and
  releases (`:306-336`), and only the acknowledgement waits on the batch
  (the group commit's pending mark `command_dispatcher.cpp:11771-11774`,
  waited at `:890-913` for `DispatchAsync` and `:1235-1240` for `Dispatch`;
  `wal.md` §1; `txn.md` §4 "When a commit becomes visible": *"a reader sees
  a commit as soon as the record is appended, which under `group` is before
  the platter has it"*). Borrows are held across a sync only by `strict`'s
  deferred commit above one core (`DefersCommit`, `:1141-1145`, BA-S7).

### 1.2 Every ask, by what it protects

Every site in `src/` and `include/` that names a `LockKey` or calls
`TryAcquire` (`grep -rn 'TryAcquire(\|LockKey::'`, the page latch's own
`TryAcquire` excluded), grouped by what the ask protects:

| # | site | unit, mode | scope | what it protects | what else protects it |
|---|---|---|---|---|---|
| A1 | `BorrowChain`, `src/server/command_dispatcher.cpp:11254-11263` (the intention above every unit ask); the relation unit asked directly, stopping at the intention (`:11307`): `InsertParsed` `:6216`, `HandlePurge` `:12344` (BH-R4, the one `IX` with no unit beneath it), `HandlePurgeDroppedColumns` `:12466`; the CI drain reads the entry's `IX` holders at `:2712` (`IntentionWriters`, `lock_table.cpp:523-526`) | relation `IX` (a write), `IS` (a foreign key's parent read) | transaction | a DDL's relation `X` waits for every open writer (AT-S5e); the first intention is where the statement's memo must be current; CI-R2's drain snapshots the `IX` holders | nothing: a writer maintaining an index a build is announcing, or writing rows into a relation a drop is tombstoning, is kept out by this alone |
| A2 | `ReadBorrow::Position`, `include/kds/server/read_borrow.hpp:97`, at every bind (AT-S1); `CREATE TABLE ... REFERENCES` on the parent, `:5154` | relation `IS` | statement; the DDL transaction | a granted reader is not overtaken by a DDL `X` for the statement's length | catalog MVCC, the statement epoch (`drop-table.md` DT1), no DDL moving data: the refused arm is right by these, so the defence is one-directional (`read_borrow.hpp`'s header) |
| A3 | `ReadBorrow::Position`, `read_borrow.hpp:120`, from `src/exec/step_vm.cpp:2089` at **every leaf page** of the outermost walk | slice `IS`, `[min_key(page), kIdSpaceEnd)` | the page, moved at each boundary: one `TryAcquire` and one `ReleaseOne` per page | **nothing today.** Its consumer was a mover (AO-R12, AR2-R13) and none exists (`heap-and-tuple.md` §7.1, BZ). No conflict test reads it: `ConflictingOverlap` skips intention holders on interval units (`lock_table.cpp:733-736`), `FenceCoversKey` counts `S`/`X` holders only (`:673`), `IsFence` excludes it from the counter (`:26`), a slice `IS` triggers no verify (`:339-353`), and a DDL's `X` meets the reader at the relation entry (A2). `LockUnit::kSlice` and `LockKey::Slice` appear nowhere else in `src/` or `include/` but the table's own definitions (`lock_table.hpp:291` the factory, `:313` `IsFenceUnit`, `lock_table.cpp:38` the name switch); `read_borrow.hpp:120` is the one consumer | - |
| A4 | `BorrowRelationForDdl`, `:11162-11177` | relation `X`: `DROP TABLE`, `CREATE`/`DROP INDEX`, `REINDEX`, `CREATE ASSERTION`, `CREATE TRANSITION`, `DROP TRANSITION` (`:4101`), `ADD`/`DROP COLUMN` (`:3513`), `CREATE`/`DROP CABIN` (`:3176`), the two renames; relation `IS` for an online `CREATE INDEX` (CI) | the DDL transaction | the DDL against open writers (A1) and positioned readers (A2) | nothing for the writers |
| A5 | the per-row `UPDATE` and `DELETE`: `:10147`, `:12782` - **asked only when the statement declared no range** (`if (!declared.has_value())`, `:10146`, `:12781`): under A7's range `X` the walk asks no tuple unit and the row conflict is the header check alone, so A5 and A7 are alternatives per statement, not layers | tuple `X` | transaction | (i) the wait **with a kick** on a row held by another core's transaction (AX-S2b); (ii) being seen by a range fence's verify (A7); (iii) refusing a foreign key's `S` (A6) and a reverse check's probe (A6'); (iv) the arbiter's row read "under its `X`" in `ON CONFLICT DO UPDATE` (`:6701-6705`) | **the header**: first-updater-wins under the page latch is the lock, and the table "adds the wait and the `S` mode, not the lock" (AO-R3) |
| A5' | the `INSERT`: every issued key `:7096`, a named key `:7037`, `ON CONFLICT DO NOTHING` on the tuple key `TupleKeyPlaced` `:6445`, `ON CONFLICT DO UPDATE`'s up-front tuple-key unit `:6660` and its arbiter row `:6698` | tuple `X` | transaction | BD-R6: two statements naming one key serialise on its row unit (`:7065-7069`); an issued key against a named one (`:7083-7090` the comment, `:7091-7100` the loop); BD-Q9's wait on an undecided insert of the same key; `TupleKeyPlaced`'s "granted: no other writer holds the key" (`:6452-6457`) | the descent under the leaf hold and `UndecidedInsert`'s header read; without a table "every borrow is granted" and the placement answers (`:6452-6457`) |
| A6 | the foreign-key forward check, `:4543` | tuple `S` on the parent row, under relation `IS` | transaction (given back on a failed check, AZ-S5) | the parent row's existence while a child writer is open: a parent `DELETE`'s `X` waits for it (D9(a)). **Its verdict relies on the `S` having refused every writer**: a busy verdict under a granted `S` "means no lock table" and with one "refuses retryably, defensively" (`:4556-4562`; `foreign-keys.md` §2a) | nothing |
| A6' | the reverse check's wait, `WaitForChildRowWriter` `:4775-4815` | a **probe** `S` on the child row, given back at once | the ask | a wake from the child writer's `X`: a refused probe parks on it; a granted one installs a pre-flipped slot and the statement re-runs at once (`:4797-4803`) | `NoteBlockingWriter`'s poll of the holder (`:4814`) |
| A7 | `DeclaredWriteBorrow`, `:11409-11441` | range `X`: `[0, kIdSpaceEnd)` for no `WHERE`, else the folded tuple-key window of two or more keys | transaction | the statement's whole walk sees one state; the wait is before the walk, with nothing written (AO-S6c-c, the operator's mark of 2026-09-08). `CheckWriteConflictBlocking`'s header (`:11621-11625`) says why the header alone cannot give this: the MVCC check "is silent about a transaction holding a range over the key that has not reached the row yet" | the header, per row, mid-walk - and then the statement that already wrote rows is refused, not parked (C4) |
| A8 | the unique check, `:11576-11578` (the key built at `src/exec/index_maintain.cpp:292`) | index key `X` | from the check to the append | two writers of one unique value cannot both find it free (BX, `index.md` §10.2) | nothing |

**Eight cells of twenty are live**: relation `IS`/`IX`/`X`, slice `IS`,
tuple `S`/`X`, range `X`, index key `X`. **Twelve have no caller**:
relation `S`; range and slice `S` (AR2's assertion row - "the slice of
the group key, `S` while checking" - was never built as a lock; the
assertion registry's reservations are its own, `assertion.md` §6.1);
slice `X` (the mover's cell, whose absence is why slice `IS` is dead);
range and slice `IX`; range `IS`; tuple `IS`/`IX`; index key
`IS`/`IX`/`S`. Of the live eight, **one is dead weight** (A3): paid at
every leaf page of every outermost walk, read by nothing. **`U` is
absent**, and so is every statement that would ask for it in the
conventional sense: the parser has no `SELECT ... FOR UPDATE`
(`grep -rn 'FOR UPDATE' include/kds/parser src/parser` is empty), and a
KDS reader takes no row lock at all (A2, A3), so the `S -> X` conversion
deadlock a `U` mode exists to break has **one** live instance in the tree
- the foreign key's (§1.3).

### 1.3 What the family is for, read off the census

Three jobs, and every live cell does one of them:

1. **The wait with a kick** (A5, A5', A6', and every refusal's slot). A
   row's conflict is decided by the header; what the table adds is a slot
   a release flips and a kick across cores. Before AX-S2b that wait was a
   poll of `IsInFlight`, re-read only when the waiter's reactor ran.
2. **The intention rule between DDL and statements** (A1, A2, A4): a
   relation entry where a DDL's `X` meets every writer's `IX` and every
   reader's `IS`, so no unit beneath it is looked at.
3. **The fences that have no header** (A6, A7, A8): a parent row's
   existence, a key window, a unique value - each a claim over something
   no tuple header names.

Job 1 is the one the census questions most sharply: **what still polls has
no slot behind it, and a holder only the header names is on that list**
(`known-gaps.md`, Locks). The row's table entry is what gives the wait a
slot - and it gives it to the *row*, where the thing being waited for is a
*transaction's decide*. §3's C4 is that observation made into a candidate.

**The one conversion deadlock.** `foreign-keys.md` §2c states it: *"Two
transactions that each write a child of P and then update P deadlock"* -
both hold `S` on P (A6), both ask `X` on P (A5), and the second is refused
naming deadlock; before AY-S5 the two updates serialised. *"Insert a
trade, then update its account"* is the shape, pinned by
`FkCrossCoreRigTest.TwoChildWritersThatThenUpdateTheirParentDeadlock`
(`tests/fk_cross_core_rig_test.cpp:471`). **Scenario 0 has the shape but
cannot show the contention**: its key is declared only under `--fk` and
its four statements form one transaction only under `--txn`, both off by
default (`tools/scenario0_stockmarket.py:1142`, `:1154`), and every trader
owns a disjoint partition of accounts (`:86-88`: "two traders never touch
one account"), so no two sessions write one parent by construction. §2c's
first cost is the same cell: *"every `UPDATE` of a parent row waits while
a child writer of it is open"*, because the `S` protects the row's
**existence** and the `X` an `UPDATE` takes says more than it means.

`[design]` KDS makes this split cleaner than PostgreSQL's (`FOR KEY SHARE`
against `FOR NO KEY UPDATE`): a foreign key references the parent's
Keystone id only (`foreign-keys.md` §1, "no parent column"), and the tuple
key is never updatable (invariant 11), so "a write that changes what a key
share protects" is exactly `DELETE` - never any `UPDATE`.

### 1.4 Prices measured so far `[measured]`

- **AO-S7** (2026-09-09, `v2.7.0-304-g5e94dc8`; the results file left
  with the bench tree, the order's §AO-S7 keeps the numbers): the family
  under 3 % of a statement, contended minus disjoint inside the lock arm's
  own floors, **zero refusals** at 8 and 32 sessions - on an engine where
  two writers of one relation could not meet (every write ran on the
  owner core; AT-S5 ended that). E12's slice `IS` was **extrapolated**,
  never measured: "≲ 3 µs × pages, under 1 % of a point read, linear in
  pages on a scan".
- **BP-H10**: `64 × cores` partitions cost +12 % of a statement's CPU at
  eight cores under a held fence - the all-partition walk `FenceCoversKey`
  runs for every tuple ask while any range fence stands on the relation.
  Fixed at 64 (BP-S6); the walk itself stands.
- **BX's close** (`workorder-bx-unique-index.md:487-488`): a unique index
  +5.0-5.5 µs per `INSERT`, +6.6-7.6 µs per key `UPDATE` - the key unit's
  lock and the check.
- **D9(a)'s costs** are stated, not measured (`foreign-keys.md` §2c): the
  parent `UPDATE`'s wait, the conversion deadlock, the cap reached at half
  the rows by a bulk insert naming distinct parents, a DDL on the parent
  waiting for child writers' `IS`.
- **CA-R1** (`bench/v3.2.0/results-ca-r1-visibility-v3.2.0-154-g7112d4a1.md`):
  scenario 2 at 2,108 booking ops/s and 1,537 committed TPS under `group`,
  fsync-bound. **The reason it gives - *"the hot row's lock held across the
  group fsync"* (`known-gaps.md:1417`) - is contradicted by the tree**
  (§1.1's last bullet): under `group` the row's `X` is released before
  the sync, and what each booker waits for is its own acknowledgement in a
  closed loop. S0 confirms with a two-session cell and corrects the entry
  (§3, "Withdrawn at the review").
- **CJ** gives the instruments and no lock number yet: `app.lock_wait`
  (timed, `p1` the holder) and `app.lock_conflict` (a count at the table's
  two refusal exits, `lock_table.cpp:228`, `:363`), `sys.lock_waits`
  (unit, mode, holder, outcome: granted / deadlock / fault net /
  cancelled), `sys.ash` and `tools/ash_panel.py`. No tally counts lock
  asks; `kFenceWalks` (`include/kds/base/contention.hpp`) is the family's
  only one.

### 1.5 What stands open in the family, and which candidate acts on it

`known-gaps.md` (Locks) and AO-8's carried items hold the full list; the
two a candidate acts on:

- **A wake is not a grant**: a third writer can take a unit between the
  flip and the re-run, and under sustained contention a waiter ends at the
  1 s net naming the last holder; "a stream of online builds can keep an
  `X`-taker waiting, since a waiting `X` has no queue position" - C5.
- **What still polls**: a holder only the header names, an assertion's
  group, a `PURGE`'s horizon wait - C4.

Left where they are (§3, "Not candidates"): the mutual-refusal pair, the
wake-registration residue, the one-directional `IS`, the detector's one
edge, the stated-not-enforced latch order.

---

## 2. The question, stated so a cell can fail it

A `(unit, mode)` cell is **kept** iff both hold on this engine:

- (i) it has a **contender** - a second transaction that asks a mode the
  matrix refuses against it, reachable from a statement a client can send
  (not only from a cell); and
- (ii) it **protects something no other mechanism protects** - a result,
  an invariant, or a wait the header, catalog MVCC, the statement epoch
  or the page latch does not already provide.

A cell that fails (i) is dead code: its matrix rows go. A cell that
passes (i) and fails (ii) is **cost**: it goes unless the wait it gives is
worth its price (A3 is the test case: it fails both). A **new** cell is
added iff a measured contention or a stated deadlock exists that the
matrix cannot express without it - `U` is judged by this rule and no
other. **The structure** that holds the cells is re-shaped iff the cells
left standing make a cheaper one sound; the argument for soundness is
written before the code, in S1. **And a wait never becomes a refusal by
accident**: every candidate below names the waits it moves and the one
candidate that would turn a wait into a refusal (C4 (ii)) carries it to
the operator as `[loud]`.

---

## 3. Candidates `[design]`

Each: the hypothesis, the correctness argument, the risk, what S0
measures, the rule that keeps or drops it. Written in the order CLA
expects them to matter; S1 re-orders on S0's numbers.

### C1 — The slice `IS` goes; the relation `IS` stays

**Hypothesis.** Every outermost walk pays two partition-latch sections per
leaf page (A3) for an entry nothing reads. On a scan the cost is linear in
pages; AO-S7 extrapolated ≲ 3 µs a page.

**Correctness.** `[source-read]` No reader of the unit exists (A3's last
column), and the reader's own correctness never rested on it
(`read_borrow.hpp`'s header). No test asserts a walk's slice entry;
`lock_family_test.cpp:483` and `lock_table_test.cpp` take slices directly
and keep passing. The relation `IS` keeps everything the bind-time defence
gives (A2). A future mover re-adds a position unit when one exists;
`txn.md` §5 says so in place of the present text.

**What goes with it**: `walk_page_min_key` (`step_vm.cpp:1825-1833`,
`:1887`) and the `:2068-2091` report, `ReadBorrow`'s `rel_`/`slice_`
state (`read_borrow.hpp:101-130`), `PositionSink`'s `lo`/`hi`, and the
slice rows of `lock_table.hpp`'s unit list if S1 finds the unit has no
other caller left (the cells that take slices directly are then amended
or deleted with the reason stated).

**Risk.** None for results. **E12 is ratified** (AR2-A: "`IS` at the
position's finest lock unit (R14); taken; without it no move can wait"),
so deleting it is the operator's (CP-Q3), not CLA's.

**S0 measures** a `SELECT` walk over ~100-1,000 leaf pages against a
measurement build whose `Position` declines slices (cell M2). **Rule**:
deleted on CP-Q3 (a) whatever M2 shows - a unit with no consumer is not
kept for being cheap; M2 reports the gain.

### C2 — The `U` mode: an `UPDATE` claims a version, a `DELETE` claims existence

**Hypothesis.** The tuple `X` an `UPDATE` takes (A5) says "this row may
cease to exist", which only a `DELETE` means. Splitting the mode resolves
§1.3's conversion deadlock and §2c's first cost at the matrix, where they
live.

**The mode.** `U` on a tuple or a range, taken by `UPDATE`'s per-row path
(`:10147`), by `ON CONFLICT DO UPDATE`'s arbiter row (`:6698`) and by
`UPDATE`'s declared window (A7); `X` kept for `DELETE` (`:12782`), for
every `INSERT` ask of A5' - including `ON CONFLICT DO UPDATE`'s up-front
tuple-key unit (`:6660`), taken before the lookup and covering an insert -
and for `DELETE`'s declared window. The matrix gains one row and column:

| held \ want | `IS` | `IX` | `S` | `U` | `X` |
|---|---|---|---|---|---|
| `IS` | yes | yes | yes | yes | no |
| `IX` | yes | yes | no | no | no |
| `S` | yes | no | yes | **yes** | no |
| `U` | yes | no | **yes** | **no** | no |
| `X` | no | no | no | no | no |

`U` covers `S` and `IS`; `X` covers `U`. **What changes and what does
not**: `UPDATE` against `UPDATE` (`U`/`U`) and `UPDATE` against `DELETE`
(`U`/`X`) refuse as today - the header's first-updater-wins decides the
re-run as before. `UPDATE` against a foreign key's `S` proceeds; `DELETE`
against it waits, as D9(a) requires. Two child writers that then update
their parent: the first `U` is granted over the other's `S`, the second
`U` waits on the first's decide - **no cycle**. A bulk `UPDATE` over a
window holding a parent row proceeds past the child writers' shares; a
bulk `DELETE` waits for them.

**Three consumers of "every writer holds `X`" that the mode has to
re-state**, found by the opening review (§7):

1. **The forward check's busy verdict** (A6, `:4556-4562`). Today a busy
   parent under a granted `S` cannot happen with a table, and the code
   refuses it "retryably, defensively". Under C2 it happens whenever
   another open transaction holds the parent in `U`, and the child
   `INSERT` would be refused `TXN_CONFLICT` where it waits today -
   `[loud]`, and "insert a trade, then update its account" is the shape.
   **The rule C2 adds**: a busy verdict whose in-flight writer holds a
   version with a committed, live predecessor of the same key - an
   `UPDATE` in flight - reads **present**: the row's existence and its key
   are certain whichever way that writer decides, since no `UPDATE`
   changes either (§1.3). An in-flight `INSERT` (no committed predecessor)
   or an in-flight delete-mark holds `X` and refused the `S` before the
   verdict was read, so busy under a granted `S` has exactly one cause,
   and it is the one the rule answers. **S1 adds the head's delete-mark
   bit to the test** (§8.3 C.1): an in-flight delete-mark also has a
   predecessor, so the predecessor alone is not the witness.
2. **The reverse check's probe** (A6', `:4791-4796`). It asks `S` on the
   child row to be refused by the child writer's `X` and parked with a
   wake; under C2 a child `UPDATE` holds `U`, `U` admits `S`, the probe is
   granted, and the parent `DELETE` re-runs at once on a pre-flipped slot
   - a hot loop over the whole child relation until the child decides.
   **The probe asks `U`**: refused by the child's own writer in `U` or
   `X`, admitted by a grandchild writer's `S` on that row, given back on a
   grant as today.
3. **The fence test** (§1.1's second bullet). `IsFence`, `FenceCoversKey`'s
   skip and `ConflictingOverlap`'s skip name modes (the verify gate reads
   `IsFence`'s answer; corrected at S1); a range `U` would raise
   no counter, run no verify and be invisible to every tuple probe, so two
   overlapping declared `UPDATE`s, or a point write inside an `UPDATE`'s
   window, would no longer wait before the walk. **The rule becomes "an
   interval unit in any non-intention mode is a fence"**, written once,
   and the three sites read it; `LockModeName` gains the name.

**Correctness.** `[source-read]` premises: the `S` protects the row's
existence and its key (`foreign-keys.md` §2c); an `UPDATE` carries every
cell into a new version of the same key (BN-R3) and never the key
(invariant 11); the reverse check runs only for `DELETE`, under its `X`;
AY-S6's Cabin-set argument rests on the `DELETE`'s `X` meeting the child
writer's `S`, which stands; `PURGE` retires delete-marked rows only
(`heap-and-tuple.md` §4.1c) and asks no row unit; neither `ON CONFLICT DO
UPDATE`'s update arm, Transitions nor `PURGE DROPPED COLUMNS` changes a
row's existence or key. **The `[quiet-wrong]` to keep in view**: any
future write that changes a row's *existence or key* under `U` would pass
a key share - `txn.md` §5 names the rule (`U` is a version claim) so the
next verb is placed under the right mode.

**Risk.** E1 is ratified ("no update mode in v1"); CP-Q2 amends it. The
two matrices are `constexpr` tables, but an unfilled row compiles and
reads `false` (S1, §8.3 A: nothing asserts on them and the build has no
`-Werror`), so S2b adds the guard; the three name-filter sites compile
past a fifth mode too, which is why item 3 makes them one rule.
`sys.lock_waits` gains a mode name; the wait-event dictionary carries no
mode (corrected at S1).

**S0 measures** M3a: the two-session rig cell is the contender (it
exists); scenario 0 under `--fk --txn` prices the `S` on the uncontended
path and shows zero contention by construction (§1.3); a shared-account
arm needs a driver flag, taken only if `bench/README.md` admits a driver
change inside a measurement stage and the budget holds, and named in the
file either way. **Rule**: built if the rig cell's deadlock or §2c's
wait has a contender a client can reach - which `FkCrossCoreRigTest`
already states - and item 1's rule is adopted with it; recorded otherwise.

### C3 — A per-relation lock header: the relation entry and its fences under one latch

**Hypothesis.** Every write's `IX` and every bind's `IS` (A1, A2) is a
hash, a partition latch, a vector scan and a ledger push, twice (ask and
release); and every tuple ask walks all 64 partitions while any range
fence stands on the relation (BP-H10's +12 % at eight cores). One
structure per relation - the relation entry's holders and waiters **and**
the list of fences over it, under the relation's own latch - does both
jobs in one section: the intention rule unchanged, "any fence over key
`k`" answered without a walk. Not a counter: CI-R2 enumerates the `IX`
holders (`:2700`), and a writer parked on a DDL's `X` needs a slot a
release flips.

**Correctness.** The intention rule is unchanged; only where the entry
lives moves. The publish-then-verify argument (`lock_table.hpp`) is
between a fence's publish and a tuple's, and holds for any structure in
which both publish under a latch before either scans - **so C3's fence
list presumes rows still publish**, and C4 decides whether they do (its
(ii)). The ordering rule - a partition latch taken under a page latch,
never parking under one - holds for the relation's latch as for a
partition's. **What needs an argument**: the structure is the instance's,
so it is reached through an instance-wide map keyed by oid (each core's
catalog memo is its own), and it outlives `DROP TABLE` as long as any
holder or waiter names it.

**S0 measures** M1 (the relation ask's share of an uncontended point
write) and M5 (a held `UPDATE ... WHERE id > k` beside point writers:
`kFenceWalks` and the point write's CPU against the no-fence arm).
**Rule**: built only if M1 puts the relation ask, or M5 the walk, at or
above a point write's floor at this box's core count; otherwise recorded.

### C4 — The row's table entry made lazy: wait on the decide, not on the row

**Hypothesis.** AO-R3 says the header is the row lock and the table adds
the wait. The wait is for a **transaction's decide**, and every in-flight
transaction already has a slot's worth of identity: its entry in the
instance's in-flight tables (`InstanceVisibility`, 1,024 per core). Give
each in-flight transaction **one decide slot**, flipped where `Release`
runs today - in `FinishCommitBody` after `RetireInFlight` **and**
`MoveSchemaWordIfCatalogWriter` (`src/txn/manager.cpp:325-336`, whose
comment says why the schema word must move before a waiter wakes) and at
`Abort` (`:611`, `:626`) - and a writer refused by a header can park on
*the holder* with a kick, which closes the "a holder only the header names
still polls" gap, and **the per-row `X` entry of A5 - `UPDATE` and
`DELETE` - stops being needed for job 1**. **Scope**: A5 only. Every
`INSERT` ask of A5' keeps its `X`: BD-R6's "two statements naming one key
serialise on its row unit", the issued-against-named serialisation and
`TupleKeyPlaced`'s grant argument all read the row unit and are not
re-stated by CP.

**What the row entry then still does, and whether each needs it.**
(i) the wait's kick: the decide slot's. (ii) **being seen by a fence's
verify**: without row entries `ConflictingOverlap` cannot find an
undecided `UPDATE`/`DELETE` inside a window. Today a point writer inside a
held window reads the counter, finds the fence and parks before writing;
a fence asked after a point write finds the row and parks before its
walk; **neither statement is refused**. Without the row's publish the
fence is granted over a row writer that read the counter as zero, meets
the row mid-walk under its original view (`UpdateParsed`'s resume,
`:10659-10671`), and `CheckWriteConflict` (`src/txn/manager.cpp:196-206`)
refuses it `TxnConflict` **with rows already written** - inside `BEGIN`
the session is poisoned (`:10646-10652`), under repeatable read it is
refused at once (`kFutile`, `:11660-11663`). `[loud]`: a wait becomes a
retryable refusal, and AO-S6c-c's "waits before it walks, having written
nothing" (the operator's mark of 2026-09-08) no longer holds for that
race. **Two ways to keep it**, S1's to choose with the operator (CP-Q4):
(α) a row writer publishes its entry **while the relation's fence counter
is nonzero** and otherwise not - which closes every case but the race of
a writer that read zero against a fence published after, and that race
needs a grace period (the fence-taker waits out every statement that
began before its publish, which the per-core statement epoch already
counts); or (β) the change is accepted and stated in `txn.md` §5. (iii)
**refusing a foreign key's `S`**: a `DELETE` still asks `X` on the tuple
and finds the `S` entry; an `UPDATE` asks nothing and so passes it - C2's
effect, by omission; C2's forward-check rule (item 1) is then needed
here too. (iv) **the reverse check's probe** (A6'): a child writer holding
no entry grants the probe at once, so the wait moves to the child
writer's decide slot, which the header names (`:4813`'s `holder`) - the
same mechanism as (i). (v) the arbiter's "under its `X`" read
(`:6700-6703`) is `INSERT`'s and keeps its unit.

**What it costs the model.** `UPDATE` and `DELETE` rows leave
`max_locks_per_txn` (the cap then counts inserts, fences, shares,
relations and keys; §2c's "half the rows" cost stands for a bulk
`INSERT`); `sys.lock_waits` rows for a row wait name the holder and no
unit; the deadlock detector's edges are already transaction-to-transaction
and move nothing. **Fairness** stays as it is: a woken waiter re-runs and
races a newcomer at the header, as it races one at the table today (C5).

**Correctness.** The re-check stays mandatory and the slot stays
level-triggered: a waiter registering on a transaction already retired
finds no slot and re-runs at once; a slot is identified by `(core, index,
trx id)` so a reused index cannot be mistaken for the transaction waited
for. The flip's place in `FinishCommitBody`'s order is `Release`'s today
and unchanged; a flip placed before `RetireInFlight` would let a woken
writer re-run against a holder still in-flight to the window and refuse
itself (AX-R2's argument - a spurious refusal, not a wrong answer), and
one placed before the schema word would let it re-run through a stale
schema boundary (`manager.cpp:327-329`).

**S0 measures** M1 (the row entry's share of a point write) and M3's
wait outcomes. **Rule**: built if M1 puts A5's ask-and-release at or above
a point write's floor, or if M3 shows header-named waits ending at the
net; recorded otherwise. **CP-Q4** marks (α) or (β).

### C5 — Fairness: a wake that is a reservation

**Hypothesis.** Under a hot row the waiter woken by a decide races every
newcomer and can lose to the 1 s net (§1.5). A release that hands the unit
to its oldest waiter - a reservation the re-check still has to confirm
against the header - ends starvation without making a wake a grant.

**Risk.** The waiter's statement is torn down and re-run under a new
transaction id in autocommit, so a reservation to the waiting id reserves
for a transaction that no longer exists; the reservation would have to be
the session's, which the table does not know. **S0 measures** M3's
`sys.lock_waits` outcomes under scenario 2's bookers and M4's contended
arm: the count of `fault_net` rows. **Rule**: nothing built while that
count is zero on the cells run (CP-Q9); recorded otherwise as a design
for its own order.

### Withdrawn at the review: release before durability

The draft's C7 proposed releasing the hot row's `X` after the commit
record's append rather than its sync, on CA-R1's reading that scenario 2's
bookers serialise on that `X` across the group fsync. **The tree does
that already under `group`** (§1.1's last bullet): the release precedes
the sync and only the acknowledgement waits, so the booking rate is each
session's acknowledgement latency in a closed loop, not a lock. Under
`strict` above one core the deferred commit holds its borrows across its
sync by design (BA-S7), and nothing here reopens that. What CP owes
instead is in S0: a two-session cell confirming the order, and
`known-gaps.md:1417` corrected. Recorded for completeness: the reader
exposure the draft tagged `[quiet-wrong]` - a session reading another's
commit before it is durable - already exists under `group`, as it does
under every group-commit protocol, and a guard for it would have to wait
on the snapshot's commit-LSN ceiling rather than on the versions
returned; it is not a lock question and is not CP's.

### Not candidates, named so they are not read as forgotten

- **The detector's one edge and `wait_latch_`**: no number says they
  cost; S0's ASH shows `wait_latch_` waits or CP leaves them.
- **The mutual-refusal pair** and the **wake-registration residue**: C4
  removes the row-side cases of both; the fence-side case stays and is
  recorded.
- **The one-directional `IS`** (A2) and the **stated-not-enforced latch
  order**: design facts, not costs.
- **A persisted lock bit** (AO-R3): no candidate above needs one; CP
  does not reopen it (CP-Q6).
- **`SELECT ... FOR UPDATE`**, `SERIALIZABLE`, savepoints: features, not
  asked, out of scope (`txn.md` §9).

---

## 4. Items for the operator (CP-Q), CLA's proposals

**Marked 2026-11-08: (a) on every item** (`raft-marks-2026-11-08.md` §1,
the operator's `$!a` over CP-Q0..CP-Q9). The table keeps the options as
they were put, so a later amendment reads against them; CP-Q2 and CP-Q3
amend AR2 E1 and E12 by this mark, and S1 carries the amendments to the
documents named in its row (§5).

| # | item | options | CLA proposes |
|---|---|---|---|
| CP-Q0 | the letter | (a) CP; (b) another | (a) |
| CP-Q1 | what "new" admits | (a) **re-derive the cells from contenders** (§2), keep the two-source model and the family's interface, land each candidate behind it; (b) replace the lock table wholesale; (c) tune the present model only | (a) - (b) is argued for only if S0 leaves fewer live cells than a table is worth, which S1 then proposes as its own item |
| CP-Q2 | the `U` mode (C2) | (a) **built**: `UPDATE` takes `U` on its tuple and its declared window, `DELETE` and every `INSERT` keep `X`, the fence rule becomes "an interval unit in a non-intention mode", the forward check reads an in-flight `UPDATE`'s parent as present and the reverse probe asks `U`, E1 amended by this mark; (b) not built, C4's omission alone relieves the `UPDATE`-against-`S` wait; (c) neither | (a): it is the mode-level truth whichever structure holds it - an `UPDATE`'s window must not exclude a key share - and it is the mode the operator named |
| CP-Q3 | the slice `IS` (C1) | (a) **deleted**, the relation `IS` kept, E12's unit un-ratified with the spec saying a mover re-adds a position unit; (b) kept, taken once per 64-page slice; (c) kept as is | (a) |
| CP-Q4 | the row entry made lazy (C4) | (a) **built if M1 or M3 says so, with (α)**: a row entry published only while a fence stands on the relation, the fence-taker waiting out the statements that began before its publish, so no wait becomes a refusal; (b) built with (β): the fenced bulk statement's wait moving from before its walk to the first undecided row it meets, a retryable refusal where today both wait, stated in `txn.md` §5 - `[loud]`; (c) not built | (a) |
| CP-Q5 | release before durability | (a) **withdrawn**: the tree already releases before the `group` sync; S0 confirms with a cell and corrects `known-gaps.md:1417`; (b) kept as a priced item | (a) |
| CP-Q6 | AO-R3, no persisted bit | (a) stands; (b) reopened | (a) |
| CP-Q7 | the bar for landing a candidate | (a) **CF's**: a measured gain on the shape it names, no cost outside the spread elsewhere, the four contract suites byte-identical, every lock cell green or amended with its reason, no wait turned into a refusal but by CP-Q4 (b); (b) a gain alone | (a) |
| CP-Q8 | the measurement budget | (a) the standing rule (20 min a stage, about an hour the milestone); (b) lifted for S0 | (a); S0's cells are cut to fit and the file says what was cut |
| CP-Q9 | fairness (C5) | (a) **nothing built** while M3 and M4 count no `fault_net` row; (b) a reservation built | (a) |

**Added by S1 on 2026-11-08 (§8.10), unmarked** - the `[loud]` and
ratification choices the model met, per §5's gate; CP-Q10 gates S2b:

| # | item | options | CLA proposes |
|---|---|---|---|
| CP-Q10 | `[loud]` the class of deadlocks `U` adds (§8.3 F.1): a transaction granted `U` on a parent row over a child writer's share, that then waits on that child writer **for any reason** - a `DELETE` of the same row, a write of another row the child wrote, a per-row statement parking mid-walk - deadlocks with it if the child writer then writes the parent row, one of the two refused retryably naming deadlock; today the updater waited at the parent row first, holding nothing there, and the child's write of the row proceeded | (a) **accepted**: `U` built as §8.3 states it, the class stated in `txn.md` §5, `foreign-keys.md` §2c and `CLAUDE.md`'s row, pinned by two rig cells (the same-row and the cross-row instance); (b) not accepted: CP-Q2 re-marked (c), no `U`, §2c's conversion deadlock and the parent `UPDATE`'s wait stay | (a): the shape removed - two child writers that each update their parent, the "insert a trade, then update its account" pair - is the commoner OLTP shape and §2c's stated cost; the class added needs a parent updater that goes on to wait on the very child writer whose share it passed, is a genuine cycle correctly detected, and is PostgreSQL's (`FOR NO KEY UPDATE` over `FOR KEY SHARE`); no scheme that lets the `UPDATE` proceed over the share avoids it |
| CP-Q11 | E4 and AR2-R6 ("Slice key: `(rel_oid, [lo, hi))`, page as hint only", ratified) describe a unit C1 deletes, while the property they ratify holds of the range unit and of every unit | (a) **re-read as the range unit's** by a dated amendment of their rows, the cell at `lock_table_test.cpp:351` re-pinning it over a range; (b) E4's row left as history of a deleted unit | (a) |
| CP-Q12 | C3's rule reads "at or above a point write's floor"; M5's walk is about 1 µs | (a) **the point write's own floor** (p0 about 36 µs): C3 recorded, a measured item for an eight-core host; (b) the harness's 0.2 µs noise floor: C3 built in S2 | (a) |

---

## 5. Stages

| stage | what | gate |
|---|---|---|
| **CP-S0** | **The census and the premise.** (1) §1.2's table re-read at the opening commit, every `path:line` checked. (2) The measurement, `build-release`, interleaved A/B, inside 20 minutes, in this priority: **M1** a point `UPDATE ... WHERE id = k` on disjoint rows, one session, `relaxed`, against a measurement build with the dispatcher's table null - the family's whole share, which that build also strips of `DeclaredWriteBorrow`, the first-intention memo check and the detector-gated park rule, as the file states; A1 and A5 apart only by a second decomposition build if the budget holds (no tally counts asks); **M3** scenario 2 (`--bookers 8`) and scenario 0 (`--fk --txn`), `sys.lock_waits` and `sys.ash` read after each, outcomes by unit and mode, `app.lock_wait`'s share of DB time, deadlock and `fault_net` counts (M3a is C2's price on the uncontended path; its contender is the rig cell, §3 C2); **M2** a `SELECT` walk over ~100-1,000 leaf pages against a build whose `Position` declines slices (C1); **M5** a held `UPDATE ... WHERE id > k` beside point writers (C3); **M4** `tools/lock_contention_benchmark.py` contended against disjoint, the shape AT-S5 made real; **M6**, a cell and not a measurement: two sessions under `group`, the second's `UPDATE` of a row the first just committed proceeding before the first's sync, confirming §1.1's last bullet and correcting `known-gaps.md:1417`. A cell cut for the budget is named in the file. (3) A results file `bench/v3.4.7/results-cp-s0-premise-<describe>.md` and this order's §7 stating each candidate's number against its rule | the operator's opening; the census is `[source-read]`, the measurement `[measured]` |
| **CP-S1** | **The model.** **Landed 2026-11-08 on the work branch (§8); CP-Q10..CP-Q12 await the operator's marks.** A design memo in this order (§8): the matrix re-derived from S0 - the cells kept by §2's rule, the cells deleted, `U` added or not with C2's three re-statements, the structure proposed (C3, C4 as S0 ranks them) with its soundness argument written as `lock_table.hpp` writes publish-then-verify, C4's (α) or (β) as marked, and the amendments owed to `txn.md` §5, `foreign-keys.md` §2a-§3, `rules.md` §3's row, AR2 E1/E12 and `lock_table.hpp`'s header. Every `[quiet-wrong]` or `[loud]` choice goes to the operator as a CP-Q amendment before S2 | S0 landed; the operator's marks on any amended CP-Q |
| **CP-S2** | **Build what S1 kept**, one sub-stage per candidate (S2a C1, S2b C2, S2c C4, S2d C3 as ranked), each with a `critics-developer` review and the full suite; each a separate commit so one that fails CP-Q7's bar is reverted alone | S1's model |
| **CP-S3** | **The close.** The overhead measured over the milestone's whole change (the opening commit against the closing one), scenario 0 and 2 beside M1's point write; the specs brought to the tree; `known-gaps.md`'s Locks section rewritten to the engine as it then is; this order's §7 and the index row | S2 landed |

**Instruments** for S0 and S3: `sys.lock_waits` (`sys-views.md` §6.3),
`sys.ash` with `tools/ash_panel.py` (§6.7), the `app.lock_wait` and
`app.lock_conflict` events (`docs/spec/wait-events.md`), `Contention`'s
`kFenceWalks` tally, `SHOW META`'s `load_c<k>_waited_<latch>_us`, and
`tools/lock_contention_benchmark.py`'s `--prove` arm, which refuses to
report a family cost against a server with no family.

---

## 6. What CP does not do

- It does not change what a client observes: no isolation level, no new
  statement surface, and **no wait turned into a refusal** unless the
  operator marks CP-Q4 (b). `U` is a mode a client never names.
- It does not persist anything of the family (CP-Q6).
- It does not touch the commit protocol: the `group` release order stands
  as it is, `strict`'s deferred commit stands as it is.
- It does not touch the page latch family, the WAL latch order or the
  visibility window: `rules.md` §3's rows other than the lock table's are
  read, not amended - except where C4 adds the decide slot's flip to
  `FinishCommitBody`'s stated order, which `txn.md` §5 and `manager.cpp`
  then carry.
- It does not re-state BD-R6, BD-S3 or `TupleKeyPlaced`'s grant argument:
  every `INSERT` keeps its tuple `X` (C4's scope).
- It does not decide E7, E13 or AO-0's carried items 22, 25, 26
  (`workorder-ao-m2-lock-family.md` §AO-8); any it meets is named in S1.

---

## 7. Stage log (CLA, appended as stages land)

### The opening review, 2026-11-07

Run by the `critics-developer` pass on the draft at `6c4b9a6d` before any
commit, as the Session Workflow's step 2 asks of a document step too. Every
finding was checked against the code by CLA before it was applied.

**Applied** (the finding's number in the review's report):

1. The forward check's busy verdict under a granted `S` refuses with a
   table (`:4556-4562`): C2 and C4 would turn the child's wait into a
   `TXN_CONFLICT`. C2 gains its item 1 (an in-flight `UPDATE`'s parent
   reads present), C4 (iii) cites it.
2. The reverse check's probe (`:4775-4815`) relies on the child writer's
   `X`: C2 gains item 2 (the probe asks `U`), C4 (iv) moves the wait to
   the decide slot; the census gains A6'.
3. `IsFence`, `FenceCoversKey` and `AcquireInner`'s verify filter by mode
   name: the draft's "the matrix needs no second rule" was false. C2's
   item 3 and §1.1's second bullet.
4. C4 (ii) is a wait turned into a refusal with rows already written, not
   "a wait-position change": tagged `[loud]`, given (α)/(β), carried to
   CP-Q4.
5. C7's premise is false under `group` (`wal/manager.cpp:441-445`,
   `txn/manager.cpp:385`, `:398`): withdrawn; §1.1's last bullet, §1.4's
   CA-R1 note, M6 and CP-Q5 record it.
6. The draft's reader guard for C7 missed absence-based answers: noted in
   the withdrawal.
7. C4 and the draft's C5 contradicted each other (a fence list with
   nothing to verify against): C3 now states it presumes rows publish and
   defers to C4 (ii).
8. C4's scope: `INSERT` keeps its `X`, the cap claim narrowed, the
   ordering constraint names the schema word, the mis-tagged
   `[quiet-wrong]` re-read as a spurious refusal.
9. The census relabelled: `:6660`/`:6698` are `ON CONFLICT DO UPDATE`'s
   arms, `:6445` is `DO NOTHING`, `:7096` is every issued key, `:10147`
   and `:12782` the per-row `UPDATE`/`DELETE`; `:2700`, `:5154`, `:3513`,
   `:3176`, `:4101` and the unique ask at `:11576` added.
10. Slice `X` added to the dead cells (the count of twelve stands).
11. Scenario 0 cannot show C2's contender by construction (`--fk`,
    `--txn` off; disjoint partitions): §1.3 and M3a corrected.
12. BX's numbers cited from the BX order; two CJ lines, not three; no
    tally counts asks; M1's null-table build named for what else it
    strips; defaults, not constants; C3's instance-wide map.

**Applied from the cuts**: C3 and the draft's C5 merged (one structure,
one latch, measured by M1 and M5); §1.1's prose matrix replaced by the
citation; the tags corrected (§1.3's PostgreSQL comparison and C2's
correctness argument are `[design]` over `[source-read]` premises);
§1.5 cut to the two items a candidate acts on; the index row corrected.

**Rejected**: none of the twelve findings. Of the cuts, §1.5 was cut to
two items rather than to a bare citation, because the "Not candidates"
list needs the names it drops to point at something.

*Nothing built, nothing measured; every claim above is `[source-read]`
at `6c4b9a6d` or tagged otherwise.*

### CP-S0, 2026-11-08: the census re-read, the premise measured

Run on `worktree-cp-lock-model-exploration` from `cc54a296`
(`v3.4.7-1-gcc54a296`; build inputs `6c4b9a6d`'s) on the operator's opening
(`raft-marks-2026-11-08.md` §1, every CP-Q (a)). The results file is
`bench/v3.4.7/results-cp-s0-premise-v3.4.7-1-gcc54a296.md`, its raw outputs
under `bench/v3.4.7/archive/cp-s0-premise-v3.4.7-1-gcc54a296/`; every
number below is `[measured]` there unless tagged.

**(1) The census re-read** `[source-read]` at `cc54a296`: every unit and
mode §1.2 states is what the code asks, and the eight-of-twenty count
stands. Corrected in place: nine citations had drifted by a few lines
(`FenceCoversKey`'s filter is `lock_table.cpp:673`, `ConflictingOverlap`'s
skip `:733-736`, the verify gate `:339-353`, `FinishCommitBody`'s callers
`txn/manager.cpp:398` and `:363`, the group commit's pending mark
`command_dispatcher.cpp:11771-11774` with its waits at `:890-913` and
`:1235-1240`, A5's arbiter re-read `:6701-6705`, the A5' row's BD-R6
note `:7065-7069` and its issued-against-named note and loop
`:7083-7100`, the A6' row's poll `:4814`); A1 gained the three relation `IX` asks the table missed
(`InsertParsed` `:6216`, `HandlePurge` `:12344`, `HandlePurgeDroppedColumns`
`:12466`) and the drain's `IntentionWriters` read (`:2712`); A3's "nowhere
else" now names the table's own three definitions. **One substantive
addition**: the per-row tuple `X` (A5) is asked only when the statement
declared no range (`:10146`, `:12781`) - under A7's range `X` the walk asks
no tuple unit and the row conflict is the header check alone - so A5 and
A7 are alternatives per statement, not layers; C4's scope reads the same
but its argument must not count the two together.

**(2) The measurement**, `build-release` at `cc54a296`, `cores = 4` pinned
to CPUs 2-5 (M2: `cores = 1`, `bm_scan.py`'s shape), data files on
`/dev/nvme0n1p1` ext4, two passes of 344 s and 241 s inside the 20-minute
budget, three binaries from one tree: the base, a null-table build
(`patch-null-table.diff` in the archive: core 0's manager and dispatcher
and every peer handed no table) and a no-slice build
(`patch-no-slice.diff`: `ReadBorrow::Position` declines slices). Neither
patch is landed. The cells and their rule, from §3:

| cell | what it says | candidate, rule |
|---|---|---|
| **M1** point `UPDATE`, one session, `relaxed`, base against null-table, interleaved, three reps | update-disjoint p50 `39.4 vs 39.0 us` and `39.1 vs 38.7 us` on the two reps whose sessions both sat on peer cores (the third, base on core 0 against a peer, flagged by the run script and set aside), noise floor `0.2 us`: **the family's whole share is about `0.4 us`, about `1 %` of the client round trip**; the proof arm shows no family cost on the null-table binary (hot - disjoint `2.6 us` inside a `55 us` floor) | C4: M1 does not say "build" - the whole family, the row entry included, is a 1 % matter on this shape. C3: the same |
| **M3** scenario 2, `--bookers 8`, `group`; scenario 0, `--fk --txn`, 8 traders, `group` | scenario 2: `2,374 TPS`, **84 lock waits, every one granted** (tuple `X`, p50 `76 us`, max `10.3 ms` on `organizations`), `app.lock_wait` **`0.03 %` of DB time**, `app.lock_conflict` 92, deadlock 0, `fault_net` 0; `commit.wal_flush` is `44 %` of the run's DB time (`51 %` since boot, the load included); 4 `UPDATE`s refused `TXN_CONFLICT` by the MVCC window (below), retried by the driver. Scenario 0: `5,344 TPS`, 641,218 child `INSERT`s each taking the parent's `IS` and `S` (of 650,200 `INSERT`s; the rest are the load's and the reporter's), **lock waits 0, conflicts 0**; the family's only trace the partition latch, `149,134` waits for `220 ms` over 240 core-seconds (`0.09 %`) | C2: the uncontended price is below what the cell resolves (0 waits); its contender - two writers of one parent - is not in either scenario by construction (§1.3), so C2 stays judged by the rig cell and §2c, not by a number. C5: no `fault_net` row anywhere - **nothing built** (CP-Q9 (a)) |
| **M2** `bm_scan.py` census, base against no-slice, `cores = 1` | the slice `IS` costs **`88-126 ns` per leaf page** of server CPU: `2.5-2.8 %` of a 603-page scan, `9-12 %` of a 304-page wide-row scan; point controls inside `0.4 us`; same pages and digest on both builds. AO-S7's extrapolation was `<= 3 us` a page; the unlatched price is thirty times smaller, and the latched price above one core was not measured (`bm_scan.py` fixes `cores = 1`) | C1: **deleted on CP-Q3 (a)**; the gain is 2.5-12 % of a scan's CPU, more on wide rows |
| **M5** a held `UPDATE ... WHERE id > 4080` beside point writers on uncovered rows, `cores = 4` | the fence walk costs **about `1 us` per point statement** (`+1.1`, `+1.2`, `+1.4 us` at p50 on three cells, block spread `0.9-3.5 us`; `-2.6 us` inside a `19 us` spread on the fourth), one 64-partition walk per statement as designed, `1,457` partition latch waits over that cell's run (`927` in the fenced blocks, `530` in the unfenced, 24,000 statements each) | C3: BP-H10's `+12 %` at eight cores is **not reproduced at four** - on a four-core instance whose sessions mostly shared core 0 (the text cells by construction, the four-writer KWP cell's writers on cores `0, 0, 0, 2`), so cross-core partition-latch contention was barely exercised; below CF's bar on this host, so C3 is not built on S0's number alone - S1 may keep it as a measured item for an eight-core host |
| **M4** `lock_contention_benchmark.py`, 8 sessions, contended against disjoint, `relaxed` | hot - disjoint `+5.0 us` at p50 (floor `2.7 us`); **`2,519` of `32,000` hot statements refused** - `TXN_CONFLICT retryable=1 row id=1 was written by transaction N`, which is `txn.md` §5's first-updater-wins over a version committed after the statement's snapshot (READ COMMITTED's deliberate no-re-read window, `txn.md:769-773`), **not a lock refusal**; the family's own counts over that server's whole run: `app.lock_conflict 1,026` (3 % of hot statements if every one is the hot arm's), 17 parked (granted, mean `40 us`), `app.lock_wait` `0.008 %` of DB time | the hot row's client-visible effect is the MVCC window (8 %), outside CP's scope (§6) but to be named in S1: a decide-slot wait (C4) relieves the undecided-holder case only, and the decided-newer case is refused by `txn.md` §5's choice, not the family's. C5: `fault_net` 0 |
| **M6** two sessions, `group`; the `strict` control | B's `UPDATE` of the row A just committed answered **before A's `COMMIT` ack in 300 of 300 iterations**, margin p50 `423 us` of an ack latency of `539 us` (`78 %`), parked on no lock; under `strict` above one core B answered first in `1.3 %` and waited the sync (305 `sys.lock_waits` rows, tuple `X` granted, p50 `440 us`). `tests/lock_release_before_sync_rig_test.cpp` pins the `group` order on the two-core rig | CP-Q5 (a) confirmed: the engine releases before durability; `known-gaps.md`'s CA entry corrected in this stage |

**What was cut** (CP-Q8 (a)): M1's first pass ran one rep and was re-run
as three; M5's first attempt lost its four cells to a relation name with a
hyphen and was re-run; scenario 2 at 24,000 cargos ended at the pool's
exhaustion after `8.4 s`, so its TPS is an average over that window.
PostgreSQL's floor was not measured for these shapes.

**What S0 says to S1.** Every live cell has a contender in the tree, and
S0 priced the two the candidates named: the slice `IS` (dead weight, C1
goes) and the fence walk (about `1 us`, C3 not on this number). The family
as a whole is a 1 % matter on a point write and `0.03 %` of a contended
scenario's DB time; where the client sees contention on a hot row, it sees
the MVCC window's refusal, which no lock cell changes. C2's reason stands
unmeasured and unchanged: §2c's conversion deadlock and the parent
`UPDATE`'s wait are correctness shapes, pinned by a rig cell, not a
throughput number, and CP-Q2 (a) builds `U` for them. C4 has no number
asking for it; S1 proposes whether it is built on §2's rule or recorded as
not taken. The decided-newer refusal (M4) is named for S1 as a boundary,
not as a candidate.

**The review** (`critics-developer`, 2026-11-08, on the step as it stood
before the commit): no engine correctness bug; the rig cell sound against
`txn/manager.cpp:287-400`, `wal/manager.cpp:424-453` and
`instance_visibility.cpp:136-157`, `:313-314`; M4's reading right. Applied:
the rig cell's stale comment on `known-gaps.md`, its last wait on A's
acknowledgement made a `KickUntil` (the kick is best-effort), one redundant
`EXPECT` cut; the two drivers refuse a relation suffix that is not `\w+`
(the hyphen that lost M5's first attempt), the release-order driver's
`--protocol kwp` mode deleted (its send stamp preceded the socket write,
so its ordering meant something else; S0 ran the text mode only), its
unused JSON extras and a unit-conversion wrapper cut, the fence driver's
docstring trimmed to pointers at §1.2; in the documents, nine drifted
citations not five, `:11307` for BorrowChain's relation-unit return,
scenario 2's `commit.wal_flush` at `44 %` of the run's DB time (`51 %`
since boot), M5's "not reproduced at four" qualified (the text cells sit
on core 0 by construction and the four KWP writers landed on cores 0, 0,
0, 2, so cross-core partition contention was barely exercised), the
`known-gaps.md` headline re-cut to "not a hot row's lock; CPU and the
booker's own acknowledgement". **Deferred, not rejected**: the rig
scaffolding (`Script`, `RunScript`, `StartsWith`, `Response`) is now the
eleventh copy across the rig tests and `host_state` the third across the
drivers - one `tests/rig_script.hpp` and a `bench_common` helper, outside
this stage because they touch build inputs and an existing driver.
**Rejected**: none.

**The suite** (`scripts/test.sh`, Debug, every label but `bs-nightly`):
run twice on 2026-11-08 - **4405 of 4405 passed** (957 s) on the tree
before the review's edits to the rig cell, and **4405 of 4405 passed**
(915 s) on the tree as committed, the two new cells
`LockReleaseBeforeSyncRigTest.*` passing in both runs (0.07 s each). The
only build inputs the stage changes are the rig cell and its line in
`tests/CMakeLists.txt`; the two measurement patches are not landed. The
overhead measurement is the milestone's close's (S3), not a stage's.

### CP-S1, 2026-11-08: the model

Run on `worktree-cp-lock-model-exploration` on the tree after `d7b75977`
(S0 at `c2937a32`, `origin/main` `adbe2638` merged; build inputs
`6c4b9a6d`'s), on the operator's word *"proceed with S1"*
(`raft-marks-2026-11-08.md` §2). The memo is §8; nothing is built, no
build input changes. What S1 settles: nine live cells after S1 (§8.1); C1
deleted whole with its inventory (§8.2); the `U` mode with C2's three
re-statements against the code, two of them corrected on the way - the
forward check's witness gains the head's delete-mark bit, the reverse
probe's guard narrows to `HoldsCovering` - and the two matrices guarded by
a `static_assert` block in place of a compile claim the tree does not
honour (§8.3); C3 and C4 recorded on S0's numbers (§8.4); the
decided-newer refusal named as `txn.md` §5's boundary (§8.5); the
amendments owed, by document (§8.7); the cells (§8.8). **Three items go
back to the operator before S2** (§4, §8.10): CP-Q10 `[loud]` the class of
deadlocks `U` adds, CP-Q11 E4/R6 re-read as the range unit's, CP-Q12 the
reading of "floor" in C3's rule. AR2's E1 and E12 rows carry their dated
amendments, the build gated on CP-Q10.

**The review** (`critics-developer`, 2026-11-08, on the step before the
commit; every finding checked by CLA against the code): the matrices, the
C.1 proof (ten adversarial sequences, no orphan), the fence predicates, the
probe's four literals and release-by-mode, "nothing persists a mode or
unit", the inventory's sites, the census of cells and the S0 numbers all
held. **Applied**: (1) CP-Q10 was stated too narrowly - update-then-delete
of one row is one instance of a class: a parent updater granted `U` over a
child writer's share that then waits on that writer *for any reason*
(another row, a per-row statement parking mid-walk) deadlocks with it when
the writer then writes the parent; F.1 restated as the class with three
instances, the §4 row, E's table, F.3, both §8.7 quotations and the index
row re-cut, a cross-row rig cell added; (2) the proposed `held_before` fix
would have released a held `S` or `U` with the door's `X`, since the
give-backs remove records by key alone (`:6460-6462`, `:7016-7019`) - the
give-back is filtered by mode too, a sibling cell added; (3) row 14's
`[quiet-wrong]` was wrong - the cell guards itself loudly (`ASSERT_TRUE(sink.done)`,
`ASSERT_EQ(divides, 1u)`, `exec_chain_test.cpp:666-667`) - rewritten by
the reviewer; (4) AR2's E1/E12 amendments read as done and now read "to be
built at CP-S2b, gated by CP-Q10" / "to be deleted at CP-S2a"; (5) §1.1
and C2 item 3 named the verify gate as a mode-name filter where the third
filter is `ConflictingOverlap`'s skip - both corrected, a §8.11 bullet
added; (6) three citation drifts (`:342`, `:600-605`, row 6's `:733`)
fixed by the reviewer. **Cuts applied**: CP-Q10 stated once (F.1) and
pointed to from §4, §8.7 and the index; the CP-Q table kept in §4 alone,
§8.10 a pointer plus its decided paragraph; §8.4's C3 figures cited from
§7; §8.6 and §8.12 cut to pointers; the method paragraph to a clause.
**Rejected**: none.

**The suite**: not re-run for this stage - no build input changes (four
documents under `instructions/`); the last full run on these build inputs
is S0's, 4405 of 4405 twice at `c2937a32` (above). The overhead
measurement is the milestone's close's (S3).

---

## 8. The model (CP-S1)

Written 2026-11-08 on `worktree-cp-lock-model-exploration` at `d7b75977`
(build inputs `6c4b9a6d`'s, `v3.4.7`), on the operator's word *"proceed
with S1"* (`raft-marks-2026-11-08.md` §2), from S0's census and numbers
(§7), by five readers, three independent designs of `U` judged by two, five
adversarial lenses, and CLA's synthesis with every `path:line` the memo
rests on re-read at `d7b75977`. **Nothing is built by S1**; every claim is `[source-read]` at
`d7b75977`, `[measured]` in S0's results file, or `[design]`. Where the
model finds the order's own text wrong it says so (§8.11) and corrects it
in place.

### 8.1 The cells after S1, by §2's rule

Five modes over four units after S1 - `IS`, `IX`, `S`, `U`, `X` over
relation, range, tuple, index key - make twenty cells, as four modes over
five units did. **Nine are live**, each with a contender (§2 (i)) and
something no other mechanism protects (§2 (ii)):

| cell | asked at | contender (i) | protects (ii) |
|---|---|---|---|
| relation `IS` | every bind (`src/exec/step_compiler.cpp:2111-2112` into `ReadBorrow::Position`, `read_borrow.hpp:97`); a foreign key's parent read (`BorrowChain`'s intention, `command_dispatcher.cpp:11255-11257`); an online `CREATE INDEX` (`:3032`) | a DDL's relation `X` (A4) | a granted reader is not overtaken for the statement's length; one-directional, the refused arm right by the statement epoch, DT2 and catalog MVCC (`read_borrow.hpp:27-47`) |
| relation `IX` | `BorrowChain` above every write unit (`:11254-11263`); `InsertParsed` `:6216`; the two `PURGE`s `:12344`, `:12466` | a DDL's relation `X`; the CI drain's `IntentionWriters` (`:2712`; `lock_table.cpp:523-526`) | a DDL waits for every open writer (AT-S5e); the first intention is the memo check; the drain's snapshot (A1) |
| relation `X` | `BorrowRelationForDdl` (`:11162-11185`) and its callers (A4) | every `IS` and `IX` holder | the DDL against open writers and positioned readers; nothing else does (A4) |
| tuple `S` | the forward check (`:4542-4546`), given back on a failed check (AZ-S5, `:4673-4683`) | a parent `DELETE`'s tuple `X` (`:12782`) or range `X` (`:12743`, met in the verify); an `INSERT` of the key (A5'); **under `U` a parent `UPDATE` is admitted** (§8.3) | the parent row's existence while a child writer is open (D9(a), `foreign-keys.md` §2a); nothing else does (A6) |
| **tuple `U`** (new) | `UPDATE`'s per-row ask (`:10147`), `ON CONFLICT DO UPDATE`'s unique-arbiter row (`:6698`), the reverse probe (`:4796`) | another `UPDATE`'s `U`; a `DELETE`'s or `INSERT`'s `X`; a range `U`/`X` over the key | the wait with a kick on a row another core's transaction holds (AX-S2b); being seen by a fence's verify (A7); refusing another writer while admitting a key share. The header stays the lock (AO-R3) |
| tuple `X` | `DELETE`'s per-row ask (`:12782`); every `INSERT` ask (`:7054-7056`, `:7096-7098`, `:6445-6449`, `:6660`) | `S`, `U`, `X`; range fences | existence (a `DELETE`); placement - BD-R6, `TupleKeyPlaced`'s grant argument (`:6452-6457`); the reverse probe's refusal (A6'); the arbiter's re-read (`:6701`) |
| **range `U`** (new) | `UPDATE`'s declared window (`:10028-10029`) | a tuple `U`/`X` inside it; another range `U`/`X`; an `INSERT`'s tuple `X` of a key inside it; **admits a tuple `S`** | the statement's whole walk sees one state, the wait before the walk with nothing written (AO-S6c-c); the header "is silent about a transaction holding a range over the key that has not reached the row yet" (`:11621-11625`) |
| range `X` | `DELETE`'s declared window (`:12742-12743`) | tuple `S`/`U`/`X` inside; range `U`/`X` | as range `U`, and it excludes the key shares: a bulk `DELETE` waits for child writers |
| index key `X` | the unique check (`:11576-11580`) | another writer's `X` on the same value | two writers of one unique value cannot both find it free (BX-R3); overlaps no fence and no tuple (`lock_table.hpp:324-327`) |

**Eleven cells have no caller**: relation `S` and `U`; range `IS`, `IX`,
`S`; tuple `IS`, `IX`; index key `IS`, `IX`, `S`, `U`. Of the census's
twelve dead cells (§1.2) the three slice cells leave with the unit (§8.2);
the `U` column adds relation `U` and index key `U`, both dead - `U` is
asked on a tuple and a range only, `BorrowChain` derives `IX` above a `U`
ask (`:11255-11257`), the unique check keeps index key `X`. **The dead
cells are combinations a generic matrix admits, not code**: `kCompatible`
and `kCovers` are mode against mode with no unit axis (`lock_table.hpp:241`,
`:254`), and the per-unit code - the factory, `IsFenceUnit`, `Overlaps`,
the name case - knows no mode. So §2's "its matrix rows go" has no object
beyond the slice unit, and the matrix stays total so a cell's
compatibility is written against the day it gains a caller. The two new
live cells are existing asks changing mode (`:10147`, `:6698`,
`:10028-10029`, `:4796`); no new ask site is added.

### 8.2 C1 - the slice `IS` deleted, the relation `IS` kept (CP-Q3 (a))

**The rule applied.** The unit's one engine consumer is
`ReadBorrow::Position` taking `LockKey::Slice` in `IS`
(`read_borrow.hpp:120`, `:125`), fed by the outermost walk's per-page
report (`step_vm.cpp:2088-2090`; `walk_page_min_key` `:1825-1833`, stored
at `:1887`). No conflict test reads it (A3's last column, re-verified at
`d7b75977`: `IsFence` `lock_table.cpp:26-28`, the verify gate `:339-343`,
`FenceCoversKey` `:673`, `ConflictingOverlap` `:733-736`), and its
intended contender, a mover, does not exist (`heap-and-tuple.md` §7.1).
It fails §2 (i) and (ii); S0 priced it at 88-126 ns a leaf page (M2).
**The unit leaves the enum whole**: with `:120` gone, `kSlice`,
`LockKey::Slice` and the slice arm of `IsFenceUnit` have no caller in
`src/` or `include/` (`grep -rn 'LockKey::Slice\|kSlice' src/ include/`:
`lock_table.hpp:209`, `:291`, `:313`, `lock_table.cpp:38`,
`read_borrow.hpp:120` - the table's three definitions, the name switch,
the one consumer), the census's slice `S`/`X`/`IX` were never asked, and
"a cell that fails (i) is dead code" (§2). Nothing persists or transmits a
`LockUnit` value: the only store is the memory-resident `LockWaitRecord`
(`stats/lock_wait_log.hpp:53`), rendered by name (`sys_views.cpp:937`),
the only numeric use the partition hash (`lock_table.cpp:97-102`); no
superblock bump, no protocol change. `[design]` the enum renumbers
(`kTuple = 3`, `kIndexKey = 4`; the header's "for the first four" becomes
"for the first three", `lock_table.hpp:194-195`) - S2a's style choice,
not a mark. **The 64-page yield slice (BA-S15, CE) is a different thing**:
`SlicePolicy::pages_per_slice` counted at `step_vm.cpp:2104-2105`, adjacent
to the report under the same `outermost` test and sharing no variable,
type or function; every "slice" in `sched.md`, `protocol.md`,
`wait-events.md`, `walk_slice_rig_test.cpp` and
`index_build_slice_rig_test.cpp` is that word and is untouched.

**The deletion inventory** (S2a's list; each row verified at `d7b75977`):

| # | site | what goes | what stands in its place |
|---|---|---|---|
| 1 | `read_borrow.hpp:101-129` | the relation-change release, the whole-space early return, `LockKey::Slice`, `slice_ = slice`; `rel_`/`slice_` (`:152-155`) | `Position` is the relation ask alone (`:94-100`, "asked once per relation") |
| 2 | `read_borrow.hpp:16-19`, `:22-25`, `:50-51`, `:85`, `:103-124` | "and `IS` on the slice its outermost walk has reached"; "one entry per bound relation plus one slice"; the in-body slice comments | the header says the borrow is the relation `IS` at the bind and names the mover as what re-adds a position unit; the one-directional paragraph (`:27-47`) stays verbatim |
| 3 | `step_vm.cpp:1825-1833`, `:1887`, `:2068-2090` | `walk_page_min_key`, its store, the per-page report and its R14 comment | nothing in production (M2's cost is this call) |
| 4 | `step_vm.cpp:2025-2035` | the first report `Position(access.oid, 0, kIdSpaceEnd)` - **S2a verifies** every `Execute`/`ExecuteAsync` caller passing `position` compiled with the same sink as `declare` (`step_compiler.cpp:2111-2112`) | deleted if so; else kept as the relation declaration's fallback with its comment rewritten |
| 5 | `step_vm.hpp:310-327` | `lo`/`hi` of `PositionSink::Position` and "an `IS` at the slice ... at every page boundary" | `Position(catalog::Oid rel)`, once per bound relation; `step_compiler.cpp:2107-2110` and `step_compiler.hpp:58-68` say "declares the relation" |
| 6 | `lock_table.hpp:209`, `:290-292`, `:312-314`; `lock_table.cpp:38-39` | `kSlice`, the factory, the slice arm of `IsFenceUnit`, the name case | `IsFenceUnit` is `unit == kRange`; the counter's raise/give-back/lower and `FenceCoversKey`'s scan read the unit through it unchanged (`:174`, `:194`, `:209`, `:225`, `:606-608`, `:664`); `ConflictingOverlap` reads `Overlaps` (`:708`), not the fence unit |
| 7 | `lock_table.hpp:27-32`, `:78-82`, `:104-105`, `:194-195`, `:272-277`, `:581-585`, `:676`, `:714-718`, `:736`; `lock_table.cpp:23`, `:716-732`; `txn/manager.cpp:62-66`; `sys_views.cpp:920` | the header's and the comments' slice prose ("relation -> range -> {slice, tuple}", "A slice is `(rel_oid, [lo, hi))` (E4)", "range- and slice-unit fences", "two slices sharing a lower bound", "a positioned reader's slice `IS` is what a writer's declared range meets here", "its finest fence is a slice") | "relation -> range -> tuple"; the E4 citation moved to the range unit (keyed in key space, no page field, `lock_table.hpp:278-282`); "range-unit"; "its finest fence is a range"; the intention-holder rule at `:733-736` restated as a table property (below) |
| 8 | `lock_wait_log.hpp:43`; `command_dispatcher.cpp:11317` | the stale identifier `AcquireResult::slot_key` | `AcquireResult::wake` (`lock_table.hpp:427`) - found on the way, bundled |
| 9 | `tests/lock_table_test.cpp:215-242` | `slice0` and its two inequalities; "Four" in the title | three `lo = 0` keys; the claim (the unit kind is in the key) stands |
| 10 | `lock_table_test.cpp:351-379`, `:382-409`, `:475-494`, `:512-533`, `:890-913` | `LockKey::Slice(...)` as an arbitrary interval unit | `LockKey::Range(...)`; each cell's claim (E4's key-space property, the counter's gate, the `S`/`X` fence pair, the underflow guard, `S`/`S` on an interval) is unit-agnostic; `:351`'s cell renamed, `:984`'s cross-reference follows |
| 11 | `lock_table_test.cpp:1132-1188` `AnIntentionModeOnAnIntervalUnitNeitherFencesNorIsFenced` | `LockKey::Slice(kRel, 100, 400)` `IS` and "The read borrow's slice `IS`" | `LockKey::Range(kRel, 100, 400)` `IS`, restated as AO-0 item 27's table property with no engine caller - **kept, not deleted** (below) |
| 12 | `tests/lock_family_test.cpp:480-487` in `AWholeRelationWriteIsNotHeldUpByAPositionedReader` | the slice `IS` acquire and "a reader's slice `IS` passes a declared range" (`:456-458`) | the relation `IS` alone; the cell's `DELETED` rests on the relation unit overlapping nothing (`lock_table.hpp:324-327`) |
| 13 | `tests/exec_chain_test.cpp:479-561` `AWalkReportsWhereItIsAtEveryPageBoundary` | the cell pinning the per-page position "as AO-S6e-b built them" | deleted, or reduced to "a walk declares each bound relation once" if row 4's report stays |
| 14 | `tests/exec_chain_test.cpp:564-587` `AResumedPrefixCoversEveryInnerRowAcrossADivideThatDropsARetiredSlot` | `DivideAtTheBoundary::Position(rel, lo, hi)` triggering on `lo != 0` (`:575-585`) | the same divide hooked to `SlicePolicy::at_boundary` with `pages_per_slice = 1` through `ExecuteAsync` (`step_vm.hpp:376-380`; the same loop position, `step_vm.cpp:2104-2108`), keeping its `ASSERT_TRUE(sink.done)` and `ASSERT_EQ(divides, 1u)` (`:666-667`). **Not optional**: with the per-page report gone the trigger never fires and the cell fails at `:666` - loud, not quiet; it would pass vacuously only if the rewrite dropped those two asserts, leaving the resume-skips-a-row defect it pins unguarded |
| 15 | new cell in `lock_family_test.cpp` beside `:884` | - | a `SELECT` walking several leaf pages inside `BEGIN` leaves exactly one lock-table entry per bound relation while it runs (M2's gain pinned: today a mid-walk `EntryCount` shows the slice entry) |
| 16 | `docs/spec/txn.md:705-709`, `:862-864`, `:913-919`, `:920-924`, `:931-935`; `CLAUDE.md:56`; AR2 E12 (`:633`) and R14's surroundings | the five slice-`IS` sentences; the row's "and the slice it has reached"; E12's status | §8.7's text; a dated inline amendment per AR2's convention, R14's text quoted not rewritten |

Kept, with no change: `read_borrow.hpp:94-100` and every `read_borrows()`
cell; the rig tests, which ask the relation unit only; `index_unique_test.cpp:511-520`
(`IsFenceUnit()` false for an index key is the reduced predicate's
contract from the outside); `drop-table.md` DT7; `ddl-transactional.md`
§5e-f; `heap-and-tuple.md` §7.1; `wait-events.md` (no unit parameter on
either lock event).

**What the spec says in place of E12.** E12 ratified "`IS` at the
position's finest lock unit (R14)" on the ground "without it no move can
wait" (AR2 `:633`; R14 `:431-440`). CP-Q3 (a) un-ratifies E12's **unit**,
not E12's reason. `txn.md:862-864` loses "and its outermost walk declares
where it is, `IS` on the slice it has reached, moved at every page
boundary" and gains: *"A statement declares every relation it binds and
nothing finer. No read declares a position: nothing in this engine moves a
key between units (`heap-and-tuple.md` §7.1), and the one operation that
waits for a reader, DDL's relation `X`, meets it at the relation entry by
the intention rule. A mover re-adds a position unit when one exists; E12's
unit was un-ratified on that ground (CP-Q3 (a), 2026-11-08), E12's reason
- a move must wait for a positioned reader - was not."* `:920-924` (the
interval `[min_key(page), kIdSpaceEnd)`) is deleted whole; `:931-935`
("What declares the relation and no slice") becomes "Every bound relation
holds `IS`, and nothing declares a position"; `:705-709`'s nesting loses
"slice"; `:913-919` loses "so it passes a reader's slice and a reader
passes its range" and keeps its rule.

**Two ratified items the slice leaves behind.** (1) **AO-0 item 27** - "an
intention mode on an interval unit neither fences nor is fenced", ratified
as built 2026-09-29 (`workorder-ao-m2-lock-family.md:532`) - is the
`continue` at `lock_table.cpp:733-736`, by mode and not by unit, so it
survives the deletion and loses its only live instance (range `IS`/`IX`
are dead cells). `[design]` S1 keeps the code and its pin (row 11),
restated as a table property with no engine caller: the rule is the
operator's ratification, and §8.3's fence rule - "an interval unit in any
non-intention mode is a fence" - is item 27's own last sentence ("what
still stops a write inside a fence is every unit that is not an
intention") said from the other side; the two are stated and pinned
together. Deleting the `continue` would un-ratify item 27 and is not
proposed. (2) **E4 and AR2-R6** ("Slice key: `(rel_oid, [lo, hi))`, page
as hint only", `:625`, R6 `:310` `[quiet-wrong]`) describe a unit that no
longer exists while the property they ratify - a `LockKey` has no page
field (`lock_table.hpp:278-282`) - holds of the range unit and of every
unit, and is what `lock_table_test.cpp:351-356` pins and row 10 re-pins
over a range. CP-Q3 (a) names E12 only; whether E4 and R6 re-read as the
range unit's is the operator's - **CP-Q11** (§8.10).

### 8.3 C2 - the `U` mode, built (CP-Q2 (a))

**A. The set and the two matrices.** `[design]` `kUpdate` in strength
order - `kShared = 3`, `kUpdate = 4`, `kExclusive = 5`, `kLockModeCount =
5` (today `lock_table.hpp:214-221`; `ModeIndex(m) = m - 1`, `:228-230`,
is the one numeric use and needs `1..kLockModeCount` contiguous, which
either placement keeps). Renumbering is free: the only store of a
`LockMode` is the memory-resident `LockWaitRecord`
(`lock_wait_log.hpp:53-54`), rendered by name (`sys_views.cpp:937`); the
partition hash reads the unit, not the mode; no ordering comparison on
`LockMode` exists in `src/`, `include/` or `tests/`.

**What `U` means** `[design]`: a **version claim**. Its holder may write a
new version of the row - or of every row of its window - under the same
key, and nothing else; the row keeps its existence and its key whichever
way the holder decides. `X` stays the **existence claim**: a placement, a
delete-mark, a retire. The split is clean on this engine: a foreign key
references the Keystone id alone (`foreign-keys.md` §1, "no parent
column"); the tuple key is never assignable (`step_compiler.cpp:1921-1927`,
`Unsupported`; invariant 11); an `UPDATE` never writes over a delete-mark
(`UpdateParsed` skips a visible one as `kNoVersion`, `:10072`,
`visibility.hpp:64-68`; `DO UPDATE` refuses a tombstone `AlreadyExists`,
`:6672-6683`); and the one key re-binding, `PURGE`, retires committed
tombstones under the relation `IX` and no row unit (`:12339-12346`).

Compatibility (held \ want; changed cells bold; today's 4×4 is
`lock_table.hpp:241-247`):

| held \ want | `IS` | `IX` | `S` | `U` | `X` |
|---|---|---|---|---|---|
| `IS` | yes | yes | yes | **yes** | no |
| `IX` | yes | yes | no | **no** | no |
| `S` | yes | no | yes | **yes** | no |
| `U` | **yes** | **no** | **yes** | **no** | **no** |
| `X` | no | no | no | **no** | no |

`S`/`U` yes, symmetric, is the cell C2 exists for: the share protects
existence and key (`foreign-keys.md` §2c), a version claim changes
neither; symmetric because `TheCompatibilityMatrixIsTheOneAR2States` pins
symmetry (`lock_table_test.cpp:82-89`) and §D's argument needs it.
`U`/`U` no: two version writers of one row serialise as two `X`s did, the
header's first-updater-wins (`txn/manager.cpp:188-206`) deciding the
re-run. `U`/`X` no, both ways. `IS`/`U` yes and `IX`/`U` no are the
standard rows, dead on this engine (§8.1), filled so the table is total.

Covering (held \ want: is the wanted mode a no-op for the same
transaction; today's is `:254-260`):

| held \ want | `IS` | `IX` | `S` | `U` | `X` |
|---|---|---|---|---|---|
| `IS` | yes | no | no | **no** | no |
| `IX` | yes | yes | no | **no** | no |
| `S` | yes | no | yes | **no** | no |
| `U` | **yes** | **no** | **yes** | **yes** | **no** |
| `X` | yes | yes | yes | **yes** | yes |

`U` covers `IS`, `S` and itself: a version claim is at least a key share,
so a transaction that updated a parent and then references it takes no
`S` entry, and a resumed walk or an `ON CONFLICT` arm re-asking its row is
`already_held` (`lock_table.cpp:189-199`), not a cap slot per visit. `X`
covers `U`: `ON CONFLICT (tuple key) DO UPDATE` holds the key's `X` up
front (`:6660`) and its arm re-asks the row at `:10147`. `S` does not
cover `U`, and `U` covers neither `IX` nor `X`: the header's honest
accounting - "a transaction needing both holds both entries ... rather
than a silent upgrade" (`lock_table.hpp:249-253`;
`ATransactionNeverConflictsWithItself`, `lock_table_test.cpp:127-142`).

**Who asks `U`** `[design]` - each a one-argument change, since
`BorrowChain` and `BorrowOrWait` already thread `mode` with `kExclusive`
as the default (`command_dispatcher.hpp:989-992`, `:1135-1137`): `UPDATE`'s
per-row tuple ask (`:10147`, inside `UpdateParsed` under `if
(!declared.has_value())`, `:10146`); `UPDATE`'s declared window
(`:10028-10029`; `DeclaredWriteBorrow` `:11409-11441` untouched); `ON
CONFLICT DO UPDATE`'s unique-arbiter holder row (`:6697-6699`; the "read
`h` again under its `X`" argument at `:6701-6705` survives, since what
could move `h` off its unique value is an `UPDATE` or `DELETE` of `h` and
`U` refuses both); the reverse probe (`:4796`, with its literals at
`:4799`, `:4804`, `:4809`). `UPDATE ... FROM` and `DELETE ... USING` reach
the same sites (the parser lowers them to a conjunct, `parser.cpp:3383`,
`:3601`); the query layer executes no DML. **Who keeps `X`**: `DELETE`'s
row (`:12781-12783`) and window (`:12742-12743`); every `INSERT` ask -
issued key `:7096-7098`, named key `:7054-7056`, `TupleKeyPlaced`
`:6445-6449`, the KWP load and `INSERT ... SELECT` through `InsertParsed`;
and `ON CONFLICT DO UPDATE`'s up-front tuple-key unit (`:6659-6662`),
taken before the lookup (`:6664`) and covering the insert arm
(`:6768-6770`) - on this order's text (§3 C2, CP-Q2 (a)) and because a
placement must not run under a key share; over-strict for the update arm
when the key is present, stated as such. `BorrowChain`'s intention is a
binary test on `kShared` (`:11255-11257`), so a `U` ask takes relation `IX`
and a DDL's `X`, the online build's drain (`IX` holders,
`lock_table.cpp:523-526`) and `DROP TABLE` see an `UPDATE` as a writer
unchanged. `UniqueKeyHolds` releases only the index-key units it added
(`:11517-11524`), so the arbiter row's `U` is held to the decide.

**The compile guard, in place of a false premise.** The header's "adding
`SIX` to the enum must not compile until somebody fills its row"
(`lock_table.hpp:236-239`), this order's §1.1 and C2's Risk are wrong as
C++: `kLockModeCount` sizes the two arrays and nothing asserts on them; the
build is `-Wall -Wextra` with no `-Werror` (`CMakeLists.txt:12`). A fifth
enumerator with the count at 4 compiles with a `-Wswitch` warning and
indexes out of bounds at run; the count at 5 with the rows unfilled
compiles silently and reads every `U` cell `false` - an unfilled
`kCompatible` row is loud (`U` refuses `S`, the conversion deadlock stays,
the amended rig cell fails), an unfilled `kCovers` row is quiet (a cap slot
per re-ask of a held `U`). `[design]` S2b adds, beside the tables:
`static_assert(static_cast<std::size_t>(LockMode::kExclusive) ==
kLockModeCount)`; `kCompatible` symmetric over every pair, its `X` row all
false; `kCovers`' diagonal all true (what stops a second record of one
`(txn, mode)`, since the covering test at `:189-199` is the only thing
before the push at `:308`), its `X` row all true; covering monotonicity
(`Covers(h, w)` implies `Compatible(h, m) => Compatible(w, m)` for every
`m` - a covering mode admits no more than the mode it covers; it holds
for the tables above); and the nine `U` cells as literals. `LockModeName`
gains `case LockMode::kUpdate: return "U";` (`lock_table.cpp:48-60`).

**B. Same-transaction accounting.** The table's own-entry handling, in
consultation order: the covering test reads the asker's own holders only
and answers `already_held` with no entry, no cap and the fence counter
given back (`lock_table.cpp:189-199`); the cap (`:208-214`); the conflict
scan skips the asker's own entries by identity (`:222`); a grant pushes
`Tenant{txn, mode}` (`:308`) and `Held{key, mode}` (`:321`); a release
matches holder and ledger by `txn` **and** `mode` (`:584-589`,
`:625-631`); both scans skip the asker (`:667`, `:715`). So a child writer
`T` holding `S(P)` from its forward check that then runs `UPDATE P` is
granted `U(P)` over another child's `S` (`Compatible(S, U)`), as a second
record; a `DELETE P` after it asks `X(P)`, which `Covers(U, X)` no longer
makes `already_held` (today `Covers(X, X)` does), so it is a third record
if granted - **one cap slot more than today** for a transaction that
references, updates and deletes one parent, and likewise for any
`UPDATE`-then-`DELETE` of one row (`kMaxLocksPerTxnDefault = 65536`,
`lock_table.hpp:333`); stated, not measured. The reverse order - `UPDATE
P`, then a child reference to `P` - is one record (`Covers(U, S)`), sound
because the transaction wrote `P`'s version and reads it as its own
(`read_view.hpp:108`). Each record's release runs `WakeWaiters`
(`:614`), so a waiter on `P` is woken when the `S` goes while the `U` is
held - a wasted poll, never a wrong answer (the re-check is mandatory). AZ-S5's
give-back selects `kShared` tuple records with a `kViolation` verdict
(`:4673-4683`) and never touches a `U` or `X` record. **One accounting
leak to close with it**: the two named-key doors test `held_before` with
the mode-agnostic `Holds` (`TupleKeyPlaced` `:6446`, the named-key `INSERT`
`:7053`), so a transaction that updated key `k` (holding `U(k)`) and then
names `k` in an `INSERT` has `held_before == true`, its `X(k)` pushes a
record (`Covers(U, X)` false) and the give-back skips it (`:7017`,
`:6460`): the row is `X`-held to the decide where the contract says `U`.
Cap cost only, never a wrong answer; S2b reads `HoldsCovering(unit,
kExclusive)` there **and filters the give-back by mode** - the give-backs
at `:6460-6462` and `:7016-7019` remove records by key alone (`h.key ==
unit`), so with the narrowed `held_before` and no mode filter a held `S`
(a child insert's) or `U` on the key would go with the `X`, breaking
D9(a)'s "held to the decide" and C.1's premise (the review caught it).
With both, the `X` the door added is what it gives back and nothing else.

**C.1 The forward check's busy verdict under a granted `S` reads present.**
The site: `ResolveForeignKeyParents` takes `IS` then `S` before the descent
(`:4542-4546`), mints the view after the grant (`:4548-4551`;
`MintCheckView` `txn/manager.cpp:87-104`), reads through
`exec::CheckParentPresent` (`:4553-4555`; `fk_check.cpp:86-123`, whose
verdict is `CheckVisibility` - busy before `deleted`,
`visibility.hpp:206-209` - and which exposes only `busy_trx`,
`fk_check.cpp:116-121`), and parks a busy verdict only under `locks_ ==
nullptr` (`:4557-4563`: "Busy under a granted `S` means no lock table").
With a table the held `kBusy` reaches `CheckForeignKeyOnWrite`'s arm and
is refused `TxnConflict` inside the write scope with the `S` kept
(`:4647-4658`), poisoning the session inside `BEGIN`. Under `U` that arm
becomes reachable - a child `INSERT` under a parent `UPDATE` in flight
would be refused where it waits today - the `[loud]` shape item 1 exists
to prevent; **item 1 lands in the same commit as the mode switch**.

**The rule** `[design]`: with a table (`locks_ != nullptr`), a `kBusy`
verdict under a granted `S` whose head version is **not delete-marked and
has a predecessor** (`!deleted && undo_ptr != kNoUndoPtr`) is held as
`kPass`. Any other busy head under a granted `S` with a table keeps
today's arm - held `kBusy`, refused retryably at `:4647-4658`, the `S`
kept, the session poisoned inside `BEGIN` as today - and is logged at
`Warn` naming the writer: the one trace of a verb placed under the wrong
mode. Without a table (`locks_ == nullptr`) nothing changes: the
`IsInFlight` poll at `:4561-4563` stays and `ForeignKeyCheckTest`'s two
busy cells (`foreign_key_test.cpp:581-599`, `:679-689`, no `set_locks`)
stand.

**The proof that busy under a granted `S` has exactly one cause with a
table.** Let `W` be the in-flight writer of `P`'s head at the mint, which
is after the `S` grant. `W` stamped the header under one of: (a) a tuple
`X` - every `INSERT` ask and `DELETE`'s row; the `S`'s in-entry scan reads
`Compatible(X, S)` false (`lock_table.cpp:223`) - refused. (b) A range
`X` containing `P` - `DELETE`'s window; the `S` is a tuple ask whose verify
runs `FenceCoversKey(want = S)` (`:343-352`) and reads `Compatible(X, S)`
false (`:674`) - refused; a range `X` published after the `S`'s scan is
refused by its own `ConflictingOverlap` finding the `S` (`:742`, §D). (c)
A tuple or range `U` - `UPDATE`'s row and window, the arbiter row:
admitted at `:223` and, once §C.3 admits `U` fences, at `:674`. (d) No
unit: nothing stamps an in-flight id into a user row's header under no
unit - `PURGE` logs at `kNoTxnId` under the relation `IX`, `PURGE DROPPED
COLUMNS` keeps the header's writer (`alter.md` AL11), a rollback restores
the predecessor's writer (`txn/manager.cpp:522-525`, `:541-545`), recovery
is not concurrent with a statement. A decided-but-unreleased `X` holder
still refuses (a decide retires before it releases), and a released one
is visible to a view minted after the release (`FinishCommitBody`
publishes at `:306` before `Release` at `:336`). Could `W` hold `U` and
`X` on `P` (`UPDATE` then `DELETE` in one transaction)? The `X` is in the
same entry and the scan at `:216-227` visits every holder - refused. So
`W` is an `UPDATE`: its head is an overwrite (`apply` appends a
`kOverwrite` record and stamps `new_undo_ptr`, `:10392-10414`) over a
version its walk saw live - a committed version of `P`, or `W`'s own
earlier overwrite of one, since another in-flight writer's version would
have refused it at `CheckWriteConflict` - and is not delete-marked.
Whichever way `W` decides a live version of key `P` stands (a rollback
restores the image with `prior_trx_id`, `:522-525`), and no `DELETE` of
`P` is granted `X` while the child's `S` is held. **The two header fields
are the witness of that discipline, and the discipline is the decision.**
Both fields are needed: an in-flight delete-mark also has a predecessor
(its rollback clears the mark back to it, `:541-545`), so the predecessor
alone would read a `DELETE` in flight as present wherever the discipline
failed; an in-flight insert has none (`visibility.hpp:70-73`), so the
`deleted` bit alone would read it as present. This order's §3 item 1 named
only the predecessor; S1 adds the mark. **One premise, stated so S2b keeps
it**: a `DELETE`'s mark is two header writes - `OverwriteTuple` with the
deleter and its undo pointer, then `DeleteMark` (`:12880-12885`) - whose
intermediate state looks like an `UPDATE`'s head to the witness; it is
never read under a granted `S` because the walk holds the page in write
mode (`:12960`) and the row's `X` (`:12782`) or range `X` (`:12743`)
precedes it. The chain walk `UndecidedInsert` (`:6394-6403`) is not taken:
with a table the one case it separates from an overwrite - an
insert-then-update by one in-flight transaction - is excluded at `:223` by
that transaction's `X`, and the walk would cost an undo read per busy
parent after the leaf is released and a move of the helper out of its
anonymous namespace. It is what the no-table arm would need if item 1 were
ever applied there; it is not (decided inside CP-Q2 (a), §8.10).

**The code shape**: `CheckParentPresent` gains one defaulted out-parameter
beside `busy_trx` - the head's `{trx_id, undo_ptr, deleted}` filled at
`fk_check.cpp:119-121` from the same `tuple.value()` (the idiom
`UndecidedWithEarlierVersion` already reads, `:47-50`) - and the hoist,
after `:4555`, converts a `kBusy` under a table with the witness true to
`kPass`, else logs. One external caller (`:4553`). The conversion applies
to the statement's held verdict (`FkParentVerdicts`, `:4568`); a later
statement of the same transaction re-resolves its parents at its own
hoist.

**Soundness of "present" across the decide.** Child `T2` places its row
and holds `S(P)` to its decide. `W` rolls back: the predecessor is
restored, live, same key, before `Release` (`:584-626`). `W` commits: a
live version of the same key, published before `Release` (`:306-336`).
`W` then deletes `P` in the same transaction: `X` refused by `T2`'s `S`
while `T2` is open; after `T2` commits the `X` is granted and the reverse
check, under a view minted after the `X` (`:4697-4708`), sees `T2`'s
committed child and answers `FkViolation`. A third transaction deleting
`P`: refused by `W`'s `U` or `T2`'s `S`, waits, re-checks at the release.
No sequence ends with a committed child referencing an absent or re-keyed
parent, on one core or two (the table and the commit window are the
instance's). The `strict` deferred commit holds its borrows across the
sync (BA-S7): a child's `S` admitted while the committer's `U` is held
reads busy, present; a failed sync aborts the committer and restores the
predecessor - the row stands either way.

**C.2 The reverse probe asks `U`.** The site: `WaitForChildRowWriter`
(`:4775-4815`) - the premise "the writer holds the row's `X`" (`:4777-4779`),
the `!Holds(unit)` guard (`:4792-4793`, reason `:4788-4790`), the `S` ask
(`:4796`), the grant arm's `ReleaseOne(kShared)` and pre-flipped slot
(`:4798-4804`, which `AwaitWriteBlock` treats as an immediate re-run with
no `sys.lock_waits` row, `:427-431`), the refused arm parking on the
refusing entry (`:4805-4809`), `NoteBlockingWriter` (`:4814`). **Why `S`
fails under `U`**: a child `UPDATE` holds tuple `U`, `Compatible(U, S)` is
true, the probe is granted, and the pre-flipped slot re-runs the whole
child walk at once, every time, until the child decides or the 1 s
deadline (`:407`) refuses the `DELETE` `TxnConflict` - a wait turned into
a spin and then a refusal. `AParentDeleteWaitsOutAChildMovedOffItAndRefusesAtItsRollback`
(`fk_cross_core_rig_test.cpp:519-536`) would pass by `StillWaiting`
(`:414-419`) while pinning that spin, which is why the pin gains a
`ParkedOn`.

**The rule** `[design]`: the probe asks `U`; the four literals (`:4796`,
`:4799`, `:4804`, `:4809`) move together. Refused by the child row's own
writer in `U` (a point child `UPDATE`) or `X` (a child `INSERT` or
`DELETE`) in-entry at `lock_table.cpp:223`, and by a bulk child `UPDATE`'s
range `U` or bulk `DELETE`'s range `X` over the row in `FenceCoversKey`
(`:674`) once §C.3 counts a range `U` - a wake registered on the refusing
entry (`:257-291`, `:675-676`) and the `DELETE` parked on it as today.
Admitted by a grandchild writer's `S` on that child row (`Compatible(S,
U)`), then given back by mode - `ReleaseOne(kUpdate)` removes only this
transaction's `U` record (`:584-589`, `:625-631`), the grandchild's `S`
intact - and the pre-flipped slot re-runs the `DELETE`, rightly: a `U`
probe granted means the row's writer holds neither `U` nor `X` on the row
or a range over it, so it released, hence decided. **Why no hot loop**:
every in-flight writer of the child row holds `U` or `X` on it or on a
window over it to its decide, and `U` is refused by all of them. A
momentary `U` refuses a concurrent child `UPDATE`'s ask for the length of
grant-and-release and wakes it (`:614`) - a spurious re-run, never a wrong
answer, the shape today's `S` probe has against an `X` asker.

**The guard narrows** `[design]` - a correction the fence lens found. The
guard at `:4792-4793` is the mode-agnostic `Holds`, and under `U` it
skips the probe in a case that is new: `p <- c <- g`, `T1` inserts into
`g` and holds `S` on child row `c5` (`:4542-4546`); `W` on another core
updates a non-fk column of `c5` and is granted `U(c5)` over `T1`'s `S`;
`T1` deletes `p7` (`c5.pid = 7`), takes `X(p7)` (`W` holds no `S` on it),
walks `c` and reads `c5` busy - and `Holds(Tuple(c, 5))` is true through
`T1`'s own `S`, so the probe is skipped, no wake is registered, and the
`DELETE` parks on `NoteBlockingWriter`'s `IsInFlight` poll (`:4814`,
`:441-448`), bounded by the net - a new instance of "what still polls"
(`known-gaps.md`, Locks). Today the script never reaches it: `W`'s ask is
`X(c5)` and `T1`'s `S` refuses it. The guard's reason under `U` is a `U` or
`X` this transaction already holds on the child row (the probe would be
`already_held` by `Covers(U, U)` or `Covers(X, U)` at `:193` and the grant
arm's `ReleaseOne(kUpdate)` would erase a record the probe never added),
not an `S`: release matches by mode, so `ReleaseOne(kUpdate)` cannot drop
an `S`. The guard becomes `!HoldsCovering(unit, kUpdate)`
(`lock_table.hpp:482-487`), which `U` and `X` satisfy and `S` does not.
With it, `T1`'s probe `U(c5)` is refused by `W`'s `U` with a wake on
`Tuple(c, 5)` and the `DELETE` parks on `W`'s release. A held `U`/`X` on
the child row cannot coexist with a busy read of it by this transaction
(its own write is visible to its check view; a foreign `U`/`X` would have
been refused by its hold), so the narrowed guard loses nothing.

**C.3 The fence rule: an interval unit in any non-intention mode is a
fence.** The three sites filter by mode name: `IsFence`
(`lock_table.cpp:26-28`: `key.IsFenceUnit() && (mode == kShared || mode ==
kExclusive)`), read at `:167`, raising the counter at `:174`, giving it
back at `:194`/`:209`/`:225`, gating the fence-side verify at `:339`,
lowering at `:606-608`; `FenceCoversKey`'s skip at `:673`;
`ConflictingOverlap`'s skip of `IS`/`IX` at `:733-736`. **A premise this
order states loosely**: both scans already read the compatibility table
after their filter (`Compatible(h.mode, want)` at `:674`,
`Compatible(h.mode, mode)` at `:742`; pinned by
`AFencesModeDecidesWhichDescendantsItAdmits`, `lock_table_test.cpp:1053`),
so what is not mode-aware is the **name filter in front** - two spellings
that agree on four modes and disagree on a fifth. A range `U` today raises
no counter, so a tuple ask never scans (`:350`); it runs no verify
(`:339-343`); and `:673` would skip it if a tuple ask scanned anyway. The
consequence without this item is the C4 (ii) `[loud]` shape reached by
omission: a point `UPDATE` inside an `UPDATE`'s window, or a second
overlapping bulk `UPDATE`, writes at once and the fenced statement meets
its version mid-walk, refused by the header with rows already written
(`:10659-10672`).

**One predicate** `[design]`: `constexpr bool IsIntention(LockMode m)` in
`lock_table.hpp` beside the tables; `IsFence` becomes `key.IsFenceUnit()
&& !IsIntention(mode)`; `:673` becomes `if (IsIntention(h.mode))
continue;`; `:733-736` the same call. The verify gate is untouched (it
reads `fence` and the unit); raise and lower share the one predicate, or
the counter is permanently nonzero or underflows (`:600-605`). By the
matrix alone: a range `U` raises the counter and lowers it; a tuple `S`
under it scans and is admitted (`:674`); a tuple `U` or `X` under it is
refused naming the fence holder with the wake on the fence's entry
(`:675-676`); a range `U`'s own verify admits tuple and range `S` inside
it and refuses tuple `U`/`X` and range `U`/`X` (`:742`); two range `U`s
refuse each other; range `IS`/`IX` stay unfenced (item 27, the skip
semantically unchanged for the four old modes). `FenceCoversKey`'s default
`want = kExclusive` (`lock_table.hpp:693`) stays; a tuple ask passes its
mode (`:351`). **What changes for a tuple `S` under a bulk `UPDATE`**: the
scan's **outcome**, from refused to admitted, not its cost - today's range
`X` already raises the counter and every tuple `S` under it already runs
`FenceCoversKey` before being refused (M5's about 1 µs is paid in both
trees). The three statements of the fence rule in the tree - `txn.md:913-919`
("what still stops a write inside a fence is every unit that is not an
intention"), `txn.md:954-958` and `rules.md:31` ("a striped `S`/`X`-fence
counter"), `lock_table.cpp:23-25` - become one: `:913-919` is the rule,
the other three are amended to it (§8.7).

**D. Soundness, in `lock_table.hpp`'s publish-then-verify terms.** The
header's argument (`:108-135`): both sides publish first and verify
second; "suppose a fence F and a write T of a key inside it both proceed.
T's scan of fences did not see F, so it ran before F was published; F's
scan of tuples did not see T, so it ran before T was published. Each side
publishes before it scans, so `publish(T) < scan(T) < publish(F) < scan(F)
< publish(T)` - a cycle, and therefore impossible." Its scope (`:137-138`):
never both granted, not never both refused.

*Unchanged for a range `U` fence F against a tuple `U` or `X` write T
inside it.* The argument orders four events and names no mode: F raises
the counter at `:174` before publishing at `:308` and scans at `:342`
with no latch of its own; T publishes at `:308`, reads the counter at
`:350` (a zero reading means F has not published, so F's scan will see T)
and scans at `:351`. Suppose both granted: the same cycle closes. The mode
enters only in what each scan does with a holder it finds - the two
`Compatible` reads at `:674` and `:742`, from one table - after the
ordering has guaranteed it is found, and the symmetry `static_assert` (§A)
is what makes the two reads one question: `Compatible(U, X) ==
Compatible(X, U) == false`, `Compatible(U, U) == false`. Whichever side
scans second finds the other and unwinds (`ReleaseOne` at `:360`, lowering
the counter through the same `IsFence` at `:606-608`). "Never both
refused" stays as today for `U`/`U` and `U`/`X`: each scan registers a
wake on the other's entry under the latch that saw it, both unwind and
flip each other, one round is a victim or a re-run
(`TwoOverlappingAsksThatBothPublishFirstAreBothRefusedAndBothWoken`,
`lock_table_test.cpp:1325`). A tuple `S` and a tuple `U`/`X` on one key are
one entry under one latch (`:177-186`), so §C.1's point case needs no
verify step at all.

*When the matrix admits the pair.* For a range `U` fence F and a tuple
`S` T inside it - a bulk parent `UPDATE` and a child's key share - both
scans read `Compatible(U, S) == Compatible(S, U) == true` and neither
unwinds: **both granted is the matrix's answer, not a race the argument
failed to close**; the same holds today for an `S` fence over an `S` tuple
(`lock_table_test.cpp:1053-1083`). Without symmetry one side could unwind
while the other kept its grant by arrival order, which is why the
`static_assert` is part of the build.

*Two writers never both proceed on one version.* That is the header's:
every write site runs `CheckWriteConflictBlocking` after its borrow
(`:10150`, `:12785`), and `CheckWriteConflict` (`txn/manager.cpp:188-206`)
refuses a writer whose row's current writer is in flight or committed
after its view, under a live write hold with no suspension between the
header read and the grant. A matrix cell decides who waits with a kick and
who is refused by the header, never whether two versions land.

**E. The waits moved.** RR = `REPEATABLE READ`, decided by the identity
test at `:11660-11663` (`rerun = (blocker != 0 && blocker != cur) ?
kCapable : kFutile`, no mode) and `NoteBlockingWriter`'s arm at
`:11153-11156`; RC = `READ COMMITTED`.

| shape | today | under `U` |
|---|---|---|
| point `UPDATE` of parent `P` while a child writer holds `S(P)` | tuple `X` refused by `S` (`lock_table.cpp:223`); parks until the child decides (`foreign-keys.md` §2c) | tuple `U` admitted; **a wait removed** |
| child `INSERT` (or `UPDATE` setting the fk) while a point `UPDATE` of `P` is in flight | `S` refused by the `X`; parks | `S` admitted; the descent reads busy, §C.1 reads present; placed. **A wait removed** |
| child `INSERT` while a bulk `UPDATE`'s window holds range `U` over `P` | `S` refused by range `X` in `FenceCoversKey`; parks on the fence | `S` admitted; present. **A wait removed** (the 64-partition scan runs in both trees) |
| bulk `UPDATE` window over parents with open child `S` | range `X` refused in `ConflictingOverlap` (`:742`); RC parks before the walk; **RR is refused `TxnConflict`** (the window asks `kFutile`, `:10028-10029`) | range `U` admitted; proceeds at every level. **A wait and a refusal removed** |
| the reverse probe against a child `UPDATE` in flight (other columns or a move-off) | `S` probe refused by the child's `X`; parks on the child row | `U` probe refused by `U`; parks the same. **Unchanged** (an `S` probe would spin to the net, §C.2) |
| the reverse probe against a child `DELETE` in flight; against a bulk child `UPDATE`'s or `DELETE`'s window | refused by `X` or range `X`; parks | refused by `X`, range `U` or range `X`; parks. **Unchanged** |
| the reverse probe where the `DELETE`'s own transaction holds a grandchild `S` on the busy child row | unreachable (the child writer's `X` was refused by that `S`) | reachable; the narrowed guard asks the probe, refused by the child's `U`, parks with a slot. **A new wait, with a kick** |
| point `UPDATE` inside a bulk `UPDATE`'s window; point `DELETE` inside an `UPDATE`'s window; point `UPDATE` inside a `DELETE`'s window | tuple vs range `X`: refused (`:351`, `:674`); `kCapable` park at every level unless the fence already wrote the row | tuple `U`/`X` vs range `U`/`X`: identical. **Unchanged** |
| two bulk `UPDATE`s overlapping; a bulk `UPDATE` over a bulk `DELETE`; a named `INSERT` inside an `UPDATE`'s window | range vs range, tuple `X` vs range `X`: refused before the walk, waits | range `U` vs range `U`/`X`, tuple `X` vs range `U`: waits. **Unchanged** |
| two point `UPDATE`s of one row; an `UPDATE` against a `DELETE` of one row | `X`/`X`: the second parks (RC) or is refused first-updater-wins (RR, `blocker == cur`) | `U`/`U`, `U`/`X`: identical. **Unchanged** |
| `ON CONFLICT (unique) DO UPDATE` whose holder row carries a child's `S` | arbiter tuple `X` (`:6698`) refused; parks | arbiter `U` admitted; proceeds. **A wait removed** |
| `ON CONFLICT (tuple key) DO UPDATE` on a key a child's `S` holds | up-front `X` (`:6660`) refused; parks | kept `X`; parks. **Unchanged**, over-strict for the update arm, stated |
| two child writers that then update their parent (`fk_cross_core_rig_test.cpp:467-494`) | both hold `S(P)`, both ask `X(P)`; the first parks, the second closes the cycle and is **refused naming deadlock** | the first `U` is granted over the other's `S`; the second `U` is refused by the first's `U` and parks, one edge, no cycle; at the first's `ROLLBACK` it answers `UPDATED 1` (after a `COMMIT`: RC re-runs with a fresh view and proceeds; RR is refused first-updater-wins as under `X`). **A refusal becomes a wait** |
| `UPDATE P` then `DELETE P` in one transaction while a child writer holds `S(P)` | the `UPDATE`'s `X` waits on the `S`, **holding nothing on `P`**; the `DELETE`'s `X` is then `already_held` | the `UPDATE` proceeds; the `DELETE`'s `X` is refused by the `S` and parks **holding `U(P)`**. **The wait moves from the `UPDATE` to the `DELETE`** - F.1 (i) |
| a parent updater that, past a child writer's `S(P)`, goes on to wait on that child writer **anywhere** - at a `DELETE` of `P`, at another row the child wrote, mid-walk in a per-row statement | the updater waited at `P` first, holding nothing on `P`; the child's later write of `P` found nothing to wait for | the updater passes `P` holding `U(P)` and waits later; the child's write of `P` then closes a cycle. **A deadlock where there was none** - F.1, `[loud]`, CP-Q10 |
| every `INSERT` shape against `S`/`X`; a parent `DELETE` against a child `S`; the online build's drain against an open `UPDATE` | waits; waits; `IX` holders waited out | **Unchanged** |

**No wait becomes a refusal by the design as written, with one exception
carried to the operator (§F, CP-Q10).** Three conditional `[loud]`s are
each avoided by landing items 1-3 and the guard in one commit: without
item 1 a child `INSERT` under a parent `UPDATE` reaches `:4647-4658` and
is refused; without item 2 the probe spins to the net; without item 3 a
range `U` is invisible and the fenced statement is refused mid-walk with
rows written. What a client sees change in the views: `sys.lock_waits`'
`mode` reads `U` for an `UPDATE`'s row or window wait (the row carries the
asked mode, `command_dispatcher.cpp:307-322`;
`LockWaitsViewTest.AGrantedRowWaitIsOneRowNamingItsUnitModeAndHolder`
expects `{"X"}` at `lock_family_test.cpp:3208` for an `UPDATE` waiter and
flips; `:3331`, a `DROP`'s relation `X`, stays); the conversion shape's
`deadlock` outcome rows become `granted` rows; `app.lock_conflict` counts
fewer events for the shapes `U` admits. The wait-event dictionary carries
no mode (`wait_event.hpp:147-152`), so it gains nothing - this order's
"the wait-event dictionary gains a mode name" is corrected (§8.11).

**F. New and kept shapes.**

*F.1 The class of deadlocks `U` adds - `[loud]`, CP-Q10.* What changes is
**where a parent updater first waits**. Today an `UPDATE` of `P` that meets
a child writer `T2`'s `S(P)` waits there, holding nothing on `P`, and
`T2`'s own later write of `P` finds nothing to wait for. Under `U` the
updater `T1` is granted `U(P)` over the share and keeps going; if it then
waits on `T2` **for any reason**, and `T2` then writes `P`, the two edges
close a cycle (`NoteWaitFor`, `lock_table.cpp:378-410`; the edges drawn at
`command_dispatcher.cpp:413-414`) and whichever added the closing edge is
refused naming deadlock. Three instances, each reachable from client
statements:

- **(i) one row, update then delete.** `T1`: `UPDATE P` -> `U(P)` granted,
  no edge. `T1`: `DELETE P` -> `X(P)` at `:12782`, own `U` skipped
  (`:222`), `T2`'s `S` refuses; the `DELETE` has written nothing of its
  own, so it parks whole (`:399-473`) **holding `U(P)`**, edge `T1 -> T2`.
  `T2`: `UPDATE P` (the "insert a trade, then update its account"
  statement) -> refused by `T1`'s `U`, `T2 -> T1` closes the cycle, `T2`
  is the victim; in the other order `T1` is. Today: `T1`'s `UPDATE` is
  refused by the `S` and parks holding nothing on `P`; `T2`'s `UPDATE`
  finds only its own `S` and proceeds; `T2` commits; `T1` re-runs.
- **(ii) two rows, explicit transactions.** `T2`: `BEGIN; INSERT INTO c
  VALUES (.., 7)` -> `S(p7)`; `UPDATE q SET .. WHERE id = 5` -> `U(q5)`.
  `T1`: `BEGIN; UPDATE p SET .. WHERE id = 7` -> `U(p7)` granted; `UPDATE q
  SET .. WHERE id = 5` -> refused by `T2`'s `U(q5)`, edge `T1 -> T2`. `T2`:
  `UPDATE p SET .. WHERE id = 7` -> refused by `T1`'s `U(p7)`, cycle. Today
  `T1` parks at its first `UPDATE` holding nothing on `p7` and `T2`
  proceeds. `q` may be another parent row - a counterparty's account.
- **(iii) one autocommit per-row statement.** `UPDATE p SET v = v + 1
  WHERE v > 0` bounds no key, so `DeclaredWriteBorrow` declares nothing
  (`:11409-11441`) and the walk asks per row: granted `U(7)` over `T2`'s
  `S(7)`, it parks mid-walk on a row `T2` wrote, with rows written; a
  mid-walk park with rows written carries the statement's transaction
  (AO-S3b), so the edge is drawn, and `T2`'s `UPDATE` of row 7 closes the
  cycle. Today the statement stops at row 7 - refused by the `S`, holding
  nothing on 7 - and `T2`'s `UPDATE` of 7 is granted.

In every instance a statement that proceeds today is refused under `U` as
a deadlock victim - a genuine cycle (`T1` holds `P`'s version and waits for
`T2`'s decide; `T2` holds `P`'s existence-share and wants its version),
correctly detected, refused retryably, and inherent in any matrix that
lets the `UPDATE` proceed over the share (PostgreSQL's `FOR NO KEY UPDATE`
over `FOR KEY SHARE` has the same class). The class needs a parent updater
that goes on to wait on the very child writer whose share it passed, and
that child writer then writing the parent. What `U` removes in exchange is
§2c's conversion deadlock between two child writers that each update their
parent - the commoner OLTP pair - and the parent `UPDATE`'s wait. The
variant where `T2`'s later statement is `DELETE P` is two refusals in both
worlds (deadlock under `U`, its own child's `FK_VIOLATION` today); the
message changes. Carried to the operator as **CP-Q10** (§4): C2 is built
only on its mark. *F.1b* `T1` holds `U(P)` and `X(P)` and `T2` asks
`S(P)`: the in-entry scan visits every holder, finds the `X`, refuses - a
child cannot be made to reference a row whose deleter is open.

*F.2 The `REPEATABLE READ` guard* reads identity, not mode. A tuple `U`
holder that wrote the row: `blocker == cur` -> `kFutile` -> an RR waiter is
refused at once, an RC one parks; one that holds the unit but has not
overwritten: `kCapable` -> both park. Identical to `X`. A range `U` fence
that has not reached the row: `kCapable` -> parks, as under a range `X`;
if the fence writes the row meanwhile the re-run is refused
first-updater-wins (`txn.md` §5). The declared window's own `kFutile`
(`:10029`, `:12743`) is "cannot tell", not a mode fact. The only pairs
that change (`S`/`U`, `U`/`S`) now admit, which never produces a blocker,
so they cannot reach the guard.

*F.3 The deadlock detector* is unchanged: edges are `waiter -> holder` by
transaction id with no mode (`lock_table.cpp:378-410`; drawn at `:413-414`,
`:643-644`, `:2729`); `U` adds no edge kind, removes the conversion cycle
(one edge where there were two) and adds F.1's class. A `U` ask is refused only
by a `U` or an `X`, one holder each on a tuple, so its edge is
unambiguous; the "one edge over several `S` holders" sentence
(`lock_table.hpp:160-164`) is `X`'s.

*F.4 AY-S6's Cabin clear* stands: it rests on the `DELETE`'s `X` meeting
the child writer's `S` (`fk_check.cpp:254-263`; `foreign-keys.md` §3a), and
no `UPDATE` reads a child set. *F.5 `TupleKeyPlaced`'s grant argument*
("no other writer holds the key", `:6453-6454`) stands: a `U` holder
refuses an `X` ask. *F.6 The online build's drain* reads `IX` holders and
a `U` ask takes `IX`. *F.7 The unique check's busy-holder wait*
(`:11598-11604`) is a header poll with no wake today and stays one; under
`U` the holder row's writer holds a unit a probe could park on, as item 2
does for the reverse check - not taken by C2, added to `known-gaps.md`'s
"what still polls" at S3. *F.8 Heap children*: an `UPDATE` of a heap child
takes `U` on `Tuple(heap_oid, hidden key)` and the probe names the hidden
key (`fk_check.cpp:331-337`); a heap parent is refused since BZ, so no `S`
lands on a hidden key.

**G. What is `[quiet-wrong]`, so the next verb lands right.**

1. **A write that changes a row's existence or key asks `X`, never `U`.**
   `txn.md` §5's rule `[design]` (§8.7): a borrow in `U` says the row
   keeps its existence and its key whichever way the holder decides, and
   a key share `S` is admitted under it; a write whose decide can end the
   row (a delete-mark), create one (an insert, an `ON CONFLICT` arm that
   may insert included) or free or re-bind a key asks `X` on the tuple or
   its window - the test is what the write does to the slot, not the
   verb's name. Verbs to place under `X` when they land: `MERGE`'s delete
   arm, a revived tombstone, a tuple-key `UPDATE` (refused today), FK
   `CASCADE`, a `PURGE` widened to live keys, `TRUNCATE`. The runtime
   trace of a mistake is §C.1's witness: a refusal plus a `Warn`, never a
   wrong answer.
2. **The witness keeps both fields** (the `deleted` bit and the
   predecessor) and **applies only behind `locks_ != nullptr`**: off the
   table nothing refuses an inserter, and an insert-then-update by one
   in-flight transaction shows `undo_ptr != kNoUndoPtr`.
3. **`Covers(U, S)` is true**, so a transaction that updated a parent
   takes no `S` record for a later reference; a tuple `U` is therefore
   released only at the decide, by the verify's unwind of the grant it
   just published (`:360`), or by the probe's give-back of a `U` it did
   not hold before (the narrowed guard).
4. **One fence predicate at both ends of the counter** (`:167`/`:174` and
   `:606`), read by `IsFence`, `:673` and `:733-736` alike; a sixth mode
   is classified by `IsIntention` and nowhere else.
5. **The compile claim is false** and the `static_assert` block is what
   makes it true; the quiet half - an unfilled `kCovers` row costing a cap
   slot per re-ask - is the one nothing else would catch.
6. **`ON CONFLICT (tuple key) DO UPDATE`'s up-front unit stays `X`**
   (`:6660`): it covers the insert arm; moved to `U` a placement would
   run under a key share.
7. **Starvation is unchanged in kind**: a `DELETE`'s `X` against a stream
   of child `S` arrivals can reach the net today and under `U`
   (`TryAcquire` is non-queueing); `U` removes the parent `UPDATE`'s
   instance and adds none; C5 is not built (CP-Q9 (a)).

### 8.4 C3 and C4 - recorded, not built

**C3, the per-relation lock header.** Its rule (§3): "built only if M1
puts the relation ask, or M5 the walk, at or above a point write's floor
at this box's core count; otherwise recorded." §7's M1 and M5 rows carry
the numbers: the family's whole share about 0.4 µs of a 39 µs point write,
the relation ask inside it and not separated; the walk about 1 µs per
point statement; BP-H10's eight-core +12 % untested. `[design]` S1 reads
"a point write's floor" as the write's own - its p0 is about 36 µs - and
not the harness's 0.2 µs noise floor, which is a property of the
measurement, not of the statement; 1 µs is below it. **Not built on S0's
numbers.** Left open: C3 as a measured item for an eight-core host,
re-proposed as its own item if that run reproduces BP-H10's +12 %, with
its correctness argument as §3 wrote it (the fence list presumes rows
still publish, which with C4 not built they do). The reading of "floor"
is CLA's and changes whether C3 is built, so it is **CP-Q12** (§4); a mark
is needed only if the operator meant the noise floor.

**C4, the row's table entry made lazy.** Its rule (§3; CP-Q4 (a)): "built
if M1 puts A5's ask-and-release at or above a point write's floor, or if
M3 shows header-named waits ending at the net; recorded otherwise." M1: A5
is inside the 0.4 µs. M3: scenario 2's 84 lock waits all granted, deadlock
0, `fault_net` 0, `app.lock_wait` 0.03 % of DB time; scenario 0's lock
waits 0; M4's 17 parked asks all granted, `fault_net` 0, its 2,519
client-visible refusals the MVCC window's (§8.5). No wait ended at the
net on any cell. Judged also by §2's structure clause ("re-shaped iff the
cells left standing make a cheaper one sound"), the nine cells make nothing
measurably cheaper. **Not built.** Left open: the **decide slot** - one
slot per in-flight transaction, flipped where `Release` runs today
(`FinishCommitBody` after `RetireInFlight` and the schema word,
`txn/manager.cpp:325-336`; `Abort` `:611`, `:626`) - as the design that
would close `known-gaps.md`'s "what still polls has no slot behind it: a
holder only the header names, an assertion's group, a `PURGE`'s horizon
wait", to which §8.3 F.7 adds the unique check's busy holder. It is
outside CP: no number asks for it, and its (ii) is a `[loud]` risk that
only (α)'s grace period closes (CP-Q4 (a) marks (α) for the day it is
built). Recorded as a design for its own order.

### 8.5 The boundary: M4's decided-newer refusal is `txn.md` §5's

M4 refused 2,519 of 32,000 hot statements - `TXN_CONFLICT retryable=1 row
id=1 was written by transaction N` - while the family parked 17 asks and
granted every one. The refusal is `CheckWriteConflict`'s last arm
(`txn/manager.cpp:188-206`): the row's current writer committed after the
statement's view, and READ COMMITTED aborts retryably rather than
re-reading (`txn.md` §5, "a deliberate simplification - there is no
re-read loop"). No lock cell reaches it: at the refused ask the earlier
writer has committed and released - under `group` before its sync (M6) -
so the table has no holder to refuse against and grants, and the header
then names a writer the snapshot cannot see. The in-flight case, the only
one a lock can see, is the one the table already parks and grants. A
decide slot (C4) relieves the in-flight case only; the `U` mode changes
which mode refuses, not whether a decided writer is visible; removing the
arm is a re-read loop, an isolation-level change §6 excludes. **S2 does
not treat it as a candidate.** The hot row's 7.9 % is the MVCC window's.

### 8.6 The structure stays (CP-Q1 (a))

With C3 and C4 recorded, nothing re-shapes the table; §1.1's structure
stands. The two-source sentence gains three words: the header names the
row's last writer; the table adds the wait, the `S` mode **and the `U`
mode**, not the lock. The soundness argument S1 owes is §8.3 D's alone;
C1 changes none of the counter's semantics for ranges (`IsFenceUnit`
reduces to `unit == kRange` and every site reads the unit through it).

### 8.7 The amendments owed, by document

Each lands with the sub-stage that makes it true (S2a for C1, S2b for `U`)
or at S3; none is applied by S1, since `docs/spec/` states what is built.
AR2's two rows are amended now (§8.11), since they record the operator's
marks of 2026-11-08.

- **`docs/spec/txn.md` §5.** Beside the two-source model (`:699-712`), a
  paragraph `[design]`: *"Five modes since CP (CP-Q2 (a),
  `raft-marks-2026-11-08.md` §1): `IS`, `IX`, `S`, `U`, `X`. `U` is a
  version claim: its holder may write a new version of the row - or of
  every row of its window - and nothing else; the row keeps its existence
  and its key whichever way the holder decides. `X` is an existence claim.
  `UPDATE` asks `U` on its tuple or its declared window, and so does `ON
  CONFLICT ... DO UPDATE`'s update arm on the row a unique arbiter found;
  `DELETE` asks `X` on its tuple and its window; every `INSERT` asks `X`
  on its key, `ON CONFLICT DO UPDATE` on the tuple key included, whose
  up-front unit covers an insert. `U` admits a foreign key's share `S` and
  refuses `U` and `X`; `U` covers `S` and `IS`, `X` covers `U`. The rule
  for a verb not yet written: a write that may end a row's existence or
  re-bind its key asks `X`; placed under `U` it would pass every key
  share. A fence is an interval unit held in any non-intention mode - `S`,
  `U` or `X`: the relation's fence counter counts them, the tuple probe
  reads them, the compatibility table says what each admits, and an
  intention mode on an interval unit neither fences nor is fenced (AO-0
  item 27). The class `U` adds (CP-Q10): a parent updater granted `U` over
  a child writer's share, that then waits on that child writer for any
  reason, deadlocks with it if the child writer then writes the parent
  row - refused retryably naming deadlock - where before CP the updater
  waited at the parent row first, holding nothing there."* (F.1 has the
  instances.) Then:
  `:717-720` "a row's `X`" -> "a row's `U` or `X`"; `:782-790` "borrows it
  in `X`" -> "in `U` for an `UPDATE`, `X` for a `DELETE`; an `UPDATE`'s
  window waits for every writer of a row of `t` and blocks every writer
  for its walk while admitting a key share on any row"; `:797-801` the
  counter is raised for a `U` window too; `:833-843` the forward check's
  park narrows to `X` holders, an in-flight `UPDATE`'s parent read present;
  `:913-919` named as **the** fence rule, "a tuple `X`" -> "a tuple `U` or
  `X`", its slice clause C1's (§8.2); `:954-958` "striped `S`/`X`-fence
  counter" -> a counter of interval units held in any non-intention mode;
  `:936-942` gains the probe's `U`; the slice sentences per §8.2.
- **`docs/spec/foreign-keys.md`.** `:169-172` the verdict row splits:
  "overwritten by an in-flight foreign `UPDATE` -> pass, its `U` having
  admitted the `S`; inserted or delete-marked in flight -> the `S` was
  refused before the read and the statement waits; without a table ->
  busy (F3)". `:232-238` "A writer of the parent row holds its `X`" -> "An
  `INSERT` or a `DELETE` of the parent row holds its `X`, a `DELETE` with a
  window a range `X`, met in the verify; an `UPDATE` holds `U`, which
  admits the `S`". `:242-245`'s last sentence -> *"Busy under a granted
  `S` has one cause with a table, and it reads present: the view is minted
  after the grant, every writer that places, delete-marks or retires the
  row holds `X` to its decide and `X` refuses `S`, so the writer the view
  cannot see holds `U` - an `UPDATE`, whose head is not delete-marked and
  has a predecessor; the check passes. A busy head that is delete-marked
  or has no predecessor is a verb placed under the wrong mode, refused
  retryably as before and logged."* §2c `:271-279` -> *"An `UPDATE` of a
  parent row proceeds past open child writers of it (CP): its `U` admits
  their `S`, and a child's check reads a parent under an open `UPDATE` as
  present; only a `DELETE` waits for them. Before CP the `UPDATE` took `X`
  and waited, and two transactions that each wrote a child of P and then
  updated P deadlocked; they now serialise on P's `U`
  (`FkCrossCoreRigTest.TwoChildWritersThatThenUpdateTheirParentSerialise`).
  The class that replaces it is narrower (CP-Q10): a parent updater
  granted `U(P)` over a child writer's share that then waits on that
  writer - at a `DELETE` of P, at another row it wrote - deadlocks with it
  if the child writer then updates P; `txn.md` §5 states it."*;
  `:300-305` gains "or its `U`, which changes neither
  existence nor key"; `:317-319` -> "The existence/version split is built
  as a mode since CP (`U`, `txn.md` §5), on the deadlock above, not on a
  measured cost (CP-S0)". §3a after `:353-358`: *"The `DELETE` gets its
  slot from a probe in `U` on the child row: refused by the row's own
  writer in `U` or `X` and parked on that entry's wake; admitted by a
  grandchild writer's `S` and given back at once. In `S` the probe would be
  admitted by a child `UPDATE`'s `U` and the `DELETE` would re-run on a
  pre-flipped slot until the writer decided."*
- **`docs/rules/rules.md:31`**: "A striped `S`/`X`-fence counter" -> a
  counter of interval units held in any non-intention mode; "relation,
  range, slice, tuple" wherever the row names the units.
- **AR2** (`instructions/v3.0.0/ar2-architecture-revision-borrow-model.md`):
  E1 (`:622`) and the Modes paragraph (`:165-168`) carry "**Amended
  2026-11-08 (CP-Q2 (a), `raft-marks-2026-11-08.md` §1):** `U` added ..."
  with the ratified text kept, the file's convention; the fence definition
  (`:170-172`) gains `U` and loses "on a slice"; the `UPDATE`/`DELETE` row
  (`:193`) splits its mode; E12 (`:633`) "**Amended 2026-11-08 (CP-Q3
  (a)):** the unit is un-ratified - no read declares a position until a
  mover exists; the relation `IS` at the bind (AT-S1) is what stands"; E4
  (`:625`) and R6 on CP-Q11's mark. **Applied by S1** for E1 and E12
  (§8.11).
- **`include/kds/txn/lock_table.hpp`**: `:34` ("no update mode (E1)"),
  `:236-239` (the compile claim -> the `static_assert`s), `:249-253` (the
  covering comment), `:307-311` (`IsFenceUnit`'s "`S` or `X` fence" -> any
  non-intention mode), `:100-106`, `:676-677`, `:736`, and the slice prose
  (§8.2 row 7); `src/txn/lock_table.cpp:23-25`, `:668-672`, `:716-732`.
- **`src/server/command_dispatcher.cpp`** comments `:4557-4560`,
  `:4648-4653`, `:4777-4779`, `:4788-4790`, `:6701`, `:10015-10016`,
  `:10578-10579`, `:11314-11316`; `command_dispatcher.hpp:193-196`,
  `:1132-1134` ("`X` for a writer's row, range or fence" -> "`U` for an
  `UPDATE`'s row or window, `X` for a `DELETE`'s and an `INSERT`'s, `S` for
  a foreign key's parent row"); `exec/fk_check.hpp:74-80`.
- **`CLAUDE.md`**: the Transactions & MVCC row (`:56`) loses "and the
  slice it has reached ... with the slice the outermost walk's" and gains
  the `U` mode; the Foreign keys row (`:70`) drops "a parent `UPDATE`
  waits for open child writers, two child writers that then update their
  parent deadlock" and names CP-Q10's shape in its place.
- **`docs/spec/sys-views.md` §6.3** (`:185-190`): one sentence naming
  `mode`'s domain, `IS`, `IX`, `S`, `U`, `X`, and `unit`'s, relation,
  range, tuple, index key. **`manual/sql/sql.md:302-306`**: "A parent
  being inserted or deleted is waited for, not refused; a parent being
  updated is read as present, since an update keeps the row and its key"
  - naming no mode (§6). **`tools/cp_s0_release_order.py:35`**: "`mode`
  X" -> "`mode` U" in its docstring. **`docs/inflight/known-gaps.md`**
  Locks (S3): the §2c pair recorded as closed by C2, CP-Q10's shape
  recorded as added, the unique check's busy holder added to "what still
  polls"; "a wake is not a grant" stands.

### 8.8 The cells S2 adds or amends

**S2a (C1)**: §8.2's inventory rows 9-15. **S2b (`U`)**:

- `tests/lock_table_test.cpp`: `TheCompatibilityMatrixIsTheOneAR2States`
  (`:51-90`) - 25 literal expectations, the symmetry loops over five;
  `EveryConflictingModePairIsRefusedOnOneUnit` (`:92-125`) - `kModes` gains
  `kUpdate`; `ARepeatAcquireOfACoveredModeAddsNoEntryAndSpendsNoCap`
  (`:144-170`) - `weaker` gains `kUpdate`, plus the six `U` covering
  literals; new `AVersionClaimCoversAShareAndIsCoveredByExclusive`; new
  `AShareThenAVersionClaimThenAnExistenceClaimAreThreeEntries` beside
  `:127-142` (`S`, `U`, `X` by one transaction -> three records, each
  `!already_held`; `ReleaseOne(U)` leaves `S` and `X`; `U` then `S` ->
  `already_held`); new `AVersionClaimFenceAdmitsAShareAndRefusesAWriterInsideIt`
  beside `:1053` (range `U`: counter 1; tuple `S` inside admitted; tuple
  `U` and `X` refused naming the fence with `wake.key` the range; both
  orders; range `S` over it admitted, range `U`/`X` refused; counter 0
  after release); `TheFenceCounterIsTheProbesGateAndARelationLockIsNotAFence`
  (`:382`) gains a range `U` raising and lowering and a refused one
  leaving it; `TwoOverlappingAsksThatBothPublishFirstAreBothRefusedAndBothWoken`
  (`:1325`) a second pass with both fences in `U`; new
  `AProbeInUpdateModeIsAdmittedByAShareAndRefusedByAWriter`; the
  `static_assert` block and `EXPECT_STREQ(LockModeName(kUpdate), "U")`.
- `tests/fk_cross_core_rig_test.cpp` (the rig's `c` gains a non-fk column,
  `:206`, and the rig a `child_oid`): `TwoChildWritersThatThenUpdateTheirParentDeadlock`
  (`:467-494`) becomes `...Serialise` - core 0's `UPDATE` answers `UPDATED
  1` at once with no waiter on `Tuple(p, 7)`, core 1's is `ParkedOn(1,
  core1, 3, parent_oid, 7)` with no "deadlock" in any reply, core 0's
  `ROLLBACK` releases and core 1 answers `UPDATED 1`; the reason stated in
  the cell (CP-Q7 (a): a refusal became a wait). New
  `TwoChildWritersThatThenDeleteTheirParentDeadlock` (the same script with
  `DELETE`: the `X` family keeps the conversion deadlock).
  `AParentDeleteWaitsOutAChildMovedOffItAndRefusesAtItsRollback`
  (`:519-536`) gains `ParkedOn(0, core0, 1, child_oid, 1)` beside
  `StillWaiting` (the item-2 pin: a park, not a spin). New
  `AParentDeleteParksOnAChildUpdatedInOtherColumns` (core 1 `BEGIN; UPDATE
  c SET note = 1 WHERE id = 1` held; core 0's `DELETE FROM p WHERE id = 7`
  parks on `Tuple(c, 1)`; at core 1's `COMMIT`, `FK_VIOLATION`). New
  `AParentDeleteParksOnABulkChildUpdatesWindow` (core 1 `BEGIN; UPDATE c
  SET note = 1` -> range `U`; core 0's `DELETE` parks with a waiter on
  `Range(c, 0, kIdSpaceEnd)`; `ROLLBACK`; `DELETED 1`). New
  `AChildInsertProceedsPastAParentUpdateOnAnotherCore` (core 0 `BEGIN;
  UPDATE p SET v = 1 WHERE id = 7` held; core 1 **`BEGIN;`** `INSERT INTO c
  VALUES (7)` -> `INSERTED` with no waiter on row 7; core 0's `DELETE FROM
  p WHERE id = 7` then parks on `Tuple(p, 7)` and at core 1's `COMMIT`
  answers `FK_VIOLATION`; a rollback arm where core 0 `ROLLBACK`s and
  `SELECT id FROM p WHERE id = 7` answers `7`) - the `BEGIN` is
  load-bearing, since an autocommit `INSERT` releases its `S` at its end.
  New `ARangeUpdateOfParentsPassesAChildsOpenReference` beside `:452`
  (`UPDATE p SET v = 1 WHERE id >= 7` answers without parking; the
  `DELETE` sibling keeps parking). New
  `AnUpdateThenADeleteOfAParentUnderAChildsShareWaitsAtTheDelete` with
  its deadlock arm (F.1 (i)) and new
  `AParentUpdaterThatThenWaitsOnTheChildWriterElsewhereDeadlocks` (F.1
  (ii): the rig gains a relation `q`; core 1 `BEGIN; INSERT INTO c VALUES
  (1, 7); UPDATE q SET v = 1 WHERE id = 5`; core 0 `BEGIN; UPDATE p SET v =
  1 WHERE id = 7` -> `UPDATED 1`, `UPDATE q SET v = 2 WHERE id = 5` parks
  on `Tuple(q, 5)`; core 1 `UPDATE p SET v = 2 WHERE id = 7` -> refused
  naming deadlock) - both on CP-Q10's mark. New three-level cell
  `AParentDeleteParksOnAChildRowItHoldsAGrandchildsShareOn` (`p <- c <-
  g`: core 0 `BEGIN; INSERT INTO g VALUES (1, 5)`; core 1 `BEGIN; UPDATE c
  SET note = 1 WHERE id = 5`; core 0 `DELETE FROM p WHERE id = 7` with
  `c5.pid = 7` -> `WaiterCount(Tuple(c, 5)) >= 1`, the narrowed guard's
  pin).
- `tests/fk_parent_hold_test.cpp`: `TheRelationsIntentionOutlivesAFailedChecksHold`
  (`:456-475`) - `:470-473`'s `UPDATE p SET v = 1 WHERE id = 10` becomes
  the positive expectation `UPDATED 1` and `S(10)` is pinned by
  `Other("DELETE FROM p WHERE id = 10")` refused `TXN_CONFLICT`. New
  synchronous `AParentUnderAnOpenUpdateIsPresentToAChildsCheck` (the
  fixture's `clock_ = nullptr` makes `may_park_` false, `:57-70`; item 1
  converts at the hoist before any park decision, so it holds there;
  `store_.OnFetch(parent_leaf_)` shows the descent ran under the granted
  `S`). New `ABulkUpdateWindowPassesAChildsShareAndABulkDeleteWaitsForIt`.
- `tests/lock_family_test.cpp`: `:3208` `{"X"}` -> `{"U"}` with a
  `DELETE`-waiter sibling keeping `{"X"}`; new
  `AChildWhoseParentIsUnderAnUpdateWindowIsNotHeldUp` beside `:789` (the
  `DELETE`-window cell keeps parking); new `ANamedInsertInsideAnUpdateWindowWaits`;
  new `ARefusedUpdateClaimUnderAFenceIsARangeRowWithModeU`; new
  `AnOnConflictDoUpdateOnAUniqueArbiterPassesAChildsShare` (the tuple-key
  form still waits); new `AnUpdateThenANamedInsertOfTheSameKeyHoldsOneRecordAfterTheDoor`
  and its sibling `AChildsShareSurvivesItsOwnNamedInsertOfTheParentKey` (a
  transaction holding `S(p, 7)` from a child insert runs `INSERT INTO p
  VALUES (7, ...) ON CONFLICT DO NOTHING`: the door's `X` goes back, the
  `S` stays, and another session's `DELETE FROM p WHERE id = 7` still
  waits) - the two `held_before` fixes and their give-back by mode.
- Unchanged, named so the census is complete: `foreign_key_test.cpp`'s
  two busy cells (no table); every fk rig and parent-hold cell resting on
  `INSERT` `X`, `DELETE` `X` or a `DELETE`'s range `X` against `S`;
  `lock_family_test.cpp:670-711`, `:789-823`; `wait_event_doc_test.cpp`;
  the sim corpus and `scripts/bs/difftest.py`, which pin neither the
  conversion deadlock nor the parent `UPDATE`'s wait (grep).

### 8.9 AO-8's carried items, and which S1 meets

§6 says CP does not decide E7, E13 or AO-0's items 22, 25, 26, and names
any it meets. Read off AO-0's table (`workorder-ao-m2-lock-family.md:529-534`)
and AR2 (`:628`, `:634`): **item 22** (an assertion-group wait's bound)
"decided 2026-09-29: one net, no group bound" - not met. **Item 25**
(`DROP TABLE` refused by arriving readers) "decided 2026-09-29: left as it
is" - C1 keeps the relation `IS` the drop waits for and C5 is not built;
not met. **Item 26** (which reads declare a position) "closed as mooted at
AT's close", its built half the outermost walk's position - **C1 meets it**:
after S2a no read declares a position and every read declares the
relation at the bind; the question dissolves, the closed order's row is
history, `txn.md` §5 carries the statement. **E7** marked local by the
operator 2026-09-26 - not met. **E13** (a named-key `INSERT`'s `sys.tables`
row as a tuple unit) open for M3 - every `INSERT` keeps its tuple `X` and
the catalog row is no lock unit; not met. Beside them: **item 19** (the
per-page `IS`, "deferred, priced, and then built" on AO-S7's ≲ 3 µs
extrapolation) is un-built by C1 - the absent consumer is the reason, M2's
88-126 ns the gain; **item 27** loses its live instance and keeps its rule
(§8.2); and AO-8's carried **item 5** - which of AR2's units has a
contender on this engine at all - is answered by §8.1: relation, range,
tuple and index key have contenders, the slice has none, and its "no unit
has an `S` mode" premise ended at AY-S5.

### 8.10 Items for the operator before S2 (CP-Q amendments)

Every `[quiet-wrong]` or `[loud]` choice S1 met, per §5's gate, is a row
appended to **§4's table - the register, and the text the operator
marks**: **CP-Q10** `[loud]`, the class of deadlocks `U` adds, stated
whole in §8.3 F.1; **CP-Q11**, E4 and AR2-R6 re-read as the range unit's,
§8.2; **CP-Q12**, the reading of "floor" in C3's rule, §8.4. CP-Q10 gates
S2b.

**Decided by CLA inside the marks, not raised** - each `[design]`, none
`[quiet-wrong]` or `[loud]`: item 1's witness is the two header fields
behind `locks_ != nullptr`, the no-table arm unchanged (a test-only
assembly; the chain walk is what it would need); a failing witness keeps
today's retryable refusal and logs at `Warn`; `:6660` stays `X` (this
order's text); the enumerator's placement and the `static_assert` block;
the probe's guard narrowed to `HoldsCovering(unit, kUpdate)`; the two
named-key doors' `held_before` read with `HoldsCovering(unit,
kExclusive)` **and their give-backs filtered by mode**, so a held `S` or
`U` on the key stays to the decide (§8.3 B); AO-0 item 27's pin kept over
a range `IS`; `sys-views.md` §6.3's domain sentence; no change to the
wait-event dictionary.

### 8.11 What S1 corrects in this order's own text, applied in place

- §1.1's second bullet and C2's Risk said the two matrices "refuse to
  compile unfilled"; they do not (§8.3 A), and the `static_assert` block
  is what will. Both sentences now say so.
- C2's Risk said "`sys.lock_waits` and the wait-event dictionary gain a
  mode name"; the dictionary carries no mode. Corrected.
- C2's item 1 named the committed predecessor alone; the head's
  delete-mark bit is the other half (§8.3 C.1). A pointer added at the
  item.
- §1.1's second bullet and C2's item 3 named `AcquireInner`'s verify gate
  as the third site that filters by mode name; the gate reads `IsFence`'s
  answer and the unit (`lock_table.cpp:339-353`). The third name filter is
  `ConflictingOverlap`'s skip of `IS`/`IX` holders (`:733-736`), the one
  §8.3 C.3 names; both sentences corrected.
- AR2's E1 and E12 rows carry their dated amendment (the operator's marks
  of 2026-11-08), the ratified text kept and the build gated on CP-Q10;
  E4 waits for CP-Q11.

### 8.12 What S2 does, as S1 ranks it

**S2a - C1**: §8.2's inventory, one commit (row 14's hook is not
optional), with §8.7's C1 amendments; gated by nothing but S1's landing.
**S2b - `U`**: §8.3's code shape (A's enumerator, tables, `IsIntention`
and `static_assert`s; the four ask sites and the probe's literals; C.1's
witness at the hoist; C.2's narrowed guard; B's two `held_before` reads
and their give-backs by mode) and §8.8's cells, one commit, with §8.7's
`U` amendments; **gated by CP-Q10's mark**. CP-Q11 gates only the wording
of one cell's comment and E4's row in S2a; CP-Q12 gates nothing unless
(b). **No S2c, no S2d**: C3 and C4 are recorded (§8.4). S3 then measures
the milestone's whole change (S2a and S2b against `cc54a296`), scenario 0
and 2 beside M1's point write, and rewrites `known-gaps.md`'s Locks
section to the engine as it then is, C4's decide slot and the unique
check's poll named there.
