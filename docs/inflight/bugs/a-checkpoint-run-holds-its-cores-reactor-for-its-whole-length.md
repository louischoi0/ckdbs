# A checkpoint run holds its core's reactor for its whole length

**Measured on a live load; not reduced to a test.** Source read at
`7e020891` on `worktree-checkpoint-stall-findings`. Measured against the
binary xrock runs, sha256 `4d364ae0…`, which
`/home/cdkbs/bench-runs/v20-insert-slowdown/binaries.sha256` records as
build-release of `19843fee` (`v2.7.0-688-g19843fee`). Its build commit was
not recorded when it was built. Its checkpoint path is this commit's but
for BF-R5's map write barrier in `FlushMaps`, which every step calls.
Found from a client's incident reports (xrock, outside this tree:
`/home/cdkbs/xrock/.claude/docs/ckdbs/`), where
a 1024-row `INSERT` went unanswered past the client's 30 s receive timeout
and a `C_HELLO` went unanswered for 30 s.

## What is wrong

A core's periodic checkpoint is one synchronous call on that core's reactor
thread, and nothing yields inside it:

- `CoreRuntime` schedules `Checkpoint()` with `SubmitEvery`
  (`src/server/core_runtime.cpp:539`; core 0's at
  `src/server/expeditor.cpp:1911`). `Checkpointer::RunGated`
  (`src/wal/checkpointer.cpp:331`) calls `RunToCompletion` (`:305`), which
  loops `Step()` until the snapshot is written. The snapshot is the shared
  pool's whole dirty table (`:155`), every core's pages.
- Each step is `pages_per_step` = 64 pages
  (`include/kds/wal/checkpointer.hpp:277`), with up to two syncs.
  `DevicePageStore::AwaitWalGate` (`src/storage/device_page_store.cpp:1770`)
  waits for the log to be durable to the batch's highest `page_lsn`. If the
  log is not yet durable that far, that is an `fdatasync`: core 0 issues it
  inline, and a peer blocks on the writer thread's. If it is, the gate does
  nothing. The step then writes its pages, and `FlushPages` (`:2234`)
  `fsync`s the data file (`:2275`) if it wrote any.
- The run then makes `CHECKPOINT_END` durable (`src/wal/checkpointer.cpp:269`)
  and publishes the anchor. The publish syncs the whole store
  (`src/server/superblock_checkpoint_anchor.cpp:132`): `DevicePageStore::Sync`
  writes back every page dirty in the pool at that moment, which is
  everything any core dirtied during the run, in one call, then `fsync`s.

So a run of N dirty pages takes N/64 steps. Each step costs its writes and
one data-file `fsync`, plus a log sync if its batch holds a page logged past
the durable point. The run then ends with one unpaced writeback of whatever
its own length let build up. For that whole time the core answers nothing:
no statement, no `C_PING`, no handshake, for any session on it.

This is not the spec's shape. `docs/spec/wal.md` §11-2 (`[PROPOSED]`) has
the flush "paced across the checkpoint window ... the checkpointer never
floods the foreground". `pages_per_step`'s own comment sizes a step "small
enough that a checkpoint does not monopolize a reactor iteration". The
steps are sized for that, but the loop takes every one of them in a single
iteration.

**An anchor gap is not, by itself, a stall of that core.** A core whose tick
finds another core's run in progress skips it (`CheckpointGate`,
`include/kds/wal/checkpointer.hpp:235`). So while one core runs, every other
core's anchors stop as well, even though those cores keep answering.

## Measurement

Measured 2026-10-08, 08:41:05-08:49:05 UTC, against xrock's own server and
load:

- **Load:** 4 sessions of 1024-row named-key `INSERT`s into one btree.
- **Server config:** `cores = 4`, `durability = relaxed`,
  `checkpoint_interval_ms = 5000`, `buffer_pool_frames = 786432`.
- **Probe:** 8 KWP connections, each sending `C_PING` every 200 ms. A ping is
  answered in the frame loop and runs no statement, so its latency measures
  how long the connection's core took to turn its reactor.
- **Thread sampling:** every 100 ms, each server thread's `/proc` state and
  `wchan`. Peer threads were identified by their pinned CPU, core 0 as the
  main thread and the writer as the remaining unpinned thread.

The connections fell into four groups whose stalls coincide, one group per
core. Each group was tied to its core by the thread that was in I/O wait
during the group's stalls. The anchors confirm the assignment
independently: of the 24 ways to assign the groups to cores, the one used
matches 21 of the 22 stalls of 2 s or more to their own core's anchor, and
the next best matches 14.

| Pings slower than | Stalls (per core, de-duplicated) | That core's own anchor within [-1.5 s, +1 s] of the end | Control: the same ends shifted +2.5 s |
|---|---|---|---|
| 1 s | 77 | 59 (77%) | 29 (38%) |
| 2 s | 22 | 21 (95%) | 12 (55%) |
| 3 s | 8 | 8 | 3 |

The longest stall in the window was 5.7 s. In the samples taken during the
22 stalls of 2 s or more:

- **The stalled core's thread** was off the CPU 93% of the time. It spent
  63% in I/O waits: `jbd2_log_wait_commit` 26% (`fsync`),
  `folio_wait_bit_common` 20%, `rq_qos_wait` 16%, and smaller ones. It
  spent 30% in `futex_do_wait`, which is a mutex or condition-variable
  wait. A peer's wait for the writer's sync is one such wait; so is the WAL
  stream latch, held across a `pwrite`. It ran 7% of the time and was never
  in `ep_poll`.
- **The other three cores' threads** were in `ep_poll` 27% of the time,
  running 27%, and in `futex_do_wait` 26%.

Every stall of 2 s or more but one (21 of 22) ends when its core publishes
its anchor, so those stalls are the core's checkpoint run. Below 2 s the
match is weaker (59 of 77, against 29 by chance), so not every 1-s stall is
a run.

This window's runs were short (≤ 6 s). One run goes at a time, and no
anchor from any core came more than 6 s after the one before it. How many
pages each run flushed was not captured: `pages_flushed` is a Debug line,
and the server logs at `info`. A core's own anchor gaps are no measure of
its runs. In this window they reached 26 s (core 0, ending 08:47:12), and
earlier in the same server run (started 08:22) they reached 17 s and 22 s,
while the instance-wide gap stayed at 6 s or less. In the 07:31 run the
per-core gaps reached 83-110 s, and the instance-wide gap reached 29 s
(ending 07:33:14).

The scripts and raw data are archived at
`/home/cdkbs/bench-runs/checkpoint-reactor-stall-19843fee/`.

## What it costs

A slow answer, never a wrong one. Every session on the checkpointing core
waits out the run. A client whose timeout is shorter than the run fails.
xrock's two incidents fit this, but neither logged which core the session
was on:

- **The 06:23 incident:** a statement went unanswered for more than 30 s.
  One run does not explain all of it. The instance-wide anchor gap there is
  20 s, from 06:23:31 to 06:23:51. After it, core 0 published every anchor
  until 06:23:59 (at :51, :55, :58 and :59), which fits back-to-back core-0
  runs.
- **The 07:32 incident:** a handshake went unanswered for 30 s. The 29 s
  instance-wide gap ends at 07:33:14, when core 1 published its anchor. That
  is the same second the client gave up.
- **The same server, after the measured window:** the instance-wide gap ran
  from 08:52:45 to 08:53:21, 36 s, and ended at core 3's anchor. An xrock
  `INSERT` sent at about 08:52:39 timed out after 30.17 s inside that gap
  (xrock `var/fetch90p/logs/2026-06-11.log`). The gap bounds that run at
  36 s or less.

**The `relaxed` loss window is not held during core 0's run.** This is
read from the source, not measured. The drain that bounds `relaxed` loss
runs on core 0's reactor: the post-task hook and its timer
(`src/server/expeditor.cpp:2008`, `:2011`). A peer's tick returns before
the loss-window branch (`src/wal/manager.cpp:495`). So while core 0 runs a
checkpoint, every core's acknowledged `relaxed` commits become durable only
through the run's own syncs. `docs/spec/wal.md` §1 bounds that class's
loss at the configured flush interval, and during the run it is bounded
only by those syncs.

The run grows with the dirty table, up to a ceiling. `CHECKPOINT_BEGIN`
carries the whole table at 12 B per page, and the record must fit the
1 MiB WAL ring (`include/kds/wal/stream.hpp:130`, refused at
`src/wal/stream.cpp:247`). So a run can reach its steps with at most about
87,000 dirty pages, about 1,370 steps. Above that size the run fails at its
first append and no step runs. xrock's table at 07:16 the same day was
about 505,000 pages (a 6,060,256-B record), and every checkpoint failed
that way (xrock's `report-20261008-kds-checkpoint-record-exceeds-wal-ring.md`).

`durability = relaxed` may lengthen a run in two ways:

- Commits do not wait for the log, so pages dirty faster than under `group`.
- No commit moves the durable point, so a step's log gate is less often a
  no-op, and the log tail it syncs is longer. The only other sync is core
  0's loss-window tick (`relaxed_flush_interval_us`, 10 ms by default), and
  that tick does not run while core 0 is the core checkpointing.

That `relaxed` lengthens runs is inferred, not measured. Each of xrock's
4-session loads has a longest instance-wide anchor gap, and that gap bounds
the load's longest run, because one run goes at a time and none failed.
Under `relaxed` the gaps were 26 s and 29 s (the 06:20 and 07:31 loads).
Under `group` they were 23 s and 16 s (06:00 and 06:32). The loads also
ran with different pools. The per-core gaps xrock reports (153 s and 46 s)
also count skips.

## The fix

- **Not scheduled.** The checkpointer already has
  `Start`/`Step`/`Complete`. Two candidates:
  - take one step per reactor turn instead of all of them, as
    `pages_per_step` intends;
  - move the writeback off the reactor.

  The first candidate alone leaves the anchor publish's whole-store `Sync`
  as one unpaced writeback at the end, and that writeback holds everything
  dirtied while the steps were spread out. Either candidate changes how
  long a run takes to reach its anchor, and that is the operator's
  decision.

## Related

- `docs/pending/v20-insert-slowdown.md` §2.5 reads the same path. For one
  load, the measurement above did part of its option (c), "reproduce a
  stall window and capture it": it captured per-thread `wchan`, but not
  PSI and not `pages_flushed` per run.
