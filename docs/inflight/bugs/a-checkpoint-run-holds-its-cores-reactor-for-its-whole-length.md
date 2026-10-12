# A checkpoint run holds its core's reactor for its whole length

**Restated at BI's close, 2026-10-11, on `worktree-bi-checkpoint-off-the-reactor`
at `ec827a19` (`v2.7.0-784-gec827a19`).** The entry as found - a run as one
synchronous timer callback, its log and data-file syncs inline, measured on
xrock's load at `19843fee` - is at `ec827a19`'s parent chain; `git show
6f3b32b0:docs/inflight/bugs/a-checkpoint-run-holds-its-cores-reactor-for-its-whole-length.md`
reads it. BI (`instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md`)
answered it in part, and its title is no longer what the engine does: a run is
a `system` task that takes one bounded call per poll and parks for every sync.
What it still holds is below, measured, so the entry stays open on that.

## What BI changed, measured

`[measured]` `bench/v3.0.0/results-bi-s5-checkpoint-stall-v2.7.0-784-gec827a19.md`,
A = `v2.7.0-779-g55ee8370`, B = `v2.7.0-784-gec827a19`, twelve 600 s cells
each, four load sessions of 1024-row named-key `INSERT`s into one btree:

- The checkpoint's reactor time per ten-minute cell fell about tenfold: at
  `cores = 4`, `group` from 290-358 s to 15-18 s.
- Its longest single hold fell from 1.1-79 s (each A run is one hold) to
  0.15-0.64 s under `group` and at `cores = 1`, and 1.6-4.2 s at `cores = 4`,
  `relaxed`.
- The same-core statement probe at `cores = 4`, `group` answers inside a run
  in 0.6 ms at p50, against A's 315-502 ms; its in-run p99 is within 2x its
  out-of-run p99 in 10 of 12 cells.

## What is still wrong

1. **A step's and a publish batch's writes stay on the reactor**, and a
   buffered `pwrite` under writeback pressure blocks: single page writes of
   up to about 300 ms, publish batches summing 21-29 s per `cores = 4`,
   `relaxed` cell. W2 moved the syncs, not the writes (BI §0, BI-R5).
2. **After 16 deferrals a step waits for the log inline**
   (`Checkpointer::kGateParks`): 13-20 s of inline log gate per
   `cores = 4`, `relaxed` cell, on pages other cores re-dirty faster than a
   `relaxed` sync lands.
3. **Under `relaxed` a run is long**: 3.5-6.2 s at p50 at `cores = 1`
   against 0.66-0.83 s, so the anchor moves less often (worst gap 11-16 s
   against 6 s). Under `group` runs are unchanged in length and count.

BI-Q10's bar (i), the checkpoint's own holds under 10 ms, is therefore not
met; (ii) is met in 10 of 12 cells.

## The fix

Not designed. Item 1 needs the page writes off the reactor - a scope W2 did
not give BI. Item 2 needs either a deferred run written from the image it
already copied once its LSN is durable, with the frame's recLSN moved to that
image's `page_lsn` (`[quiet-wrong]`: a recLSN set too high lets the redo start
pass a change), or a bound other than an inline wait. Item 3 follows from 2.
