# Order — AT close: what AR0-5 left open

Issued: 2026-09-26, revised the same day with the operator's marks on
Q1-Q3 (§3), against `main` at `1f9592a` (`v2.7.0-*`; the shallow
clone carries no tags - take `git describe --tags` from a full clone).
Author: CLA, for the operator. Place at `instructions/v3.0.0/`, add its row
to `index.md`, and carry each stage below into `workorder-at-m3-uniformity.md`
AT-5 / AT-6 as it opens. Stage numbers continue from AT-S13; AT-S11's number
stays unused.

Claim tags as always: `[source-read]` with `path:line` and commit,
`[design]`, `[measured]` with the invocation. Nothing in this order is
`[measured]`; every bug statement below is **found by reading, not
reproduced**, and the first obligation of each stage is to reproduce it.

---

## 0. Placement — 2026-09-26, on `at-close-order` from `6b0694d`

**Placed on the operator's word** ("Place the order", 2026-09-26). The
body from §1 on is the order as issued against `1f9592a`; `main` had moved
by placement. Items 1-3 are what the body says that is no longer true,
item 4 is a finding the body could not have, and item 5 lists smaller
drifts. They are recorded here rather than in the body, so the order
reads as issued and this section is what binds where the two disagree.

1. **E7 is decided.** The operator marked it on 2026-09-26, *"local as
   default"* (`2b41d31`; `raft-marks-2026-09-26.md` §1), after AT-S13's
   cell 1 was read and before this order was placed. §6's *"It does not
   decide E7"* stays true of this order; AT-S20's carry line *"E7's
   default (AT-0 item 2), read after AX"* is **struck** - AT-9 records E7
   as marked, and AX (the in-flight predicate) remains the owner of the
   cross-core refusal part of AT-S13's price.
2. **AT-S14 is built**, before placement, as AT-0 item 12's stage
   (AT-6's AT-S14 row owns the detail); suite 2959/2959 at `eceecb3`, the
   merged tree pushed as `6b0694d` (Debug, one pre-existing disabled
   cell). Its cells are **storage-level** - a forwarding store that runs
   another core's divide on the looked-up leaf's re-fetch, a threaded
   race cell, and `AWriteLookupHoldsItsLeafExclusiveAndItsMissIsAuthoritative`
   - and **no SQL-level cell** for `step_vm` or `fk_check` was written, as
   §4's S14 asks; whether those are still owed is the operator's. Its
   survey found the window wider than §4 names (the point `UPDATE`/`DELETE`,
   rollback's relocation, `VerifyTupleAt` dropping its own hold) and
   closed it there.
3. **AT-S20's "Rows first" is done**: AT-6's rows for AT-S10a/b/c/e, AT-S4
   folded into AT-S10b, the stale last row removed, `index.md` carried
   past AT-S9 (`e0b81e1`, review `8483701`). AT-S20 keeps AT-9 and the
   close.
4. **A fifth window of §1's family was found after issue** - by AT-S14's
   review, found by reading, not reproduced:
   `docs/inflight/bugs/an-insert-is-logged-after-its-leaf-is-released.md`.
   An `INSERT` appends its full-page images, `HEAP_INSERT` and `page_lsn`
   stamp after its leaf is released, on the single-cooperative-thread
   argument AT-S5 retired (`wal.md` §11a, `command_dispatcher.hpp`'s
   ordering note). It is AT's by §2's first conclusion, **and this order
   gives it no stage**: it is the operator's whether it becomes AT-S21
   (gating S19's §8 and S20) or is carried by AT-9.
5. **Smaller drifts.** §3's *"Record the word verbatim"* is not met for
   Q1-Q3: the verbal word is not held, and `raft-marks-2026-09-26.md` §4
   says so. §1's first row and §4's S14 cite
   `a-lookups-location-is-stale-before-its-caller-reads-it.md`, which
   AT-S14 deleted (`974a844`) - read it at `1f9592a`. §6's "the
   blueprint's" is `docs/conceptnotes/` since `c835464`.

---

## 1. Background

AR0-5 (`ar0-5-amendment-uniformity.md`) retired ownership completely. Its M3
list is executed: AT-S0..S13 built or struck every entry. Two things AR0-5
owes are not done.

1. **Its §8 names one quiet-wrong surface** (a statement executing against a
   stale schema) and defends it with the relation `IS`. That defence holds.
   But the route (AT-S5) opened a **family** of cross-core windows AR0-5 did
   not name, all of one shape: a structure whose soundness rested on *one
   core runs the write to completion* is now written by two cores. Four are
   open in `docs/inflight/bugs/` at `1f9592a`, each a quiet wrong answer:

   | entry | window | reached by |
   |---|---|---|
   | `a-lookups-location-is-stale-before-its-caller-reads-it.md` | `BtreeLookup` returns an unheld `(page, slot)`; a divide compacts slots before the caller re-fetches | any concurrent divide under a point read or FK check |
   | `a-secondary-index-descent-is-not-revalidated-across-cores.md` | window 1: the index leaf's coverage is never re-checked after the descent; window 2: the divide's walk up | ordinary index maintenance from two cores |
   | `a-btree-divide-can-promote-into-a-parent-another-core-divided.md` | the clustered tree's walk up (`PromoteSeparator` → `DivideInternalNode`) over an unchecked recorded path | concurrent below-mark named-pk inserts |
   | `two-cores-can-create-one-name-twice.md` | name check and name write are two latch holds | concurrent `CREATE TABLE` / `RENAME` / `CREATE NAMESPACE` |

2. **Its body is still a DRAFT** (AT-0 item 1). `raft-marks-2026-09-05.md`
   §2 marked §6's D17–D22 and not the amendment. AT-7 records where the body
   and the tree disagree, and AT-S9 took D17 and D18 differently from the
   proposal.

AT-7 item 17 adds a third: *a predicate's removal is not done when the
branch it gated is left*. `MayWrite` survives as an always-true predicate
(`include/kds/storage/page_store.hpp:402`, `src/storage/device_page_store.cpp:959`),
and AT-S12's commit message (`f48d213`) lists the dead code AT-S6 left.

## 2. Conclusions (what this order decides)

- AT does not close with a known quiet wrong answer that AT-S5 opened. The
  four windows are AT's, not the following letter's: the following letter
  (AT-0 item 6) is new semantics (D7, D9(a)), and a stale-structure repair
  is not.
- AR0-5 is ratified as **revised**, not as written: the revision states what
  was built, and its §8 lists the family rather than one surface.
- What AT carries forward is listed once, in AT-9 (S20), and nowhere else.

## 3. Operator items - marked 2026-09-26

These are quiet-wrong class, so the standing instruction did not adopt
them silently; they were put to the operator, and **the operator took
CLA's proposal on all three on 2026-09-26** (verbal). Record the word
verbatim in a `raft-marks-2026-09-26.md` with this order's first commit.
The ruling column is what binds; the session does not re-open it. If a
stage's survey finds a ruling cannot be built as written, the stage stops
and reports - it does not substitute the alternative on its own account.

| # | item | gates | ruling (operator, 2026-09-26: CLA's proposal) |
|---|---|---|---|
| Q1 | **The walk-up's shape**, for both trees: latch coupling on the way up, or re-validate the parent's coverage of the separator and re-descend on a miss | S16 | **Marked: re-validate and re-descend.** It is AT-S5c's shape one level up, keeps the descent latch-free, and reuses its bounded-restart progress argument - which S16 must re-prove for the walk up rather than cite. Latch coupling is the fallback if the re-descent cannot be shown to make progress |
| Q2 | **Name atomicity**: hold the `sys.objects` root page exclusive across check and insert (`Catalog::RegisterPattern`'s shape), or give a name a lock unit in the lock family (`txn.md` §5) | S17 | **Marked: the held page.** It is in the tree, it is one latch, and it is what AR0-5 §0 asks for (a system relation protected by the page latch like any other). A name lock unit is a new unit with no other consumer. Note what it does not cover: a `CREATE` in an open transaction holds the name only until its latch drops, so the duplicate check must see uncommitted rows of other transactions - `ddl-transactional.md`'s unfiltered check - and S17 verifies that it does |
| Q3 | **The v3.0.0 annotated tag** at AT's close | S20 | **Marked: not at AT's close.** v3.0.0 still owes the following letter; the tag is cut when AR0 §8's chain is done. AT-9 says so |

Item 12(a) (`BtreeLookup` returns the held `PageRef`) was marked
2026-09-10. With Q1-Q3 marked, **no stage in this order waits on a design
decision**; each waits only on the operator's word to start, and on the
dependencies in §5.

## 4. Stages

Every stage: fresh clone; survey by source read before code; the
reproduction written **first** and shown failing on the unfixed tree
(two-core rig, `tests/two_core_rig.hpp`, or real threads as in
`tests/btree_race_test.cpp` - a barrier before the window plus rounds, one
run proves nothing); the fix; mutants run repeatedly and killed; a
`critics-developer` pass applied in the next commit; the suite green on the
committed tree with its count; the bug entry deleted by the bucket's rule;
specs updated; `CLAUDE.md` row if a behaviour changes. **Overhead not
measured** unless a stage runs a measurement, in which case under
`bench/v3.0.0/` by AS-E's rules.

### AT-S14 — `BtreeLookup` returns the held leaf (AT-0 item 12(a))

*Built before placement - §0 item 2.*

- **What.** `BtreeLookup` returns the `PageRef` beside the `Location`; the
  seven callers read through it instead of re-fetching by id:
  `src/exec/step_vm.cpp:596`, `:1386`, `:1515`; `src/exec/fk_check.cpp:99`,
  `:259`; `src/server/command_dispatcher.cpp:9072`;
  `src/server/cabin_optimizer_exec.cpp:367` (re-verify the lines at the
  survey's commit). `btree.cpp`'s `Descent` comment ("Shape C") is rewritten
  to say why the ref is kept.
- **Also in scope** - the entry's last section: `step_vm.cpp:562`'s probe
  memo (`memo_page_`, `memo_slot_`) carries a location across statements and
  suspension points and does not check `relayout_epoch`. Either it checks the
  epoch or it is removed; the survey decides and the row says which.
- **Rule to hold.** No pin is held across a park. If a caller suspends
  between the lookup and its read, the survey names it and the stage copies
  out before the suspension (AT-S2a's `kwp_load_server.cpp` precedent).
- **Cells.** The entry's reproduction for `step_vm` (a point read answers
  one row, not zero, under a concurrent divide) and for `fk_check` (the
  verdict names the row asked about). `ALookupDoesNotMissARowADivideMovedUnderIt`
  asserts on status only; add the payload assertion beside it.
- **Done.** No caller of `BtreeLookup` fetches the returned page id again -
  a grep over `src/`, stated in the row with its output.
- **Size** M. **Gate** the word.

### AT-S15 — the secondary index's leaf coverage (window 1)

- **What.** AT-S5c's answer applied to `src/storage/index/index_tree.cpp`:
  once `DescendTo(..., leaf_for_write=true)` holds the leaf exclusive, check
  that it still covers the sort key; a stale descent restarts, bounded as in
  `btree.cpp`. The index root's bump (`Catalog::UpdateIndexRoot`) has no
  coverage check underneath - the same check covers a stale root.
- **The difference the entry names.** An index leaf has no immutable
  `min_key`; coverage is asked of the right sibling's first entry, which is
  stable **only because nothing removes an index entry today**. Write that
  premise at the check, in `index_tree.hpp`, and in the spec that owns the
  index tree, so the day an entry is removed the check is revisited.
- **The read side.** Decide, by the same directional argument AT-S5c made
  for `BtreeSeekLeaf`, whether an index probe needs the miss-path check
  `BtreeLookup` got. State the answer at the site either way.
- **Cells.** Two cores, one secondary index on `v`, a barrier between A's
  shared read and its exclusive re-fetch released after B's divide; probe
  A's key. And the outrun-before-the-leaf case AT-S5c found (the parent
  released before the child is read).
- **Size** M. **Gate** the word.

### AT-S16 — the walk up, both trees (window 2) - Q1: re-validate and re-descend

- **What.** Close the separator's walk up the recorded path in
  `src/storage/btree/btree.cpp` (`PromoteSeparator`, `DivideInternalNode`)
  and in `index_tree.cpp`'s divide, by Q1's ruling: before inserting the
  separator, check that the recorded parent still covers it; on a miss,
  re-descend from the root for the parent of the new node. One shape for
  both trees unless the survey finds a reason they differ; if so, the row
  states it.
- **Survey first.** Establish, as AT-S5c's review did for the leaf, that
  nothing holds a node and asks for its left neighbour or its parent, so the
  re-descent cannot deadlock; and whether the append path's claimed safety
  (the split leaf's two pins held across the promotion, monotonic ids
  splitting only the tail leaf) holds as the entry states it.
- **Progress.** Prove a bound on restarts for the walk
  up (a divider holding what across its own promotion), write it at the
  site, and give the exhaustion a message that names a stale path rather
  than churn.
- **Cells.** The clustered entry's reproduction (two below-mark named-pk
  inserts into sibling leaves under one full parent, barrier before A's
  parent fetch, released after B's divide; read A's key by pk). The index
  equivalent. Each checks the tree's structural invariant after the rounds -
  every separator routes to the subtree holding its key - not only the one
  key.
- **If the bound cannot be proved**, the stage stops and reports with the
  interleaving that defeats it; latch coupling is the operator's next
  decision, not the stage's.
- **Size** L. **Gate** the word. S15 before S16 for the index
  tree, because window 2 is reached through window 1's descent.

### AT-S17 — one name, one row - Q2: the held page

*Built 2026-09-26 - AT-6's AT-S17 row.*

- **What.** `CREATE TABLE` (`CommandDispatcher::HandleCreateTableSql` →
  `Catalog::FindTableOidByName` → `Catalog::CreateTable`), `ALTER TABLE ...
  RENAME TO` (`Catalog::RenameTable`), and `CREATE NAMESPACE`
  (`Catalog::CreateNamespace`) check and write under one hold, by Q2's
  ruling: the `sys.objects` root page is held exclusive across the name
  check and the insert (or the rewrite, for `RENAME`), as
  `Catalog::RegisterPattern` holds its relation's root page. No name lock
  unit is added. The survey states the latch order against the page latches
  a DDL already takes and adds the row to `rules.md` §3 if the order is new. `CREATE INDEX` and `CREATE ASSERTION` names are in the survey's
  scope: if their name check has the same two-hold shape, they are fixed
  here too; if not, the row says why.
- **Transactional DDL.** A create inside an open transaction commits later
  than its latch drops. The stage shows by cell that a second core's create
  of the same name, while the first is uncommitted, is refused (or waits)
  rather than admitted - that is the unfiltered duplicate check
  `ddl-transactional.md` states, and it must hold across cores, not only
  within one catalog view.
- **Cells.** Barrier between each session's lookup and its write, both
  creating `t`: exactly one `OK`, `sys.objects` holds one `t` row. The same
  for `RENAME` onto one name and for `CREATE NAMESPACE`. The uncommitted
  case above.
- **Size** M. **Gate** the word.

### AT-S18 — the retired predicates and AT-S6's leftovers

- **What.** AT-7 item 17 applied. Delete `MayWrite` (the base virtual, the
  override, every call site); and from `f48d213`'s list: `CommitAck` /
  `CommitAckScope`, `CommitLocal`'s `commit_lsn` out-parameter,
  `AdoptSnapshot`, `next_ship_session_id_`, `Transaction::MarkPrepared` and
  the prepared state (with `IsInDoubt`), `LogTxnPrepare`, the two-argument
  `LockTable::ClearWaitFor`, `WireResultSink::set_batch_target_bytes`, and
  `tools/multicore_benchmark.py`'s lease fields. `Status::UnknownOutcome`
  stays: its code is pinned.
- **Not in scope.** Anything with a production caller at the survey's
  commit. The survey re-derives the list rather than trusting `f48d213`.
- **Recovery.** Removing the prepared state touches the mount scan.
  `prepared_recovery_test.cpp` synthesises a `TXN_PREPARE` record; AT-S6's
  third done-condition (*the mount scan's own cell states what replaced
  it*) was left unmet. This stage meets it: a volume carrying a
  `TXN_PREPARE` from a pre-AT engine mounts and does what the cell states.
- **Done.** Greps for each symbol answer nothing outside records of what
  went; the golden log is unchanged (`WalGoldenLog.TheSingleCoreScriptWritesThePinnedBytes`).
- **Size** M. **Gate** the word. May run in parallel with S14-S17.

### AT-S19 — AR0-5 revised for ratification

- **What.** Append **AR0-5-R** to `ar0-5-amendment-uniformity.md`: the
  amendment as built, section by section, so the operator ratifies one
  text. The body above it is left as written (history); AR0-5-R is what is
  ratified.
- **Contents, at minimum.**
  - §1's table with the affinity row as built: **no statistic stored**
    (D18 at AT-S9), NS10's verb.
  - §2.1: the `IS` taken at resolve time (AT-R1, AT-S1), the ask
    non-blocking, and what closes the DDL-wins direction (AT-7 item 10,
    AT-0 item 10).
  - §2.2: two lease families retired, not three (AT-3 H); the shared
    allocator after the route, not before (AT-7 item 12); D20's cache as
    built at AT-S10b; AN-R13 kept.
  - §3: E13 answered **no** (AT-7 item 11); D17 as **reserved bytes**, no
    format event (AT-S9); D19 with its fallback built (AT-S10c) on an inbox
    and a kick, not a ring.
  - §4: the retire list closed, each entry pointing at the stage that
    retired it; the `(M5)` census closed by AT-S12's grep.
  - **§8 rewritten**: the surface is not one. It lists the family AT-S5
    opened - schema staleness (the relation `IS`), the leaf descent
    (AT-S5c), the lookup's payload (S14), the index leaf (S15), the walk up
    (S16), the name (S17), the assertion directory (AT-S5d), the index
    build (AT-S5e), the FK forward window (open; the following letter's,
    D9(a)), the in-flight predicate (open; AX) - each with its defence and
    its cell, or its owner if open.
  - Every AT-7 item that corrects AR0-5, cited by number.
- **Then** a `raft-marks-*.md` records the operator's word on AR0-5-R,
  verbatim, and AT-0 item 1 is closed there.
- **Size** M. **Gate** S14-S17 landed (so §8 can say *closed*), and the word.

### AT-S20 — AT's close (AT-9)

- **Rows first.** AT-6 gains rows for AT-S10a, S10b, S10c, S10e (each from
  its commits: `cbdd41e`/`f146721`/`2be01fa`; `59ed9c0`/`ad01383`/`6d31641`;
  `879b15b`/`286d99c`/`77f43be`; `4cc5fc5`/`1198fdd`/`7c17b41` - verify),
  AT-S4's row says *folded into AT-S10b*, and the last row (*AT-S4, AT-S10
  not started*) is removed. `index.md`'s AT row is brought to the close.
- **AT-9**, in AO-8's form: what AT delivered, what it measured (AT-S13),
  and **what it carries forward, once**:
  - the FK forward window and D9(a)'s fence, D7, E3, E5, E10, AR1 AQ/AR,
    AO-0 items 9/22/25 → the following letter (AT-0 item 6);
  - AO-0 item 26 - state whether AT-S9/S10a's retirement of the fan-in
    producer mooted it, and close it or carry it;
  - AO-0 item 27 → ratified or carried, by the operator's word;
  - a write refused, not waited, on another core's undecided holder
    (`known-gaps.md` Locks) → AX (`workorder-ax-inflight-publication.md`),
    with AX-Q1/Q2;
  - E7's default (AT-0 item 2), read after AX, since part of AT-S13's
    refusal price is that gap;
  - the ~0.4 s stall at `cores = 1` (AT-S13) → open, owner named;
  - no transaction lifetime ceiling (AN-R14's instance half, AT-S6) →
    lettered or explicitly deferred;
  - AT-S7's unmet `SHOW ACCESS` A/B and the unmeasured serialisation points
    (AT-S7 page 11, AT-S8's re-mark) → the next measurement epoch;
  - AT-0 item 3 (the cap's narrowing) and item 11 (the words' struct),
    closed as proposed or carried;
  - the remaining `docs/inflight/bugs/` entries, each with an owner:
    `a-chunked-assertion-snapshot-can-be-split-by-another-cores-record.md`,
    `assertion-reservations-stranded-by-a-failed-settle.md`,
    `two-cores-growing-one-heap-chain-can-orphan-a-page.md`.
- **Q3 as marked**: AT-9 states that no tag is cut at AT's close, that
  v3.0.0 is cut when AR0 §8's chain completes, and that measurements keep
  carrying `v2.7.0-*` until then.
- **Done.** AT-6 has a row per stage; `index.md` says CLOSED with the
  close's commit; every AT-0 item is marked, closed, or carried by name.
- **Size** S. **Gate** S14-S19.

## 5. Order and parallelism

```
S14 ─────────────┐
S15 ── S16 ──────┤
S17 ─────────────┼── S19 ── S20
S18 ─────────────┘
```

S14, S15, S17 and S18 are independent of each other and may run in
parallel worktrees. S16 follows S15.
S19 follows S14-S17. S20 is last.

## 6. What this order does not do

- It opens no new semantics: no D7, no D9(a), no loose FK, no CN-series
  note. Those are the following letter's and the blueprint's.
- It does not build AX. AT-9 carries it.
- It does not decide E7.
- It runs no performance measurement; a stage that finds it must, says so
  and asks.
