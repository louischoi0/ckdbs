# Two writebacks of one page can land the older image last, and the frame reads clean

**Found** 2026-09-25 by the `critics-developer` pass on AT-S10b (`59ed9c0`
on `at-s10-ring-consumers`), as its C1, and confirmed by reading
`DevicePageStore::WriteBack` (`src/storage/device_page_store.cpp`) on the
same branch at `879b15b`. **Not fixed.** No cell reproduces it.

**Cost: a lost committed update with no crash** - the one kind of loss the
WAL does not answer, because nothing asks for a replay.

## The wrong behaviour

Since AT-S8 step 1b a writeback copies each page under its page latch,
records the frame's dirty generation with the copy, writes the copies after
the WAL gate, and cleans a frame only if its generation still equals the
one the copy saw. Nothing marks a frame as being written, so two writebacks
of one frame run unordered:

1. Flush B copies page P at generation g1, then waits at the WAL gate.
2. A writer on another core mutates P (g2); flush A copies it, writes it
   and cleans the frame - the generation matches its copy.
3. B's write of the g1 image reaches the device **after** A's.
4. B's clean sees the generation moved and leaves the frame alone - but A
   already cleaned it.

The device now holds g1, the frame is clean, and a clean frame is never
written again: a clean shutdown's checkpoint anchor moves past the lost
change, and a clean eviction re-reads the older page.

## Reach

Any two concurrent writebacks of one page. Since AT-S8 a checkpoint runs on
any core (one at a time), and since AT-S10b every core's transaction-id
carve runs `DevicePageStore::Sync()` - a whole-pool flush - from its own
thread through `Expeditor::PersistTrxIdCeiling`, once per 4,096
transactions per core and at each idle burn. So a carve on one core and a
checkpoint, or another core's carve, can overlap. Page 0 itself is mostly
covered: the mount raises the ceiling to what the log names.

## The fix, known and unscheduled

Mark a frame as being written at the copy, under the structure latch, and
have a second writeback of it wait (`kWait`) or skip (`kSkip`), with a
barrier-forced cell. Separately, a carve needs page 0 durable, not the pool
flushed; narrowing `PersistTrxIdCeiling` shrinks the reach but does not
close the ordering. Owner: `docs/spec/page.md` §6 and the writeback's own
header.
