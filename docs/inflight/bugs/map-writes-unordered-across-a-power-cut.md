# The free map can outlive the headerless map across a power cut

**The wrong behaviour.** `DevicePageStore::FlushMaps`
(`src/storage/device_page_store.cpp`) writes a region's headerless map and
then its free map, and its comment rests the pair's safety on that order: the
free map is what makes a page id exist, so a crash between the two leaves a
headerless bit for an id nothing allocated. **No sync stands between the two
writes**, and a disk is free to persist either one of two unsynced writes
without the other. A power cut that keeps the free map and loses the
headerless map publishes an allocated headerless page - a Waystone page, which
carries no checksum - whose headerless bit never landed: the next read
verifies a checksum that was never written and refuses a page that is intact.
The order the comment relies on holds only for a device that keeps writes in
issue order, which neither a file nor `MemoryPageDevice::CrashPersisting` is.

**Verified at** `5d58f3ca` (`v3.3.0-77-g5d58f3ca`) with BS-S4's power loss, on
`bs-test-hardening`: the sim's integrity sweep finds the region's headerless
map page allocated and all zero, and a Waystone page (135) failing its
checksum.

Re-verified by source read at `7dd9b789`: `FlushMaps`
(`src/storage/device_page_store.cpp:418`) still writes the headerless map
then the free map with no sync between.

**Smallest reproduction.** The minimized case beside this file:
`build-bs-noassert/ckdbs-sim --replay docs/inflight/bugs/map-writes-unordered-across-a-power-cut.sim`
(found as `--seed 13 --ops 1500 --iterations 40 --mode sync-crash
--fail-nth power-loss-at-page-write:7`, iteration 35). The committed seeds'
nightly sweep does not reach it.

**What it costs.** A refusal, never a wrong answer: after such a cut every
read of the page is `Corruption`, and the integrity sweep reports it. The page
is advisory (invariant 8), so what is lost is a trail, but a read that meets
it is refused rather than falling through.

**The fix, known and unscheduled.** A data-file sync between the two writes
of a region whose headerless map changed. The checkpoint calls `WriteMaps`
from a coroutine whose data syncs park on the data-sync thread (BI), so the
barrier cannot be a blocking sync inside `FlushMaps`: the map write splits
into two phases with the syncer between them, a change to the checkpointer
and the store's checkpoint-target interface. BS-R2 keeps BS from changing
engine behaviour; the fix is an order of its own.
