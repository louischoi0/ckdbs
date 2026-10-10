# A strict commit's marker caps every core's snapshot across its sync

**Narrowed by BA-S1c** to another session's commit: a session's own commit
is now waited for. The entry as it stood before is at
`git show dfabae1:docs/inflight/bugs/a-strict-commits-marker-caps-every-cores-snapshot-across-its-sync.md`.

## What is wrong

While a `strict` commit syncs, no new snapshot on any core can cover a
commit published after that strict commit began. That includes a commit
already acknowledged to its client.

**A commit holds a marker from its append until its publish.**

- It stores its snapshot marker before its append (`pending.emplace`,
  `src/txn/manager.cpp:309-318`).
- It lifts the marker after its publish (`:351-361`).

**Every new snapshot is capped at the lowest held marker.**
`SnapshotCeiling()` returns that cap (`src/txn/instance_visibility.cpp`),
and every mint reads it (`src/txn/manager.cpp:96`, `:139`).

**Under `strict` (D1), the marker's span includes the sync.** Under D2 the
span between append and publish is the append alone. Under D1,
`WalManager::Commit` syncs before it returns (`src/wal/manager.cpp:383`):

- on core 0, inline on its reactor;
- on a peer, waiting on the writer's condition variable.

**What BA-S1c closed.** A session records its last commit's LSN, and a
statement whose bound sits above the ceiling waits at the statement boundary
until the markers below it lift (`CommandDispatcher::DispatchAsync`,
`docs/spec/txn.md` §4.1). The session that committed therefore always sees
its own commit.

**What is left.** A commit acknowledged to **another** session before a
statement began, yet above the cap, is still not seen by that statement for
up to one sync. This breaks `docs/spec/txn.md`'s level table, which promises
that under `READ COMMITTED` a statement *"sees everything committed before
it began"*.

The header's design note (`include/kds/txn/instance_visibility.hpp`) argues
the marker for AN-Q3 and for monotonicity, and since BA-S1c names this
remainder.

**Classes mix on one instance.** The durability class is per transaction
(`docs/spec/wal.md` §1), so `strict`, `group` and `relaxed` commits can run
side by side.

**Reproduced** at `6b0d8412` on `worktree-fix-strict-marker-other-session`, on
every run (3 of 3), by the two-core rig: the cells
`StrictMarkerSnapshotRigTest.DISABLED_AnotherSessions*`
(`tests/strict_marker_snapshot_rig_test.cpp`), disabled until the fix below
lands. Session B's `SELECT` answered no row. The own-session case was
reproduced and fixed by BA-S1c at `fb94070`.

## Smallest reproduction of what is left

1. Start with `cores = 2`.
2. On core 0, a session commits `strict`, paused inside its `fdatasync`
   (the rig's `gated_log_sync`).
3. On core 1, session A commits `relaxed` and is acknowledged.
4. Session B, also on core 1 or on any core, which learned of A's commit
   from outside the engine, selects the row. It is missing until core 0's
   sync returns.

## What was tried, and why it did not land

A statement boundary that waits for the instance's published commit ceiling
(`max(session's last commit, CommitCeiling())`, a one-function change in
`CommandDispatcher::UncoveredCommit`) turns both cells green and passes the
suite. It was measured, `cores = 2`, `relaxed`, against `6b0d8412`
(`bench/v3.0.0/results-boundary-wait-overhead-v2.7.0-749-ga80de010.md`):

- a write costs about 1 percent more (+0.6 to +1.0 us), the statement after a
  commit paying for the ceiling walk;
- **a read-only session beside a `strict` committer and a `relaxed` one lost
  89 percent of its throughput** - 13,401 to 1,402 QPS, p99 128 us to
  4.7 ms - about 38 percent of its reads waiting roughly 1.7 ms each.

The wait is the method's, not the marker's looseness: tightening a marker to
its commit's own LSN would cut the waits that are not needed, but a statement
that begins while a `strict` commit is unpublished below a published one must
wait for it under any snapshot that is one number. It is not a change CLA
makes on its own; the answer is the fix below.

## What it costs

- **A quiet wrong answer.** A statement misses a commit acknowledged to
  another session before it began, for up to the length of one sync.
  Scenario 0's `cores = 1` `strict` cell implies about 1.5 ms per sync
  (`bench/v3.0.0/results-scenario0-stockmarket-v2.7.0-531-g9a0525d.md` §5).
- **A parallelism cost.** For the length of every `strict` sync, every
  core's snapshots stop advancing. The marker also holds back the window's
  reclaim (`instance_visibility.hpp`), so the window grows, and more rows
  sit above the floor and take its latch. Since BA-S1c a session's statement
  after its own commit can also wait out that sync.

## The fix

**BA-Q3 (c)**: snapshots carry their in-flight set, so no commit holds a
marker across its sync. That is a `txn.md` §4 redesign and would be its own
order (`instructions/v3.0.0/workorder-ba-parallelism.md`). Unmarked.
