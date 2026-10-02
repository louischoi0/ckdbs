# A peer's writeback runs core 0's WAL sync on the peer's thread

## What is wrong

**The shared page store's WAL gate is core 0's `WalManager`**
(`src/server/expeditor.cpp:825`). A peer borrows the store without
re-gating it (`src/server/core_runtime.cpp:154-168`).

**Four peer paths reach that gate:**

1. The peer's checkpoint flush (`src/wal/checkpointer.cpp:214`).
2. The peer's checkpoint anchor publish
   (`src/server/superblock_checkpoint_anchor.cpp:126`, `store_.Sync()`).
3. The peer's transaction-id carve, whose persist is `store_->Sync()`
   (`src/server/expeditor.cpp:1171-1174`, wired to every peer at
   `:1752-1754`).
4. A client `SYNC` on a peer session
   (`src/server/command_dispatcher.cpp:1215`).

Each reaches the gate (`src/storage/device_page_store.cpp:1327`). If the
highest page LSN is not yet durable, `EnsureDurable` returns `Sync()`
(`src/wal/manager.cpp:303`). Core 0's manager owns its stream, so that is
the inline sync (`:158-168`).

**That `Sync()` runs on the peer's thread against core 0's manager.** It:

- issues the `fdatasync` of every segment;
- increments `stats_` (`:225`) and writes `last_sync_ns_` (`:226`);
- calls `ResolveBatches()` (`:237`, defined at `:264`), which reads and
  clears both `pending_group_commits_` and `highest_group_commit_lsn_`
  (`:285-286`).

Core 0's reactor reads and writes the same plain fields at the same time:
its commit path increments the count (`:368`), and its drain reads it.

**Two documents say this cannot happen:**

- `include/kds/wal/manager.hpp:29-31` says everything a manager holds *"is
  its core's and is touched by no other thread"*.
- `docs/spec/wal.md` §3 says *"a peer performs no device sync"*.

Both are false on these four paths.

Verified by reading at `d3d90b5`, on `worktree-parallelism-workorder`, and
by that order's review. Not run.

## Smallest reproduction (not run)

1. Run `cores = 2`, `durability = group`.
2. A session on core 0 commits in a loop, so core 0 usually has a staged
   group batch.
3. A session on core 1 dirties a page whose LSN is past the durable point,
   then sends `SYNC`.
4. Core 0's `Sync()` now runs on core 1's thread.

The same happens if, instead of `SYNC`, the session on core 1 runs past
4,096 transactions (a carve), or waits for core 1's checkpoint.

An assertion of the owning thread in `WalManager::Sync()` fails there.

## What it costs

- **A data race on plain fields.** In C++ that is undefined behaviour.
- **A lost commit count, and a late commit.** If core 0 increments
  `pending_group_commits_` while the peer's `ResolveBatches` clears it, the
  increment is lost, although that commit is not yet durable. Core 0's next
  drain then sees no pending commit and asks for no sync. The parked
  statement waits for the D3 interval branch (`manager.cpp:455-461`,
  `relaxed_flush_interval_ns`, 10 ms) instead of the next reactor pass, and
  that interval is timed from the `last_sync_ns_` the peer just wrote.
- **Miscounted counters.** Core 0's `SHOW META` sync and batch counters can
  be wrong.
- **Not a quiet wrong answer.** The parked statement waits on
  `IsDurable(lsn)`, which reads the watermark itself
  (`src/server/command_dispatcher.cpp:743-745`), so it is never
  acknowledged early.
- **A stalled peer.** The peer's reactor pays an `fdatasync` of every
  segment, which `wal.md` §3 says a peer never does, and that peer's
  sessions wait for it.

## The fix

Known and not yet scheduled: `instructions/v3.0.0/workorder-ba-parallelism.md`
BA-R1, built by BA-S1. The gate answers from the calling core's manager, so
a peer's writeback waits on the writer the same way its commits do.
