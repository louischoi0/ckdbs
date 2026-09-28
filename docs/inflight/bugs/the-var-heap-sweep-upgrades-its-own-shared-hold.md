# The var-heap sweep asks for a page exclusive under its own shared hold

**Found by reading, not reproduced.** Verified at `8f9a887` on
`at-s21-log-under-hold`, by AT-S21's survey.

## What is wrong

`exec::SweepChain` (`src/exec/varheap_sweep.cpp`) walks a relation's
var-heap chain holding each page **shared** (`store.GetForRead(page_id)`),
and for an unreferenced slot calls `txn::ReleaseVarHeapSlot`, which asks for
the **same page exclusive** (`store.Get(page_id)`). The page latch is never
upgraded (`page_latch.hpp`): an exclusive request against this core's own
shared hold is the self-deadlock the store diagnoses - a debug abort naming
the page, a hang in release.

The sweep runs at mount (`expeditor.cpp`, `SweepUnownedSpills`) **after**
the latch is armed (`SetLatchArmed(core_count > 1, ...)`), so it is reached
at `cores > 1` whenever a crash left an unreferenced var-heap value.

## Cost

A mount that hangs (release) or aborts (debug) at `cores > 1` after a crash
that left an orphaned spill - which the var-heap's own recovery story
creates by design (a value logged ahead of a tuple that was not).

## The fix

Release the shared hold before the release call and re-take the page per
slot, or collect the page's unreferenced slots under the shared hold and
release them after it drops. Unscheduled.
