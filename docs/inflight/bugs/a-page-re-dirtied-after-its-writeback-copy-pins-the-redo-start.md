# A page re-dirtied after its writeback copy pins the redo start, and the log stops recycling

## The wrong behaviour

A writeback copies a dirty page, writes the copy, and then cleans the
frame only if nothing re-dirtied it after the copy
(`DevicePageStore::WriteBack`, the dirty-generation check, `wal.md` §11
step 2). A frame that was re-dirtied keeps `dirty = true` **and the
`rec_lsn` it had before the copy**: `StampPageLsn` sets `rec_lsn` only when
it is unset, and nothing raises it after a written copy. A page written
continuously, whose copy is overtaken every time, keeps its first `rec_lsn`
across any number of checkpoint runs. `RedoStartFrom` takes the minimum
`rec_lsn` of the dirty table, the anchor fold takes the minimum over every
core's latest anchor, and the recycler removes only segments wholly below
it (BC). So one hot page pins the instance's redo start and the log is not
recycled at all.

Nothing in the code bounds it. Safety holds (a `rec_lsn` that is too low
replays more, never less), but the liveness consequence is unbounded:
- the live log grows with the load;
- every WAL sync covers every live segment (`FileLogDevice::SyncTimed`), so
  each sync's breadth grows with it;
- a crash's replay range is the whole unrecycled log.

## Verified at

Measured at `4cf30270` (`v2.7.0-909-g4cf30270`, BQ-S1's instruments, on
`worktree-wal-segment-roll-order`), and the code is unchanged at `8db9bb5b`
on `worktree-bp-per-core-residue`. Data:
`bench/v3.0.0/results-bq-s1-segment-roll-v2.7.0-909-g4cf30270.md`; the
analysis is in `~/bench-runs/bq/verdicts/recycle/`.

## Reproduction

BQ-S1's C3 cell: `tools/bl_mixed.py --cores 4 --arm insert --readers 0
--writers 0 --durability relaxed --seconds 30`, one bulk inserter at about
80 MB/s of log. Over the 30 s window:

| | relaxed | group |
|---|---|---|
| `wal_segments_removed` | 0 (3 runs) | 21 |
| live segments at the last roll | 38 (3 runs) | 5 |
| fold redo start | LSN 4704/5120 (segment 0) at every load-time publish | advances about one segment per run |
| checkpoint run wall | 1.1-3.5 s, 3.5-12k dirty pages | 38-412 ms, about 1.6k |

At relaxed the page's `page_lsn` is rarely durable when its copy is taken,
so the step parks on the log 16 times and then waits inline
(`log_gate_run`). That widens the window between the copy and the clean
from microseconds to milliseconds, and the page is re-dirtied inside it.
In all three relaxed runs the pin released only in a snapshot taken right
after a run with `log_gate_run_n = 0`, and every pinned snapshot followed
one with `log_gate_run_n = 1`. A second, stated cause compounds it: a run
longer than period/cores makes the gate's rotation (`Turn()`, BI-Q7 (a))
refuse free-gate ticks, so each core checkpoints about every
cores × (period + period/cores), about 25 s here.

## What it costs

A slow answer and a growing disk, never a wrong answer:
- at relaxed a roll's own non-tail sync grew from a median 359-391 us to
  1.7-1.8 ms over the 30 s (4-5 to 35-37 live segments, about 50 us per
  segment), and a segment roll waits out
  two such syncs (`docs/spec/wal.md` §4.1, BQ's subject);
- the log on disk and the crash replay range grow without bound for as long
  as the page keeps being overtaken; observed pins lasted 12.5-18.9 s.

## The fix, known and unscheduled

**Not "raise `rec_lsn` to just above the copy's `page_lsn`".** That rule
assumes every record at or below the copy's `page_lsn` is in the image and
every change applied after the copy carries a higher LSN, which holds only
for a record appended and stamped under its page's hold (`wal.md` §8-1).
`AssertionEnforcer::AbortTxn` appends `ASSERT_ROLLBACK` *before* it takes
the page and marks it after (`records-appended-after-their-page-is-released.md`,
its last row). Failing case: core A's abort appends at L1; core B stamps
the same Bound Cabin page at L2 > L1; a writeback copies it (`page_lsn` L2,
no orphan mark); A marks and stamps L1, so the clean is skipped; the raise
sets `rec_lsn` to L2 + 1 > L1; a publish and BC's recycler remove L1's
segment; a crash loses the mark and the directory no longer matches its
page. Quiet, and wider than today's window for the same root cause.

The shape that does not lean on that order: track, per frame, the lowest
LSN stamped **after** the copy (reset under the structure latch where
`dirty_gen` is read, min'd in `StampPageLsn` under the same latch). On a
skipped clean, set `rec_lsn` to that LSN when one exists, and otherwise
keep the old `rec_lsn`. Every change missing from the image is stamped
after its own mutation, so it is stamped after the copy, and either lands
in that minimum or, stamped after the clean, sets `rec_lsn` itself. A
headerless page (no `page_lsn`, no records) and a frame whose `rec_lsn` is
unset are left alone. It is `[quiet-wrong]` if wrong, so it needs its own
proof and crash cells (one forcing the `AbortTxn` interleaving), and it is
not BQ's subject (BQ's is the roll's hold). No work order carries it yet;
`wal.md` §11-4's "When the bound does not move" should name this case
until one does.
