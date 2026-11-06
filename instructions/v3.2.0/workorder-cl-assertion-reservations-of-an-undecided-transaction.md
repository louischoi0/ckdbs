# Work order CL — the reservations of an undecided transaction: a crash loser and a failed commit

Written and opened 2026-11-01 on `main` from `a59f9a2e` (`v3.4.6-16-ga59f9a2e`),
on the operator's word, recorded in English (`raft-marks-2026-11-01.md` §1):

- **W0** (2026-11-01): *"open the crash-loser and failed-commit fix order"*,
  after CK's close report named two fail-open paths that CK does not fix.

**Written 2026-11-01; opened 2026-11-02** on the operator's word *"go ahead
with CL, CL-Q all as proposed"* (`raft-marks-2026-11-02.md` §1): CL-Q0 (a),
CL-Q1 (a), CL-Q2 (a), CL-Q3 (a), CL-Q4 (a), CL-Q5 (a) are **the operator's own
marks**. **CL-S0 then found that CL-Q2 (a)'s and CL-Q3 (a)'s premises do not
hold (§6), so CL-S1 and CL-S2 waited on the operator.** **Resumed 2026-11-02
on the operator's standing go-ahead** (go-ahead-achieving-milestone,
`raft-marks-2026-11-02.md` §2): **CL-Q2 (b')** and **CL-Q6 (a)** adopted as CLA
proposed - not the operator's per-item marks - and the operator's **CL-Q3 (a)**
kept, CL-S0's revised (b) withdrawn (§3). The letter CL is CLA's
proposal: CA..CK are taken (CJ by another session). The operator may reletter
it.

**Claim tags**: `[source-read]` with `path:line` and a commit; `[repro]`
with the cell; `[design]` for a proposal. Every claim names the worktree
and commit it was true of. Nothing here is a measurement.

---

## 0. What CL is

A reservation's delta enters its group's total at admission. Two events take
it back out: the live `AbortTxn` (`ASSERT_ROLLBACK`) and that record's
replay. Two paths leave a transaction's rows undone while its deltas stay
in the total, and **assertions are upper bounds only** (`assertion.md:63`),
so a total too low is a wrong answer with no error:

| Stranded contribution | Total | Outcome |
|---|---|---|
| An arrival of a non-negative value | too high | refuses writes it could admit (fails closed) |
| **A departure** (`DELETE`, an `UPDATE` moving out) | **too low** | **admits writes the assertion forbids** `[quiet-wrong]` |
| A negative `SUM` arrival | too low | the same, `[quiet-wrong]` |

- **P1 - a crash with the transaction undecided.** Recovery's undo takes the
  rows back and no mount path compensates the reservations. Reproduced on
  `ck-assertion-commit-settle` at `b66a0dbe` on the base binary in both
  directions `[repro]` (§1.1).
- **P2 - a failed `TransactionManager::Commit`.** `CommitTxn` drops the
  pending set before `Commit` runs, so a commit that fails reaches
  `AbortTxn` with nothing to reverse. Found by the CK-S1 review's reading at
  `9fd02f2e`, **not reproduced** (§1.2).

**The hypotheses CL tests**
- **H1 - the analysis phase's outcomes are enough to compensate P1.** A
  transaction's entry in `AnalysisResult::transactions`
  (`include/kds/wal/analysis.hpp:92`) says winner, aborted or loser, and
  that is the only sound source (CK-R4: never the page's bit).
- **H2 - P2 closes by ordering alone.** Dropping the pending set only after
  the commit succeeds, and aborting in the failure arms, needs no new
  record and no format change.
- **H3 - neither fix changes a statement on a relation with no assertion**,
  byte for byte and in cost.

---

## 1. The tree as read `[source-read]` at `a59f9a2e`

### 1.1 P1 - the crash loser

- `ASSERT_ROLLBACK` is appended by the live `AbortTxn` only
  (`src/exec/assertion_check.cpp`); `ReplayRollback`
  (`assertion_replay.cpp:129`) folds it, marks the entry
  `kEntryOrphaned` and compensates the delta.
- `src/wal/recovery.cpp` and `src/txn/recovery_undo.cpp` name no assertion
  record. `redo.cpp:495-499` only defers them to the assertion pass.
- The assertion pass (`RecoverAssertions`, `assertion_recover.cpp`, called
  from `mount_recovery.cpp:249`) takes `(device, core, from_lsn, store,
  targets, log)`: **no outcome of any transaction reaches it**, so its fold
  applies every `ASSERT_RESERVE` after the base (`:268-312`).
- The linkage scan re-attaches every entry that is not orphaned
  (`assertion_recover.cpp:69`).
- `sim/instance.cpp:159` and `sim/loop.cpp:416-425` crash the sim with an
  assertion declared, and no cell re-sums a group after a mount.
- **Cells** (`tests/assertion_crash_test.cpp`, `DISABLED_`, red when
  enabled): `DISABLED_AnUndecidedArrivalAtTheCrashIsGivenBack` (2 rows under
  `<= 3`, an open `INSERT`, a mount: both probes refused, one should be
  admitted) and `DISABLED_AnUndecidedDepartureAtTheCrashIsGivenBack` (3 rows,
  an open `DELETE`, a mount: a fourth row is admitted). The spike of CK and
  CK's own change answer the same, so P1 does not depend on the settle.
- A committed departure across a crash is correct
  (`ACommittedDepartureFreesItsSlotAcrossACrash`).
- **The snapshot carries it forward.** A loser's delta, once in a checkpoint's
  `ASSERT_SNAPSHOT`, is in the base of every later mount, so a fix that only
  reads the log after the base cannot reach it (§2, CL-R2).

### 1.2 P2 - the failed commit

- `CommitLocal` (`command_dispatcher.cpp`, the commit of an explicit
  transaction) calls `enforcer_->CommitTxn(id)` and only then
  `txn_->Commit` / `CommitDeferred`. Its failure arm unwinds through
  `txn_->Abort` and **never calls `enforcer_->AbortTxn`**.
- `EndWrite`'s manager arm does the same through `AbortOwnedScope`, whose
  `AbortTxn` finds an empty set.
- The deferred path's `wal_->stopped()` abort (`FinishDeferredCommits`) finds
  the set gone too.
- `AbortTxn` is idempotent on an id with no pending set (a no-op), which is
  what lets an abort arm call it unconditionally.
- **A writer on another core** waiting on a transaction that reserved
  (`ReserverOnLocked`, `assertion_check.cpp:494`) is woken by the commit's
  publish and release. If the pending set outlives the commit's publish, that
  writer can find the transaction still pending. Whether the lock family
  answers a decided id at once or runs to the 1 s fault net is **not known**
  (the CK-S1 review could not confirm it); CL-S0 settles it first (§3).

### 1.3 What neither path touches

Admission and its O(1) header read, the hold, `ASSERT_RESERVE`'s format,
`AbortTxn`'s record, AS6a's snapshot, fold and linkage scan, AS6b's
`kEntryOrphaned`, and `VerifyAgainstEntries`. CK-R1 binds both fixes:
`kEntryReserved` is "written by the reservation path" and is never read as
"in flight".

---

## 2. Rulings proposed `[design]`

**CL-R1 - the loser is told by the analysis phase's outcome.** The set of
transaction ids that did not commit comes from `AnalysisResult::transactions`
and reaches the assertion pass. Never the bit (CK-R4) and never the absence
of a later record.

**CL-R2 - a loser's reservation is compensated in the pass, whichever side of
the base it fell.** `[quiet-wrong]` The pass must (a) skip, or fold and
unapply, every `ASSERT_RESERVE` of a loser at or after the base, and (b)
unapply a loser's reservation that the base already counts, which needs the
entry's own bytes (tuple key, value, group, departure, from the page or the
record). A fix that does (a) alone leaves a checkpointed loser's delta in the
total, in the same direction.

**CL-R3 - the compensation marks the entry `kEntryOrphaned`**, as a live abort
does, so a rebuild that reads only the pages agrees with the directory (AS6b).

**CL-R4 - a transaction aborted live is not compensated twice.** An `aborted`
transaction whose `ASSERT_ROLLBACK` is in the log is already compensated; one
whose abort record is present and whose `ASSERT_ROLLBACK` was never written
(a crash between them) is a loser for this pass.

**CL-R5 - P2 closes by ordering.** The pending set is dropped after the commit
is published (or at the failure arm's `AbortTxn`), and `CommitLocal`'s failure
arm calls `AbortTxn`. The deferred path moves its drop into the decide.

**CL-R6 - what does not change** is §1.3, plus every statement on a relation
with no assertion.

---

## 3. Questions

Each carries CLA's proposal. None is marked.

**CL-Q0 - one order or two.**
- (a) One order, P1 and P2 together.
- (b) Two orders.

**CLA proposes (a)**: both are "an undecided transaction's reservation outlives
the rows" and share one verification oracle (a group's total against a
re-summation after the event).

**CL-Q1 - how the pass compensates P1.**
- (a) Fold every reservation, then unapply each loser's, by the entry's bytes,
  through `ReplayRollback`'s own compensation, marking the entry orphaned.
- (b) Skip a loser's reservation at the fold, and unapply from the base only
  the ones the base counts.
- (c) Write `ASSERT_ROLLBACK` records for the losers at the mount, so the next
  mount is exact without special handling.

**CLA proposes (a)**: one path for the loser inside and outside the base, no
new record, and it reuses the compensation a live abort replays. (c) writes at
mount and changes what a mount does to the log, which nothing asks for.

**CL-Q2 - where the entry's bytes come from for a loser the base counts.**
- (a) The cabin pages: the entry names group, value and departure.
- (b) The `ASSERT_RESERVE` record, found by a scan back from the base.

**CLA proposes (a)**: the linkage scan already reads the pages, and the record
may lie below the log's redo start. CL-S0 checks that every entry the base
counts is on a page at the mount.

**CL-Q3 - the failed-commit ordering (P2).**
- (a) CL-R5: drop after the commit, abort in the failure arms, inside CL.
- (b) Leave P2 open and fix only P1.

**CLA proposes (a)**, **after CL-S0 shows what the lock family does for a
waiter on a decided id**. If it runs to the 1 s fault net, CL-R5's ordering
costs a stall on a hot group and CLA revises the proposal (drop the set at the
publish, before the release) and records it.

**Resolved 2026-11-02 on the standing go-ahead** (`raft-marks-2026-11-02.md`
§2): CL-S0 showed (a) cannot work (§6 item 3); **(b') of §6 adopted**: the
checkpoint writes `ASSERT_PENDING` - the txn id, page, index, key, delta and
departure of every reservation the registry holds pending or is unwinding -
after each `ASSERT_SNAPSHOT` run, and the pass compensates those of a
transaction analysis did not find committed. `[quiet-wrong]` through CL-R2.

**CL-Q3 kept at (a), CL-S0's revised (b) withdrawn** (same record): (b')
needs CL-R5's ordering. A set dropped before the commit record leaves a
checkpoint taken between the two listing nothing for a transaction that then
crashes undecided, so its base counts a loser no record attributes. The
waiter question is moot: the set is dropped once the record is logged, before
the publish and the release.

**CL-Q4 - the `DISABLED_` cells.**
- (a) Enabled by CL-S1 and kept as regression cells.
- (b) Deleted and replaced.

**CLA proposes (a)**: they state the right answer already.

**CL-Q5 - a sim oracle.**
- (a) The sim re-sums every assertion group against its surviving rows after
  each mount, and fails on a difference.
- (b) Cells only.

**CLA proposes (a)**: the sim crashes at random ops with an assertion declared
and has no check on its total; this is what would have found P1.

**CL-Q6 - the superblock** (written 2026-11-02, at CL-S1).
- (a) Bump to 35, refused with no migration (the standing order of 2026-10-07).
- (b) No bump: a log holding `ASSERT_PENDING` is refused by an older build as
  an unknown record kind.

**CLA proposes (a)**, adopted on the standing go-ahead of 2026-11-02
(go-ahead-achieving-milestone): (b) refuses only a scan that reaches the
record, so a version-34 binary whose scan holds none mounts a CL volume and
leaves a loser's departure counted `[quiet-wrong]`; and a version-34 volume's
checkpoints list nothing, so this build cannot compensate a loser its base
counts. Every format change a neighbour would mishandle has bumped
(29 for `ANCHOR_SLOT_REMOVE`, 34 for CI).

**CL-Q7 - an abort's unwind refused part-way** (written 2026-11-02, at the
CL-S1/S2 review). `AbortTxn` gives a transaction's reservations back one by
one; a refused page fetch, `Unapply`'s missing pair, the mark or the stamp
returns early, the caller's `Abort` still takes the rows back, and the
reservations not reached stay counted with nothing naming them - live, and in
every later checkpoint's base. It predates CL (the live total was already
wrong) and is not P1 or P2, but CL's repair is the one that can reach it.
- (a) Keep the remainder in `aborting_` and flag its `ASSERT_PENDING` entries
  "abort refused", compensated at a mount whatever analysis says (analysis
  forgets the transaction once its `TXN_ABORT` is below the scan start, so the
  listing alone would compensate nothing); fail closed live by evicting the
  assertion to unenforcing.
- (b) Fail-stop the log on the refusal: no `TXN_ABORT` and no later checkpoint
  becomes durable, so the next mount finds the transaction a loser in its
  base's active list with its reservations listed or folded, and CL-R1 gives
  them back.
- (c) Record it as a known gap.

**CLA proposes (b)**, adopted on the standing go-ahead of 2026-11-02
(go-ahead-achieving-milestone): it reuses CL's own repair exactly, adds no
flag or on-disk meaning, and fails closed the way a refused log write already
does (a refused `Append` in the same loop has always stopped the log). Its
cost is availability on a page read error or a directory defect during an
abort, both of which already leave a wrong total. `[quiet-wrong]` in kind.

---

## 4. Stages

Every stage is reviewed by `critics-developer` and gated by the full Debug
suite. A stage lands carrying "overhead not measured; measured at the
milestone's close". The measurement is held to the standing hour budget,
building included.

**CL-S0 - the premise, verified.** No engine change.
1. Reproduce P2 by a cell: an injected `Commit` failure (the log or the pool
   refusing at the commit record), then the group's total against a
   re-summation, for an arrival and a departure.
2. Settle §1.2's open question: what a waiter on a transaction whose
   `TXN_COMMIT` is published and released does when the pending set is still
   present.
3. Check that every entry a base counts is on a page at the mount (CL-Q2).
4. Census the paths that reach the assertion pass with a loser: a crash, a
   restore (`kds_server --restore-from`), PITR.

**CL-S1 - P1.** The outcomes reach `RecoverAssertions`; the compensation of
CL-Q1 (a); the two cells enabled (CL-Q4); AS6b's marks; the sim oracle
(CL-Q5). A checkpoint-variant cell: a loser the base counts.

**CL-S2 - P2.** CL-R5, with the P2 cells from CL-S0.

**CL-S3 - verification.** A mutant that skips a loser's compensation fails a
cell; the sanitizer builds (clang ASan/TSan where installed; otherwise stated
as not run); the sim corpus with the oracle; `scripts/bs/mutants.sh`.

**CL-S4 - the close.** The overhead A/B (`ck-tester`, `build-release`) over
the commit the milestone opened from against its closing commit, with the
control relation unmoved; `assertion.md` §6.2 and §7 (the recovery sentence
CK corrected), `known-gaps.md`, the bug entry closed, the index row, CLAUDE.md
only if a row states it. **Added at the CL-S1/S2 review**: `wal.md` - kind 34
in its list and the assertions row, analysis's `ASSERT_PENDING` exception
(§12), the checkpoint's write after each `ASSERT_SNAPSHOT` run - and superblock
35 wherever a spec or CLAUDE.md row states 34.

---

## 5. Out of scope

- A change to admission, holds, `ASSERT_RESERVE`'s format or AS6a.
- The abort's batching (CK-Q6 (a)).
- The settle (CK, closed).
- A tag or a version.

---

## 6. Stage log

Written 2026-11-01. Opened 2026-11-02 in worktree `ck-assertion-commit-settle`
at `b4a9e102` (`origin/main` `b4a9e102`, nothing to merge).

### CL-S0 - the premise (2026-11-02)

**1. P2 is not observable in a process; H2 refuted as stated.** Every refusal
of `TransactionManager::Commit` other than "no longer active" comes from the
log, and the log stops on all of them: a refused write or sync fail-stops
(`WalStream::FailStop`, `stream.cpp:88`), and a ring-full append refused after
its drains is fail-stopped by `WalManager::Append` ("a refused record after a
mutation is the case fail-stop exists for", `manager.cpp:402`). So the
reservations a failed `Commit` strands sit in an instance that refuses every
later write until a restart, and the restart is P1's case (the transaction has
no durable commit, or has one and is a winner whose rows stand). Pinned by
`StrictParkRigTest.AFailedStrictCommitsStrandedDepartureAdmitsNothingOnAStoppedLog`
(`tests/strict_park_rig_test.cpp`): a `strict` `DELETE` whose sync fails on the
gated rig, then an `INSERT` into the asserted group that would be the third row
under a bound of 2 - it is refused, and `wal().stopped()` holds. **CK's review
read P2 as a fail-open path; it is a fail-open total behind a stopped log, which
changes no admission.** The fix CL-R5 proposed would reorder the pending set's
drop across the commit's publish and release for no observable gain, and it
would put a waiter on another core behind a transaction the lock family has
already decided (§1.2, not measured). **CL-Q3 (a)'s premise ("P2 produces a
wrong total that something reads") does not hold.**

**2. §1.2's open question was not answered**, because item 1 makes it moot for
CL-Q3: what the lock family does for a waiter on a decided id was not measured.

**3. CL-Q2 (a) cannot work.** A page entry carries `tuple_key`, `flags`, the
location hint, `value`, `group_id`: **no transaction id** (`cabin_bound_page.hpp:125`,
29 of its 32 bytes, 3 spare - a 48-bit id does not fit), and since CK the
reserved bit says nothing about commit (CK-R1). The only record that names the
reserving transaction is `ASSERT_RESERVE`'s envelope. The checkpoint's
active-transaction table carries `txn_id` and `last_undo_ptr` only
(`payload.hpp:496`), no first LSN, and a log segment wholly below the redo start
is recycled. So **a loser that reserved before the last checkpoint is counted by
the base, and nothing durable says which entries are its**: CL-R2 (b)'s "the
entry's own bytes, from the page or the record" has no source for that case. A
loser that reserved after the base is identifiable (the fold's records name it).

**4. The paths that reach the assertion pass with a loser** are one: mount
recovery (`mount_recovery.cpp:249`). A crash, a restore from a backup
(mount recovery from the backup's start, stopping at its stop) and PITR run it;
each has losers at its end point.

**Not run**: a cell with a loser alive across a checkpoint (it reproduces by
reading: the snapshot is taken at the checkpoint and the base counts the loser's
delta); the lock family's answer on a decided id.

### What the operator's word is needed for

The marked items cannot be built as marked. CLA's revised proposals, none
adopted:

- **CL-Q3: (b)** leave P2 out of CL. It changes no admission (item 1). What
  stays owed is the dispatcher's and the bug entry's wording (CK-S3 already
  states the path), which CL-S4 corrects.
- **CL-Q2: three ways to know a checkpoint-spanning loser's entries:**
  - (a') **a writer id on the entry**: 48 bits do not fit the 3 spare bytes, so
    the entry widens (32 to 40 bytes, 204 entries a page to 163) or a per-page
    side table carries it; a superblock bump either way, and CK-Q-style option
    (c) of CK §0 comes back, now with a reason.
  - (b') **a per-checkpoint list** of each active transaction's reservation
    entries `(assertion, page, index)`, a new record kind, written with the
    snapshot; the entries stay 32 bytes.
  - (c') **match by the loser's undo records**: an undone insert or
    delete-mark names `(relation, tuple_key)`, and the page entry with that
    key, kind and group is unapplied; identical entries are interchangeable for
    the total, and **a SUM entry with a different value is ambiguous** - a
    `[quiet-wrong]` risk, so CLA does not propose it.
  CLA proposes **(b')**: no change to the entry or the page format, an
  additive record kind, and the list is bounded by the active transactions'
  reservations. It still needs the operator's mark because it adds a record
  kind and a checkpoint write.

### CL-S1 and CL-S2 - P1 and P2 (2026-11-02, one commit)

Resumed on the standing go-ahead (`raft-marks-2026-11-02.md` §2) over a diff
an earlier session of this worktree left uncommitted on `d75c4539`, read whole
and taken as the stage's draft. The two stages land together because CL-Q2
(b') needs CL-R5: the pending set must outlive the commit record for a
checkpoint to list it.

**What is built.**
- **`ASSERT_PENDING` (34)**, superblock 35 (CL-Q6 (a)): after each
  `ASSERT_SNAPSHOT` run, in the same hold, the checkpoint lists every
  reservation the registry holds pending - and, new, every one an abort has
  taken out of `pending_` and not yet given back (`aborting_`), popped in the
  hold that logs its `ASSERT_ROLLBACK`. `ASSERT_ROLLBACK`'s payload and flag.
- **CL-R5**: `TransactionManager::Commit` and `CommitDeferred` take an
  `after_logged` callback, run once the commit record is appended (after the
  sync for `strict`) and before the publish; the dispatcher drops the pending
  set there, and the failed-commit arms call `AbortTxn`.
- **CL-R1/R2/R3**: the mount hands `RecoverAssertions` the analysis phase's
  non-winners (less `kBootstrapXid`); the pass keeps what is owed - the base's
  `ASSERT_PENDING` plus every `ASSERT_RESERVE` after the base, less each
  `ASSERT_ROLLBACK` - and gives back what an undecided transaction still owes
  through `GiveBackReservation`, the one path a replayed rollback also takes
  (mark `kEntryOrphaned`, relink if skipped, unapply). Unlogged, redone at every
  mount until a checkpoint's base carries it.
- **Analysis**: an `ASSERT_PENDING` alone does not note its transaction - a
  checkpoint may list a transaction whose commit lies just below the scan start.
- **CL-Q7 (b)**: an abort's unwind refused part-way fail-stops the log.
- **CL-Q4**: both `DISABLED_` crash cells enabled and green.
- **CL-Q5 (a)**: the sim re-sums every group of `a0`'s assertion against its
  rows after each restart, exactly and in both directions, and **the stream
  now writes `a0` inside explicit transactions** (`AssertedTxnWrite`). As the
  draft wired it, every constrained write was autocommit, so no crash ever left
  a loser holding a reservation and the oracle could not see P1 - measured: the
  mutant below killed 0 of 20 runs. With it, 3 of 20.

**Review, round 1** (five `critics-developer` lenses, each finding put to an
independent skeptic): 12 confirmed, 6 refuted. Applied: the record and payload
comments (C0, C9, C11); one compensation path instead of two (C1); the exact
oracle (C4) and no oracle-unknown gate (C5); an unenforcing assertion fails a
fault-free run and is counted under faults (C6); cells for analysis's exception
(C7) and for the mid-abort listing (C8). **C2/C3** - an abort refused part-way
leaves reservations counted with nothing naming them - **not as proposed**:
both skeptics showed that keeping the remainder in `aborting_` compensates
nothing once analysis forgets the transaction, so CL-Q7 was written and (b)
adopted. **C10** (the specs) is CL-S4's, which the order already scheduled;
its list is extended (§4). Refuted and not applied: `ReplayAssertionRecord`
refusing kind 34 (unreachable; it now returns OK anyway, harmless), a peer
`CoreRuntime` passing no undecided set (no production path), a coverage gap
on the deferred commit (the claimed wrong result cannot occur), three
suggested cells the order does not require, and "no format event" (the
superblock was bumped).

**Review, round 2** (two lenses over round 1's fixes): 5 confirmed. Applied:
the replay leftovers (C0); **a poisoned transaction's remaining ops are not
sent** (C1 - a refusal inside `AssertedTxnWrite`'s transaction left the stream
drawing a `DROP` that then ran in autocommit, and its `ROLLBACK` revived the
table in the workload's books: a false failure in a fault-free run); an
at-mount fault counts as a fault for the oracle (C2); the closing rollback's
fail-stop is a fail-stop (C3); a lost assertion fails a clean fault-free run
and is counted otherwise (C4). Rejected: one shared `IsAssertionRecord` for
`wal` and `exec` - it predates CL, the two agree, and `wal` cannot depend on
`exec`.

**Mutations, measured** (Debug, the cells named): `list(aborting_)` removed,
and the give-back's `pop_back` removed - each killed by
`AssertionPendingListTest.ACheckpointDuringAnAbortListsWhatItHasNotGivenBack`;
`FailStop` removed - killed by `AnUnwindRefusedPartWayStopsTheLog`; analysis
noting `ASSERT_PENDING` - killed by
`AnalysisTest.AnAssertPendingAloneDoesNotMakeItsTransactionALoser`; the
pass compensating nothing - killed by both crash cells, the recover cells and,
in `build-release`, 3 of 20 sim runs (seeds 3, 4 and 20261102002,
`sync-crash`; one of them a stranded departure, `rows=1/total=0`).

Overhead not measured; measured at the milestone's close.

### CL-S3 - verification (2026-11-06)

Resumed on the standing go-ahead (go-ahead-achieving-milestone, invoked as
*"cl /go-ahead-achieving-milestone"*) in `ck-assertion-commit-settle` at
`8751ec2f` (`v3.4.6-19-g8751ec2f`). An earlier session's `mutants.sh` run had
died with its `holds-ignored` mutant still applied to
`src/exec/assertion_check.cpp`; the file was restored from the script's own
snapshot (`build-bs-noassert/mutants/snapshot`, differing in that one line)
before anything ran.

**The full Debug suite at `8751ec2f` was red**: 4352 of 4353 passed, and
`AssertionRaceTest.EverySnapshotEqualsTheFoldOfTheRecordsBeforeIt` hung for
4960 s and was killed. CL-S1/S2 had landed without a full-suite run. The
cell's two writers reserved 400 rows each under one never-committed
transaction per thread, so with CL-R1 every pass of its back-to-back snapshot
loop listed up to 800 `ASSERT_PENDING` records under the registry's latch (a
plain, unfair `std::mutex`): the snapshot thread ran at 100 % CPU, the writers
waited on the latch, and the memory log grew ~1.5 GB per 10 s. **Fixed in the
cell**: one transaction per row, its pending set dropped after the reservation
as the dispatcher drops it once `TXN_COMMIT` is logged. The claim is checked
as before (the fold reads `ASSERT_RESERVE` alone; kind 34 falls through the
scan). The cell now takes ~40 s in Debug on this host (41, 53, 38, 38 s); the
old shape with the listing disabled takes ~30 s, so the 8.8 s in
`CTestCostData` is not this host's number. The engine-side cost is recorded in
`known-gaps.md` ("A checkpoint's `ASSERT_PENDING` list is as long as the open
reservations"): R records per checkpoint under the instance's directory latch,
nothing bounding R but the transactions' size. No other cell holds
reservations open across a tight snapshot loop (the review's census).

**Verification** (code at `8751ec2f`, the cell fix on top):
- **Sanitizers**: no clang on this host (`scripts/bs/config.sh`'s
  `KDS_LLVM` is absent), so gcc 14.2 ASan (`build-gcc-asan`) and TSan
  (`build-gcc-tsan`) Debug builds ran the cells matching
  `Assertion|Analysis|Recover|StrictParkRig|TransactionManager|Checkpoint|MountRecovery`:
  326/326 under each. The rest of the suite was not run under a sanitizer.
- **Sim corpus with the CL-Q5 oracle**: `scripts/sim.sh 4` on
  `build-release/ckdbs-sim`, 280 runs, 0 failures.
- **`scripts/bs/mutants.sh`** (3 rounds, `build-bs-noassert`): baseline 0/9
  sim, 0/3 cell rounds; every mutant killed - `admit-everything`,
  `bound-off-by-one` and `parent-delete-unchecked` by 9/9 sim and 3/3 cell
  rounds, `holds-ignored` by 3/3 cell rounds, and CL's two,
  **`loser-not-given-back`** (the pass compensating no undecided transaction)
  and **`pending-unlisted`** (the checkpoint listing no pending set), each by
  3/3 cell rounds (0/9 sim at 1500 ops, `clean` mode, which crashes nothing).
- **The cell's own mutation re-measured** on the per-row shape (Debug):
  `ReserveOne` logging before it takes the latch for the header change -
  killed 5 runs in 5.

**Review** (`critics-developer`, one lens over the cell fix and the gap):
no correctness finding. Applied: the gap bullet widened to say the latch is
the registry's one (every assertion's writers wait, not one's), that the
appends can incur a ring-full flush or a roll's sync under it, and that no
existing cap bounds R (`AssertionSnapshotFits` sizes groups only,
`max_locks_per_txn` counts one coarse unit for a bulk write, autocommit has
no lifetime ceiling); "fixed in the cell" reworded (the gap is not fixed);
the cell's comment tightened; the mutation count re-measured (above).
Rejected: none.

Overhead not measured; measured at the milestone's close.
