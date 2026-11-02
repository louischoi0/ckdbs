# An assertion keeps the reservations of a crash loser and of a failed settle, and a stranded departure fails open

**Path 1 reproduced 2026-11-01** in worktree `ck-assertion-commit-settle` at
`b66a0dbe` (`v3.4.6-12-gb66a0dbe`), on the base binary, in both directions
(below); path 2 is still found by reading only. Found by reading in the main
checkout on `main` at `18264195` (`v3.4.6-4-g18264195`), 2026-10-31, while
writing `instructions/v3.2.0/workorder-ck-assertion-commit-settle.md`. The
reading was confirmed by an independent read at the same commit.

## What is wrong

A reservation's delta enters its group's total at reserve
(`src/exec/assertion_check.cpp:600-601`). Only two things take it back out:
the live `AbortTxn`, which appends `ASSERT_ROLLBACK`
(`assertion_check.cpp:941-951`), and that record's replay. Two paths leave
a transaction's rows undone while its deltas stay in the total.

1. **A crash with the transaction undecided.** Recovery's undo rolls the
   rows back. Nothing compensates the reservations, and
   `docs/spec/assertion.md:477-478` says otherwise:
   - `src/wal/recovery.cpp` and `src/txn/recovery_undo.cpp` name no
     assertion record;
   - `src/wal/redo.cpp:495-499` defers assertion records to the assertion
     pass;
   - the fold (`src/exec/assertion_recover.cpp:268-312`) filters by
     assertion id and base only, and folds every `ASSERT_RESERVE`;
   - `src/server/mount_recovery.cpp:171-305` hands the pass no list of losers;
   - the linkage scan re-attaches every entry not orphaned
     (`assertion_recover.cpp:69`).
2. **A failed `CommitTxn`** (a page fetch, open, append or stamp refused
   part-way, under drain mode). It took `pending_[txn]` out before failing,
   so the abort that follows finds nothing (the 2026-09-09 entry, closed
   unreproduced at `d83bd502`:
   `git show d83bd502^:docs/inflight/bugs/assertion-reservations-stranded-by-a-failed-settle.md`).

## What it costs - it depends on the contribution's sign

| Stranded contribution | Total | Outcome |
|---|---|---|
| An arrival of a non-negative value | too high | refuses writes it could admit (fails closed) |
| **A departure** (`DELETE`, an `UPDATE` moving out: `assertion_check.cpp:498`, `:802`) | **too low** | **admits writes the assertion forbids** `[quiet-wrong]` |
| A negative `SUM` arrival | too low | the same, `[quiet-wrong]` |

Assertions are upper bounds only (`assertion.md:63`), so a total too low is
a wrong answer with no error. It survives every later mount, because the
snapshot carries it forward. The dispatcher's comment calling path 2
"Fail-closed" (`src/server/command_dispatcher.cpp:11653-11660`) misses the
second and third rows.

## The reproduction (path 1, run on the base binary)

Two cells in `tests/assertion_crash_test.cpp`, `DISABLED_` because they are
red: `DISABLED_AnUndecidedDepartureAtTheCrashIsGivenBack` and
`DISABLED_AnUndecidedArrivalAtTheCrashIsGivenBack`
(`kds_tests --gtest_filter='AssertionCrashTest.*' --gtest_also_run_disabled_tests`).
Each runs the statements on a file-backed rig with an open transaction,
takes the crash image and mounts it through production's mount.

- **Departure, fails open `[quiet-wrong]`.** A group holds 3 rows under
  `COUNT(*) <= 3`. An open transaction `DELETE`s one, and the crash image is
  taken. After the mount 3 rows stand, and an `INSERT` into the group is
  **admitted** (4 rows under a bound of 3).
- **Arrival, fails closed.** A group holds 2 rows under `COUNT(*) <= 3`. An
  open transaction `INSERT`s one, and the crash image is taken. After the
  mount 2 rows stand, and **both** probing `INSERT`s are refused
  `ASSERTION_VIOLATION`, where one should be admitted.

The same two cells give the same answers on a binary with CK's spike (the
settle removed), so the crash path does not depend on the settle. A
committed departure across a crash is correct on both
(`ACommittedDepartureFreesItsSlotAcrossACrash`). The BS sim crashes at random ops with an assertion declared
(`sim/loop.cpp:416-425`), but no cell checks a group's total against a
re-summation after a mount.

## The fix

- **Path 2** goes with CK (`workorder-ck-assertion-commit-settle.md`
  CK-R2): once the settle does no page work, `CommitTxn` cannot fail.
- **Path 1** needs the analysis phase's losers to reach the assertion
  pass, so that their `ASSERT_RESERVE` deltas are unapplied and their
  entries marked orphaned. The decision must come from `TXN_COMMIT`, never
  from the entry's `kEntryReserved`: a crash between `ASSERT_COMMIT` and
  `TXN_COMMIT` leaves a cleared bit on a loser (CK §1.8, CK-R4). No order
  carries it yet. CK-Q5 proposes that CK-S0 reproduce it and the operator
  open its order.

## After CK-S1 (worktree `ck-assertion-commit-settle`, `9fd02f2e`, 2026-11-01)

- **Path 2 as written is closed**: `CommitTxn` no longer touches a page or a
  record, so it cannot fail part-way (CK-R2, CK-Q7 (a)).
- **A neighbouring path stays open, and CK-R5's "no commit-time step can
  strand a reservation" was wrong.** `CommitTxn` still takes the pending set
  before `TransactionManager::Commit`, as it did on the base. A failed
  `Commit` reaches `AbortTxn` with an empty set, so the reservations stay in
  their totals while undo takes the rows back (a departure, or a negative SUM,
  fails open `[quiet-wrong]`). `CommitLocal`'s failure arm never calls
  `AbortTxn`, and the deferred path's `wal_->stopped()` abort finds the set
  gone too. Not reproduced by a cell; found by the CK-S1 review's reading.
  The fix is to drop the set only after the commit succeeds and to abort in
  `CommitLocal`; it changes what `ReserverOnLocked` sees on another core, so
  it belongs with path 1's order.
- **Path 1 is untouched** by CK and is still open.

## The order

`instructions/v3.2.0/workorder-cl-assertion-reservations-of-an-undecided-transaction.md`
(CL, written 2026-11-01, not opened for work) carries the fix for path 1 and
the failed-`Commit` path above.

## After CL-S0 (worktree `ck-assertion-commit-settle`, 2026-11-02)

The failed-`Commit` path above **changes no admission**: every refusal of
`Commit` stops the log, and a stopped log refuses every later write
(`StrictParkRigTest.AFailedStrictCommitsStrandedDepartureAdmitsNothingOnAStoppedLog`).
Its stranded total is dropped at the restart, whose case is path 1. Path 1
stays open, and a loser that reserved before the last checkpoint cannot be
told from the page (an entry carries no transaction id), so its fix needs a
durable record of which entries a transaction reserved
(`instructions/v3.2.0/workorder-cl-assertion-reservations-of-an-undecided-transaction.md` §6).
