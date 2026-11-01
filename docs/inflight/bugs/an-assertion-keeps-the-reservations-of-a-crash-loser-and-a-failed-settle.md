# An assertion keeps the reservations of a crash loser and of a failed settle, and a stranded departure fails open

**Found by reading, not reproduced.** Read in the main checkout on `main`
at `18264195` (`v3.4.6-4-g18264195`), 2026-10-31, while writing
`instructions/v3.2.0/workorder-ck-assertion-commit-settle.md`. The reading
was confirmed by an independent read at the same commit.

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

## The smallest reproduction (not yet run)

Path 1, departure. Use an assertion `COUNT(*) <= 1` on a group holding one
row:
1. In a transaction, `DELETE` that row, and crash before `COMMIT`.
2. Mount. The row is back, and the group's count reads 0.
3. An `INSERT` into the group is admitted, giving 2 rows under a bound of
   1.

The recovery rig (`tests/assertion_recover_test.cpp`'s fixture) can drive
it. The BS sim crashes at random ops with an assertion declared
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
