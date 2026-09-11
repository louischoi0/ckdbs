# A chunked assertion snapshot can be split by another core's record, and recovery drops the rest

**Found** 2026-09-11 by the `critics-developer` pass on AT-S5d (`3d58041` on
`m3-at`). **Not fixed**, and not this stage's: it has been reachable since
AR0 M0 made the WAL one stream for the instance. Verified by source read at
`3d58041`.

**Cost: a quiet wrong answer**, and only for a big cabin. The groups in the
chunks recovery skips come back with no base, so their aggregates restart
from the records after the snapshot and understate every row already in
them; later admissions fit rows the constraint forbids.

## The wrong behaviour

`wal::LogAssertionSnapshot` (`src/wal/checkpointer.cpp`) writes one cabin's
group headers as several `ASSERT_SNAPSHOT` records when they do not fit in
one - chunked so no record outgrows a segment. Recovery
(`src/exec/assertion_recover.cpp`, `visit`) treats **the first record that
is not a snapshot** as the end of every open base, closes it, and from then
on skips any snapshot of that assertion as "a later checkpoint's"
(`context.based(id)`). Its own comment states the premise: *"nothing else
can land between"* a cabin's chunks.

Under one stream that is false. The registry's directory latch (AT-S5d)
keeps `ASSERT_*` records out of the gap, but a heap insert, an undo record
or a commit from any other core takes no such latch and can land between two
chunks. Recovery then closes the base after the first chunk and skips the
rest.

## Reproduction

Not reproduced. It needs a cabin with enough groups to chunk - more than one
record's payload of `{group_id, key, count, sum}` - checkpointed while
another core writes, then a crash and a mount.

## The fix, and whose it is

Either close a base only on the next `ASSERT_*` record that is not a
snapshot or on a `CHECKPOINT_END` rather than on any record, or make a
snapshot carry its chunk count so recovery knows when the base is whole.
Both are changes to AS6a's recovery contract (`assertion.md` §7), so this
waits for a stage that owns it; no work order names one yet.
