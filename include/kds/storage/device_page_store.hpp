#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>
#include <functional>
#include <utility>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/base/log.hpp"
#include "kds/storage/data_syncer.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/page_device.hpp"
#include "kds/storage/page_latch.hpp"
#include "kds/base/current_core.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/durability.hpp"

// The disk-backed PageStore: the same three-operation contract catalog,
// bootstrap and the dispatcher already speak, served out of a PageDevice.
// On a FilePageDevice a database survives a restart; on a MemoryPageDevice
// the identical code runs under the simulator with fault injection.
//
// Two durable facts make that work, and this class owns both: which page
// ids exist (a free-map bitmap page at kFreeMapPageId, so Get() still
// answers NotFound after a reopen - which is what bootstrap reads to
// decide whether to run Catalog::Bootstrap()), and which page bytes are
// current (resident frames, written back by Flush()).
//
// Every *headered* page it writes is stamped with a CRC32C over the common
// page header (page.md sections 8 and 10) and verified when it is read back
// on a miss - never on a hit. Corruption is therefore detected at load; the
// FULL_PAGE_IMAGE that heals it is the WAL's job (wal.md section 10).
//
// ---- Headerless pages ---------------------------------------------------
//
// A page class whose payload tiles 8 KiB exactly - an array of fixed-size
// entries sized to a power of two, addressed by shift and mask - has no
// room for the common header, and no caller wants one: a header would cost
// an entry and break the addressing that is the point of such a layout.
// Stamping a checksum into one at offset 4 would overwrite data, and
// verifying one on read would reject it.
//
// CreateNewHeaderless() marks a page as such, and the mark is **durable**,
// in a second bitmap page of the same shape as the free map
// (kHeaderlessMapPageId). It cannot be an in-memory side table: the pool
// evicts (BE), so a page is read off the device on every miss, the first
// one right after open, and every such read must know whether to verify.
// It cannot be recomputed from the catalog either, because the store is
// opened before bootstrap has a catalog to ask.
//
// These pages carry no damage detection, so the mechanism suits only a
// structure whose corruption is survivable. One class qualifies: the
// Waystone directory's interior pages (stats/waystone_dir.hpp), whose 2048
// child ids tile the page and whose damage costs a lookup that falls
// through to the authoritative path. A database with no Waystone directory
// pays one reserved id per region and nothing else.
//
// **A bounded pool** (eviction.md, BE): frames are slots in chunks that
// never move, at most `buffer_pool_frames` of them, reclaimed by a CLOCK hand
// in bounded batches; a full pool refuses a fill `ResourceExhausted`, only
// outside a mutation's no-refuse window (page_store.hpp). A freed page's
// frame is discarded by `FreePage` (BF-R5), dirty or not. The sections
// below say what each lock protects.
//
// Not here, deliberately:
//   - Dirty tracking is by which accessor the caller chose, not by what it
//     wrote: Get() marks the frame dirty, GetForRead() does not, and both
//     hand out a mutable span. A writer that calls GetForRead() loses its
//     write.
//   - The free map is region-based (page.md section 5); beyond its ceiling
//     is OutOfRange, not silently unmapped.
//
// ---- The WAL gate (page.md section 8, wal.md section 8-1) ---------------
//
// A dirty page may reach the device only once the log records describing
// its modifications are durable. Enforced here, not asked of callers:
// every write path (WriteBack, so Flush, Sync, FlushPages) first calls
// EnsureDurable() on the highest page_lsn of the batch - one call per
// batch, a no-op once the watermark has passed. With no gate installed the
// store writes ungated, which the WAL-free unit tests rely on and which is
// sound only for a caller that logs nothing.
//
// **Which gate: the calling core's** (BA-S1). SetWalGate() installs the
// default, SetCoreWalGate() one core's own; a writeback asks the gate of
// CurrentCore(), or the default. Every manager answers for the one log,
// but a manager's sync writes its own core's state (wal/manager.hpp), so a
// peer registers its attached manager (flush through the stream latch,
// wait on the writer) and core 0 keeps the default, its owning manager.
//
// StampPageLsn() is the other half. A mutation path appends its record,
// then calls it with the record's LSN; that both records the page_lsn the
// gate reads and captures the frame's **recLSN** - the LSN of the first
// record to dirty the frame since it was last clean, which is what a
// checkpoint's dirty table must carry for recovery's redo start to be
// correct (wal.md section 11-1). A frame keeps that value until it is
// written back, then drops it.
//
// Concurrency: one store serves every core. Four latches, all armed only
// at `cores > 1` and null otherwise: the page latch word per frame (below),
// the structure latch over the frame table (`frames_latch_`), the free
// map's latch (`map_latch_`) and the map write barrier
// (`map_flush_latch_`). Each says at its declaration what it guards.
//
// ---- The page latch (AM-R3, AR2-R2) ---------------------------------------
//
// Every frame carries one 32-bit word (`Frame::latch`, page_latch.hpp): an
// exclusive bit, the owning core, and a count. **Armed only at `cores > 1`**
// (SetLatchArmed, from the superblock's core_count); unarmed - `cores = 1`,
// every hand-built store, the simulator - the accessors never touch the
// word, G2's zero overhead as a property of the code. In debug builds
// `KDS_TEST_PAGE_LATCH=1` arms every store so the whole suite runs armed.
//
// What it protects: a frame against a concurrent reader or writer on
// another core, and against the writeback and sweep paths; writeback
// copies each page under its latch (AT-S8 step 1b).
//
// Taken where the pin is taken and released where it is released: a
// PageRef holds both. Get and the Create* accessors take it exclusive,
// GetForRead shared. **Re-entrant for the owning core** - one task holds a
// page twice on every chain-growth and split path (heap_chain.cpp's
// re-fetch after CreateNew, btree.cpp's rebuild, LogFullPageImage under
// its caller's handle) - and the owning core stands for the running task
// on the discipline that a core runs one task at a time and no task parks
// holding a pin. A discipline, not a gate: exec::InstallSuspendAudit
// records a violation in debug builds and is not fatal, and a park under a
// latch would be a silent second exclusive grant to the next task on this
// core. **Never upgraded**: a task holding a page shared may not ask for
// it exclusive. That is a self-deadlock - aborted in debug naming the page
// (PinFrame's thread-local multiset of the pages this thread holds
// shared, AM-S2 step 3b), an unbounded spin on the reactor thread in
// release (base/latch.hpp).
//
// Acquisition order (rules.md section 3's row):
//   - **Outer to the WAL stream latch.** A task appends while holding its
//     page latches - LogFullPageImage takes the page again under the handle
//     its caller already holds, catalog and Bound Cabin chain growth append
//     under the old tail's - and **no WAL path asks for a page latch while
//     holding the stream latch**, so no cycle exists. The stream latch is
//     held only inside WalStream's Append/Seal/Flush/Sync, none of which
//     can see a PageStore. wal/ *does* take page latches in one place -
//     recovery's redo (`wal/redo.cpp`'s Get/CreateAt), on a store this
//     mount already armed - and that is on the mount thread with no stream
//     latch held and no second thread alive. The cost accepted: a page
//     latch can be held across an append's section, a segment roll
//     included; a reader of that page elsewhere spins, then yields.
//   - **Held across a durability wait by no path in the tree** (BB-S1's
//     census). WriteBack takes the WAL gate (EnsureDurable: a wait on the
//     writer thread for a peer, an inline sync for core 0) before it writes
//     any byte. It takes each page's latch shared for the copy (AT-S8 step
//     1b), one page at a time and released before the gate, so no frame of
//     its own is latched across the wait - waiting for a foreign exclusive
//     holder on a flush, trying once and skipping in the background drain
//     (`WriteBack`'s `HeldFrames`). An ordinary fault does not reach
//     WriteBack: its sweep (`SweepLocked`, at a fill's reservation) only
//     queues a dirty frame, and every WriteBack caller holds no page latch.
//     **Drain mode is the one fault path that does** (BE-Q11: recovery, a
//     rollback, the assertion loops), in `kSkip` and only on a thread
//     holding no pin: a shared try re-enters this core's own exclusive
//     hold (`page_latch.hpp`'s `Next`), so a writeback from a thread
//     holding one could copy a page mid-write.
//     `HighestDirtyPageLsn` reads the batch's page_lsns under the structure
//     latch and drops it before EnsureDurable, so that scan holds nothing
//     across the wait either.
//   - **The writeback claim** (AT-S10e, `Frame::writing`): taken under
//     the structure latch before the copy's page latch, held across the
//     WAL gate and the device write, given back at the clean. A writeback
//     waits on another's claim holding no claim of its own and no page
//     latch, so a claim's holder never waits on its waiter - provided no
//     flush caller holds a page latch across the flush (`WriteBack`'s
//     `kWait` note).
//   - **Outer to the visibility window latch, which is a leaf** (BB-S1's
//     census): it is taken by a visibility read, by undo growth's
//     reclaim under whatever page the write holds, and by the delete-mark
//     purge under a catalog page; nothing holding it asks for a page
//     latch, and no path holds a PageRef at commit.
//   - **Never across a park**: the suspend audit's `live_pins() != 0`
//     covers it in debug builds, recording rather than failing, because
//     the pin and the latch share a handle; nothing covers it in release.
//   - **Page against page: the tree's own shapes are the order.** Two
//     page latches are held at once on every split (the old leaf and its
//     new sibling), every catalog chain growth (the old tail and the new
//     page), a split's parents (a leaf then its parents, bottom-up,
//     `SecureParents`, AT-S16), and a scan ring's shared hold on a leaf
//     across its consumer's var-heap fetches (AM-R8: `S(leaf)` then
//     `S(var-heap page)`, the Cabin build's phase 2). The order: old before new, leaf before parent, a leaf
//     before its right neighbour. A descent holds one at a time. Since the
//     owner-core invariant retired (AT-S5, AT-S7; history at cf889b63) any
//     core may take any pair, so an ABBA is possible against a path that
//     breaks the order. A write's `WHERE` subquery, which reads leaves on
//     either side, was one (BB-S1's census) and runs under no exclusive
//     hold since: the write collects its targets first (`parser-v2.md`
//     R2). One cross-relation pair stays: a parent `DELETE`'s reverse
//     foreign-key walk reads the child's leaves under the parent leaf's
//     `X`, sound because the key graph is acyclic and no child-side hold
//     asks the parent (`rules.md` §3's page latch row). Starvation is not
//     ruled out: shared is granted
//     whenever X is clear, so a hot page's readers can starve an exclusive
//     request.
//   - **A user relation page, then a `sys.tables` chain page** (BB-R4),
//     stated beside the two cross-relation pairs declared where they are
//     taken (a relation page before a Bound Cabin page, `assertion.md`
//     §6.1; page 9 before page 7, `rules.md` §3's catalog row). `page.md`
//     §6 is its home: who takes it, BB-R4's four levels, and the census.
//
// Waits spin with a pause hint, then yield; there is no queue and no
// writer preference. A holder is in a critical section measured in
// nanoseconds, or in an append.
//
// Logging (component tag "pagestore"): allocation and write-back are the
// two things that change what is on disk, so both are logged - allocation
// and per-page write-back at Trace (one line per page), batch write-back
// and sync at Debug, and every device-level failure at Error. A failed
// checksum verify is Error and says "corruption" in as many words: it is
// the one thing this class can detect that no caller can diagnose from a
// Status alone, and it names damage rather than a policy refusal.

namespace kds::storage {

// Fixed home of the free-map page, in the reserved sub-128 system range
// alongside the superblock (0) and the catalog's fixed pages (4-15).
inline constexpr PageId kFreeMapPageId = 1;

// The headerless bitmap (PageType::kHeaderlessMap), immediately after it
// in the same reserved sub-128 range. One bit per page id: set means the
// page carries no common header, so it is neither checksum-stamped on the
// way out nor verified on the way in.
inline constexpr PageId kHeaderlessMapPageId = 2;

// Region 0's pair, by the placement arithmetic in free_map.hpp. These two
// names stay because they read better at their call sites than
// FreeMapPageIdFor(0) does, but they are no longer independent facts: if
// D1's arithmetic ever moves, this is where the engine finds out.
static_assert(kFreeMapPageId == FreeMapPageIdFor(0));
static_assert(kHeaderlessMapPageId == HeaderlessMapPageIdFor(0));

// How many frames a pool may hold - the store's one size, required at
// `Open` (BE-R3). A type of its own so a call that names only a first page
// id, as every `Open` did before the capacity was required, does not compile
// into a pool of that many frames.
struct FrameCapacity {
    std::size_t frames = 0;
};

class DevicePageStore final : public PageStore {
public:
    // `device` must outlive the store. A device with no pages, or one whose
    // free-map page was never written, is initialized as a fresh database;
    // otherwise the existing free map is loaded, and a bad checksum or
    // header is Corruption rather than a guess.
    //
    // `first_new_page_id` is where CreateNew() starts looking; pick a value
    // above any id that gets CreateAt'ed.
    //
    // `capacity` is the pool's ceiling: resident frames, fills in flight
    // included, never exceed it (BE-R1, BE-R3). Zero is `InvalidArgument` -
    // there is no unbounded pool.
    static StatusOr<std::unique_ptr<DevicePageStore>> Open(PageDevice& device,
                                                           FrameCapacity capacity,
                                                           PageId first_new_page_id = 1);

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override;
    // Takes an allocated id whose device bytes fail their checksum too
    // (redo's heal, wal.md §10); a page that verifies is still in use.
    StatusOr<std::span<std::byte, kPageSize>> CreateOverTornUnpinned(PageId page_id) override;
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override;
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override;

    // `PageStore::ClaimNew`'s halves: a claim reserves the frame slot and
    // claims the id with its device room, a create publishes it, and a
    // release gives both back (`claimed_`).
    StatusOr<PageId> ClaimNewId() override;
    StatusOr<std::span<std::byte, kPageSize>> CreateClaimedUnpinned(PageId page_id) override;
    void ReleaseClaimedId(PageId page_id) noexcept override;

    // ---- Recovery's high-water repair (RV4, workplan RC04) --------------
    //
    // Raises where CreateNew() starts its search (page_store.hpp), for every
    // core - there is one free map. **It sets no free-map bit**: the map
    // records which ids exist, and marking one no page was written at would
    // turn Get()'s NotFound into a read of whatever the device holds. The
    // floor is the separate fact "do not hand this out".
    Status RaiseAllocationFloor(PageId first_allocatable_page_id) override;

    // Get() that leaves the frame clean (page_store.hpp). Without it a
    // read-only statement dirties every page it touches and the next
    // checkpoint writes the whole working set back with nothing changed.
    StatusOr<std::span<std::byte, kPageSize>> GetForReadUnpinned(PageId page_id) override;

    // CreateNew() for a page that will carry no common header - see the
    // note above. The whole 8 KiB is the caller's, and this store will
    // neither stamp nor verify a checksum on it, now or after a reopen.
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewHeaderlessUnpinned() override;

    // Whether `page_id` was created headerless. False for an id that does
    // not exist, which is the safe answer: an unknown page is treated as
    // headered and therefore verified.
    //
    // **Takes the free map's latch** (AM-S3): the headerless bitmap is a
    // `unique_ptr` installed lazily (FM6), so this reads a pointer another
    // core may be storing to.
    bool IsHeaderless(PageId page_id) const noexcept;

    // Writes dirty frames back in page-id order, which is file order
    // (page.md section 13), then the free map. Data pages go first so a
    // crash between them can only orphan a page a set bit names, never
    // publish one whose bytes never landed. A bit `FreePage` cleared needs
    // no order: no replay names the id any more (BF-R4), and its frame is
    // gone. Not durable on its own - Sync() adds the fsync.
    Status Flush();
    Status Sync() override;

    // The dirty map regions written back, then the device synced - the maps
    // alone, not the frames. **The map sync a reclaim orders its frees by**
    // (BF-R7): behind the map write barrier (`map_flush_latch_`), a call that
    // returns has written and synced every region state copied before it
    // began, on any core. It is also the narrower operation a cell uses to
    // construct "the claim is durable and the page is not" - the state
    // `FaultIn`'s all-zero arm answers `NotFound` for.
    Status PersistMaps();

    // ---- The writeback primitive (docs/spec/eviction.md §4, EVT03) ------
    //
    // Durable → checksum → write → clean, for exactly these ids, skipping
    // any that are non-resident or already clean. **The single code path**
    // §4 requires: Flush(), FlushPages() (the checkpointer's route) and
    // the dirty-queue drain all run through here, so the checkpointer is a
    // consumer of the writeback machinery rather than a parallel
    // implementation. Neither syncs the device nor touches the maps - the
    // callers own those, because when they happen is what distinguishes a
    // checkpoint from a background drain.
    //
    // Ascending contiguous runs are coalesced into one WritePageRun of at
    // most kWritebackRunPages, through a bounded copy into scratch -
    // best-effort, never a correctness property (§4). The copy is what
    // freezes the bytes and their page_lsn together (AM-S3), and it would
    // be needed anyway: consecutive page ids sit in unrelated slots.
    //
    // Each page is copied under its page latch, shared (AT-S8, step 1b), so
    // what goes out is never half of another core's write, and each frame is
    // claimed from its copy to its clean (AT-S10e, `Frame::writing`), so two
    // writebacks of one page land in copy order. `held` says what to do
    // about a frame another core holds exclusive or another writeback has
    // claimed: `kWait` waits out the writer or the claim - both running,
    // since no task parks holding a pin and a claim's holder waits on no
    // claim - and is what a flush owes, `Sync()` being a durability barrier;
    // `kSkip` leaves the frame to its holder, and is the background drain's
    // (`DrainDirtyEvictionQueue`, on core 0's cadence), which owes no
    // barrier and so has no reason to wait.
    //
    // **What `kWait` rests on, unenforced**: no flush caller holds a page
    // latch across the flush. Holding X(Q) while waiting on a writer of P
    // that wants Q is a cycle, and so is holding X(P) - another writeback can
    // claim P and wait on that latch while this flush waits on the claim.
    // Every caller releases first (the anchor publish, `PersistSuperBlock`,
    // the carve's persist, the checkpointer, SYNC), and "no task parks
    // holding a pin" - audited in debug only - keeps the awaited writer
    // running.
    //
    // Returns how many pages it wrote.
    //
    // **`defer`, when given, turns both log gates into a return** (BI-S3,
    // BI-Q4 (a)): where the batch or a run needs the log durable past an LSN
    // it is not yet, the writeback stops before writing that run, gives back
    // its claims with the frames still dirty, stores the LSN in `*defer` and
    // returns what it wrote before. A checkpoint task parks on that LSN and
    // asks again; nothing is written ahead of its record either way.
    enum class HeldFrames : std::uint8_t { kWait, kSkip };
    StatusOr<std::size_t> WriteBack(std::span<const PageId> page_ids,
                                    HeldFrames held = HeldFrames::kWait,
                                    wal::Lsn* defer = nullptr);

    // Pages one coalesced run may span, and so the scratch bound: 8 pages
    // = 64 KiB, chosen as the largest single write the background task
    // should hold the core for under run-to-completion. `[PROPOSED]` -
    // nothing may depend on the number.
    static constexpr std::uint32_t kWritebackRunPages = 8;

    // Drains §4's dirty queue through WriteBack(): the sweep found these
    // dirty at usage zero and queued them instead of reclaiming (§3.2's
    // fourth branch); once written clean here, the sweep's next visit may
    // reclaim them - "keeping the page cached until frames are actually
    // needed", exactly as §4 words it. Returns how many it wrote.
    StatusOr<std::size_t> DrainDirtyEvictionQueue();

    // §4's watermark loop, on the writeback tick (BE-R2). When the pool's
    // free count - the capacity less the slots in use and the open windows'
    // promises - is below `low` (capacity / 16, `eviction.md`'s
    // `free_watermark`), it reclaims in bounded batches until the count
    // reaches `high` (capacity / 8), at most one deficit per call. The latch
    // is released between batches, and the dirty queue is drained between
    // them, so queued dirt becomes reclaimable on the hand's next visit. It
    // stops early as `ReclaimWalk` says. Returns frames reclaimed.
    //
    // **Its caller holds no page latch**, which is what makes the drain's
    // `WriteBack(kSkip)` sound here and nowhere on the fault path (the
    // header's "Held across a durability wait" bullet).
    std::size_t MaintainFreeReserve();

    // BE-R2's numbers, each a function of the budget so none is a second
    // name for it (BE-Q5). The batch is 64 frames, or budget / 16 when that
    // is smaller - so a pool of a few hundred frames does not empty a
    // quarter of itself per batch; a batch walks at most `8 * batch` slots
    // per latch hold.
    static constexpr std::size_t kReclaimBatch = 64;
    static constexpr std::size_t kBatchStepsPerFrame = 8;
    std::size_t ReclaimBatch() const noexcept {
        return std::clamp<std::size_t>(capacity_ / 16, 1, kReclaimBatch);
    }

    // ---- Scan ring (docs/spec/eviction.md §5, EVT06) --------------------
    //
    // The real cyclic ring: a page absent from the pool is faulted into
    // the ring's next slot, whose previous occupant is dropped from the
    // page table - **unless the foreground got there**. A pin, a usage
    // bump (only foreground accessors bump; ring fetches never do), a
    // dirty write, or pinned-class membership each abandon the frame to
    // the table instead of dropping it, which is §5's interaction rule and
    // the whole of pin-safety - and so does a latch another core holds,
    // which is the refusal a shared pool adds. A page already resident -
    // foreground's or a ring slot's - is used in place, with no usage bump
    // and no rotation.
    //
    // **The ring holds a shared pin, and the page latch, on exactly the
    // page its last `Fetch` returned** (AM-R8); every other slot is an
    // ordinary evictable frame. That is what makes "the span is valid
    // until the next `Fetch`" a statement about the pool rather than about
    // there being one thread, and the latch is what keeps a scan from
    // reading a page a writer on another core is in the middle of. It also
    // runs in the other direction: a foreground *exclusive* request on the
    // page the ring is holding waits until the ring's next `Fetch`.
    //
    // What this bounds: a full scan grows residency by at most the ring's
    // size, plus one frame for the length of a `Fetch` that faults - the
    // slot is released after the fault, not before, because the fault
    // answer comes from the fetch. The frames are ordinary frames in the
    // page table while resident, so a foreground hit on one behaves as a
    // hit anywhere - only the lifecycle differs.
    std::unique_ptr<ScanFetcher> OpenScanRing(std::size_t frames = kScanRingFrames) override;

    // Installs the default WAL-before-data gate described above - the one
    // every core without a gate of its own asks. Null (the default)
    // disables it for those cores. `gate` must outlive the store.
    void SetWalGate(wal::WalDurability* gate) noexcept { wal_gate_ = gate; }

    // Gives `core` a gate of its own (BA-S1), which that core's writebacks
    // ask instead of the default until `ClearCoreWalGate` takes it back.
    // `CoreRuntime::Open` sets it before the core's first writeback and
    // `~CoreRuntime` clears it before the gate is destroyed; only threads
    // running as that core read it. **One owner per slot, enforced**: a
    // slot already holding another gate refuses with AlreadyExists, so a
    // second runtime for a live core id cannot take the slot, and cannot
    // later clear it out from under the first. InvalidArgument for a null
    // gate, or past the page latch's core-id bound, which every core
    // already lives under.
    Status SetCoreWalGate(std::uint32_t core, wal::WalDurability* gate);

    // Takes back `core`'s gate if it is still `gate`; anything else is left
    // alone. A no-op past the bound.
    void ClearCoreWalGate(std::uint32_t core, const wal::WalDurability* gate) noexcept;

    // `SetStreamCoreId` and `core_id()` retired (AM-S2 step 3); their
    // history is at cf889b63. `CurrentCore()` is set by the thread, and
    // `CoreRuntime::Open`'s guard supplies it for the whole mount pass.


    // **There is no read predicate**: one frame table serves every core, so
    // every core faults every page. `MayFault` retired (AW-S1b); its history
    // is at cf889b63.

    // Records that the record at `lsn` modified `page_id`: stamps the
    // page header's page_lsn and, if this is the first record to dirty the
    // frame since it was last written back, adopts `lsn` as its recLSN.
    //
    // Call it *after* appending the record and *before* returning to the
    // client. Fails with NotFound if the page is not resident, and with
    // InvalidArgument for lsn 0 (kNoPageLsn means "never logged", so it
    // cannot also mean "logged at 0"; a real record LSN is never 0 because
    // offset 0 is the segment header - record.hpp).
    Status StampPageLsn(PageId page_id, std::uint64_t lsn) override;

    // Page ids currently dirty, in ascending order.
    std::vector<PageId> DirtyPageIds() const;

    // The same set with each frame's recLSN attached - what a checkpoint
    // snapshots (wal.md section 11-1). A frame dirtied by something that
    // logged nothing reports 0, which the checkpointer reads as "nothing
    // to replay for this page"; that is accurate for the unlogged paths
    // (catalog writes, bootstrap) and wrong for a logged one, which is why
    // every logged mutation must call StampPageLsn().
    std::vector<std::pair<PageId, wal::Lsn>> DirtyPagesWithRecLsn() const;

    // Writes back exactly these pages and syncs - a checkpoint step. Ids
    // that are not dirty, or not resident, are skipped rather than treated
    // as an error - something else may have flushed them since the caller's
    // snapshot. **Writes no map** (BI-Q12): the publish's `Flush` does,
    // after every dirty page.
    Status FlushPages(std::span<const PageId> page_ids);

    // ---- The data file's sync (BI-S4, `storage/data_syncer.hpp`) ---------
    //
    // Every sync of the data file in this store goes through `data_sync()`:
    // one at a time, sticky on failure, and on the instance's thread once
    // `data_sync().Start()` has run. A checkpoint task requests and parks;
    // every other caller waits.
    DataSyncer& data_sync() noexcept { return data_sync_; }

    // A checkpoint task's writes, without the sync (the task asks
    // `data_sync()` and parks): `WriteBack` with its `defer`, and the
    // publish's map write, after every dirty page. Each returns what it
    // wrote.
    StatusOr<std::size_t> WriteBackPages(std::span<const PageId> page_ids, wal::Lsn* defer) {
        return WriteBack(page_ids, HeldFrames::kWait, defer);
    }
    StatusOr<std::size_t> WriteMaps() { return FlushMaps(); }

    // **Every resident map region's pages as they stand in memory**, each
    // stamped with its checksum, and **nothing written** (BR-S5): a backup's
    // STOP puts these in its copy, so the copy's maps carry every allocation
    // up to its stop. Writing them to the device instead would put bits
    // there ahead of an unlogged page's bytes - BI-Q12's hazard, which a
    // crash after the STOP turned into a page allocated and never written.
    // Every region is resident from the mount on, so a region absent here
    // was never created and its bits are all clear.
    struct MapImage {
        PageId page_id;
        std::array<std::byte, kPageSize> bytes;
    };
    std::vector<MapImage> MapImages() const;

    // **One page made durable, and nothing else** (BA-S13, BA-R10): written
    // back through `WriteBack`'s claim (AT-S10e) and then the data file
    // synced **unconditionally** - unlike `FlushPages`, which skips its sync
    // when another core's writeback carried the page, because a caller here
    // has no anchor publish behind it to cover the skip. What a
    // transaction-id carve persists the superblock through: before BA-S13 it
    // synced the whole pool, every dirty page written for one page's sake,
    // and BA-S4's census measured carves of up to 2.2 s. The one data file
    // means the `fdatasync` covers every write issued before it, so the
    // anchor's order (every page a checkpoint vouches for written before the
    // anchor rises) is untouched.
    Status PersistPage(PageId page_id);

    // **BF-R5's free, in one hold of the frame table then the free map**
    // (the declared order). The resident frame - dirty or not - is erased,
    // the bit cleared, `allocated_pages()` decremented, the region marked
    // dirty and the id pushed onto the free list `CreateNew` asks first
    // (BF-R6). `PageStore::FreePage` states the contract and the caller's
    // precondition.
    //
    // **Deferred**, changing nothing, while the frame is pinned, latched or
    // claimed by a writeback (`Frame::writing`: a writeback holds raw
    // `Frame*` pointers from its copy to its clean), or while a fault
    // (`loading_`) or a `CreateAt` (`claiming_`) of the id is in flight.
    // **Refused `Corruption`** below `first_new_page_id` (the system range
    // `Open` was given), for a map id, a headerless id, and a resident
    // `kCabinBound` frame; `NotFound` for an id already free.
    //
    // **What this makes true, replacing "free-map bits are never cleared"**:
    // no clear bit has a resident frame - the erase and the clear are one
    // hold, and every accessor tests the bit under the frame table's hold
    // before it serves a frame - and no id is handed out while a fault on
    // it is in flight (the pop's `loading_` skip).
    StatusOr<FreeOutcome> FreePage(PageId page_id) override;

    // BF-R11's store half: pages `FreePage` has freed, and creations served
    // from the free list rather than the cursor. Both rise only.
    std::uint64_t pages_freed() const noexcept;
    std::uint64_t pages_reused() const noexcept;
    // All three under one map hold: `SHOW META` calls this from any core,
    // and `allocated_pages_` is written under the map latch only.
    AllocationCounters allocation_counters() const noexcept override {
        AssertNotUnderMapHold("allocation_counters");
        LatchGuard map(map_latch());
        return AllocationCounters{allocated_pages_, pages_freed_, pages_reused_};
    }

    // Diagnostic log, null (discard) by default. Set after Open(), since
    // the store has to exist before a server has anything to log about;
    // `log` must outlive the store.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    // ---- Frame reclamation (eviction.md; EV01-EV02 at 1769487) ----------
    //
    // The pool evicts (BE), so a frame is safe only under a pin: every
    // accessor returns a pinned `PageRef` (`page.md` §3), and a raw span
    // from an `*Unpinned` accessor is valid only while nothing can reclaim
    // its frame.

    // The engine-wide pinned handle (page_store.hpp, MG01): move-only RAII,
    // construction pins, destruction unpins. The alias keeps the spelling
    // `DevicePageStore::PageRef`.
    using PageRef = storage::PageRef;

    // The pre-MG01 spellings of the pinned accessors, kept for
    // eviction_test.cpp's callers.
    StatusOr<PageRef> PinnedGet(PageId page_id) { return Get(page_id); }
    StatusOr<PageRef> PinnedGetForRead(PageId page_id) { return GetForRead(page_id); }

    // Whether `page_id` belongs to a **pinned class** (`eviction.md` EV3):
    // never a sweep candidate at any pressure. Implemented as
    // *id-range-or-kind*, not EV3's "resolved from page kind" alone: the
    // fixed catalog pages are formatted `PageType::kHeap` like any user
    // relation's, so only their reserved id range (`catalog/well_known.hpp`)
    // tells them apart. The kind half answers for `PageType::kCabinBound`,
    // whose pages carry the aggregate an admission check reads, and is
    // asked only of a resident frame - a non-resident page is no sweep
    // candidate, and a header read would turn a skip test into an I/O. A
    // free-map region's page ids answer true by arithmetic (FM8).
    bool IsPinnedClass(PageId page_id) const noexcept;

    // Raises the pinned id range's upper bound. **Additive only**: a
    // structure that declared itself un-evictable must not become evictable.
    // A residency-only knob, read as no authorization (its write-boundary
    // arm retired at AT-S5/AT-S18; history at cf889b63). The one value
    // installed is the volume's layout boundary (`server::kFirstUserPageId`).
    // A Bound Cabin page must not be made resident through this - raising
    // the floor past it would pin every user page beneath it; the kind half
    // of `IsPinnedClass` covers it.
    //
    // **Install-time only**, which is what makes the unlatched read sound:
    // `Expeditor::Open` sets it before the first peer is built, and a
    // `CoreRuntime` opened on a store of its own sets it at its open.
    void SetResidentLimit(PageId first_evictable_page_id) noexcept;

    // The floor, for the assembly cell that checks the setter was called -
    // a wiring line whose absence changes no other result (as
    // `AttachWakers` was, AU-S1b).
    PageId first_evictable_page_id() const noexcept { return first_evictable_page_id_; }

    // ---- The capacity (BE-R3) ------------------------------------------
    //
    // How many frames the pool may hold, fixed at `Open` (`eviction.md` §7:
    // no runtime resize). In debug builds `KDS_TEST_FRAME_BUDGET` in the
    // environment can only **lower** it, and never below
    // `kWorkingMinimumFrames` - which is how MG05 runs the whole suite under
    // eviction pressure without threading a knob through every fixture,
    // and why that run is the floor (BE-R3).
    std::size_t frame_capacity() const noexcept { return capacity_; }

    // The working minimum of BE-Q6's mount floor: frames beyond the
    // resident-class pages that a statement needs to run at all.
    static constexpr std::size_t kWorkingMinimumFrames = 256;

    // BE-R4: walks a full pool retries before it refuses (`eviction.md`'s
    // `evict_retry_budget`, a constant since BE-Q5).
    static constexpr std::size_t kRefuseRetries = 8;

    // Resident frames of a pinned class (EV3) - never reclaimed, so what
    // the mount floor counts (BE-R3).
    std::size_t resident_class_frames() const;

    // **BE-Q6's mount floor**, asked once the volume is mounted: the
    // resident-class frames now resident plus `kWorkingMinimumFrames`. A
    // configured capacity below it is refused `InvalidArgument`, naming both
    // numbers, because a pool its own pinned pages exhaust would refuse
    // every statement. A capacity only the debug override lowered below it
    // is raised to it instead (BE-R3's clamp). A snapshot: Bound Cabin pages
    // grow after mount, and one that cannot get a slot is refused like any
    // creation. Mount-time only - before any peer shares the store.
    Status ApplyMountFloor();

    // ---- The page latch's switch (AM-S1) ---------------------------------
    //
    // Armed from the superblock's core count by the assembly (core 0 in
    // Expeditor::Open, a peer in CoreRuntime::Open) and by nothing else.
    // The debug census (`KDS_TEST_PAGE_LATCH=1`, Open()) arms first and
    // wins: the assembly's call cannot disarm it, so "the whole suite ran
    // armed" holds for assembly-driven fixtures too.
    // `concurrent_pinners` scales `kPinCeiling`, a *per-operation* bound,
    // into a bound on `live_pins_`, the sum over every thread pinning this
    // store (AM-S2: the unscaled assert aborted correct traffic at nine
    // concurrent readers). One-thread callers pass nothing.
    void SetLatchArmed(bool armed, std::uint32_t concurrent_pinners = 1) noexcept {
        latch_armed_ = armed || latch_forced_;
        pin_ceiling_ = kPinCeiling * (concurrent_pinners == 0 ? 1 : concurrent_pinners);
    }
    bool latch_armed() const noexcept { return latch_armed_; }

    // Test hooks: hold a resident frame's latch as if another core held it,
    // with no pin, so the sweep's and `FreePage`'s latch refusals can be
    // exercised before a second core can reach a frame (AM-S2). Absent
    // frame: NotFound. The word is read back by `latch_word_for_test`.
    // The structure and free-map latches, null unarmed: what BA-S2's cells
    // hold to make a fetch wait under each kind (`base/contention.hpp`).
    Latch* StructureLatchForTest() const noexcept { return structure_latch(); }
    Latch* MapLatchForTest() const noexcept { return map_latch(); }
    Status LatchFrameForTest(PageId page_id, PinMode mode, std::uint32_t core);
    Status UnlatchFrameForTest(PageId page_id, std::uint32_t core);
    StatusOr<std::uint32_t> latch_word_for_test(PageId page_id) const;

    // **A test seam between a writeback's copy and its clean** (BC-S1):
    // runs once per page a writeback copies, after the copy and the page
    // latch are given back and before the page goes out or is cleaned,
    // holding no latch. A write there is the write that races the copy -
    // the frame stays dirty (`wal.md` §11-2) - placed where a two-core cell
    // wants it rather than where the threads put it.
    // **It still holds the run's writeback claims**, so the hook must not
    // flush (`Sync`, `FlushPages`, a carve's sync): a `kWait` writeback
    // meeting its own claim waits in `AwaitWritebackClaim` forever. Set and
    // cleared while no writeback runs - the hook is read unsynchronised.
    // Unset in production, where it costs one empty-function test per page
    // written back.
    void SetAfterWritebackCopyForTest(std::function<void(PageId)> hook) {
        after_writeback_copy_for_test_ = std::move(hook);
    }

    // **BF-S2's two seams**, set and cleared while nothing they name runs
    // (read unsynchronised, as the hook above). The first runs on an armed
    // fault after its id is published to `loading_` and the structure latch
    // is dropped, before the device read: a cell parks a stale fault there
    // to show the free list's pop skips its id. The second runs inside
    // `FlushMaps` after the copy and before the write, holding the map write
    // barrier: a cell parks one core's flush there to show another core's
    // `PersistMaps` waits for its write.
    void SetAfterFaultPublishedForTest(std::function<void(PageId)> hook) {
        after_fault_published_for_test_ = std::move(hook);
    }
    void SetAfterMapCopyForTest(std::function<void()> hook) {
        after_map_copy_for_test_ = std::move(hook);
    }

    // Pin accounting (MG04). Live pins across all frames, and the highest
    // that count has ever been - the number the per-operation ceiling
    // decision needs measured rather than assumed.
    std::size_t live_pins() const noexcept override { return live_pins_; }
    std::size_t pin_high_water() const noexcept { return pin_high_water_; }

    // One clock pass, reclaiming at most `budget` frames. Returns how many
    // it actually reclaimed - so a caller can tell "nothing to do" from
    // "nothing allowed", which are very different states under pressure.
    //
    // **Never reclaims a pinned frame or a resident-class one**, at any
    // budget - what a Bound Cabin's residency rests on (AST04);
    // `EvictionTest` asserts it against a sweep reclaiming other frames in
    // the same pass.
    //
    // **A dirty frame at usage zero is queued, not reclaimed** (§3.2's
    // fourth branch): reclaiming it would lose a write.
    // `DrainDirtyEvictionQueue` writes the queue back, after which a later
    // sweep may reclaim the frame.
    //
    // Every accessor returns a pinned `PageRef`, which is what makes a sweep
    // safe (`page.md` §3); its callers are named at `EvictColdFrames` below.
    // A frame `FreePage` discarded is never a victim here: it is gone.
    //
    // The CLOCK usage counter's ceiling (`docs/spec/eviction.md` EV1,
    // `[PROPOSED] 5`). A cap and not a free-running count: it bounds how
    // many sweep rotations a hot frame can survive, so a page that fell out
    // of use cannot hold a frame for an unbounded time on the strength of
    // how popular it once was. **Nothing may depend on the number** - it is
    // the knob EV1 leaves open, and a sweep sizes its own laps from it.
    static constexpr std::uint8_t kClockUsageCap = 5;

    // **Takes the structure latch** (AM-S2 step 3's eraser half). Every
    // caller outside this class reaches it with the latch not held - the
    // eviction cells. The bounded batches (BE-R2) - a reservation's and
    // `MaintainFreeReserve`'s - call the `Locked` body under holds of
    // their own, one batch per hold.
    std::size_t EvictColdFrames(std::size_t budget);

    // Pages the sweep found dirty at usage zero, in the order it found them
    // - §4's dirty queue, returned and cleared; `DrainDirtyEvictionQueue`
    // takes it here.
    std::vector<PageId> TakeDirtyEvictionQueue();

    // Under the hold (AM-S2 step 3f): another core may be growing the
    // table. Not `noexcept`: `LatchGuard` locks a `std::mutex`.
    std::size_t resident_pages() const {
        LatchGuard structure(structure_latch());
        return frames_.size();
    }

    // How many frames currently hold at least one pin. Test and §11
    // observability: an unbalanced pin shows up here as a number that never
    // returns to its floor.
    std::size_t pinned_frames() const;

    // The per-operation pin ceiling (MG04; `docs/workplan-pageref.md` §7 at
    // 1769487). Derivation (MG03 audit): a btree grow path holds `2 + k`
    // for a split that climbs `k` levels - its leaf, the `k` parents
    // `SecureParents` holds (AT-S16) and a level's created node
    // (btree.cpp's `PromoteSeparator`) - 3 in the ordinary case, and `k` is
    // at most 6 on a 678-way tree. **An index's `k` has no such bound**
    // (index_tree.cpp's walk up): open, AT-S16's review F1. Since AT-S21 an
    // insert keeps its leaf and parents held until its record is stamped,
    // so an index's split path stacks on the clustered one's; an outer
    // chain walk adds 1, and index maintenance under a statement adds 2.
    // 8 bounds the ordinary case, and a clustered split under an outer walk
    // up to `k = 5`. The debug assert in PinFrame() aborts naming the count
    // when a workload exceeds it.
    //
    // **A scan ring adds 1, not `kds.scan_ring_frames`** (AM-R13): it pins
    // only the page its last Fetch returned. The suite would not catch a
    // regression here - the ring's two 32-slot consumers run in it only
    // over `InMemoryPageStore`'s pass-through fetcher - so the arithmetic
    // is the check.
    static constexpr std::size_t kPinCeiling = 8;

    std::uint32_t allocated_pages() const noexcept;
    // **Takes the free map's latch** (AM-S3), not the frame table's (see
    // `map_latch_`): another core may be inserting a region. A caller
    // already inside the hold wants `IsAllocatedLocked`; asking here would
    // hang, and debug builds abort naming the reader instead.
    bool IsAllocated(PageId page_id) const noexcept;

private:
    // Returns once no writeback holds `page_id`'s claim (`Frame::writing`,
    // AT-S10e) - its clean done, or the frame gone. Holds nothing while it
    // waits. `WriteBack`'s alone.
    void AwaitWritebackClaim(PageId page_id);

    using Page = std::array<std::byte, kPageSize>;

    // The one refusal a caller sees when a page id is not allocated here,
    // carrying the id.
    Status NotAllocated(PageId page_id) const;

    // The gate every accessor goes through, so a third one cannot forget
    // it: the allocation check, the adoption below when it misses, then
    // the frame. `mark_dirty` is the only thing Get and GetForRead differ
    // in: false only for a read-only fetch, whose frame enters the map clean
    // and is never written back. `heat` is how the fetch counts
    // (`FetchHeat`): `kScan` and `kRing` fault cold, and `kRing` alone never
    // bumps a hit.
    StatusOr<std::span<std::byte, kPageSize>> Resolve(PageId page_id, bool mark_dirty,
                                                      FetchHeat heat);

    // **A resident page's frame**, held in the page table's node (BE-R1, as
    // restated at BE's close): the lookup that finds a page has its frame in
    // the same allocation, so a hit costs one hash probe and no second
    // cache line - the cost a frame held in its slot added (`workorder-be-
    // bounded-pool.md` §6, BE-S6). Node references survive the table's
    // rehashing, so a `Frame*` is good for as long as the page is resident;
    // the bytes it points at sit in a slot that never moves.
    struct Frame {
        // The page's bytes, in its slot. Set at publish and never re-pointed.
        Page* bytes = nullptr;
        PageId page_id = kInvalidPageId;
        // The slot `bytes` sits in, which the erasers give back.
        std::uint32_t slot = 0;
        bool dirty = false;
        // On the dirty eviction queue already (BE-R1): the sweep's test,
        // which was a `std::find` over the queue per dirty victim.
        bool queued = false;
        // The page latch word (page_latch.hpp). A plain integer rather than
        // a `std::atomic`, as it was while the table moved frames in; every
        // access goes through `std::atomic_ref`, and only when the store is
        // armed - unarmed it stays 0 for the frame's whole life.
        std::uint32_t latch = 0;
        // Lowest log record to dirty this frame since it was last written
        // back; 0 when nothing logged touched it. See StampPageLsn().
        wal::Lsn rec_lsn = 0;
        // **The lowest LSN stamped since the last writeback copy**, 0 when
        // none, under the structure latch like `rec_lsn`: reset where a copy
        // reads `dirty_gen`, min'd in by every stamp. A clean the generation
        // skips sets `rec_lsn` to it, where one exists (`WriteBack` says why
        // that is sound), so a page whose copy is overtaken every time no
        // longer keeps its first recLSN, which pinned the instance's redo
        // start and stopped the log recycling. The minimum rather than "just
        // above the copy's page_lsn" by choice: it needs no argument about
        // the order LSNs are stamped in (wal.md §11-2).
        wal::Lsn stamped_since_copy = 0;

        // ---- Reclamation state (eviction.md; EV01 at 1769487) ----------
        //
        // How many live `PageRef`s hold this frame. **Plain, not atomic**:
        // every read and write of it is under the structure latch (AM-S2
        // step 1), which is what the exclusion rests on. A frame with
        // pins > 0 is never a victim at any pressure, and all three erasers
        // read this field under that latch.
        std::uint32_t pins = 0;

        // The CLOCK usage counter (`docs/spec/eviction.md` EV1 / §3.1-2):
        // **saturating on access, decremented by the sweep, reclaimed at
        // zero.** A counter rather than a single reference bit, so a page
        // touched five times outlives one touched once - which a bit cannot
        // express, and which is the whole of EV1's "no LRU lists".
        std::uint8_t usage = 0;

        // **A writeback holds this frame between its copy and its clean**
        // (AT-S10e), under the structure latch like `dirty`. Two writebacks
        // of one frame used to run unordered: the one that copied the older
        // image could reach the device last, after the newer one had cleaned
        // the frame, leaving the disk behind a frame that read clean - a
        // lost committed update with no crash to replay it. A writeback that
        // meets a claimed frame waits for the claim and looks again
        // (`HeldFrames::kWait`), or leaves the frame to its writer
        // (`kSkip`). In the padding after `usage`, so the frame's size does
        // not move.
        bool writing = false;

        // **Moved at every dirty mark** (AT-S8, step 1b), under the
        // structure latch like `dirty` itself, to the next value of the
        // store's `dirty_gens_` - store-wide, so a frame evicted and
        // re-faulted never repeats a generation a stale writeback copied. A
        // writeback records it at the copy and cleans the frame only if it
        // has not moved: a write fetched after the copy marks the frame
        // again, and its bytes are not in what went out. The fetch's mark
        // precedes the page latch, so an exclusive hold marks the frame once
        // more on acquiring it (`AcquirePageLatch`) - a write fetched
        // *before* the copy and latched after it is covered too. 32 bits: a
        // wrap back to one copied value inside a single device write would
        // need four billion marks in it.
        std::uint32_t dirty_gen = 0;

        // Every dirty mark goes through here, so none can miss the move.
        void MarkDirty(std::uint32_t gen) noexcept {
            dirty = true;
            dirty_gen = gen;
        }
    };
    // 56 bytes, under 0.7% of the page: `dirty_gen` is 32 bits because a
    // 16-bit one can wrap inside one device write on a hot page (AT-S8 step
    // 1b), `page_id`/`queued`/`slot` came with BE-R1, and
    // `stamped_since_copy` is a full LSN.
    static_assert(sizeof(Frame) == 56, "Frame grew; say why beside this assert");
    static_assert(std::atomic_ref<std::uint32_t>::required_alignment <= alignof(Frame),
                  "std::atomic_ref needs the word aligned inside the frame");

    // One region's pair of bitmaps: the unit of residency, and the unit of
    // dirtiness.
    //
    // One dirty flag for both pages: they are written together, at the same
    // points, so a flag per page would only add a state where one is on
    // disk and the other is not (FM2: per region, not per page).
    struct MapRegion {
        Page free_map{};
        // **Null until this region holds a headerless page** (FM6, D2(a)):
        // the one headerless class is the Waystone directory's interior
        // pages (`src/stats/waystone_dir.cpp`), so the bitmap is built only
        // where it records something. Its id is still **reserved** in
        // free_map at region creation - only the bytes are deferred - which
        // is why allocated_pages() counts two for a fresh region.
        std::unique_ptr<Page> headerless_map;
        bool dirty = false;
    };

    DevicePageStore(PageDevice& device, PageId first_new_page_id) noexcept;

    // The all-zero page a region that does not exist reads as. Shared and
    // never written - the const accessors hand out a read-only view of it.
    static const Page& AbsentRegionPage() noexcept;

    const MapRegion* FindRegion(std::uint32_t region) const noexcept {
        auto it = map_regions_.find(region);
        return it == map_regions_.end() ? nullptr : &it->second;
    }

    // The same lookup for a caller that is about to mark a bit. Separate
    // from `EnsureRegionResident` because it is the half that cannot touch
    // the device, which is what lets it run under the free map's latch.
    MapRegion* MutableRegion(std::uint32_t region) noexcept {
        auto it = map_regions_.find(region);
        return it == map_regions_.end() ? nullptr : &it->second;
    }

    // Loads `region` from the device if it holds one, formats a fresh one
    // if it does not (FM5's growth), and returns it resident either way.
    // A torn or wrong-typed map page is Corruption and refuses, the rule
    // Open has always applied to region 0 and RV3 applied to the catalog.
    StatusOr<MapRegion*> EnsureRegionResident(std::uint32_t region);

    // ---- Allocation, with the free map's latch held ----------------------
    //
    // **The claim and the mark are one step** (AM-S2): as two calls, any
    // number of threads could scan the same clear bit before one set it
    // (~14% of allocations doubled at four threads,
    // `tests/alloc_race_test.cpp`).
    //
    // **The device work stays outside the hold**: `EnsureRegionResident`
    // may read the device or grow the file, so a scan that reaches a region
    // not loaded returns `kInvalidPageId` and names it in `missing_region`;
    // the caller drops the hold, loads it and asks again. What is left
    // under the hold is a bitmap scan over one 8 KiB page.
    StatusOr<PageId> ClaimNextFreeIdLocked(std::uint32_t* missing_region);

    // The named-id half, and it decides one question out of **two**
    // structures: the bit is the free map's and the in-use test
    // `frames_.count` is the frame table's, so its caller holds both, frames
    // outer (AM-S3). `device_spare` is the answer to
    // `DeviceHoldsNoPage(page_id, ...)` read *outside* the hold, and
    // `device_read` says whether it was read at all - a caller that found
    // the bit clear has no need for it, and gets `AlreadyExists`'s
    // `kNeedsDeviceRead` back if the bit turned out to be set after all.
    enum class ClaimOutcome : std::uint8_t { kClaimed, kNeedsDeviceRead };
    StatusOr<ClaimOutcome> ClaimNamedIdLocked(PageId page_id, bool device_read,
                                              bool device_spare);

    // `CreateAtUnpinned` and `CreateOverTornUnpinned`, which differ only in
    // whether a torn page counts as no page.
    StatusOr<std::span<std::byte, kPageSize>> CreateNamedIdUnpinned(PageId page_id,
                                                                    bool over_torn);

    // Loads `region` only if the device already holds it, leaving it
    // absent otherwise. What mount walks: a region the file is not large
    // enough to hold, or whose map page reads as never-written, does not
    // exist and must not be created by the act of looking.
    Status LoadRegionIfPresent(std::uint32_t region);

    // The free map's readers, without the hold, for callers that already
    // have it. Never public: the acquisition is what a caller from outside
    // this class is owed (AM-S3).
    bool IsAllocatedLocked(PageId page_id) const noexcept;
    bool IsHeaderlessLocked(PageId page_id) const noexcept;

    // Restores a region's dirty bit after `FlushMaps`' write of it failed.
    void RemarkRegionDirty(std::uint32_t region) noexcept;

    // ---- Ids claimed but not yet resident (AM-S3) ------------------------
    //
    // `CreateAt` sets the map bit under the hold and inserts the frame
    // *after* releasing it, because `InsertFrame` takes the frame table's
    // latch itself. Between the two the page is allocated, not resident,
    // and its bytes on the device are still all zeros - which is exactly
    // the signature `ClaimNamedIdLocked` reads as a **torn creation** and
    // re-claims - so without this set a second caller naming the id in that
    // window got a page the first already held (found by AM-S3's cell). An
    // id here is claimed by somebody, so the torn-creation reading does not
    // apply to it - the same shape `loading_` uses for a fault in flight.
    // Guarded by `map_latch_`, and erased by an RAII guard so a failed
    // `EnsureAddressable` or a throwing allocation does not strand an id.
    std::unordered_set<PageId> claiming_;

    // ---- The free list (BF-R6) -------------------------------------------
    //
    // Ids `FreePage` cleared, which `CreateNew` hands out again ahead of the
    // cursor. Under `map_latch_`. In memory only: a crash forgets it, and
    // until a clean restart the floor hides any id below it from the cursor
    // - the undo log's recycle list states the same loss (UP4). The cursor
    // and the floor never move for it, because a clear bit below the floor
    // can be an id the log names, while the list holds only ids a reclaim
    // checked and gated.
    //
    // **Ordered, and asked by the cursor too** (BF-S2's review, B2). The
    // cursor claims the lowest clear bit at or above it, and every id a
    // mount's reclaim frees lies above a fresh mount's cursor - so without
    // a skip the cursor would hand a listed id out past the pop's
    // `loading_` test, and to a headerless create. `ClaimNextFreeIdLocked`
    // passes over a listed id, and the pop takes the lowest first.
    std::set<PageId> free_list_;
    // `free_list_.size()`, stored under `map_latch_` and read without it, so
    // a creation with nothing listed - the common case - takes no extra
    // hold. A stale zero skips one pop; a stale nonzero costs one hold.
    std::atomic<std::size_t> free_listed_{0};
    void NoteFreeListSize() noexcept {
        free_listed_.store(free_list_.size(), std::memory_order_relaxed);
    }
    // BF-R11's counters, under `map_latch_`.
    std::uint64_t pages_freed_ = 0;
    std::uint64_t pages_reused_ = 0;

    // **The map write barrier** (BF-R5). `FlushMaps` copies the dirty
    // regions under the map latch and writes the copies after it, so without
    // this a `Sync` running between another core's copy and its write found
    // every region clean and returned before that write landed. Held across
    // `FlushMaps`' copy and write, it makes a `PersistMaps` or a `Sync` that
    // returns cover every region state copied before it began - which is
    // what a reclaim's "the cleared bits are durable" rests on (BF-R7). A
    // plain mutex, outer to the map latch and never taken under it; it is
    // held across device I/O, which is why it is not the map latch. Null
    // unarmed, as the other two.
    mutable Latch map_flush_latch_{LatchKind::kFreeMapFlush};
    Latch* map_flush_latch() const noexcept { return latch_armed_ ? &map_flush_latch_ : nullptr; }

    // Debug: a public free-map reader must not be reached from inside the
    // *map* hold, because `Latch` is not recursive (`base/latch.hpp`) and a
    // second acquisition **hangs**, naming nothing. This aborts naming the
    // reader instead (AM-S3's census). Holding `frames_latch_` here is the
    // declared order.
    void AssertNotUnderMapHold(const char* reader) const noexcept;

    // Debug: the other half of the order - nothing may take the frame table
    // while holding the map. Called where `frames_latch_` is acquired on a
    // path the map could reach.
    void AssertOrderBeforeFrames(const char* site) const noexcept;

public:
    // FM10. Resident is in-existence for the free map (see map_regions_),
    // so `regions` counts what the file holds and the gap between
    // `resident_pages` and twice that is what FM6's deferred headerless
    // bitmaps save.
    //
    // **Under the hold** (AM-S3): a concurrent `EnsureRegionResident`
    // inserts regions and `EnsureHeaderlessMap` stores the pointers it reads.
    MapResidency map_residency() const noexcept override {
        AssertNotUnderMapHold("map_residency");
        LatchGuard map(map_latch());
        MapResidency out;
        out.regions = map_regions_.size();
        for (const auto& [region, pages] : map_regions_) {
            out.resident_pages += 1 + (pages.headerless_map != nullptr ? 1 : 0);
        }
        out.coverage_ids = static_cast<std::uint64_t>(out.regions) * kFreeMapBitsPerPage;
        out.has_headerless = any_headerless_.load(std::memory_order_acquire);
        return out;
    }

private:

    // Stamps a checksum unless the page is headerless. The one place that
    // decision is made, so no write path can forget it.
    void StampIfHeadered(PageId page_id, std::span<std::byte, kPageSize> page) const;

    // Writes every dirty region's bitmap pair out, after the data pages
    // they describe (a page is reachable only once the map says so), and
    // answers **how many regions it wrote** (AM-S3) - what tells a caller
    // whether a sync is owed, taken once and under the hold.
    //
    // **Copy under the hold, write outside it** (no device call under the
    // latch): the bytes are checksummed and copied, and the dirty flags
    // cleared, while the map is held, so a concurrent allocation re-dirties
    // the region instead of being lost. **A failed write re-marks that
    // region and every one after it**: the batch's flags were cleared at
    // the copy, and a tail left clean would never reach the device, so a
    // restart would re-issue live pages.
    //
    // With one store for every core, the first caller copies and every
    // later one **waits on the map write barrier until that copy is
    // written** (BF-R5), so a returning `Sync` or `PersistMaps` covers every
    // region state copied before it began.
    StatusOr<std::size_t> FlushMaps();

    // Waits, through `gate`, for the log records of `page_ids` to be durable
    // before any of them is written. A no-op with a null gate or no logged
    // page in the batch. With `defer`, an LSN not yet durable is stored
    // there instead of waited for (`WriteBack`'s `defer`).
    Status AwaitWalGate(std::span<const PageId> page_ids, wal::WalDurability* gate,
                        wal::Lsn* defer);
    // The highest `page_lsn` among `page_ids` that are resident and dirty,
    // headerless pages skipped; `wal::kNoLsn` when none is logged. Under the
    // structure latch, released before it returns.
    wal::Lsn HighestDirtyPageLsn(std::span<const PageId> page_ids);

    // The gate a writeback on this thread asks: the calling core's own
    // (SetCoreWalGate), or the default.
    wal::WalDurability* GateForCaller() const noexcept;

    // `Resolve`'s miss half: reserve a slot, read, verify, publish. It
    // reaches the frame table only under holds of its own (`ReserveFrame`'s
    // sweep, `InsertFrame`), so the armed miss path calls it with the
    // structure latch released instead of a `find` another core's insert
    // could be rehashing.
    StatusOr<std::span<std::byte, kPageSize>> FaultIn(PageId page_id, bool mark_dirty,
                                                      FetchHeat heat);

    // Whether the device holds nothing for `page_id` - not addressable, or
    // every byte zero: a page allocated in the map and never written. What
    // lets CreateAt take an allocated id (redo re-creating a page whose
    // PAGE_INIT outran its first flush) without ever clobbering a live one.
    // A failed read answers false, the refusing side. With `torn_is_none`,
    // a headered page that fails its checksum counts as nothing too: bytes
    // nobody may interpret, which redo's heal replaces whole. A page that
    // verifies is always a page.
    bool DeviceHoldsNoPage(PageId page_id, bool torn_is_none) const;

    // The ring's rotation half: drops `page_id`'s frame unless the
    // foreground claimed it (dirty, pinned, usage-bumped, or pinned-class)
    // - in which case the frame is abandoned to the page table, having
    // graduated to ordinary life. kInvalidPageId and non-resident ids are
    // no-ops.
    void ReleaseScanSlot(PageId page_id) noexcept;

    // The ring fetches through `FetchAndPin` (AM-R8a): its pin is taken
    // under the hold that made the frame resident, and it holds the page
    // latch like any `PageRef`. It replaced `PinForScan`, which faulted
    // outside the loading set and retried a lost race (AM-S2-P S-P1).

    class ScanRing;  // the ScanFetcher over this store; defined in the .cpp

    // PageRef's callbacks, overriding the base no-ops onto Frame metadata
    // (MG01). Private on purpose: a pin is only ever taken and dropped by a
    // handle through the base interface, so no caller can unpin someone
    // else's frame by hand.
    // AM-S2: the fetch-and-pin pair, held together (`page_store.hpp`).
    StatusOr<std::span<std::byte, kPageSize>> FetchPinned(PageId page_id, PinMode mode,
                                                         bool for_read, FetchHeat heat) override;

    // `FetchPinned`'s whole body, plus the answer only the scan ring needs:
    // **did this call take the branch that loads?** The ring rotates a slot
    // only on a fault (AM-R8), and a residency probe outside this hold would
    // be stale. `faulted` may be null (`FetchPinned` passes null).
    // **Meaningful only on the ok arm**: an error on a later turn of the
    // loop leaves it true. It also says true when another core loaded the
    // page while this call was outside the latch.
    StatusOr<std::span<std::byte, kPageSize>> FetchAndPin(PageId page_id, PinMode mode,
                                                          bool for_read, FetchHeat heat,
                                                          bool* faulted);

    // The create half of the same pair. See the definition for why its
    // window is narrower than `Get`'s and why it can still close.
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreatePinned(
        CreateKind which, PageId page_id) override;

    void PinFrame(PageId page_id, PinMode mode) noexcept override;
    void UnpinUnlatchedFrame(PageId page_id) noexcept override;
    void UnpinFrame(PageId page_id) noexcept override;
    void MarkFrameDirty(PageId page_id) noexcept override;

    // The two halves of taking a pin, split because `FetchPinned`'s armed
    // path reaches them already holding the structure latch and cannot call
    // `PinFrame` (which takes it). One definition each, so the MG04 ceiling
    // and the never-upgrade census run on every path.
    //
    // `CountPin` runs **under** the structure latch, `AcquirePageLatch`
    // **outside** it - the order that keeps neither latch held across the
    // other (PinFrame's comment says why).
    void CountPin(Frame& frame) noexcept;
    void AcquirePageLatch(PageId page_id, Frame& frame, PinMode mode) noexcept;
    // `CountPin`'s mirror, under the structure latch, for a pin that holds
    // no page latch (a writeback that met a held frame and skipped it).
    void UncountPin(Frame& frame) noexcept;
    // One attempt at a shared hold, with the debug bookkeeping
    // `AcquirePageLatch` keeps. Always true on an unarmed store.
    bool TryAcquirePageLatchShared(PageId page_id, Frame& frame) noexcept;

    // **The pin tail, with `hold` held on entry** (AM-S2), shared by
    // `FetchPinned`'s hit and miss arms: find, pin, take the span, release
    // the structure latch, wait for the page latch.
    //
    // `nullopt` means the frame is gone, **with `hold` still held**; both
    // callers round the loop and re-test. On the value arm `hold` is
    // released before the page latch is waited for.
    //
    // **It does not re-apply the dirty mark** (AM-S2-P F-1): a frame
    // evicted and re-faulted clean between the caller's load and this pin
    // would drop a `Get`'s write, and what makes that unreachable is the
    // loading set: every fault publishes its id there first.
    std::optional<std::span<std::byte, kPageSize>> PinResidentAndRelease(
        PageId page_id, PinMode mode, std::unique_lock<Latch>& hold) noexcept;
    static PageLatchMode LatchModeFor(PinMode mode) noexcept {
        return mode == PinMode::kShared ? PageLatchMode::kShared : PageLatchMode::kExclusive;
    }
    class ReservedFrame;
    // Publishes `slot`, which `ReserveFrame` handed this caller and the
    // caller has filled, as `page_id`'s frame. A page already resident wins
    // and `slot` goes back to the free list - except under a create.
    //
    // **A create refuses to lose the race** (BF-R5): `kCreate` is the three
    // `Create*` paths, whose id no other caller holds - so a frame already
    // resident is a fault on an id a reclaim handed out again, reading the
    // dead image, and the create answers `Corruption` rather than serve it.
    enum class Inserting : std::uint8_t { kFault, kCreate };
    StatusOr<std::span<std::byte, kPageSize>> InsertFrame(PageId page_id, ReservedFrame&& slot,
                                                          bool dirty, Inserting why,
                                                          bool warm = true);
    // The two creations' common tail: the slot they reserved before
    // claiming an id, zeroed and published dirty, since a brand-new page
    // exists only in its frame until it is written back.
    StatusOr<std::span<std::byte, kPageSize>> PublishFreshPage(PageId page_id,
                                                               ReservedFrame&& slot);

    // The body of both `CreateNew*Unpinned`. A `headerless` create never
    // pops the free list (BF-R6): a headerless page is unlogged and carries
    // no checksum, so after a crash a reused id reads as its dead image
    // rather than as the all-zero "never written" signature. Its id is also
    // zeroed on the device and marked headerless **before its frame exists**
    // (BF-S1's Census A, R1), so no writeback can stamp a checksum - child 1
    // of a directory page - into it while it still reads as headered.
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewFrom(bool headerless);
    // Its id half: a fresh id claimed - the free list first, unless
    // `headerless` - and made addressable, the claim given back if that
    // fails. `popped` says where it came from, for `ReleaseClaim`.
    struct FreshId {
        PageId page_id = kInvalidPageId;
        bool popped = false;
    };
    StatusOr<FreshId> ClaimFreshId(bool headerless);
    // The ids `ClaimNewId` claimed and no create has published yet: the slot
    // each holds and where its id came from. Under `map_latch_`.
    struct HeldClaim {
        std::uint32_t slot = 0;
        bool popped = false;
    };
    std::unordered_map<PageId, HeldClaim> claimed_;
    // Takes `page_id`'s entry out of `claimed_`, or nullopt for an id this
    // store never claimed.
    std::optional<HeldClaim> TakeClaim(PageId page_id) noexcept;
    // The headerless arm's zero-write and mark, run on a claimed id with no
    // latch held and no frame inserted yet.
    Status MarkHeaderlessBeforeInsert(PageId headerless_page);

    // Pops the free list under both holds (BF-R6): the lowest listed id
    // whose bit is still clear and which is neither resident nor in
    // flight, claimed - bit set, counted - in the same hold.
    // `kInvalidPageId` when none is ready.
    PageId PopFreeListLocked();

    // Undoes a claim whose create failed before its frame existed: the bit
    // cleared and the count restored, a popped id back on the list.
    void ReleaseClaim(PageId page_id, bool popped) noexcept;

    // ---- Slots (BE-R1) ---------------------------------------------------
    //
    // **A slot is reserved before it is filled** - before a miss's device
    // read, and before a creation claims its id - so the slots in use, which
    // the capacity bounds, include every fill in flight (BE-R1). Under the
    // limit it takes a slot; at it, it reclaims in bounded batches (BE-R2)
    // with the structure latch released between them, until a batch frees
    // one or the walk ends (`ReclaimWalk`). Then BE-R4: in drain mode the
    // dirty queue is written back and the walk runs again; otherwise the
    // walk is retried `kRefuseRetries` times, and the fill is refused
    // `ResourceExhausted` - or, inside a window whose share is spent, the
    // instance is stopped (`FailStop`). Nothing on this path writes back
    // outside drain mode, and nothing waits.
    //
    // **The limit**: outside a window, the capacity less every open
    // window's unspent share; inside one with share left, the capacity. A
    // window's fill spends its share as it goes, so `in use + promised`
    // never exceeds the capacity and a window with share left always finds
    // a slot.
    StatusOr<std::uint32_t> ReserveFrame();
    // Logs and aborts: a fill inside a window that the pool cannot serve.
    [[noreturn]] void FailStop() const;
    // Gives a slot back to the free list - a reserved slot never published
    // (a failed read, a lost race) or a released frame's - poisoning its
    // bytes in debug builds. Latch held.
    void ReturnSlotLocked(std::uint32_t slot) noexcept;
    // **The erasers' one tail**: unmaps a published, unpinned frame and frees
    // its slot. Latch held, and the frame's refusals already asked by the
    // caller.
    void ReleaseFrameLocked(Frame& frame) noexcept;
    // A slot's page bytes, and the page it holds (`kInvalidPageId` while free
    // or reserved: the hand skips those).
    Page& SlotPage(std::size_t index) noexcept {
        return *reinterpret_cast<Page*>(chunks_[index / kFrameChunk].pages.get() +
                                        (index % kFrameChunk) * kSlotStride);
    }
    // **A slot is a page and one cache line** (BE's close): pages laid at an
    // exact 8 KiB stride map their headers and slot directories - what every
    // walk reads first - onto the same few cache sets, and evict each other.
    // Measured on a resident 64k-page pool: 33.5 ns a hit at 8 KiB stride
    // against 23 ns for pages `malloc`ed one at a time, whose allocator
    // header broke the stride by accident. One line of padding breaks it on
    // purpose, for 0.8% of the pool's memory.
    static constexpr std::size_t kSlotStride = kPageSize + 64;
    PageId& SlotOwner(std::size_t index) noexcept {
        return chunks_[index / kFrameChunk].owners[index % kFrameChunk];
    }
    static constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;
    // Returns a reserved slot to the free list unless `InsertFrame` took
    // it, so every early return between a reservation and its publish -
    // a failed read, a checksum refusal, a throw - gives it back.
    class ReservedFrame {
    public:
        ReservedFrame(DevicePageStore& store, std::uint32_t slot) noexcept
            : store_(store), slot_(slot) {}
        ~ReservedFrame() {
            if (slot_ == kNoSlot) return;
            LatchGuard structure(store_.structure_latch());
            store_.ReturnSlotLocked(slot_);
        }
        ReservedFrame(const ReservedFrame&) = delete;
        ReservedFrame& operator=(const ReservedFrame&) = delete;

        std::span<std::byte, kPageSize> bytes() const noexcept {
            return std::span<std::byte, kPageSize>(store_.SlotPage(slot_));
        }

    private:
        friend class DevicePageStore;
        DevicePageStore& store_;
        std::uint32_t slot_;
    };

    // `IsPinnedClass` for a frame the caller already holds, so the sweep
    // asks it per step without a second lookup.
    bool IsPinnedClassFrame(const Frame& frame) const noexcept;

    // The sweep body, with the structure latch **already held**: reclaims
    // up to `want` frames, walking at most `max_steps` slots from the hand.
    // `EvictColdFrames` asks for full laps; the bounded batches (BE-R2) ask
    // for `ReclaimBatch()` frames in `kBatchStepsPerFrame` times as many
    // steps.
    struct SweepResult {
        std::size_t reclaimed = 0;
        std::size_t steps = 0;
        bool progressed = false;  // a usage counter came down: a later batch may reclaim
    };
    SweepResult SweepLocked(std::size_t want, std::size_t max_steps);
    // One inline batch (BE-R2) at the reservation's or a window's limit,
    // booked in the inline counters. Latch held.
    SweepResult InlineBatchLocked();
    std::size_t SlotCountLocked() const noexcept { return slot_count_; }
    std::size_t SlotsInUseLocked() const noexcept {
        return SlotCountLocked() - free_slots_.size();
    }
    // **What bounds a reclaim loop that drops the latch between batches**
    // (BE-R2), shared by the reservation and `MaintainFreeReserve`. It stops
    // after one lap in which no batch freed, cleaned or lowered anything -
    // a pool that another lap cannot change - and, whatever happens, after
    // `kClockUsageCap + 1` laps in all: other cores' hits can keep raising
    // counters this loop lowers, so "progress" alone need never end.
    struct ReclaimWalk {
        std::size_t idle = 0;
        std::size_t total = 0;
        bool Done(const SweepResult& batch, bool progressed, std::size_t slots) noexcept {
            total += batch.steps;
            idle = progressed ? 0 : idle + batch.steps;
            return batch.steps == 0 || idle >= slots || total >= slots * (kClockUsageCap + 1);
        }
    };
    // Pops a free slot, adding a chunk when there is none.
    std::uint32_t TakeSlotLocked();
    Status EnsureAddressable(PageId page_id);

    // The two bitmaps covering `page_id`, read-only, answering as an empty
    // map for a region that does not exist. Empty is the *correct* answer
    // there and not a fallback: a region no mount ever created holds no
    // allocated page, so every bit in it reads clear. This is what lets
    // IsAllocated and IsHeaderless stay `const noexcept` across a map that
    // is no longer wholly resident - see the note above map_regions_.
    std::span<const std::byte, kPageSize> free_map_bytes_for(PageId page_id) const noexcept {
        const MapRegion* region = FindRegion(FreeMapRegionOf(page_id));
        return std::span<const std::byte, kPageSize>(region != nullptr ? region->free_map
                                                                       : AbsentRegionPage());
    }
    std::span<const std::byte, kPageSize> headerless_map_bytes_for(
        PageId page_id) const noexcept {
        const MapRegion* region = FindRegion(FreeMapRegionOf(page_id));
        const bool present = region != nullptr && region->headerless_map != nullptr;
        return std::span<const std::byte, kPageSize>(present ? *region->headerless_map
                                                             : AbsentRegionPage());
    }

    // Creates this region's headerless bitmap if it has none yet. The one
    // caller is MarkHeaderlessBeforeInsert - the moment the fact the
    // bitmap records stops being "no".
    StatusOr<std::span<std::byte, kPageSize>> EnsureHeaderlessMap(PageId page_id);

    PageDevice& device_;
    // Syncs `device_`. Declared second, so it is destroyed (its thread
    // joined) after the store's later members; its thread touches only
    // `device_`, which outlives the store.
    DataSyncer data_sync_{device_};
    Logger* log_ = nullptr;
    wal::WalDurability* wal_gate_ = nullptr;
    // One slot per core id the page latch can name (SetCoreWalGate); null
    // means "ask `wal_gate_`". Atomic so that each slot's one writer and its
    // core's readers are ordered by a store and a load, not by a promise.
    std::array<std::atomic<wal::WalDurability*>, kPageLatchMaxCoreId + 1> core_wal_gates_{};

    // The page latch's switch and gauge (SetLatchArmed). Off by default:
    // arming is the assembly's act, on the superblock's core count.
    bool latch_armed_ = false;
    // Set by Open()'s debug census override and never cleared: SetLatchArmed
    // keeps the store armed once this is true.
    bool latch_forced_ = false;
    // SetAfterWritebackCopyForTest's hook; empty outside a test.
    std::function<void(PageId)> after_writeback_copy_for_test_;
    // BF-S2's seams; empty outside a test.
    std::function<void(PageId)> after_fault_published_for_test_;
    std::function<void()> after_map_copy_for_test_;

    // ---- AM-S2's structure latch --------------------------------------
    //
    // Guards the frame **table** and its bookkeeping: `frames_`, the slot
    // array and free list, `clock_hand_`, `live_pins_`, `pin_high_water_`,
    // `dirty_eviction_queue_`, `window_promised_`, `dirty_gens_` and each
    // frame's `pins`, `dirty`, `rec_lsn`, `writing` and `dirty_gen`. Under
    // it: the pin accounting, the insert, all three erasers
    // (`ReleaseScanSlot`, `FreePage`, the sweep), the stamp and dirty mark
    // (AM-S2 step 3e) and the dirty enumerations (step 3f). The armed
    // fault, which runs with it released, reaches the table only under holds
    // of its own (`FaultIn`). Still read without it, from tests only: the
    // `*Unpinned` accessors' path through `Resolve`, `IsPinnedClass`'s lookup
    // and the test hooks.
    //
    // **Null where the store is not shared** (`LatchGuard`'s shape, G2: a
    // null test at `cores = 1`). Armed by the same switch as the page latch.
    //
    // **Acquisition order: the page latch is taken OUTSIDE this one, and
    // this one is never held across device I/O.** `PinFrame` takes the pin
    // under this latch, releases it, then waits for the page latch - the pin
    // keeps the frame from eviction while it waits. A miss drops it before
    // the device read (the `loading_` set below), and `FetchPinned` /
    // `CreatePinned` latch across fetch-and-pin, so no frame is evicted
    // between the two.
    //
    // `mutable` so the const readers (`DirtyPageIds`,
    // `DirtyPagesWithRecLsn`, `resident_pages()`) can take it.
    mutable Latch frames_latch_{LatchKind::kFrameTable};
    Latch* structure_latch() const noexcept { return latch_armed_ ? &frames_latch_ : nullptr; }

    // ---- The free map's latch, and the order (AM-S3) ---------------------
    //
    // **A second latch because the two are nested**: `FetchPinned`'s hit
    // path holds the frame table across `Resolve`, which asks `IsAllocated`
    // (and on a miss `FaultIn` asks `IsHeaderless`) - free-map reads that would
    // re-acquire a non-recursive latch (`base/latch.hpp`) if the map shared
    // `frames_latch_`.
    //
    // **The declared order is `frames_latch_` then `map_latch_`, never the
    // reverse** (`docs/rules/rules.md` §3): the allocation paths end their
    // map hold before `InsertFrame`, and region loading does its device
    // work outside both. `AssertOrderBeforeFrames` checks it in debug.
    //
    // What it guards: `map_regions_` itself - a `std::map` that region
    // creation *inserts* into while other cores are inside `find` - and
    // every `MapRegion` in it, including the lazily installed
    // `headerless_map` pointer and the `dirty` flag.
    mutable Latch map_latch_{LatchKind::kFreeMap};
    Latch* map_latch() const noexcept { return latch_armed_ ? &map_latch_ : nullptr; }

    // ---- AM-S2 step 2b: the loading set ---------------------------------
    //
    // The page ids a fault is in flight for, and the only reason the miss
    // path can drop the structure latch before the device read.
    //
    // **A set of ids rather than a flag on `Frame`.** The read fills a
    // reserved slot that is not in the table until `InsertFrame` publishes
    // it (BE-R1), so a second fault cannot find it by id - and making it
    // findable would put a slot with unread bytes in front of every other
    // reader of the table. An id here is invisible to all of them.
    //
    // **What it buys is the dedup**: a second core missing the same page
    // **waits** instead of reserving a second slot and issuing a duplicate
    // read whose `InsertFrame` would race the first.
    std::unordered_set<PageId> loading_;
    // Broadcast when a load finishes, either way. Waiters re-check their own
    // page and sleep again if it was not theirs: loads are rare against
    // hits, and a fault in flight has no frame to hang a condition variable
    // on (BE-R1).
    std::condition_variable_any loading_done_;

    // FM2: the resident map pages, keyed by region (free_map.hpp's
    // placement arithmetic). Ordered rather than hashed so a flush writes
    // regions in ascending id order - deterministic, and the order
    // WriteBack's run coalescing would want if map pages ever joined it.
    //
    // **Not frames** (D4 of docs/inflight/in-progress/workplan-multi-free-map.md
    // at 1769487): store-owned, which makes FlushMaps' maps-after-data order
    // trivially true and keeps them out of the checkpoint dirty table and
    // away from PageRef, at the cost of a second cache in the store.
    //
    // Every region the device holds is loaded at mount, so "resident" and
    // "exists" are the same set and a missing key means the region was
    // never created. That equivalence is what the const accessors above
    // rest on, and it is why they may answer from an empty page instead of
    // faulting - a `const noexcept` predicate cannot read a device.
    std::map<std::uint32_t, MapRegion> map_regions_;

    // FM6's whole-instance fast path: false until some region holds a
    // headerless bitmap, so a database with no Waystone directory answers
    // IsHeaderless (fault path, writeback, WAL gate) with no lookup. Seeded
    // at mount, moved by the one writer that can change it.
    // **Atomic, because it gates the latched read** (AM-S3's review):
    // `IsHeaderless` reads it before the map hold, and a torn read of a
    // `bool` set on another core would let `StampIfHeadered` write a
    // checksum into a headerless page - silent corruption (invariant 14).
    // Release on the store, acquire on the load: a reader that sees `true`
    // sees the installed bitmap. It never returns to `false` - nothing
    // removes a headerless page, and `FreePage` refuses one `Corruption`.
    std::atomic<bool> any_headerless_{false};

    // D8(a): the instance's allocated-page count, maintained rather than
    // swept. Seeded at mount, raised by every site that sets a free-map bit
    // and lowered by `FreePage` (BF-R5); under `map_latch_`.
    std::uint32_t allocated_pages_ = 0;
    PageId next_new_page_id_;
    // `Open`'s `first_new_page_id`: the system range below it is never
    // freed (BF-R5).
    const PageId system_floor_;

    // First id that may ever be evicted; everything below is resident by
    // class (EV3).
    //
    // **0 means nothing has been declared resident**: this layer does not
    // know the catalog's page layout. `Expeditor::Open` installs the first
    // id that is not a fixed system structure (`SetResidentLimit`).
    PageId first_evictable_page_id_ = 0;

    // §4's dirty queue: what the sweep found dirty at usage zero, written
    // back by `DrainDirtyEvictionQueue` (core 0's cadence, and drain mode).
    std::vector<PageId> dirty_eviction_queue_;

    // The clock hand (BE-R1): the slot index the next sweep step visits,
    // advancing modulo the slots that exist. One step is one slot, O(1);
    // the sort that re-found an id-valued hand per sweep is gone with it.
    std::size_t clock_hand_ = 0;
    std::size_t capacity_ = 0;  // set once, at Open
    // What `Open` was asked for, before the debug override lowered it.
    std::size_t configured_capacity_ = 0;
    // Frames promised to open no-refuse windows and not yet spent, under
    // the structure latch.
    std::size_t window_promised_ = 0;
    std::size_t live_pins_ = 0;
    // The source of every `Frame::dirty_gen` (AT-S8 step 1b), under the
    // structure latch.
    std::uint32_t dirty_gens_ = 0;
    // `kPinCeiling` scaled by how many threads may pin this store at once
    // (`SetLatchArmed`). One operation's bound times the operations in
    // flight; unscaled it is the per-operation number and the assert below
    // fires on correct traffic the moment two threads pin together.
    std::size_t pin_ceiling_ = kPinCeiling;
    std::size_t pin_high_water_ = 0;

    // ---- The slot array (BE-R1) -----------------------------------------
    //
    // Chunks of `kFrameChunk` slots, each with one allocation of page bytes
    // that never moves while the store lives, and the id each slot holds -
    // what the hand walks. The chunk vector itself may reallocate; a chunk's
    // arrays do not, so every span survives growth. Allocated with
    // `make_unique_for_overwrite`: a slot's bytes cost the process nothing
    // until a page is read or built into it. Slots are kept once allocated -
    // the capacity bounds them, and what is resident within them.
    struct FrameChunk {
        std::unique_ptr<std::byte[]> pages;  // `kSlotStride` apart
        std::unique_ptr<PageId[]> owners;
    };
    std::vector<FrameChunk> chunks_;
    // Slots that exist: whole chunks, and a last one cut at the configured
    // capacity (`TakeSlotLocked` says why not `capacity_`).
    std::size_t slot_count_ = 0;
    // Free slots, popped from the back. A reserved slot is on neither this
    // list nor the table.
    std::vector<std::uint32_t> free_slots_;
    // The page table: each resident page's frame. Every structural change -
    // publish, unmap - is under the structure latch.
    std::unordered_map<PageId, Frame> frames_;

public:
    // Slots per chunk: 1,024, about 8 MiB of padded pages (BE-R1, BE-Q5). The
    // unit the array grows by and nothing else.
    static constexpr std::size_t kFrameChunk = 1024;

    // The array's census, for cells and `SHOW META`: slots that exist, and
    // how many of them are free. Every slot is free, reserved or resident,
    // so with no fill in flight `slots == free + resident_pages()`.
    struct FrameSlots {
        std::size_t slots = 0;
        std::size_t free = 0;
    };
    FrameSlots frame_slots() const {
        LatchGuard structure(structure_latch());
        return FrameSlots{SlotCountLocked(), free_slots_.size()};
    }

    PoolCounters pool_counters() const override;

    Status BeginNoRefuseWindow(std::size_t frames) override;
    void EndNoRefuseWindow() noexcept override;
    void BeginDrainOnPressure() noexcept override;
    void EndDrainOnPressure() noexcept override;

private:
    // BE-R2's counters (`PoolCounters`). Relaxed atomics: the hit path
    // counts outside the structure latch on an unarmed store's raw
    // accessors, and a gauge needs no order.
    struct Counter {
        std::atomic<std::uint64_t> value{0};
        void Add(std::uint64_t n = 1) noexcept { value.fetch_add(n, std::memory_order_relaxed); }
        std::uint64_t Load() const noexcept { return value.load(std::memory_order_relaxed); }
    };
    Counter hits_, misses_, reclaimed_inline_, reclaimed_background_, batches_inline_,
        batches_background_, batches_partial_, batch_steps_, batch_steps_background_,
        dirty_queued_, dirty_drained_, refused_;
};

}  // namespace kds::storage
