# Work order CG — `INSERT ... ON CONFLICT DO UPDATE`: an exploratory concept check

Written 2026-10-21 on `worktree-pending-on-conflict-do-update` from
`722006ec` (`v3.3.0-104-g722006ec`), on the operator's words, recorded in
English (`raft-marks-2026-10-21.md` §5):

- **W0**: *"From the point of view of production-level DB software, is it
  fatal that a pk-update (delete-purge-insert) cannot be expressed as one
  transaction?"*
- **CLA's answer to W0**: a tuple key that is identity and never rebound
  (K1, K2) is within common practice. The pattern production workloads
  hit more often is a delete and re-insert of the same key inside one
  transaction (REPLACE-style upsert, an ORM's delete-and-reinsert, a sync
  job rewriting rows). That pattern is refused today (§1), and most of it
  means the same as an `UPDATE` of every non-key column. `ON CONFLICT DO
  UPDATE` would express it without touching K1.
- **W1**: *"Record ON CONFLICT DO UPDATE in docs/pending."* CLA wrote
  `docs/pending/on-conflict-do-update.md` on the work branch, unpushed.
- **W2**: *"Not as a pending issue - add it as an exploratory concept
  validation milestone and push it, documents only."* The pending entry is
  withdrawn before it reached `main`, and this order carries its content.

**Written 2026-10-21. Opened 2026-10-23** on the operator's W3 and W4
(`raft-marks-2026-10-23.md` §1): CG-Q0..Q7 marked as CLA proposed - Q0
(a), Q1 (c), Q2 (a), Q3 (a), Q4 (a), Q5 (a), Q6 (a), Q7 (a). **CG-S0 ran
the same day; its verdict is narrow** (§7). **Narrowed on the operator's
W5** (`$!narrow`, `raft-marks-2026-10-23.md` §2): **CG-Q1 re-marked (b)**
- S1 builds the tuple key and one named unique index as arbiters - and
CG-Q3 (b) dropped from CG. CG-S1 is next. **Exploratory since W2** (§3): CG-S0
checks the concept before any surface is built, and a candidate that
fails the check is recorded, not built.

**Claim tags**: `[operator]` for W0-W2; `[source-read]` with `path:line`
at `722006ec`; `[design]` for reasoning about code not yet written;
`[quiet-wrong]` where a wrong choice turns a correct answer into a wrong
one with no error.

**The letter.** `[source-read]` CF is the last work order under
`instructions/v3.2.0/` on `origin/main` at `722006ec`, no worktree in
`git worktree list` names a `CG`, and no commit subject on `origin/main`
begins with the token. CLA's choice.

**CG against other orders.** CG extends BU-S8's `ON CONFLICT`
(`instructions/v3.0.0/workorder-bu-basic-query-surface.md` BU-Q18,
`docs/spec/parser-v2.md` I20) and reuses BJ's `SET` expressions
(`instructions/v3.0.0/workorder-bj-expression-update.md`, `types.md` §3.2b) and BX's unique
check (`workorder-bx-unique-index.md`, `index.md` IX11). It neither
reopens nor relaxes `PURGE` (BH, `heap-and-tuple.md` §4.1c) or K1. It is
not part of AR0 §8's chain.

---

## 0. What CG is

The question is whether an upsert can be added to KDS as **one more arm
of the write paths the engine already has**: a new spelling that adds no
second write path, no record kind and no superblock bump. It must also
leave K1 and K2 standing. CG-S0 answers that before anything is built:

- **if it can**, CG-S1 builds the narrowest surface the marks allow, and
  later stages widen it;
- **if it cannot**, CG closes with S0's finding as its result.

---

## 1. The tree as read `[source-read]` at `722006ec`

- `DO UPDATE` is parsed as far as its word and refused `NotImplemented`
  at its byte (`src/parser/parser.cpp:2619-2621`: *"ON CONFLICT DO
  UPDATE is not supported ...; DO NOTHING is"*). A conflict target of
  more than one column is refused `NotImplemented`
  (`parser.cpp:2606-2609`).
- **`DO NOTHING`'s conflict target is the tuple key only.** A named
  target must be the tuple key, and any other column is refused
  `NotImplemented`. On a heap no column can be named, because its key is
  hidden (`src/server/command_dispatcher.cpp:5761-5776`).
- **`DO NOTHING` judges every row up front.** Each row's verdict is
  decided before any row is admitted, through `TupleKeyPlaced`
  (`command_dispatcher.cpp:5924`). It takes the key's row unit `X` the
  way a named-key placement would, BD-Q9's wait included. It keeps the
  unit only when the key is absent, or when the transaction held it
  before asking (`:5940-5945`).
- **What counts as "placed".** "Placed" is `NamedKeyVerdict`'s duplicate
  (`command_dispatcher.cpp:5901-5922`). It covers three cases:
  - a live row;
  - a **delete-marked row not yet purged**;
  - **any version of the asking transaction's own**, its own delete-mark
    included (`:5916`).

  Under `DO NOTHING`, then, a row naming a deleted-and-unpurged key is
  dropped, where PostgreSQL would insert it. A plain `INSERT` of that key
  is refused `AlreadyExists`, also inside the transaction that deleted it.
- **Rebinding a committed key is never atomic.** A `SET` of the tuple key
  is refused `Unsupported` at compile (`src/exec/step_compiler.cpp:1920-1925`,
  K2). `PURGE` runs in autocommit only, and frees a key only once every
  reader sees the delete (`heap-and-tuple.md` §4.1c). So a rebind takes at
  least three commits with a horizon wait between them, and other
  transactions see the row absent in between.
- **What is atomic today.**
  - `[design]` Moving a row to a fresh key binds no committed key twice:
    `INSERT` the new key, repoint the children, `DELETE` the old one, in
    one transaction. This is inferred from K1, not run.
  - Changing a column under a unique index is atomic: IX11 checks latest
    state, and a moved-off row holds no value (`index.md` §10.2).
- **What CG-S1 would reuse.** A `SET` value is a typed expression,
  evaluated per row against the before-image (`types.md` §3.2b, BJ).
  `UPDATE ... FROM` refuses a row that matches two `FROM` rows with
  `CardinalityViolation` (`parser-v2.md` §7, BU-Q16 `[quiet-wrong]`).
  No write records a Waystone trail
  (`workorder-cf-waystone-premise.md` §1.1 item 2).

---

## 2. Rules every candidate keeps `[design]`

- **CG-R1 - one write path per arm.** The update arm is `UPDATE`'s
  per-row path, not a second one. That path runs assertion admission, a
  Transition's `ON UPDATE` edge, the forward foreign-key check on a moved
  fk column, the Cabin witness on a
  moved input, index maintenance and the unique checks. The insert arm is
  `InsertOneRow`'s (`bulkinsert.md` BI2).
- **CG-R2 - K1 and K2 stand.** No arm rebinds a committed key, and no
  `SET` names the tuple key.
- **CG-R3 - no format event.** No record kind is added, the superblock
  version does not move, and nothing new is persisted.
- **CG-R4 - the existing paths are unchanged.** A plain `INSERT` and
  `ON CONFLICT DO NOTHING` answer byte for byte as at `722006ec`.
  The exception is a refusal text this order changes on purpose.

---

## 3. The exploratory method (W2) `[design]`

- **CG-S0 checks the concept and writes no engine code to `main`.** It
  runs once CG is opened and CG-Q1..Q4 are marked, because what it checks
  rests on them. Its result goes in §7 and says one of three things:
  build, narrow, or drop.
- **A spike may be written on the work branch to answer S0.** It is never
  merged: it is deleted or rewritten as CG-S1.
- **A candidate that fails is recorded in §7 with the reason that sank it,**
  and is never built.
- **Nothing merges to `main` without the operator's word.**

---

## 4. Questions - marked 2026-10-23 as CLA proposed (W4)

**CG-Q0 - who decides after S0.**
- (a) The operator marks build, narrow or drop after S0's report.
- (b) S0's finding decides, and CG-S1 runs on the standing go-ahead.

**CLA proposes (a)**: Q2 and Q3 change what a client can rely on about a
deleted key, and a finding should not decide that alone.

**CG-Q1 - the arbiter.** Which conflict `DO UPDATE` reacts to.
- (a) The tuple key only, as `DO NOTHING` does. A heap gets no `DO
  UPDATE`, because its key cannot be named.
- (b) The tuple key, or one named unique index (IX11). A unique index is
  the only arbiter a heap can have, and it is where a business key lives.
  The check takes the `kIndexKey` unit and resolves latest state, as
  BX-R1 and BX-R3 do. `DO NOTHING` gains the same target.
- (c) (a) in CG-S1, and (b) as CG-S2 once S1 is closed.

**CLA proposes (c)**: (a) alone leaves heaps out, and (b) at once doubles
S1's surface before the concept is shown. **Marked (c) on W4, re-marked
(b) on W5** (`$!narrow`, after CG-S0 found the demand is the
unique-index arbiter and read its pipeline, §7 reading 1b): CG-S1 and
CG-S2 merge.

**CG-Q2 - a committed tombstone.** The key has a committed delete that has
not been purged. There is no live row to update, and the slot refuses an
insert.
- (a) Refuse `AlreadyExists` with the `PURGE` hint, as a plain `INSERT`
  is answered. This keeps K1 and diverges from PostgreSQL, which inserts.
- (b) Revive the row: write a new version over the tombstone. This is
  K1's question: is a version stacked on a committed delete the same
  tuple? Every consumer `PURGE`'s census reached would have to be asked
  again:
  - assertions, because the `DELETE` wrote a departure;
  - Transitions, because `ON DELETE FROM` already passed;
  - indexes, Cabin and Waystone.
- (c) Mirror `DO NOTHING`'s reading: the key counts as placed, so the row
  is neither inserted nor updated. `[quiet-wrong]`: the statement
  succeeds, and the row the client wrote exists nowhere.

**CLA proposes (a)**: (b) relaxes K1 for a rare case, and (c) answers
wrongly with no error.

**CG-Q3 - the asking transaction's own delete-mark.** `DELETE k` followed
by `INSERT ... ON CONFLICT DO UPDATE` of `k`, in one transaction.
- (a) As `NamedKeyVerdict` answers today: the key counts as placed, and
  the update arm finds no live row. Refuse it, as in Q2 (a).
- (b) Revive one's own uncommitted delete.
  - To every other reader this reads as an `UPDATE`. The delete-marked
    version carries the transaction's own `trx_id`
    (`heap-and-tuple.md` §3.2), so no reader outside the transaction
    ever sees the delete, older readers walk undo past it, and no
    committed tombstone is involved.
  - The census remains. The `DELETE` already ran its pipeline: it wrote
    its assertion departure (`assertion.md` §4.2), passed the
    Transition's deletable set, and on a parent with children it was
    refused RESTRICT (`foreign-keys.md` F2). So the revival is an arrival
    to assertions, a Transition chooses between `ON INSERT`'s initial set
    and an edge from the pre-delete state, and the pattern is covered
    only where the `DELETE` was admitted.
- (c) (b), offered to a plain `INSERT` as well. This changes BD-R5's rule
  that a slot keyed `k` holding the transaction's own version is a
  duplicate.

**CLA proposes (a) for CG-S1, with (b)'s census as part of S0**: (b) is
the case W0's answer named. Whether it is worth the census is what S0
finds out. (c) is BD-R5's ruling to make, not this order's.

**CG-Q4 - one row met twice in a statement.** Two rows of one `VALUES`
name one key, or the second names a key the first inserted.
- (a) Refuse `CardinalityViolation`, as PostgreSQL and BU-Q16 do.
- (b) Apply the rows in order.

**CLA proposes (a)**: under (b) the answer depends on row order. Under
(a) every verdict can stay up front, as `DO NOTHING`'s are (§1).

**CG-Q5 - the `SET` surface.**
- (a) `EXCLUDED.<col>` names the proposed row. It is resolved as a
  qualifier, never a reserved word (CLAUDE.md, "Nothing new is reserved
  lightly"), and typed as the relation's columns. Every BJ form carries
  over with BJ's refusals. A `SET` of the tuple key stays refused (K2),
  and no `DO UPDATE ... WHERE` is built (refused `NotImplemented` at its
  byte).
- (b) (a), with `DO UPDATE ... WHERE`, three-valued like every WHERE.

**CLA proposes (a)**: the filter is not needed to show the concept and
can follow.

**CG-Q6 - the row unit on "placed".** The update arm needs the key's `X`,
which `TupleKeyPlaced` gives back when the key is placed (§1).
- (a) Keep it for `DO UPDATE`, held to the decide, as any `UPDATE`'s
  row unit is.
- (b) Give it back and take it again at the update. A writer could then
  delete the row in between, and the update arm would have to re-ask.

**CLA proposes (a)**: (b) opens a window that (a) never has.

**CG-Q7 - what reaches the client.**
- (a) `rows_affected` counts inserted plus updated rows, as PostgreSQL
  does. `RETURNING` returns rows from both arms. `SHOW ACCESS` records
  one shape.
- (b) Inserted and updated rows are counted separately in the tag.

**CLA proposes (a)**: a client ported from PostgreSQL reads one count.

---

## 5. Stages

Every stage is reviewed by `critics-developer` and gated by the full
suite. The overhead is measured once, at the close, inside the budget
of 2026-10-13: about an hour, building included.

**CG-S0 - the concept check.** No engine code reaches `main`. Three parts:

1. **The pipeline reading.** Can the update arm hand a row, located
   under the kept `X` (CG-Q6), to `UPDATE`'s per-row path without a
   second write path (CG-R1)? Named functions, `path:line`, and what
   would have to be split out to share the path.
2. **The census for CG-Q3 (b).** Each consumer `PURGE`'s census reached
   (assertions, Transitions, foreign keys, indexes, Cabin, Waystone,
   `sys.access_stats`) is asked what a revived own delete does to it,
   and what it would cost. A consumer with no sound answer sinks (b).
3. **The demand reading.** Does a client in reach use an upsert, or the
   delete-and-reinsert pattern? Read the xrock client and the
   `anti-kdbs` repros for it, as source-read, never assumed.

Optionally, a spike on the work branch behind the parser refusal, showing
parts 1 and CG-R4 hold on the existing tests. It is deleted or rewritten
as CG-S1. The verdict goes in §7: **build**, **narrow** (which candidates
survive), or **drop**.

**CG-S1 - the narrow surface**, on the operator's mark (CG-Q0):
- the tuple key arbiter (CG-Q1);
- the tombstone and own-delete answers CG-Q2 and CG-Q3 mark;
- CG-Q4's refusal, CG-Q5's `SET` surface, CG-Q6's unit and CG-Q7's
  counts;
- the tests: each refusal with its byte; each arm through every
  consumer of CG-R1; CG-R4's byte-for-byte check over the existing
  `ON CONFLICT` and `INSERT` tests; and a concurrent writer of the same
  key on another core.

**CG-S2 - the unique-index arbiter**, only under CG-Q1 (b) or (c). It
covers `DO NOTHING` and `DO UPDATE` alike, and heaps.

**CG-S3 - the close.**
- The close measurement: plain `INSERT`, `DO NOTHING`, and both arms of
  `DO UPDATE`, interleaved A/B in `build-release` against `722006ec`.
- The order's stage log, `parser-v2.md` I20, `manual/sql/sql.md`, and
  `known-gaps.md` (what stays refused).
- CLAUDE.md's query-language row.

---

## 6. Out of scope

What CG does not reach:
- swapping or renumbering committed keys (A <-> B) inside one
  transaction, which only relaxing K1 inside a transaction reaches;
- a rebind of a key whose delete committed earlier, which stays
  `PURGE`'s three-commit path unless CG-Q2 (b) is marked;
- `MERGE`, and `REPLACE INTO` as a spelling;
- a multi-column or expression conflict target other than one unique
  index;
- `ON CONFLICT ON CONSTRAINT`.

---

## 7. Stage log

### CG opened 2026-10-23 (W3, W4)

**W3**: *"go cg"*. Asked for the marks S0 needs, the operator chose
`$!a` (**W4**): CG-Q0..Q7 as CLA proposed (`raft-marks-2026-10-23.md` §1).
Worked on `cg-on-conflict-do-update` from `4fb01b46`
(`v3.4.2-8-g4fb01b46`). Citations below are `[source-read]` at `4fb01b46`;
§1's line numbers were `722006ec`'s, and `NamedKeyVerdict` and
`TupleKeyPlaced` are now `src/server/command_dispatcher.cpp:6167` and
`:6190`.

### CG-S0 - the concept check: **narrow**

No engine code was written. Three readings:

**1. The pipeline: the update arm on a live row is `UPDATE`'s point path
(CG-R1 holds), behind one split.**
- `UPDATE` already has a tuple-key point path: `TupleKeyEqualityTarget`
  (`command_dispatcher.cpp:6857`) then `LocateByTupleKey` (`:7227`),
  which descends with the leaf held exclusive and hands the slot to the
  same per-row `apply` lambda the walk uses (`:9426`, `:9908-9944`).
  `apply` carries every consumer CG-R1 names that `UPDATE` runs:
  assertion admission (`:9684`), the Transition edge (`:9629`), the
  forward fk check on assigned columns (`:9616`), the Cabin witness
  (`:9803`), index maintenance with the before-image (`:9811`), and the
  unique keys a `SET` moves (`:9655`) - staged there and checked by
  BX-R4's round `clear_unique` (`:9879`), outside `apply`, which the
  point path's loop runs (`:9912-9944`). CG-R1's "reverse check on a
  parent" has no `UPDATE` half: K2 leaves the referenced key unmovable
  (`docs/spec/foreign-keys.md` §3, "There is no reverse check for parent
  UPDATE").
- **The split**: `UpdateInner` (`:9166`) parses its statement from text
  (`ParseKeepingShape(line)`) and does per-statement setup (resolution,
  `CompileAssignments` at `:9243`, fk assignments at `:9336`, the unique
  holds) inside one ~880-line function (`:9166-10041`). The arm needs an entry that takes
  a resolved relation, compiled assignments and a target key. Per row,
  `EXCLUDED.<col>` values are constants, so CG-Q5 (a) can bind them as
  literals; that also keeps BJ-S4's rule that a fk assignment reads no
  column. `UpdateInner` resolves fk parents once per statement over
  constants (`:9347-9375`); `EXCLUDED`-bound values vary per row, but for
  `SET fk = EXCLUDED.fk` the INSERT's own extraction pass (`:6077`) already
  resolves the same parents.
- **Three S1 design points this reading leaves**, none a blocker:
  - **the arm's view** `[quiet-wrong]`: "placed" is judged at latest
    state under the kept `X` (CG-Q6 (a)), but `apply` filters by the
    statement's snapshot (`:9443`). A row **inserted** by a transaction
    that committed after that snapshot classifies `kNoVersion` (no undo
    behind it, `include/kds/txn/visibility.hpp:70-75`) and `apply` returns
    OK having written nothing - the row is neither inserted nor updated,
    with no error. A row **updated** after the snapshot classifies
    `kNeedsUndoWalk` and the conflict check refuses it retryably, which is
    KDS's stated `READ COMMITTED` answer (`docs/spec/txn.md` §5, "there is
    no re-read loop"). The window is a statement's under `READ COMMITTED`
    and the transaction's under `REPEATABLE READ`; S1 must close the first
    case in both, by a view minted after the `X` (RC) or a refusal (RR);
  - **waits**: with the row's `X` held, the arm can still wait on a unique
    key it moves (`clear_unique`, `:9886`), an assertion reservation
    (`:9684-9696`) and, with `EXCLUDED`-bound fk values, a parent's `S`.
    None parks in place: each notes its blocker and stops, and
    `DispatchAsync` waits and re-runs the statement only if it wrote
    nothing (`:1026-1036`, `:10944-10946`). `UPDATE` also resumes a
    partly written walk at its `WalkCursor` (AO-S3b); an `INSERT` has no
    such resume, so a wait met after the statement's first write is a
    retryable refusal, as the insert arm's `CheckUniqueKeys` (`:6330`, the
    same function) already answers;
  - **`DO NOTHING`'s up-front verdicts** (`:6026-6060`) extend to three
    outcomes per row: insert, update, refuse (CG-Q2 (a), CG-Q3 (a), CG-Q4
    (a)).

**2. The census for CG-Q3 (b): (b) survives as a concept but is not
`UPDATE`'s path, so CG-R1 fails for it in substance.** Reviving one's own
delete-mark is a third arm:
- insert-style checks: an assertion **arrival only** (`UPDATE`'s
  `AdmitAndReserveUpdate` departs the old row a second time,
  `src/exec/assertion_check.cpp:771`); the forward fk check on **every** fk
  column (`UPDATE` checks only assigned ones, `command_dispatcher.cpp:9336`;
  counter-example: delete child `k`, delete parent `P` - RESTRICT passes,
  `k` is self-hidden - revive `k` naming `P`); a unique check on **every**
  unique key (counter-example: insert `k2` with `v`, revive `k` holding
  `v`);
- update-style maintenance: the Transition edge from the delete-marked
  state, never `ON INSERT`'s set (`transition.md` §2's loophole), index
  and Cabin with that image as `previous`;
- a heap primitive that does not exist: `OverwriteTuple` never clears
  `DELETED` (`src/storage/heap/heap_page.cpp:269-299`), so overwrite plus
  `ClearDeleteMark` with a forward `HEAP_DELETE_UNMARK`, a kind whose
  comments name it rollback's (`src/wal/redo.cpp:128-130`);
- **an undo that is correct only while KDS has no statement-level
  rollback**: compensating the overwrite leaves `DELETED` clear and the
  earlier `kDeleteMark` compensation restores the committed row
  (`src/txn/manager.cpp:518-556`); a future savepoint would leave the row
  live where its delete should stand, with no error, unless a revive undo
  kind - a format event CG-R3 forbids - is added.
Cabin, Waystone, `sys.access_stats`, var-heap spills and `PURGE`/K1 have
sound answers. Reused as it is, the update path fails with no error on
three counts: the assertion double departure, the fk unchecked on
unassigned columns, and the unique key unchecked where it did not move.

**1b. The unique-index arbiter (read after S0's review, for the CG-Q1
re-mark below): buildable, with one re-verification.** `CheckUniqueKeys`
takes the value's `kIndexKey` `X` and keeps it to the decide
(`command_dispatcher.cpp:10921`), and `exec::CheckUniqueKey` answers
`kHeld` with the holder's tuple key (`:10941-10942`). So the arm is: the
unique check finds the holder `h` of value `v`; the arm takes `h`'s row
`X` as `TupleKeyPlaced` would; then it **re-reads `h` under that `X`**. The
`kIndexKey` unit keeps every other writer from moving a row *onto* `v`,
but a writer moving `h` *off* `v` takes no unit for `v` (BX checks only
the keys a write moves to). So between the check and the `X`, `h` can
leave `v`, and an arm that updated `h` anyway would update a row that no
longer conflicts, `[quiet-wrong]`. Under the re-read: if `h` still holds
`v`, update it through reading 1's path; if it moved off, `v` is free,
because the statement still holds its unit, and the row takes the insert
arm. A busy holder is reading 1's waits. `DO NOTHING` takes the same
arbiter with no re-read: "held" is its whole answer.

**3. The demand: no client asks for the tuple-key arbiter; three ask for
a business key.** Read as source in `/home/cdkbs/xrock`,
`/home/cdkbs/anti-kdbs` and `/home/cdkbs/autotrade`:
- **`DO UPDATE` on the tuple key: 0 call sites.** Every client lets the
  server issue the id or computes it, and never upserts by it.
- **An upsert on a non-key column: 3, each by hand** - a `SELECT` then
  `UPDATE ... WHERE id` or `INSERT`, in autocommit, so two writers can both
  insert: xrock `Registry::add_account` and `add_model` on `name`
  (`src/session/registry.cpp:247-264`, `:293-306`), autotrade `set_sector`
  on `symbol_id` (`src/quantlab/data/kds/repo.py:428-452`). xrock's header
  still reads "KDS has no UNIQUE" (`registry.hpp:75`), which BX made false.
  Two more sites are `DO NOTHING`-shaped (autotrade `add_instrument`,
  `append_bars`).
- **Delete-then-reinsert of one key in one transaction: 0.** xrock and
  autotrade say in comments that they avoid it because a deleted key is
  never given back (`src/data/kds/kds_store.hpp:20-21`; `repo.py:15-17`,
  `:432-434`), and xrock's `replace_day` writes new keys instead.

**Verdict: narrow** - S0's finding and CLA's proposal; under CG-Q0 (a)
build, narrow or drop is the operator's mark, and nothing below is
decided.
- **Build**: the update arm on a live row, as `UPDATE`'s point path behind
  the split (reading 1), with CG-Q2 (a), Q3 (a), Q4 (a), Q5 (a), Q6 (a),
  Q7 (a).
- **Re-target the arbiter**: the demand is entirely CG-S2's unique-index
  arbiter, which CG-Q1 (c) schedules after a tuple-key S1 no client in
  reach would use. **CLA proposes re-marking CG-Q1 to (b)**: S1 builds the
  tuple key and one named unique index together - the index check is
  BX-R1/BX-R3's, already built, and reading 1b is the arm's added step -
  so the first surface is the one clients hand-roll today. This widens
  S1 by the arbiter and its re-read.
- **Drop CG-Q3 (b) from CG**: recorded here as its own arm, buildable later
  only with the three counter-examples as tests and with savepoints named
  as the event that reopens its undo.

Not run: no spike was written; the suite is unchanged by this stage
(documents only).

### Narrowed 2026-10-23 (W5)

**W5** (`raft-marks-2026-10-23.md` §2): *"$!narrow"*, read as the verdict
above as CLA proposed it. **CG-Q1 is re-marked (b)**: CG-S1 builds the
update arm with both arbiters - the tuple key and one named unique index,
the latter with reading 1b's re-read under the holder's `X` - and
`DO NOTHING` takes the unique-index target too; CG-S2 is folded into S1.
**CG-Q3 (b) leaves CG**, recorded above as its own arm. CG-Q2..Q7 stand as
marked on W4. CG-S1 opens next and must close reading 1's `[quiet-wrong]`
snapshot case.

### CG-S1 built 2026-10-23 (W6) - the narrow surface

**W6** (`raft-marks-2026-10-23.md` §5): *"go cg-s1"*. Built on
`cg-on-conflict-do-update` from `f44846ef`: `b7ddff92` (the surface),
`8df074cf` (the instance cells), `31e9f554` (texts), `54dbc716` (the
review's fixes).

- **Parse**: `ON CONFLICT [(col)] DO UPDATE SET <list>` takes `UPDATE`'s SET
  list (`Parser::ParseAssignmentList`, factored from `ParseUpdate`); a SET
  value there may name a qualified column, and `DO UPDATE ... WHERE` is
  refused `NotImplemented` (CG-Q5 (a)).
- **The arbiter** (CG-Q1 (b), `ResolveConflictArbiter`): the tuple key, or
  the one column of a unique index; none written is the tuple key, refused
  for `DO UPDATE` on a heap. `DO NOTHING` takes the unique-index target too.
- **Row by row** (`InsertOnConflict`): CG-Q4's refusal and the forward fk
  check over every row first, then each row's verdict and write together,
  so a unique key's unit is never held across two rows. The insert arm is
  `InsertOneRow`. The update arm is `UPDATE`'s point path on the holder
  (`UpdateParsed`, split from `UpdateInner`, with the holder as its target
  and no WHERE, refusing rather than walking), under the holder's row `X`,
  with `EXCLUDED.<col>` bound to the proposed row (`BindExcluded`). On a
  unique index the holder is read again under its `X` (reading 1b,
  `RowHoldsUniqueKey`). A delete-marked holder is refused (CG-Q2 (a), Q3
  (a)); a holder the arm's view cannot see is refused `TxnConflict` (S0's
  `[quiet-wrong]` case). The reply counts both arms, and `RETURNING`
  returns both (CG-Q7 (a)).
- **Cells**: `DmlFormsTest` (both arms, the refusals with their bytes, the
  unique-index arbiter, EXCLUDED of the tuple key, a row updated twice
  through an earlier arm), `HeapHiddenKeyTest` (a heap's unique column),
  `ExpeditorTest` (every consumer of the update arm - Transition edge, fk,
  assertion, unique check, Cabin witness - and the insert arm's; the unseen
  row refused; a writer of the key on another core waited for; an arm
  holding its row, not the relation). CG-R4: plain `INSERT` and the tuple
  key's `DO NOTHING` take their old path; the one changed text is the
  non-key target's refusal.
- **Mutations, each run three times, all killed 3/3**: the unseen-row
  refusal dropped; the tuple key arm given `TupleKeyPlaced`'s pre-wait
  verdict; CG-Q4's refusal dropped; a delete-marked holder revived;
  `RETURNING`'s overwrite entries dropped.
- **The review** (`critics-developer`) found and fixed three defects: each
  arm declared a write borrow over the whole relation (its empty WHERE),
  holding every other writer of the relation until the decide; `EXCLUDED`
  of the tuple key read NULL under a unique-index arbiter; a row updated
  twice through an earlier arm's move escaped CG-Q4. Applied besides: only
  `NotFound` reads as an absent key; the arbiter is a pointer; the refusals
  pinned to their texts; the fk hoist over skipped rows stated in the texts.
  **Not applied**: one slot-lookup helper for the three lookups -
  `NamedKeyVerdict` is CG-R4's unchanged path, and the other two read
  different things; factoring `InsertParsed`'s ordinal wrap and fk loop for
  sharing - the same reason. Recorded in `known-gaps.md`: an explicit
  READ COMMITTED transaction's arm reads at the statement's view (a
  refusal where PostgreSQL updates), and `sys.relation_stats` counts
  updated rows as inserts.
- **Texts**: `parser-v2.md` I20, `manual/sql/sql.md`, `known-gaps.md`.
- **Overhead not measured**; measured at CG's close (CG-S3), which is next
  with CG-S2 folded into S1.

### CG-S3 measured 2026-10-23 (W7) - CG's close is the operator's

**W7** (`raft-marks-2026-10-23.md` §6): *"go cg-s3"*. Measured on
`cg-on-conflict-do-update` at `6775aec1` (`v3.4.3-8-g6775aec1`),
`bench/v3.4.3/results-cg-close-v3.4.3-8-g6775aec1.md`, inside the close's
hour (builds 10:38-10:40, rotations 10:43-10:54 and 11:22-11:24 UTC):

- **A is the commit CG opened from**, `f44846ef` (`v3.4.3`), not §5's
  `722006ec`: 30-odd commits of other milestones lie between the two, and
  the A1 close's precedent - A is the milestone's opening commit - is
  followed. The results file says so.
- **The touched paths** (plain `INSERT`, `DO NOTHING` inserting and
  skipping, the point `UPDATE`), two whole rotations of ten servers per
  build: inside noise, no consistent sign.
- **The walking `UPDATE`** (10,000 rows) has a slow mode on B in 15 of 20
  runs and none on A - no per-row work was added to its loop, and the
  source is unattributed (`known-gaps.md`).
- **The arms, B only**: each about 5 us over its plain counterpart (the
  arm re-runs `UpdateParsed`'s setup per row, `known-gaps.md`); **the
  unique-column upsert is 60-65 us against 130-860 us for the `UPDATE ...
  WHERE name = ...` it replaces**.
- **Texts**: this row; `known-gaps.md`; CLAUDE.md's query row and
  `parser-v2.md` I20 and the manual were written at S1.
- **`tools/cg_close_benchmark.py`** is the close's driver.

**Every CG stage is built and measured; closing CG is the operator's word.**
