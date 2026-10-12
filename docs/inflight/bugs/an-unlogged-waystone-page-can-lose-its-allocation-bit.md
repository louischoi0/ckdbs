# An unlogged Waystone page can lose its allocation bit at a crash

**Found by reading, not reproduced.** Verified at `27f8a0ae` on
`br-backup-restore`, by the `critics-developer` review of BR-S0's census
(`instructions/v3.0.0/br-s0-census.md` §3 item 1).

## What is wrong

Waystone directory and trail pages are created with no log record
(`src/stats/waystone_dir.cpp:47`, `:104`; `src/stats/trail_store.cpp:126`),
so redo never recreates them and analysis's highest page id does not count
them (`src/wal/high_water.cpp:35`). Their allocation bit reaches the device
only with a map write (`DevicePageStore::FlushMaps`), which a checkpoint step
skips (BI-Q12) and the publish performs.

A crash after a Waystone parent page (or a `sys.patterns` row naming a
root) reached the device, but before the map write carrying its child's bit,
leaves the child's id free on the restarted volume while the parent still
names it. The cursor can then hand the id to a logged relation page, and the
next Waystone write through the parent formats or overwrites that page with
no record.

## What it costs

A **quiet wrong answer**: rows on the overwritten relation page are lost,
and invariant 8 (deleting Waystone may cost performance, never change a
result) is broken in the opposite direction - Waystone changes a result by
writing where it should not.

## The fix, not scheduled

Either a Waystone page's creation joins the log (an allocation record, which
the free map's unlogged design refuses today), or a mount treats every
Waystone root as advisory garbage - invariant 8 allows dropping them whole
after a crash. BR closes the backup form of the hole by copying the maps
last (`br-s0-census.md` §3 item 1); the crash form needs its own order.
