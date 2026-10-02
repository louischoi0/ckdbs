# A peer's writeback runs core 0's WAL sync on the peer's thread

## What is wrong

A peer that writes pages back can run core 0's WAL sync on its own thread,
and that sync mutates core 0's manager.

**Core 0's manager is every peer's WAL gate.**

- The shared page store's WAL gate is core 0's `WalManager`
  (`src/server/expeditor.cpp:825`).
- A peer borrows the store without re-gating it
  (`src/server/core_runtime.cpp:154-168`).

**Two peer paths reach that gate.**

- The peer's checkpoint flush (`src/wal/checkpointer.cpp:214`).
- The peer's transaction-id carve, whose persist is `store_->Sync()`
  (`src/server/expeditor.cpp:1171-1174`, wired to every peer at `:1752-1754`).

Both reach the gate at `src/storage/device_page_store.cpp:1327`. If the
highest page LSN is not yet durable, `EnsureDurable` returns `Sync()`
(`src/wal/manager.cpp:303`). Core 0's manager owns its stream, so that is
the inline sync (`:158-168`).

**That `Sync()` runs on the peer's thread, against core 0's manager.** There
it:

- issues every segment's `fdatasync`;
- increments `stats_` (`:225`) and writes `last_sync_ns_` (`:226`);
- runs `ResolveBatches()` (`:264`), which reads and clears
  `pending_group_commits_` (`:285`).

Meanwhile core 0's reactor reads and writes those same plain fields: its
commit path increments the count (`:368`), and its drain reads it.

**Two statements are false on this path:**

- `include/kds/wal/manager.hpp:29-31`: everything a manager holds *"is its
  core's and is touched by no other thread"*.
- `docs/spec/wal.md` §3: *"a peer performs no device sync"*.

Verified by reading, at `d3d90b5` on `worktree-parallelism-workorder`. Not
run.

## Smallest reproduction (not run)

1. Start `cores = 2` with `durability = group`.
2. On core 0, a session commits in a loop, so core 0 usually has a staged
   group batch.
3. On core 1, a session dirties a page whose LSN is past the durable point.
   Then either it runs past 4,096 transactions, which makes the peer carve,
   or it waits for the peer's checkpoint (`checkpoint_interval_ms`, 5,000 by
   default).
4. The carve or the checkpoint flush now runs core 0's `Sync()` on core 1's
   thread.

An assertion of the owning thread in `WalManager::Sync()` fails there.

## What it costs

- **It is a data race on plain fields**, which in C++ is undefined
  behaviour.
- **A commit can wait up to 10 ms longer.** Core 0's commit path can
  increment `pending_group_commits_` while the peer's `ResolveBatches` clears
  it; the increment is then lost although that commit is not yet durable.
  Core 0's next drain sees no pending commit and asks for no sync. The parked
  statement then waits for the D3 interval tick (`relaxed_flush_interval_ns`,
  10 ms, `include/kds/wal/manager.hpp:112`) instead of the next reactor pass.
- **Core 0's `SHOW META` sync and batch counters can miscount.**
- **It is not a quiet wrong answer.** The parked statement waits on
  `IsDurable(lsn)`, which reads the watermark itself
  (`src/server/command_dispatcher.cpp:743-745`), so it is never acknowledged
  early.
- **The peer's reactor stalls.** It runs an `fdatasync` of every segment,
  which `wal.md` §3 says never happens on a peer. That peer's sessions wait
  out the sync.

## The fix

The fix is known and not yet scheduled. It is
`instructions/v3.0.0/workorder-ba-parallelism.md` BA-R1, built by BA-S1: the
gate answers from the calling core's manager, so a peer's writeback waits on
the writer, the same way its commits do.
