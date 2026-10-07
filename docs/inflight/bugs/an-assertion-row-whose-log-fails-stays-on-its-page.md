# An assertion row whose log fails stays on its page

**Found by reading, not reproduced.** Verified at `38bb8a8` on
`worktree-az-s2-peer-create-assertion`, by the review of AZ-S2
(`instructions/v3.0.0/workorder-az-ay-carry-forward.md` §6).

## What is wrong

`InsertAssertion` (`src/exec/assertion_catalog.cpp`) places the
`sys.assertions` row with `heap::ChainInsert` under `kBootstrapXid`, then
logs it with `LogChainInsert`. If the log call fails, the row stays on the
page. `CreateAssertion` then evicts the directory and emits `ASSERT_DROP`
and the statement answers `ERR`.

AZ-S1 closed the same shape for the catalog's own `InsertRow`
(`ReportPlacedRow`); this path does not go through it.

## What it costs

**Narrowed by BC-S4's fail-stop** (`docs/spec/wal.md` §6-5, re-read on
`worktree-wal-recycling` on 2026-10-07). The failed log call stops the log, so
the running instance refuses every write after it - the unchecked writes below
cannot happen - and no page is written back after the stop. What is left is a
window *before* the stop: another core's writeback that copies the page
between the row's placement and its failed append. The page's LSN has not
moved, so the WAL gate passes it, and the restart then revives the row. The
two costs below are as they were before BC-S4, now reachable only through that
window.

- **On the running instance**: `ListAssertions` returns the row, so the name
  is taken and `SHOW ASSERTIONS` lists it, but the registry holds neither a
  live directory nor an unenforceable record for it. The relation's writes
  are admitted unchecked, and nothing says so.
- **After a restart**: the row is revived. If the create's own publish run
  is in the scan's range, the assertion enforces from a base that misses
  every write admitted unchecked since - an under-count, so a quiet wrong
  answer.

Reached only by a failed WAL append.

## Reproduction

Not reproduced. AZ-S1's technique applies: fill the ring and fail its
drain (`catalog_test.cpp`'s log-refused cell), here at the assertion row's
append.

## The fix

The row taken back or retired on the log's failure, as `ReportPlacedRow`
does for `InsertRow`. AZ-S1's dead-slot question, which this would have
carried, is closed by fail-stop for a page held across the placement and
the append (`docs/spec/wal.md` §6-5); this path releases its page in between,
so the fix also has to log the row under the hold that placed it.
