# A strict commit's marker caps every core's snapshot across its sync

## What is wrong

While a `strict` commit syncs, no new snapshot on any core can cover a
commit published after that strict commit began. That includes a commit
already acknowledged to its client.

**A commit holds a marker from its append until its publish.**

- It stores its snapshot marker before its append (`pending.emplace`,
  `src/txn/manager.cpp:309-318`).
- It lifts the marker after its publish (`:351-361`).

**Every new snapshot is capped at the lowest held marker.**
`SnapshotCeiling()` returns that cap (`src/txn/instance_visibility.cpp:93-115`),
and every mint reads it (`src/txn/manager.cpp:96`, `:139`).

**Under `strict` (D1), the marker's span includes the sync.** Under D2 the
span between append and publish is the append alone. Under D1,
`WalManager::Commit` syncs before it returns (`src/wal/manager.cpp:357-365`):

- on core 0, inline on its reactor;
- on a peer, waiting on the writer's condition variable (`:190-198`).

So for the whole sync, every core's new snapshot is capped below every commit
published after the marker was set.

**This breaks `docs/spec/txn.md:92`**, which promises that under `READ
COMMITTED` a statement *"sees everything committed before it began"*. A
commit that was acknowledged before a statement began, yet sits above the
cap, is not seen.

The header's design note (`include/kds/txn/instance_visibility.hpp:70-101`)
argues the marker for AN-Q3 and for monotonicity. It says nothing about a
marker held across a sync.

**Classes mix on one instance.** The durability class is per transaction
(`docs/spec/wal.md` §1), so `strict`, `group` and `relaxed` commits can run
side by side.

Verified by reading, at `d3d90b5`, on `worktree-parallelism-workorder`.
Found by that order's review. Not run.

## Smallest reproduction (not run)

1. Start with `cores = 2`.
2. On core 0, a session commits `strict`. Pause core 0 inside its
   `fdatasync`; this needs a test seam on the log device.
3. On core 1, a session commits `relaxed`. That commit is acknowledged at
   its publish.
4. Still on core 1, the same session selects the row it just wrote. The row
   is missing until core 0's sync returns.

With `group` on core 1 instead, the same thing needs core 1's sync, done by
the writer thread, to finish before core 0's inline one. Two threads syncing
at once allow that ordering.

## What it costs

- **A quiet wrong answer.** A statement misses a commit that was
  acknowledged before it began, its own session's commit included, for up
  to the length of one sync. Scenario 0's `cores = 1` `strict` cell implies
  about 1.5 ms per sync
  (`bench/v3.0.0/results-scenario0-stockmarket-v2.7.0-531-g9a0525d.md` §5).
- **A parallelism cost.** For the length of every `strict` sync, every
  core's snapshots stop advancing. The marker also holds back the window's
  reclaim (`instance_visibility.hpp:102-110`), so the window grows, and
  more rows sit above the floor and take its latch.

## The fix

Not decided. `instructions/v3.0.0/workorder-ba-parallelism.md` carries both
fixes.

- **BA-R1c, built by BA-S1c, closes a session's own case.**
  - A session records its last acknowledged commit LSN.
  - A statement whose bound sits above `SnapshotCeiling()` parks at the
    statement boundary until the markers below the bound lift. That wait is
    at most one sync.
- **BA-Q3 (c) closes the rest.** Snapshots carry their in-flight set, so no
  commit holds a marker across its sync. That is a `txn.md` §4 redesign and
  would be its own order.
