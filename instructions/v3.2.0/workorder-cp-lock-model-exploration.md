# Work order CP — the lock model re-examined: modes and units against what KDS contends on

Written 2026-11-07 on `worktree-cp-lock-model-exploration` from `6c4b9a6d`
(`v3.4.7`), on the operator's word, recorded in English
(`raft-marks-2026-11-07.md` §1):

- **W0**: *"A milestone opens here. First we re-explore the relations among
  the locks - `IS`, `S`, `IX`, `U` and the like. It is an exploratory,
  experimental milestone. The gist: not the conventional lock system, but
  one that is new, fits the KDS system better, is optimized, and keeps
  correctness - that is what we research."*

**Written 2026-11-07; not opened.** Reviewed the same day by the
`critics-developer` pass (§7, the opening review): the census corrected,
two missed risks written into C2 and C4, one candidate (release before
durability) withdrawn because the tree contradicts its premise. CP-Q0..CP-Q9
carry CLA's proposals, none marked. **Exploratory on W0**: CP-S0 (the
census and the premise measurement) and CP-S1 (the model) decide which
candidates of §3 are built; a candidate not taken is recorded with the
number that refused it, not built.

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
  `S` or `X` (`:675`), and `AcquireInner` runs the verify only for a
  fence or a tuple (`:340-356`). A mode added to the enum compiles past
  all three; only the two tables refuse to compile unfilled.
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
  `FinishCommitBody` (`src/txn/manager.cpp:385`, `:398`), which publishes,
  retires, moves the schema word and releases (`:306-336`), and only the
  acknowledgement waits on the batch (`command_dispatcher.cpp:11768-11771`,
  `wal.md` §1). Borrows are held across a sync only by `strict`'s deferred
  commit above one core (`DefersCommit`, `:1141-1145`, BA-S7).

### 1.2 Every ask, by what it protects

Every site in `src/` and `include/` that names a `LockKey` or calls
`TryAcquire` (`grep -rn 'TryAcquire(\|LockKey::'`, the page latch's own
`TryAcquire` excluded), grouped by what the ask protects:

| # | site | unit, mode | scope | what it protects | what else protects it |
|---|---|---|---|---|---|
| A1 | `BorrowChain`, `src/server/command_dispatcher.cpp:11254-11263`; the CI drain reads the entry's holders at `:2700` | relation `IX` (a write), `IS` (a foreign key's parent read) | transaction | a DDL's relation `X` waits for every open writer (AT-S5e); the first intention is where the statement's memo must be current; CI-R2's drain snapshots the `IX` holders | nothing: a writer maintaining an index a build is announcing, or writing rows into a relation a drop is tombstoning, is kept out by this alone |
| A2 | `ReadBorrow::Position`, `include/kds/server/read_borrow.hpp:97`, at every bind (AT-S1); `CREATE TABLE ... REFERENCES` on the parent, `:5154` | relation `IS` | statement; the DDL transaction | a granted reader is not overtaken by a DDL `X` for the statement's length | catalog MVCC, the statement epoch (`drop-table.md` DT1), no DDL moving data: the refused arm is right by these, so the defence is one-directional (`read_borrow.hpp`'s header) |
| A3 | `ReadBorrow::Position`, `read_borrow.hpp:120`, from `src/exec/step_vm.cpp:2089` at **every leaf page** of the outermost walk | slice `IS`, `[min_key(page), kIdSpaceEnd)` | the page, moved at each boundary: one `TryAcquire` and one `ReleaseOne` per page | **nothing today.** Its consumer was a mover (AO-R12, AR2-R13) and none exists (`heap-and-tuple.md` §7.1, BZ). No conflict test reads it: `ConflictingOverlap` skips intention holders on interval units (`lock_table.cpp:719-722`), `FenceCoversKey` counts `S`/`X` holders only (`:675`), `IsFence` excludes it from the counter (`:26`), a slice `IS` triggers no verify (`:340-356`), and a DDL's `X` meets the reader at the relation entry (A2). `LockUnit::kSlice` and `LockKey::Slice` appear nowhere else in `src/` or `include/` but the table's own name switch | - |
| A4 | `BorrowRelationForDdl`, `:11162-11177` | relation `X`: `DROP TABLE`, `CREATE`/`DROP INDEX`, `REINDEX`, `CREATE ASSERTION`, `CREATE TRANSITION`, `DROP TRANSITION` (`:4101`), `ADD`/`DROP COLUMN` (`:3513`), `CREATE`/`DROP CABIN` (`:3176`), the two renames; relation `IS` for an online `CREATE INDEX` (CI) | the DDL transaction | the DDL against open writers (A1) and positioned readers (A2) | nothing for the writers |
| A5 | the per-row `UPDATE` and `DELETE`: `:10147`, `:12782` | tuple `X` | transaction | (i) the wait **with a kick** on a row held by another core's transaction (AX-S2b); (ii) being seen by a range fence's verify (A7); (iii) refusing a foreign key's `S` (A6) and a reverse check's probe (A6'); (iv) the arbiter's row read "under its `X`" in `ON CONFLICT DO UPDATE` (`:6700-6703`) | **the header**: first-updater-wins under the page latch is the lock, and the table "adds the wait and the `S` mode, not the lock" (AO-R3) |
| A5' | the `INSERT`: every issued key `:7096`, a named key `:7037`, `ON CONFLICT DO NOTHING` on the tuple key `TupleKeyPlaced` `:6445`, `ON CONFLICT DO UPDATE`'s up-front tuple-key unit `:6660` and its arbiter row `:6698` | tuple `X` | transaction | BD-R6: two statements naming one key serialise on its row unit (`:7062-7066`); an issued key against a named one (`:7085-7093`); BD-Q9's wait on an undecided insert of the same key; `TupleKeyPlaced`'s "granted: no other writer holds the key" (`:6452-6457`) | the descent under the leaf hold and `UndecidedInsert`'s header read; without a table "every borrow is granted" and the placement answers (`:6452-6457`) |
| A6 | the foreign-key forward check, `:4543` | tuple `S` on the parent row, under relation `IS` | transaction (given back on a failed check, AZ-S5) | the parent row's existence while a child writer is open: a parent `DELETE`'s `X` waits for it (D9(a)). **Its verdict relies on the `S` having refused every writer**: a busy verdict under a granted `S` "means no lock table" and with one "refuses retryably, defensively" (`:4556-4562`; `foreign-keys.md` §2a) | nothing |
| A6' | the reverse check's wait, `WaitForChildRowWriter` `:4775-4815` | a **probe** `S` on the child row, given back at once | the ask | a wake from the child writer's `X`: a refused probe parks on it; a granted one installs a pre-flipped slot and the statement re-runs at once (`:4797-4803`) | `NoteBlockingWriter`'s poll of the holder (`:4813`) |
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
   and it is the one the rule answers.
2. **The reverse check's probe** (A6', `:4791-4796`). It asks `S` on the
   child row to be refused by the child writer's `X` and parked with a
   wake; under C2 a child `UPDATE` holds `U`, `U` admits `S`, the probe is
   granted, and the parent `DELETE` re-runs at once on a pre-flipped slot
   - a hot loop over the whole child relation until the child decides.
   **The probe asks `U`**: refused by the child's own writer in `U` or
   `X`, admitted by a grandchild writer's `S` on that row, given back on a
   grant as today.
3. **The fence test** (§1.1's second bullet). `IsFence`, `FenceCoversKey`
   and `AcquireInner`'s verify name `S` and `X`; a range `U` would raise
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
two matrices are `constexpr` tables so the widening fails to compile until
every row is filled; the three name-filter sites do not, which is why
item 3 makes them one rule. `sys.lock_waits` and the wait-event
dictionary gain a mode name.

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

---

## 5. Stages

| stage | what | gate |
|---|---|---|
| **CP-S0** | **The census and the premise.** (1) §1.2's table re-read at the opening commit, every `path:line` checked. (2) The measurement, `build-release`, interleaved A/B, inside 20 minutes, in this priority: **M1** a point `UPDATE ... WHERE id = k` on disjoint rows, one session, `relaxed`, against a measurement build with the dispatcher's table null - the family's whole share, which that build also strips of `DeclaredWriteBorrow`, the first-intention memo check and the detector-gated park rule, as the file states; A1 and A5 apart only by a second decomposition build if the budget holds (no tally counts asks); **M3** scenario 2 (`--bookers 8`) and scenario 0 (`--fk --txn`), `sys.lock_waits` and `sys.ash` read after each, outcomes by unit and mode, `app.lock_wait`'s share of DB time, deadlock and `fault_net` counts (M3a is C2's price on the uncontended path; its contender is the rig cell, §3 C2); **M2** a `SELECT` walk over ~100-1,000 leaf pages against a build whose `Position` declines slices (C1); **M5** a held `UPDATE ... WHERE id > k` beside point writers (C3); **M4** `tools/lock_contention_benchmark.py` contended against disjoint, the shape AT-S5 made real; **M6**, a cell and not a measurement: two sessions under `group`, the second's `UPDATE` of a row the first just committed proceeding before the first's sync, confirming §1.1's last bullet and correcting `known-gaps.md:1417`. A cell cut for the budget is named in the file. (3) A results file `bench/v3.4.7/results-cp-s0-premise-<describe>.md` and this order's §7 stating each candidate's number against its rule | the operator's opening; the census is `[source-read]`, the measurement `[measured]` |
| **CP-S1** | **The model.** A design memo in this order (§8): the matrix re-derived from S0 - the cells kept by §2's rule, the cells deleted, `U` added or not with C2's three re-statements, the structure proposed (C3, C4 as S0 ranks them) with its soundness argument written as `lock_table.hpp` writes publish-then-verify, C4's (α) or (β) as marked, and the amendments owed to `txn.md` §5, `foreign-keys.md` §2a-§3, `rules.md` §3's row, AR2 E1/E12 and `lock_table.hpp`'s header. Every `[quiet-wrong]` or `[loud]` choice goes to the operator as a CP-Q amendment before S2 | S0 landed; the operator's marks on any amended CP-Q |
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

---

## 8. The model (CP-S1, to be written)

*Reserved for S1's memo.*
