# Work order CL — the reservations of an undecided transaction: a crash loser and a failed commit

Written and opened 2026-11-01 on `main` from `a59f9a2e` (`v3.4.6-16-ga59f9a2e`),
on the operator's word, recorded in English (`raft-marks-2026-11-01.md` §1):

- **W0** (2026-11-01): *"open the crash-loser and failed-commit fix order"*,
  after CK's close report named two fail-open paths that CK does not fix.

**Written 2026-11-01; not opened for work.** Every CL-Q below carries CLA's
proposal and **none is marked**. The letter CL is CLA's proposal: CA..CK are
taken (CJ by another session), so CL is the next free letter. The operator
may reletter it.

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
only if a row states it.

---

## 5. Out of scope

- A change to admission, holds, `ASSERT_RESERVE`'s format or AS6a.
- The abort's batching (CK-Q6 (a)).
- The settle (CK, closed).
- A tag or a version.

---

## 6. Stage log

Written 2026-11-01. No stage has run.
