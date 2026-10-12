# A checkpoint whose dirty table exceeds the log's ring is refused

**Found by measurement.** BR-S10 (`bench/v3.0.0/results-br-close-backup-restore-v2.7.0-913-ga8110b05.md`
§M3), on `br-backup-restore` at `a8110b05`; the mount's half fixed in the
commit after it, the cadence's half not.

## What is wrong

A `CHECKPOINT_BEGIN` carries the whole dirty-page table, 12 bytes a page
(`wal/payload.hpp` `kDirtyEntrySize`), and a record may not exceed the WAL
ring (`kDefaultRingCapacity`, 1 MiB; `WalStream::AppendLocked` refuses
"record of N bytes is larger than the ring"). About 87,000 dirty pages fill
it. BR-S10 met it at a mount: after a bulk load at `cores = 4`, a `kill -9`
left a redo window that dirtied more pages than that, and the pre-BR build
(`fb953f07`) refused the mount with "completion checkpoint: record 1381352
larger than the ring"; a restore of a 3.5 GiB backup window failed the same
way.

**The mount's completion checkpoint is fixed** (BR, `CheckpointAfterRecovery`):
nothing else runs at a mount, so it writes every dirty page first and begins
with an empty table (`MountRecoveryTest.AReplayThatDirtiedMoreThanTheRingHoldsStillCheckpoints`).

**The cadence's is not**: a running instance whose pool holds more dirty
pages than one record carries - `buffer_pool_frames` above about 87,000 with
a write-heavy load - has every cadence checkpoint refused at its
`CHECKPOINT_BEGIN`, so the anchor stops advancing and the log stops being
recycled.

## What it costs

A **refusal**, never a wrong answer: no anchor is published over a table it
could not log. The cost is an anchor that does not advance - a growing log
and a longer recovery - until the dirty set shrinks.

## The fix, not scheduled

A dirty table split across records (`CHECKPOINT_BEGIN` and continuation
records analysis folds), or one capped and the redo start taken from the
oldest page left out. Either is a WAL format question for the checkpoint's
owner (`wal.md` §11).
