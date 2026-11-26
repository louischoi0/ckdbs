# Two cores incrementing one row: one core can lose 201 retries in a row

**The wrong behaviour.** Two autocommit sessions, one a core, each run
`UPDATE c SET v = v + 1 WHERE id = 1` in a loop and retry a retryable
refusal. One of them can be refused `TXN_CONFLICT retryable=1 row id=1
was written by transaction <n>` on the same statement 201 times running:
each attempt mints its view, the other core commits a new version of the
row before the attempt's claim, and first-updater-wins refuses it
(`txn.md` §5: under `READ COMMITTED` "KDS aborts retryably rather than
re-reading"). Nothing bounds how often one core loses that race to the
other, and no increment is lost or applied twice - the loser simply stops.

**Verified at** `60dc593af` (`v3.4.17-150-g60dc593af`), Debug, built from
`git archive` outside the repository, on a 24-CPU Fedora host, 2026-11-26:
`SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement`
(`tests/set_expr_two_core_rig_test.cpp`) failed 12 of 60 runs alone, the
driver's 200 retries an increment (`kRetriesPerIncrement`) spent. The same
12 of 60 on `worktree-du-keyed-write` at DU-S3's tree, so DU did not move
it. It passed in seven full-suite runs of that day and failed in one.

**The smallest reproduction.**

```
build/tests/kds_tests \
  --gtest_filter='SetExprTwoCoreRigTest.TwoCoresIncrementingOneRowLoseNoIncrement' \
  --gtest_repeat=60
```

**What it costs.** A refusal, retryable and truthfully so, and a suite
cell that fails about one run in five alone. No quiet wrong answer: the
cell's own check - the row's value is the increments accepted - holds in
every failing run (`v = 628` after 628 accepted statements).

**The fix is not known.** Two readings, neither tried: the cell's bound
is too tight for a host where one reactor is scheduled much less often
than the other, and wants a fairness-independent stop; or the engine owes
a waiter that lost first-updater-wins to a committed writer something
better than an immediate re-run that can lose again - EE's decide slot
(`instructions/v3.2.0/workorder-ee-decide-slot.md`) is the order nearest
it. No work order carries it.
