# A failed data-file fsync leaves its pages clean

**Found by reading; not reproduced.** Source read at `65a28553` on
`worktree-checkpoint-reactor-workorder`, by the `critics-developer` review
of `instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md`
(BI-S0). The Linux behaviour it rests on is the one `docs/spec/wal.md` §6
item 5 gives as the reason the log fail-stops: *"on Linux a failed `fsync`
can drop the dirty pages and clear the error, so the next one reports
success over bytes that are gone"*.

## What is wrong

The data file has no fail-stop. The log does, since `fsync-fail-stop`.

- `[source-read]` `FilePageDevice::Sync` returns an `fsync` failure, and
  nothing records it (`src/storage/file_page_device.cpp:241-249`). A later
  call can report OK.
- `[source-read]` A writeback cleans each frame after its `pwrite` and
  before any sync (`src/storage/device_page_store.cpp:2095-2106`).
  `FlushPages` syncs only afterwards (`:2275`), so a failed sync leaves the
  frames clean.

Two consequences follow, by reading:

1. **At runtime.** `[source-read]` A clean frame is discarded on eviction.
   `[design]` If the kernel dropped the page-cache pages when the `fsync`
   failed, the next fault reads the device's older bytes. A committed
   update then reads back stale until restart.
2. **Across a crash.** `[source-read]` A failed checkpoint step leaves the
   run in progress with its cursor where it was, and the next tick retries
   the same pages (`src/wal/checkpointer.cpp:214-226`, `:314-318`). They
   are clean now, so `FlushPages` writes nothing and skips its sync
   (`device_page_store.cpp:2274`), and the step succeeds. The publish's
   `store_.Sync()` (`src/server/superblock_checkpoint_anchor.cpp:132`) can
   then report OK, which publishes an anchor whose redo start is past
   records those pages still need. `[design]` After a crash, recovery
   starts there and never replays them.

## What it costs

A **quiet wrong answer**, in both cases, and only after a data-file `fsync`
has failed once. The data is not lost from the log, which stays durable
and correct. What goes wrong is that the pool, and later the anchor, stop
pointing at it.

## The fix

**Not scheduled.** BI-Q3 in
`instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md` puts the
options. CLA proposes that a failed data-file sync stop the instance, which
is PostgreSQL's `data_sync_retry = off`. The defect predates BI, and BI's
data-file thread is where the gate would go.
