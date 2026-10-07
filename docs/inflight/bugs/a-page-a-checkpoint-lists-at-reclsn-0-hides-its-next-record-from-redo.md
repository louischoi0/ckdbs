# A page a checkpoint lists at recLSN 0 hides its next record from redo

## What is wrong

Analysis rebuilds the dirty-page table and recomputes the redo start from it
(`src/wal/analysis.cpp`). Two rules combine badly:

- A page record enters the table with `dirty_pages.emplace(page_id, lsn)`
  (`src/wal/analysis.cpp:131`). `emplace` keeps an existing entry.
- A `CHECKPOINT_BEGIN` seeds every page it lists (`:207-208`), and a page
  dirtied by a path that logged nothing is listed at recLSN 0
  (`include/kds/storage/page_store_checkpoint_target.hpp:23-28`).
  `RedoStartFrom` skips 0 (`:23-31`).

So a page seeded at 0 stays at 0 when its next record arrives, and that
record never pulls the redo start down. If no other entry is at or below it,
redo starts past the record. Redo then never reads it, and the page on disk,
written back by the checkpoint before the record, never receives it. Redo
itself would apply the record: an entry of 0 admits every LSN
(`src/wal/redo.cpp:313-316`). The loss is the scan's start.

**The engine reaches the precondition at every mount.** Recovery's redo
dirties the pages it replays without a recLSN, and the mount's completion
checkpoint lists them all at 0. Read from the log of
`WalRecyclePremiseRigTest.ARowCommittedAfterAMountSurvivesTheNextCrash`
(`tests/wal_recycle_premise_test.cpp`):

```
BEGIN 5768: 0@0 4@0 5@0 6@0 7@0 8@0 9@0 11@0 12@0 13@0 14@0 129@0 130@0 131@0
6048 HEAP_OVERWRITE page 7      <- the insert's sys.tables bump: seeded at 0
6208 PAGE_INIT page 132         <- the first page not seeded: redo starts here
6264 UNDO_WRITE page 132
6344 HEAP_INSERT page 129
```

A crash after this, before the next checkpoint, recovers from 5768. Redo
starts at 6208, and the bump at 6048 is not replayed.

Verified at `7dc36da8` on `worktree-wal-recycling`, 2026-10-07, while
writing BC-S1 (`instructions/v3.0.0/workorder-bc-wal-recycling.md` §1.11).
Found by BC-S0's `critics-developer` review reading `analysis.cpp:131`;
reproduced at the analysis level by a scripted log; the precondition and the
skipped record observed through production's mount.

## Smallest reproduction

`WalRecyclePremiseTest.DISABLED_APageSeededAtRecLsnZeroStillHasItsLaterRecordReplayed`
(`tests/wal_recycle_premise_test.cpp`), red at `7dc36da8`. Run it with
`--gtest_also_run_disabled_tests`. In a scripted log:

1. A checkpoint writes page P back whole.
2. A second checkpoint lists P at recLSN 0 and writes it back.
3. P's next record follows the second checkpoint's BEGIN, and the crash
   comes before any further checkpoint.
4. Analysis from the second checkpoint returns a redo start past that
   record, and redo leaves P without it.

## What it costs

**A committed record that redo does not replay. This is a quiet loss, but no
client-visible wrong answer has been reproduced.**

- **What is at risk.** Records logged after a mount, onto pages that mount
  redid, before the first record of the run lands on a page that mount did
  not redo.
  - A transaction's first undo record usually goes to a fresh undo page,
    because a previous run's undo pages are not reused, so it ends the
    window.
  - What precedes it in an `INSERT` is exposed: the `sys.tables` bump, and
    per `wal.md` §11a each secondary index's `INDEX_INSERT` and each spill's
    records.
- **Observed.** The `sys.tables` bump is skipped. The high-water repair
  restores the mark from the replayed row, so `DESCRIBE`'s `next_id` and
  the next issued id came out right. The same cell's index probe still found
  the row: the `INDEX_INSERT` was not in the window on that run.
- **Not established.** Whether any path loses a record that nothing
  re-derives: an index entry, a var-heap value, or a catalog row.
- **Two crashes are needed.** One crash creates the precondition at its
  mount, and a second crash, before the next checkpoint, exposes it.

## The fix

Not decided. Two shapes:

- **In analysis:** a page record overwrites a seeded 0 with its LSN, so a
  0-seeded page takes the LSN of its first record in range. This is local,
  and it is what `RedoStartFrom` already assumes every entry means.
- **At the source:** recovery's redo stamps a recLSN on the frames it
  dirties, so the completion checkpoint lists no logged page at 0. This is
  wider, because it changes what the checkpoint says about every redone
  page.

Either fix turns the disabled cell green, and its `DISABLED_` prefix comes
off with the fix.
