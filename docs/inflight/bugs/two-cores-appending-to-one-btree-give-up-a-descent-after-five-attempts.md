# Two cores appending to one btree give up a descent after five attempts

**Reproduced as a test failure, intermittent; the cause is not read.** Seen at
`6b0d8412`'s source on `worktree-fix-date-wrapped-literal-stored` (3 of 3 full
`ctest -j8` runs, host load about 10) and on
`worktree-fix-bad-alloc-reported-as-ok` (1 of 3 runs of the 81-test subset
below, and 1 of 2 full `ctest -j4` runs). Not seen on four earlier worktrees
whose suites ran on a quieter host. The tests of those branches do not touch
the btree, so this is not theirs.

## What happens

`SortedLeafRigTest.AppendsFromTwoCoresKeepTheTreeWholeAndDense`
(`tests/sorted_leaf_rig_test.cpp`) has both cores of the rig append
`kPerCore` ascending keys to one clustered relation and expects every insert
to answer `INSERTED`. Under a parallel test mix an insert answers:

    ERR TXN_CONFLICT retryable=1 btree descent for key 200 from page 129 gave
    up after 5 attempts: each leaf it reached had already given the key away.
    Either the chain is being split faster than a descent can cross it, or
    this core is descending from a root that has since grown a level

and the relation then holds 1199 of 1200 ids.

It fails 0 of 30 runs alone and 0 of 20 with eight busy-loop processes on the
host. It fails about one run in three when ctest runs the btree, purge,
drop-table, bounded-pool and insert-log crash tests together:

    ctest -j4 -R 'SortedLeaf|PurgeKey|DropTableReclaim|BoundedPool|FrameTableRace|InsertLogCrash'

## What it may cost

The refusal is the engine's, and says `retryable=1`: a client that retries
gets through, and nothing is lost or wrong. What it is not known to be is a
bounded wait that a loaded host can exhaust, or a descent that a root growth
under a second core can starve (`AT-S15`/`AT-S16`'s restart bound is five).
The test treats any `ERR` as a failure and does not retry, so the suite is
red on a loaded host for a reason a client would not see.

## Not done

Nothing was read in `src/storage/btree/btree.cpp`'s restart loop. The first
question is whether five attempts is a number chosen for a quiet host, and
whether the test should retry a retryable refusal as a client does.
