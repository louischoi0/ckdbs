# A synchronous anchor publish writes page 0 before the sync that covers its pages

**Found by reading, not reproduced.** Verified at `27f8a0ae` on
`br-backup-restore`, by the `critics-developer` review of BR-S1.

## What is wrong

The cadence checkpoint's publish (BI-Q5 (a), `src/wal/checkpointer.cpp`)
syncs every page the anchor stands for first, then encodes, writes and
syncs page 0. The synchronous publish - `RunToCompletion` →
`SuperBlockCheckpointAnchor::Publish`
(`src/server/superblock_checkpoint_anchor.cpp`), the mount's completion
checkpoint and a shutdown's - encodes page 0 into its frame and then runs
one whole-store `Sync`, which writes page 0 beside the remaining dirty pages
under one `fsync`.

If the device drops a data page in that write (an `EIO` the `fsync`
reports) while page 0 reaches the platter, the next mount reads an anchor
that stands for a page the device does not hold. The failed sync stops the
instance (`storage/data_syncer.hpp`, sticky), so nothing runs on; the
restart trusts the anchor.

## What it costs

A **quiet wrong answer** after a device fault during a mount's or a
shutdown's checkpoint: redo starts past records the dropped page needed.
Since BR the same write discharges a restore (`restore_backup_id` cleared
with the anchor), so a restore whose completion checkpoint meets that fault
is discharged over an unrepaired page.

## The fix, not scheduled

`Publish` syncs the store before it encodes, then writes and syncs page 0
alone - the cadence path's order. BR-S1 tried exactly that and withdrew it:
`SuperBlockFoldTest.AFailedSyncKeepsTheCoresContributionForTheNextPublish`
pins that a failed publish still records its core's contribution to the
fold, which a sync failing before `EncodeLanding` would not. The fold's
failure semantics and the publish order are the checkpoint owner's to rule
on together; BR changes neither.
