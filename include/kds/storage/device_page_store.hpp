#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>
#include <utility>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/base/log.hpp"
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
// (kHeaderlessMapPageId). It cannot be an in-memory side table: this store
// never evicts, so a page comes off the device exactly once, on first touch
// after open - which is exactly when a verify would reject it. It cannot be
// recomputed from the catalog either, because the store is opened before
// bootstrap has a catalog to ask.
//
// The cost is that these pages carry no damage detection at all, so the
// mechanism is only appropriate for a structure whose corruption is
// *survivable* - one a reader validates against an authoritative source
// and can fall back from. Exactly one page class qualifies today: the
// interior pages of the waystone directory (stats/waystone_dir.hpp), whose
// 2048 child ids tile the page exactly and whose damage costs a lookup that
// falls through to the authoritative path. A database with no waystone
// directory pays one reserved page for the bitmap and nothing else.
//
// Not here, deliberately:
//   - No eviction. Everything touched stays resident, as InMemoryPageStore
//     already did. Clock eviction needs PageRef (page.md section 3) and
//     the frame-reclamation policy is an open decision in CLAUDE.md.
//   - Dirty tracking is by which accessor the caller chose, not by what it
//     actually wrote: Get() marks the frame dirty, GetForRead() leaves it
//     alone, and both hand out the same raw mutable span. A reader that
//     calls Get() costs a needless write-back; a writer that calls
//     GetForRead() loses its write. PageRef (page.md section 3) is what
//     replaces the convention with a type.
//   - One free-map page, so coverage is kFreeMapBitsPerPage ids; beyond
//     that is OutOfRange, not silently unmapped.
//
// ---- The WAL gate (page.md section 8, wal.md section 8-1) ---------------
//
// A dirty page may reach the device only once the log records describing
// its modifications are durable. That rule is enforced here rather than
// asked of callers: SetWalGate() installs a WalDurability, and every write
// path below (Flush, Sync, FlushPages) first calls EnsureDurable() on the
// highest page_lsn among the pages it is about to write. With no gate
// installed the store behaves exactly as it did before one existed, which
// is what the WAL-free unit tests and the simulator rely on - and which is
// sound only for a caller that logs nothing.
//
// The gate is one call per flush batch, not per page: EnsureDurable() is a
// no-op once the watermark has passed, so gating on the batch maximum
// costs at most one sync for the whole batch instead of one per page.
//
// StampPageLsn() is the other half. A mutation path appends its record,
// then calls it with the record's LSN; that both records the page_lsn the
// gate reads and captures the frame's **recLSN** - the LSN of the first
// record to dirty the frame since it was last clean, which is what a
// checkpoint's dirty table must carry for recovery's redo start to be
// correct (wal.md section 11-1). A frame keeps that value until it is
// written back, then drops it.
//
// Concurrency: core-local, no internal synchronization (rules.md #3) -
// **but for the page latch word**, below, which is the first thing in the
// storage layer another thread may touch.
//
// ---- The page latch (AM-R3, AR2-R2) ---------------------------------------
//
// Every frame carries one 32-bit word (`Frame::latch`, page_latch.hpp): an
// exclusive bit, the owning core, and a count. **Armed only at `cores > 1`**
// (SetLatchArmed, from the superblock's core_count - the same fact the
// stream latch arms on, without its topology conjunct: frames have no
// stream); unarmed - `cores = 1`, every hand-built store, the simulator -
// the accessors never read or write the word, which is G2's zero overhead
// as a property of the code rather than of a build flag. In debug builds
// `KDS_TEST_PAGE_LATCH=1` arms every store so the whole suite runs armed.
//
// What it protects through M1: a frame against a concurrent *reader* on
// another core, and against the writeback and sweep paths (AM-R1: writes
// still route to the relation's owner). In AM-S1 the pools are still per
// core, so no second core can reach a frame and the latch is inert; the
// primitive, its order and its cells are what that stage lands. Writeback,
// StampPageLsn, the hit path's usage bump and the scan ring still touch
// frames unlatched - AM-S2 and AM-S3 own those.
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
// core (a per-task owner is AM-S2's escalation if the audit ever records
// one). **Never upgraded**: a task holding a page shared may not ask for
// it exclusive. That is a self-deadlock, aborted in debug naming the page
// (PinFrame - a thread-local multiset of the pages this thread holds
// shared, since AM-S2 step 3b; it read this store's own `pins > 1` as
// "the shares are mine", which one table serving every core makes
// indistinguishable from two cores holding one pin each) and an unbounded spin on the reactor thread in release, which
// a recursive std::mutex at least blocks on (base/latch.hpp).
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
//   - **Held across a durability wait only on the fault path.** WriteBack
//     takes the WAL gate (EnsureDurable, a wait on the writer thread)
//     before it writes any byte, and it takes no page latch at all today,
//     so no frame is latched across the wait; but the sweep that reached
//     WriteBack runs inside a fault, and the faulting task may hold *other*
//     frames latched while it waits. Sound, because the writer thread takes
//     no page latch; a latency cost under a shared pool, and AM-S3's to
//     measure. **AM-S2 inherits one obligation here**: AwaitWalGate reads
//     each frame's page_lsn *before* the gate call, so whatever latch that
//     scan comes to need must be dropped before EnsureDurable, or the wait
//     acquires exactly the "latched across a durability wait" shape this
//     bullet rules out.
//   - **Never nested with the visibility window latch** in either
//     direction: that latch is taken holding nothing (AN-R9), and no path
//     holds a PageRef at commit.
//   - **Never across a park**: the suspend audit's `live_pins() != 0`
//     covers it in debug builds, recording rather than failing, because
//     the pin and the latch share a handle; nothing covers it in release.
//   - **Page against page: unordered through M1, and AM-S2 owes the
//     rule.** Two page latches are held at once on every split (the old
//     leaf and its new sibling), every chain growth (the old tail and
//     the new page), and - since AM-R8 - **a scan ring's shared hold on a
//     heap leaf across its consumer's var-heap fetches**: the ring drops
//     its hold at the *next* `Fetch`, so the Cabin build's spill fetches
//     (`cabin_optimizer_exec.cpp`, phase 2) run inside it, `S(leaf)` then
//     `S(var-heap page)`. Shares never block shares, so a cycle needs an
//     exclusive waiter on both, and today there can be none: the Cabin
//     build runs on the relation's owner core (`cabin_optimizer_exec.cpp`,
//     `ServableBy`) and every write to that relation routes to the same
//     owner, so the ring's consumer and the only writer of both pages are
//     one thread. **That is the invariant keeping this pair safe, and
//     nothing enforces it** - it dissolves the day a cross-core write path
//     lands, which is the shape to check when the order below is finally
//     stated rather than one to leave unlisted.
//     A descent holds one at a time, its handle dying per
//     iteration; through M1 one core
//     owns its pool, so no two holders of different pages can ever wait on
//     each other and no order is needed. The shared pool is where an ABBA
//     becomes possible, and the tree's own shapes - parent before child,
//     old before new - are what AM-S2 will state as the order. AM-S2 also
//     inherits: the self-deadlock check's `pins` proxy (PinFrame); a
//     re-validation after the re-fetch btree.cpp's and index_tree.cpp's
//     leaf-for-write paths now do on a dropped read handle; and starvation
//     - shared is granted whenever X is clear, with no writer preference,
//     so a hot page's steady readers can starve an exclusive request.
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

class DevicePageStore final : public PageStore {
public:
    // `device` must outlive the store. A device with no pages, or one whose
    // free-map page was never written, is initialized as a fresh database;
    // otherwise the existing free map is loaded, and a bad checksum or
    // header is Corruption rather than a guess.
    //
    // `first_new_page_id` is where CreateNew() starts looking; pick a value
    // above any id that gets CreateAt'ed.
    static StatusOr<std::unique_ptr<DevicePageStore>> Open(PageDevice& device,
                                                           PageId first_new_page_id = 1);

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override;
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override;
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override;

    // ---- Recovery's high-water repair (RV4, workplan RC04) --------------
    //
    // Raises where CreateNew() starts its search (page_store.hpp).
    //
    // **Note what this does not do: it does not set a free-map bit.** The
    // map is the durable record of which ids *exist*, and marking an id
    // allocated that no page was ever written at would turn Get()'s honest
    // NotFound into a read of whatever bytes the device happens to hold
    // there. The floor is a separate fact - "do not hand this out" - and
    // keeping the two apart is what lets recovery close the hazard without
    // inventing pages.
    //
    // It refused on a **leased** store until AW-S1b: a core allocating from
    // an extent core 0 had reserved never consulted this floor, so raising
    // it there would have been a repair that reported success and changed
    // nothing. Every core allocates from the one free map now, so the floor
    // it raises is the floor every core searches from.
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
    // **Takes the structure latch** (AM-S3), and the reason is sharper than
    // the free map's: the headerless bitmap is a `unique_ptr` installed
    // lazily on first use (FM6), so this reads a pointer another core may
    // be storing to, not merely bits it may be setting.
    bool IsHeaderless(PageId page_id) const noexcept;

    // Writes dirty frames back in page-id order, which is file order
    // (page.md section 13), then the free map. Data pages go first so a
    // crash between them can only orphan a page, never publish one whose
    // bytes never landed. Not durable on its own - Sync() adds the fsync.
    Status Flush();
    Status Sync() override;

    // The two map pages written back if dirty, then the device synced - the
    // maps alone, not the frames. It is what an extent grant needed before
    // it left core 0 - a run of ids a peer would write into had to be
    // allocated on the device first, or a crash freed the run for the next
    // mount's allocator to hand out over committed rows. **That caller went
    // at AW-S1b and the tests are what is left**: `Sync()` is what
    // production reaches for, and this is the narrower operation a cell uses
    // to construct "the claim is durable and the page is not" - the state
    // `ResidentBytes`' all-zero arm answers `NotFound` for.
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
    // best-effort, never a correctness property (§4). The copy exists
    // because frames are separate heap allocations; the zero-copy run
    // arrives with page.md §9's preallocated slab, not here.
    //
    // Returns how many pages it wrote.
    StatusOr<std::size_t> WriteBack(std::span<const PageId> page_ids);

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

    // §4's watermark loop: sweep rotations (each followed by a drain, so
    // queued dirt becomes reclaimable on the next lap) until the free
    // reserve - `pool_frames` minus resident frames - meets `watermark`,
    // or a full rotation reclaims nothing and drains nothing. Returns
    // frames reclaimed.
    //
    // The pool size and watermark are **parameters, not fields**: no
    // bounded pool exists yet (EVT02's unbuilt half), so this layer owns
    // the loop's shape and EVT02/EVT04 will own its numbers. Nothing calls
    // it in production until then - the same stance the sweep itself takes.
    std::size_t MaintainFreeReserve(std::size_t pool_frames, std::size_t watermark);

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

    // Installs the WAL-before-data gate described above. Null (the
    // default) disables it. `gate` must outlive the store.
    void SetWalGate(wal::WalDurability* gate) noexcept { wal_gate_ = gate; }

    // **`SetStreamCoreId` and `core_id()` are gone** (AM-S2 step 3). The
    // first existed for an ordering that no longer exists: recovery stamps
    // pages before the lease may be installed, so a peer had to be told its
    // id early or it stamped core 0's onto its own pages. `CurrentCore()` is
    // set by the thread rather than by the object, so `CoreRuntime::Open`'s
    // guard supplies it for the whole mount pass and there is no ordering
    // left to get wrong. The second had one caller, a cell asserting the
    // default.

    // **Suppresses the PL-C stream stamp on the mutation path** for the
    // length of a recovery pass that is not this core's own (AR0 M0,
    // AL-R5/AL-R6). Under one stream core 0's mount pass replays and rolls
    // back records belonging to *every* core, and the stamp is a claim on
    // the page - it said which stream's records may name it, and until
    // AW-S1b it was also read at the next fault to decide who may write.
    // Stamping core 0 onto a page core 2 owns therefore did not merely
    // mislabel it: core 2 faulted the page, was granted nothing, and
    // **could never write its own page again**, because a heap data page was
    // in no relation write grant and the extent lease was re-drawn each
    // mount.
    //
    // Redo skips its own restamp under one stream (`wal/redo.cpp`), but
    // undo's compensations reach this function through the ordinary
    // mutation path (`txn/recovery_undo.cpp`), so the suppression has to
    // live here, where every writer passes, rather than at each caller.
    // The page_lsn is still stamped: idempotence is that field's job and it
    // is not ownership.
    //
    // Set for the mount pass and cleared after it; a store serving
    // statements never has it on, so the ordinary write path is unchanged.
    void SetStampSuppressed(bool suppressed) noexcept { stamp_suppressed_ = suppressed; }
    bool stamp_suppressed() const noexcept { return stamp_suppressed_; }

    // **There is no read predicate.** `MayFault` stood here until AW-S1b -
    // "core 0 may reach anything; any other core may reach ids inside an
    // extent it was granted, plus the fixed system range read-only" -
    // enforced on the frame-load path in debug builds only. One frame table
    // serves every core, so every core faults every page and the question
    // has no content.

    // Whether this store's core may **write** `page_id` - i.e. take a frame
    // it is allowed to dirty.
    //
    // Narrower than reading, which no predicate gates at all since the pool
    // became one: the system range is readable by every core and writable
    // only by core 0. That asymmetry is the whole of P6's soundness.
    // Catalog pages have exactly one writer, so a peer's view can be stale
    // (which is a retryable "not found", crosscore.md §5) but never torn by
    // a second writer.
    //
    // **One question since AW-S1b**, where it used to ask four: the lease,
    // a write grant, a stamp claim and the system range. The first three
    // were a leased store's, and a leased store no longer exists - one
    // frame table serves every core, so who may write a *user* page is the
    // Expeditor's routing decision and not this layer's. What survives is
    // the asymmetry AM-R2 and AO-R14 keep: the system range is readable by
    // every core and writable only by core 0.
    bool MayWrite(PageId page_id) const noexcept override;

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

    // Writes back exactly these pages and syncs. Ids that are not dirty,
    // or not resident, are skipped rather than treated as an error -
    // something else may have flushed them since the caller's snapshot.
    Status FlushPages(std::span<const PageId> page_ids);

    // Drops these pages' frames so the next access re-reads them from the
    // device. Ids that are not resident are skipped.
    //
    // **This is what makes a peer's cache invalidation mean anything**
    // (docs/inflight/in-progress/workplan-crosscore.md P6). A peer holds catalog pages this
    // store faulted at some earlier moment; core 0 then does a DDL and
    // flushes. Dropping the *catalog* cache is not enough - the next scan
    // would read the same stale frame back and reach the same conclusion.
    // The bytes have to go too.
    //
    // Refuses with InvalidArgument if any named page is **dirty**:
    // evicting a dirty frame silently discards a write. On a peer they
    // never are - the pages it evicts are exactly the ones it may not
    // write - so the check guards against this being called somewhere it
    // does not belong rather than against normal operation.
    Status EvictClean(std::span<const PageId> page_ids);

    // Diagnostic log, null (discard) by default. Set after Open(), since
    // the store has to exist before a server has anything to log about;
    // `log` must outlive the store.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    // ---- Frame reclamation (docs/inflight/in-progress/workplan-eviction.md EV01-EV02) -------
    //
    // Read that document's §0 before changing anything here. The short of
    // it: raw spans from `Get()` are only safe because nothing evicts, so
    // the pinned accessors below are the seam EV03 migrates every caller
    // onto, and **eviction must stay off until it has** (EV2).

    // A move-only RAII handle that keeps its frame resident for as long as
    // it lives (`page.md` S2). Construction pins, destruction unpins.
    //
    // Copy is deleted rather than refcounted: two handles to one frame is
    // not a thing any caller needs, and making it expressible would make
    // "who unpins" a question. Move leaves the source empty, which is what
    // lets a handle be returned from a factory without a double unpin.
    // The engine-wide pinned handle (page_store.hpp, MG01). This used to be
    // a nested class with exactly the same shape; the base one replaced it
    // when the pin hooks became PageStore virtuals, and the alias keeps the
    // existing spelling `DevicePageStore::PageRef` meaning the same thing.
    using PageRef = storage::PageRef;

    // The pre-MG01 spellings of the pinned accessors, kept for their
    // existing callers until MG06 folds them into plain Get()/GetForRead().
    StatusOr<PageRef> PinnedGet(PageId page_id) { return Get(page_id); }
    StatusOr<PageRef> PinnedGetForRead(PageId page_id) { return GetForRead(page_id); }

    // Whether `page_id` belongs to a **pinned class**
    // (`docs/spec/eviction.md` EV3): never a sweep candidate at any
    // pressure. v1's classes are the fixed catalog pages and - when AST04
    // lands - Bound Cabin pages.
    //
    // **A finding against EV3, recorded here because it changes how the rule
    // can be implemented.** EV3 says pinning is "a page-class attribute …
    // resolved from page kind at load", and for a Bound Cabin that works,
    // because it gets a page type of its own. It does **not** work for the
    // fixed catalog pages: they are formatted `PageType::kHeap`, exactly as
    // a user relation's pages are, so the page kind cannot tell them apart.
    // The id range is the only thing that can, and it is a sound
    // discriminator because those ids are reserved and fixed
    // (`catalog/well_known.hpp`). So the rule is implemented as
    // *id-range-or-kind*, and EV3's "resolved from page kind" is true of one
    // of its two v1 classes rather than both.
    //
    // **Both halves are live.** The kind half answers for
    // `PageType::kCabinBound` (AST04): a Bound Cabin page carries the
    // aggregate an admission check reads, so reclaiming one takes a
    // *constraint* out of memory. It is asked only of a resident frame -
    // reading a header off the device to answer would turn a skip test into
    // an I/O, and a non-resident page cannot be a sweep candidate anyway.
    bool IsPinnedClass(PageId page_id) const noexcept;

    // Raises the pinned id range's upper bound. **Additive only** - the
    // limit never falls, because a structure that declared itself
    // un-evictable and then found itself evictable is exactly the failure
    // the declaration exists to prevent.
    //
    // **It is the write boundary too since AW-a, so this is no longer a
    // residency-only knob.** The two members that carried this one quantity
    // were collapsed into it, so `MayWrite` reads *this* value as "the
    // system range". Raising it therefore moves an authorization boundary:
    // a raise to `N` makes every page below `N` writable only from core 0.
    //
    // So the only value any caller may install is the volume's own layout
    // boundary (`server::kFirstUserPageId`). **AST04 must not use this**:
    // a Bound Cabin page is allocated above the system range, and declaring
    // it resident by raising the floor past it would take every user page
    // beneath it out of the peers' write set. Its residency needs the kind
    // half of `IsPinnedClass` (which already answers for
    // `PageType::kCabinBound`) or a pin, not this.
    //
    // **Install-time only**, and that is what makes it safe to read
    // unlatched: the member is a plain `PageId` that `MayWrite` reads from
    // every core's thread, so a raise after the peers exist would
    // be a data race on the shared store as well as an authorization
    // change. `Expeditor::Open` sets it before the first peer is built.
    void SetResidentLimit(PageId first_evictable_page_id) noexcept;

    // The floor, for the assembly cell that checks it was installed. The
    // *mechanism* has cells of its own (`eviction_test.cpp`); what has no
    // other witness is whether anything called the setter, which is the
    // failure this class has now seen twice - a wiring line whose absence
    // changes no result and no cell (`AttachWakers`, AU-S1b).
    PageId first_evictable_page_id() const noexcept { return first_evictable_page_id_; }

    // ---- The frame budget: what arms the sweep (MG06) -------------------
    //
    // How many frames may stay resident. 0 - the default - means unbounded,
    // which is exactly the pre-eviction behaviour; a nonzero budget makes
    // every fault that pushes residency past it run the CLOCK sweep for the
    // excess, inline, on the faulting path (EV5's on-demand trigger). The
    // page just faulted is never its own victim: its usage counter was just
    // bumped, and the sweep decrements before it reclaims.
    //
    // In debug builds `KDS_TEST_FRAME_BUDGET` in the environment overrides
    // an unset budget at Open() - which is how MG05 runs the entire suite
    // under brutal eviction pressure without threading a knob through every
    // fixture.
    void SetFrameBudget(std::size_t frames) noexcept { frame_budget_ = frames; }
    std::size_t frame_budget() const noexcept { return frame_budget_; }

    // ---- The page latch's switch (AM-S1) ---------------------------------
    //
    // Armed from the superblock's core count by the assembly (core 0 in
    // Expeditor::Open, a peer in CoreRuntime::Open) and by nothing else:
    // a store nobody armed - `cores = 1`, every hand-built one - never
    // touches a latch word. The header's "The page latch" section says
    // what arming buys and what it costs.
    // The debug census (`KDS_TEST_PAGE_LATCH=1`, Open()) arms first and
    // wins: the assembly's own call cannot disarm a store the census armed,
    // or every assembly-driven fixture would run the census unarmed at one
    // core and the "whole suite ran armed" claim would be narrower than it
    // reads.
    // `concurrent_pinners` re-scopes `kPinCeiling` rather than adding a
    // second knob for the same quantity (`CLAUDE.md`'s rule). **The ceiling
    // is a *per-operation* bound and `live_pins_` was only ever a valid
    // proxy for it because one core ran one operation at a time.** AM-S2
    // breaks that coupling: once several threads pin one store, the global
    // sum is the per-operation bound times the number of operations in
    // flight, and asserting the unscaled value aborts the process on
    // entirely correct traffic - measured, at nine concurrent readers.
    // Callers that pin from one thread pass nothing and get today's bound.
    void SetLatchArmed(bool armed, std::uint32_t concurrent_pinners = 1) noexcept {
        latch_armed_ = armed || latch_forced_;
        pin_ceiling_ = kPinCeiling * (concurrent_pinners == 0 ? 1 : concurrent_pinners);
    }
    bool latch_armed() const noexcept { return latch_armed_; }

    // Test hooks: hold a resident frame's latch as if another core held it,
    // with no pin, so the sweep's and EvictClean's latch refusals can be
    // exercised before a second core can reach a frame (AM-S2). Absent
    // frame: NotFound. The word is read back by `latch_word_for_test`.
    Status LatchFrameForTest(PageId page_id, PinMode mode, std::uint32_t core);
    Status UnlatchFrameForTest(PageId page_id, std::uint32_t core);
    StatusOr<std::uint32_t> latch_word_for_test(PageId page_id) const;

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
    // budget. That is the guarantee `docs/workplan-assertion.md` AST04
    // rests on, and `EvictionTest` asserts it against a sweep that is
    // reclaiming other frames in the same pass, so it is not a tautology.
    //
    // **A dirty frame at usage zero is queued, not reclaimed** (§3.2's
    // fourth branch). The writeback that would clean it is EVT03's - a
    // background-group task doing durable-then-write-then-clean - and
    // reclaiming before that runs would lose a write. `DirtyEvictionQueue()`
    // is what EVT03 will drain.
    //
    // **Nothing calls this yet**, deliberately: `page.md` §3's first line is
    // that raw spans are unsafe the moment eviction exists, and every caller
    // still holds one, so enabling the sweep before the `PageRef` migration
    // would be a use-after-free. It exists now so the pinned-class guarantee
    // a Bound Cabin rests on is testable before that migration lands.
    // The CLOCK usage counter's ceiling (`docs/spec/eviction.md` EV1,
    // `[PROPOSED] 5`). A cap and not a free-running count: it bounds how
    // many sweep rotations a hot frame can survive, so a page that fell out
    // of use cannot hold a frame for an unbounded time on the strength of
    // how popular it once was. **Nothing may depend on the number** - it is
    // the knob EV1 leaves open, and a sweep sizes its own laps from it.
    static constexpr std::uint8_t kClockUsageCap = 5;

    // **Takes the structure latch** (AM-S2 step 3's eraser half). Every
    // caller outside this class reaches it with the latch not held - the
    // eviction cells, and `MaintainFreeReserve`, whose loop must drop it
    // between rotations because the drain in between does device I/O.
    // The fault path's inline sweep does *not* come through here: it runs
    // inside `InsertFrame`, under the hold that insert already has, and
    // calls the `Locked` body directly.
    std::size_t EvictColdFrames(std::size_t budget);

    // Pages the sweep found dirty at usage zero, in the order it found them
    // - §4's dirty queue, populated here and drained by EVT03's writeback
    // task. Cleared by `TakeDirtyEvictionQueue()`.
    std::vector<PageId> TakeDirtyEvictionQueue();

    // Under the hold (AM-S2 step 3f): `size()` on a table another core may
    // be growing is a read of a member the growth writes. Not `noexcept` any
    // more - `LatchGuard` locks a `std::mutex`, which may throw - and no
    // caller relied on it: the log line in `Sync` and the eviction cells.
    std::size_t resident_pages() const {
        LatchGuard structure(structure_latch());
        return frames_.size();
    }

    // How many frames currently hold at least one pin. Test and §11
    // observability: an unbalanced pin shows up here as a number that never
    // returns to its floor.
    std::size_t pinned_frames() const noexcept;

    // The per-operation pin ceiling (MG04, `docs/workplan-pageref.md` §7's
    // open decision, given a first value here). Derivation, from the MG03
    // audit rather than from air: a btree grow path holds at most 4
    // (SplitLeafAndInsert's leaf + created leaf, stacked under
    // PromoteSeparator's parent + created node per level), an outer chain
    // walk adds 1, and index maintenance stacked under a statement adds 2
    // more of its own split path. 8 bounds that with one frame of slack;
    // the debug assert in PinFrame() is what turns the estimate into a
    // measurement, because a workload that exceeds it aborts naming the
    // count rather than quietly holding more of the pool than EV8 assumes.
    //
    // **A scan ring adds 1 to a walk, not `kds.scan_ring_frames`**
    // (AM-R13): it holds one pin, on the page its last Fetch returned. The
    // slot-pinning ring this milestone tried first held one per occupied
    // slot - up to 32 - against a ceiling that scales with *cores* and not
    // with rings, so a Cabin build walking a 17-page relation at
    // `cores = 2` would have aborted a debug build on entirely correct
    // traffic. No cell reached it, and not because the walks are short -
    // `eviction_test.cpp`'s four-slot ring walks twelve pages. What bounded
    // the old model was the *slot* count, since a rotation released the
    // previous occupant's pin, and no cell opens a ring wider than four;
    // the two consumers that take the 32-slot default - the relayout
    // survey and the Cabin build - are reached in the suite only over an
    // `InMemoryPageStore`, whose `OpenScanRing` is the pass-through
    // fetcher. Which is the whole reason to write the arithmetic down here
    // rather than trust the suite.
    //
    // The number is unchanged because the ring's cost is now the same 1 an
    // outer chain walk already contributes.
    static constexpr std::size_t kPinCeiling = 8;

    std::uint32_t allocated_pages() const noexcept;
    // **Takes the structure latch** (AM-S3). The free map is one structure
    // every core reads and grows, and this predicate sits on the fault
    // path, the writeback path and the WAL gate - so it is the most
    // frequent reader of the map a peer may be inserting a region into.
    // A caller already inside the hold wants `IsAllocatedLocked`; asking
    // here would hang, and debug builds abort naming the reader instead.
    bool IsAllocated(PageId page_id) const noexcept;

private:
    using Page = std::array<std::byte, kPageSize>;

    // The one refusal a caller sees when a page id is not allocated here,
    // carrying the id and - on a leased core - which authority answered.
    Status NotAllocated(PageId page_id) const;

    // The gate every accessor goes through, so a third one cannot forget
    // it: the allocation check, the adoption below when it misses, then
    // the frame. `mark_dirty` is the only thing Get and GetForRead differ
    // in.
    StatusOr<std::span<std::byte, kPageSize>> Resolve(PageId page_id, bool mark_dirty,
                                                      bool bump_usage);

    struct Frame {
        std::unique_ptr<Page> bytes;
        bool dirty = false;
        // The page latch word (page_latch.hpp), sitting in the padding
        // after `dirty` so the frame's size does not move. A plain integer
        // rather than a `std::atomic` so `Frame` stays movable (the table
        // moves it in); every access goes through `std::atomic_ref`, and
        // only when the store is armed - unarmed it stays 0 for the frame's
        // whole life.
        std::uint32_t latch = 0;
        // First log record to dirty this frame since it was last written
        // back; 0 when nothing logged touched it. See StampPageLsn().
        wal::Lsn rec_lsn = 0;

        // ---- Reclamation state (docs/inflight/in-progress/workplan-eviction.md EV01) --------
        //
        // How many live `PageRef`s hold this frame. **Plain, non-atomic,
        // core-local** - `page.md` §6 makes that the model rather than a
        // shortcut: one pool per core, and through M1 cross-core page
        // access does not exist (the latch word above is the one field a
        // shared pool will read from another core; the pin count is not).
        // **AM-S2 decided against making this atomic** - the pointer this
        // sentence used to carry to "AM-S2 owns that change" is retired:
        // step 1 put pin *accounting* under the structure latch, so an
        // atomic here would be redundant with it. **Every mutation is now
        // latched**: the last unlatched one was `ResidentBytes`' inline-sweep
        // guard pin, and step 3 moved that inside `InsertFrame`'s own hold.
        //
        // A frame with pins > 0 is never a victim, at any pressure (EV4
        // answers OutOfSpace instead of waiting), and all three erasers read
        // this field under the structure latch - so that exclusion rests on
        // the latch now rather than on one thread reaching a store.
        std::uint32_t pins = 0;

        // The CLOCK usage counter (`docs/spec/eviction.md` EV1 / §3.1-2):
        // **saturating on access, decremented by the sweep, reclaimed at
        // zero.** A counter rather than a single reference bit, so a page
        // touched five times outlives one touched once - which a bit cannot
        // express, and which is the whole of EV1's "no LRU lists".
        std::uint8_t usage = 0;
    };
    // The latch word landed in existing padding: the frame is the size it
    // was before AM-S1, and the first assert on it is this one.
    static_assert(sizeof(Frame) == 32, "Frame grew; the latch word was meant to fill padding");
    static_assert(std::atomic_ref<std::uint32_t>::required_alignment <= alignof(Frame),
                  "std::atomic_ref needs the word aligned inside the frame");

    // One region's pair of bitmaps: the unit of residency, and the unit of
    // dirtiness.
    //
    // One flag for both pages, deliberately kept from the single-map form:
    // they are written together, in the same order, at the same points, and
    // a flag per page would only create a state where one is on disk and
    // the other is not. What FM2 made per-*region* is the pair, not the
    // page - region 5 being clean says nothing about region 0.
    struct MapRegion {
        Page free_map{};
        // **Null until this region holds a headerless page** (FM6, D2(a)).
        // The headerless class is Waystone directory interior pages and
        // nothing else - `src/stats/waystone_dir.cpp` is the engine's only
        // caller of CreateNewHeaderless - so a database with no Waystone
        // directory has no headerless pages anywhere, and building a bitmap
        // to say so costs a page of memory and a page of mount I/O per
        // region to record a fact that is already implied by the bitmap's
        // absence.
        //
        // The id is still **reserved** in free_map at region creation, so
        // an ordinary allocation can never take it and the bitmap can
        // always be created later at its computed id. Only the bytes are
        // deferred, never the reservation - which is why allocated_pages()
        // still counts two for a fresh region.
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
    // the device, which is what lets it run under the structure latch.
    MapRegion* MutableRegion(std::uint32_t region) noexcept {
        auto it = map_regions_.find(region);
        return it == map_regions_.end() ? nullptr : &it->second;
    }

    // Loads `region` from the device if it holds one, formats a fresh one
    // if it does not (FM5's growth), and returns it resident either way.
    // A torn or wrong-typed map page is Corruption and refuses, the rule
    // Open has always applied to region 0 and RV3 applied to the catalog.
    StatusOr<MapRegion*> EnsureRegionResident(std::uint32_t region);

    // ---- Allocation, with the structure latch held ----------------------
    //
    // **The claim and the mark are one step, and that is the whole of it**
    // (AM-S2). They used to be two calls in two functions -
    // `FreeMapFindFirstFree` chose an id in `CreateNewUnpinned` and
    // `FreeMapAllocate` marked it inside `CreateAtUnpinned` - so any number
    // of threads could scan the same clear bit before one of them set it.
    // Measured at four threads: **~14% of allocations handed the same id to
    // two callers** (`tests/alloc_race_test.cpp`). No latch that fails to
    // span the gap fixes that, which is why this is a split rather than a
    // guard added at the top of what was there.
    //
    // **The device work stays outside the hold**, which is the rule this
    // latch has had since 2b: `EnsureRegionResident` may read the device or
    // grow the file, so a scan that runs into a region this store has not
    // loaded returns `kInvalidPageId` and names the region in
    // `missing_region`. The caller drops the hold, loads it, and asks again
    // - the retry idiom `FetchPinned` already uses for a page in flight.
    // What is left under the hold is a bitmap scan over one 8 KiB page.
    StatusOr<PageId> ClaimNextFreeIdLocked(std::uint32_t* missing_region);

    // The named-id half, same hold, same reason: the in-use test and the
    // mark decide one question together. `device_zeros` is the answer to
    // `DeviceHoldsOnlyZeros(page_id)` read *outside* the hold, and
    // `zeros_known` says whether it was read at all - a caller that found
    // the bit clear has no need for it, and gets `AlreadyExists`'s
    // `kNeedsDeviceRead` back if the bit turned out to be set after all.
    enum class ClaimOutcome : std::uint8_t { kClaimed, kNeedsDeviceRead };
    StatusOr<ClaimOutcome> ClaimNamedIdLocked(PageId page_id, bool zeros_known,
                                              bool device_zeros);

    // Loads `region` only if the device already holds it, leaving it
    // absent otherwise. What mount walks: a region the file is not large
    // enough to hold, or whose map page reads as never-written, does not
    // exist and must not be created by the act of looking.
    Status LoadRegionIfPresent(std::uint32_t region);

    // **`maps_dirty()` is gone** (AM-S3). It walked `map_regions_` to answer
    // whether `FlushMaps` had anything to do, and every caller then walked
    // it again inside `FlushMaps` - two unsynchronised passes to answer one
    // question, and a window between them in which another core's flush
    // could take the work. `FlushMaps` returns how many regions it wrote
    // instead, which is the same answer taken once and under the hold.

    // The free map's readers, without the hold, for callers that already
    // have it. Never public: the acquisition is what a caller from outside
    // this class is owed (AM-S3).
    bool IsAllocatedLocked(PageId page_id) const noexcept;
    bool IsHeaderlessLocked(PageId page_id) const noexcept;

    // Restores a region's dirty bit after `FlushMaps`' write of it failed.
    void RemarkRegionDirty(std::uint32_t region) noexcept;

    // Debug: a public free-map reader must not be reached from inside the
    // *map* hold, because `Latch` is not recursive (`base/latch.hpp`) and a
    // second acquisition **hangs**, naming nothing. This aborts naming the
    // reader instead, and running the suite armed with it in place is the
    // census AM-S3 used - the same method AM-S1's never-upgrade detector
    // used for the page latch, and it is what found that `FetchPinned`'s
    // hit path reaches `IsAllocated` and `IsHeaderless` under the frame
    // table's hold, which is why the map has a latch of its own.
    //
    // Holding `frames_latch_` here is fine and expected: that is the
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
    // **Under the hold** (AM-S3): it walks every region and reads each
    // one's headerless pointer, which is exactly what a concurrent
    // `EnsureRegionResident` inserts into and `EnsureHeaderlessMap` stores
    // to.
    MapResidency map_residency() const noexcept override {
        AssertNotUnderMapHold("map_residency");
        LatchGuard map(map_latch());
        MapResidency out;
        out.regions = map_regions_.size();
        for (const auto& [region, pages] : map_regions_) {
            out.resident_pages += 1 + (pages.headerless_map != nullptr ? 1 : 0);
        }
        out.coverage_ids = static_cast<std::uint64_t>(out.regions) * kFreeMapBitsPerPage;
        out.has_headerless = any_headerless_;
        return out;
    }

private:

    // Recomputes the maintained count from the resident regions.
    // (Declared after the public block above; still private.) It existed
    // for the one path that changed bits without going through a site that
    // could report them - the peer's free-map refresh, which unioned in
    // whatever core 0 had published and could not say how many bits that
    // added. That path went at AW-S1b; this stays as the recount a region
    // load performs. O(regions) and rare, where the count it maintains is
    // O(1) and printed on three paths.

    // Stamps a checksum unless the page is headerless. The one place that
    // decision is made, so no write path can forget it.
    void StampIfHeadered(PageId page_id, std::span<std::byte, kPageSize> page) const;

    // Writes back whichever of the two bitmap pages are dirty, after the
    // data pages they describe. Same ordering rule the free map always
    // followed: a page is only reachable once the map says so.
    // Writes every dirty region's bitmap pair out, and answers **how many
    // regions it wrote** (AM-S3) - which is what tells a caller whether a
    // sync is owed. It used to return `Status` and callers asked
    // `maps_dirty()` first; that was two unsynchronised walks of the region
    // map to answer one question, with a window between them in which
    // another core's flush could take the work and leave the caller
    // syncing nothing.
    //
    // **Copy under the hold, write outside it.** The device calls may not
    // run under the latch (this class's rule since AM-S2 2b), so the bytes
    // are checksummed and copied into a local while the map is held and
    // the dirty flags are cleared *there*. Clearing at copy time rather
    // than after the write is what makes a concurrent allocation re-dirty
    // the region instead of being lost; a failed write re-marks it.
    //
    // It also answers "who runs the map writeback when one store serves
    // every core" without a rule: N checkpointers each call this, the first
    // to take the hold clears the flags, and the rest write nothing.
    StatusOr<std::size_t> FlushMaps();

    // Waits for the log records of `page_ids` to be durable before any of
    // them is written. A no-op with no gate installed or no logged page in
    // the batch.
    Status AwaitWalGate(std::span<const PageId> page_ids);

    // `mark_dirty` is false only for a read-only fetch: a frame faulted in
    // by a reader has not been modified, so it enters the map clean and
    // nothing writes it back. `bump_usage` is false only for a ring fetch
    // (§5: a scan's touch must not look like heat).
    StatusOr<std::span<std::byte, kPageSize>> ResidentBytes(PageId page_id, bool mark_dirty,
                                                            bool bump_usage = true);

    // Whether the device holds nothing for `page_id` - not addressable, or
    // every byte zero: a page allocated in the map and never written. What
    // lets CreateAt take an allocated id (redo re-creating a page whose
    // PAGE_INIT outran its first flush) without ever clobbering a live one.
    // A failed read answers false, the refusing side.
    bool DeviceHoldsOnlyZeros(PageId page_id) const;

    // The ring's rotation half: drops `page_id`'s frame unless the
    // foreground claimed it (dirty, pinned, usage-bumped, or pinned-class)
    // - in which case the frame is abandoned to the page table, having
    // graduated to ordinary life. kInvalidPageId and non-resident ids are
    // no-ops.
    void ReleaseScanSlot(PageId page_id) noexcept;

    // `PinForScan` stood here until AM-S2-P S-P1, under a doc block that
    // argued a ring "may not" take the page latch because it would deadlock
    // against the foreground. That argument is retired: `UnpinFrame`
    // releases the page latch with the pin, so every `PageRef` in the tree
    // already holds one across arbitrary caller code, and the hazard it
    // reached for - a latch held across a *park* - is `heap_chain.hpp`'s
    // rule about which walks may take a fetcher, not a reason to omit the
    // latch. The function itself faulted through
    // `ResidentBytes` **outside** the loading set and then re-found the
    // frame under the latch, up to eight times, answering a lost race
    // with `ResourceExhausted`. That re-created in one function the race
    // step 2b closed for every other accessor, and the retry loop was the
    // symptom rather than the remedy. The ring fetches through
    // `FetchAndPin` now (AM-R8a), where the pin is taken under the hold
    // that made the frame resident and there is nothing left to retry.

    class ScanRing;  // the ScanFetcher over this store; defined in the .cpp

    // PageRef's callbacks, overriding the base no-ops onto Frame metadata
    // (MG01). Private on purpose - overriding with narrower access is legal,
    // and a pin is only ever taken and dropped by a handle through the base
    // interface: a caller that could unpin by hand could unpin someone
    // else's frame.
    // AM-S2: the fetch-and-pin pair, held together (`page_store.hpp`'s
    // recorded obligation). See the definition for what this does and does
    // not yet close.
    StatusOr<std::span<std::byte, kPageSize>> FetchPinned(PageId page_id, PinMode mode,
                                                         bool for_read, bool bump_usage) override;

    // `FetchPinned`'s whole body, plus the one answer the scan ring needs
    // and no other caller does: **did this call fault the page in, or find
    // it resident?** The ring rotates a slot only on a fault (AM-R8), and
    // the answer comes from here rather than from a residency probe of the
    // ring's own because a probe outside this function's hold is stale
    // before the fetch acts on it. `faulted` may be null, and `FetchPinned`
    // passes null: the virtual seam stays four parameters, since nothing
    // reachable through `PageStore` has a use for the fifth.
    //
    // **Meaningful only on the ok arm.** A rounded miss sets it before the
    // pin, so an error returned on a later turn of the loop leaves it true;
    // the ring reads it after `bytes.ok()` and nothing else reads it at
    // all. It over-reports in one other place, and correctly: if another
    // core loaded the page while this call was outside the latch, the miss
    // arm inserted nothing and this still says faulted, because the
    // question it answers is "did this call take the branch that loads".
    StatusOr<std::span<std::byte, kPageSize>> FetchAndPin(PageId page_id, PinMode mode,
                                                          bool for_read, bool bump_usage,
                                                          bool* faulted);

    // The create half of the same pair. See the definition for why its
    // window is narrower than `Get`'s and why it can still close.
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreatePinned(
        CreateKind which, PageId page_id) override;

    void PinFrame(PageId page_id, PinMode mode) noexcept override;
    void UnpinFrame(PageId page_id) noexcept override;
    void MarkFrameDirty(PageId page_id) noexcept override;

    // The two halves of taking a pin, split because `FetchPinned`'s armed
    // path reaches them already holding the structure latch and cannot call
    // `PinFrame` (which takes it). One definition each: writing the
    // increments out at the second site is what silently took the MG04
    // ceiling and the never-upgrade census off the armed path.
    //
    // `CountPin` runs **under** the structure latch, `AcquirePageLatch`
    // **outside** it - the order that keeps neither latch held across the
    // other (PinFrame's comment says why).
    void CountPin(Frame& frame) noexcept;
    void AcquirePageLatch(PageId page_id, Frame& frame, PinMode mode) noexcept;

    // **The pin tail, with `hold` held on entry** (AM-S2). One copy, because
    // `FetchPinned`'s hit and miss arms had grown into the same seven lines
    // - find, pin, take the span, release the structure latch, wait for the
    // page latch - and the miss arm's comment said "exactly as the hit path
    // above", which is a duplication warning written in prose.
    //
    // `nullopt` means the frame is gone, **with `hold` still held**: nothing
    // was unlocked on that arm, so both callers answer it by rounding the
    // loop and re-testing from the top. On the value arm `hold` is released
    // before the page latch is waited for, which is the ordering the whole
    // stage is about.
    //
    // **It does not re-apply the dirty mark, and that is a conclusion
    // rather than an omission** (AM-S2-P F-1). The frame this pins need not
    // be the one the caller loaded - the miss arm runs outside the latch -
    // so a frame evicted and re-faulted clean in that window would take a
    // `Get`'s write and never carry it. What makes that unreachable is the
    // loading set, not this function: see `ScanRing::Fetch` for the one
    // path that used to insert without publishing to it, and which S-P1
    // removed.
    std::optional<std::span<std::byte, kPageSize>> PinResidentAndRelease(
        PageId page_id, PinMode mode, std::unique_lock<Latch>& hold) noexcept;
    static PageLatchMode LatchModeFor(PinMode mode) noexcept {
        return mode == PinMode::kShared ? PageLatchMode::kShared : PageLatchMode::kExclusive;
    }
    // `sweep` runs MG06's inline reclaim **inside this call's own latch
    // hold**, which is the only place it can run without a window: the
    // frame it must protect is the one just inserted, and a hold that ends
    // at the insert leaves that frame unprotected for the instant before
    // the sweep re-takes the latch. Only `ResidentBytes`' miss path passes
    // it - the create paths never swept and still do not, since a create
    // that evicted would be a behaviour change nobody asked for.
    std::span<std::byte, kPageSize> InsertFrame(PageId page_id, std::unique_ptr<Page> bytes,
                                                bool dirty, bool warm = true, bool sweep = false);

    // The sweep body, with the structure latch **already held**. Split from
    // the public entry point because the two callers differ in exactly that:
    // `InsertFrame` has the hold, everyone else needs it taken.
    std::size_t EvictColdFramesLocked(std::size_t budget);
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
    // caller is CreateNewHeaderlessUnpinned - the moment the fact the
    // bitmap records stops being "no".
    StatusOr<std::span<std::byte, kPageSize>> EnsureHeaderlessMap(PageId page_id);

    PageDevice& device_;
    Logger* log_ = nullptr;
    wal::WalDurability* wal_gate_ = nullptr;

    // The page latch's switch and gauge (SetLatchArmed). Off by default:
    // arming is the assembly's act, on the superblock's core count.
    bool latch_armed_ = false;
    // Set by Open()'s debug census override and never cleared: SetLatchArmed
    // keeps the store armed once this is true.
    bool latch_forced_ = false;

    // ---- AM-S2's structure latch --------------------------------------
    //
    // The frame **table** has never had one. AM-S1 gave each *frame* a latch
    // word; `frames_`, `clock_hand_`, `live_pins_`, `pin_high_water_`,
    // `dirty_eviction_queue_` and `Frame::pins` were plain, because through
    // M1 one pool serves one core (`page.md` §6). AM-S2 shares the pool, and
    // this is what a shared table runs under: the pin accounting (step 1),
    // the insert (2b), and **all three erasers** - `ReleaseScanSlot`,
    // `EvictClean` and `EvictColdFrames`. What is still outside it is every
    // *reader* of the table that is not one of those, which is step 3's
    // list and is stated below rather than implied.
    //
    // **Null where the store is not shared**, which is `LatchGuard`'s whole
    // shape and what keeps G2: at `cores = 1` the guard is a null test and
    // no atomic (`base/latch.hpp`). Armed by the same switch as the page
    // latch, so the two cannot disagree about whether this store is shared.
    //
    // **Acquisition order: the page latch is taken OUTSIDE this one, and
    // this one is never held across device I/O.** Both halves matter.
    // Holding the structure latch while spinning for a page latch would put
    // every core's frame lookup behind one page's contention; holding it
    // across a read would put them behind a disk. `PinFrame` therefore takes
    // the pin *first*, under this latch, and only then waits for the page
    // latch - the pin is what keeps the frame from being evicted while it
    // waits, so the two are ordered without either being held across the
    // other.
    //
    // **What is under it and what is not, stated so nobody reads more into
    // it.** The two gaps step 1 named are closed, and by the stages that
    // said they would be:
    //
    //   1. The lookup-and-fault path. Closed at 2b by the `loading_` set
    //      below: a miss releases the latch before the device read instead
    //      of holding one across it, and a second core missing the same page
    //      waits rather than issuing a duplicate read.
    //   2. **The fetch-and-pin pair.** Closed by `FetchPinned` and
    //      `CreatePinned` - one operation each, latched across the pair, so
    //      the window where a frame could be evicted between the fetch and
    //      the pin no longer exists. It cost a `PageStore` interface change,
    //      which is why it was never something this class could do alone.
    //
    // **What is still outside is everything else**, stated as a rule rather
    // than a list because the list is long and a short one would read as
    // exhaustive: every `frames_` access that is not a pin, an insert or an
    // erase runs with no hold. That includes **writers**, not only readers -
    // `ResidentBytes`' resident branch sets `dirty` and bumps `usage`,
    // `StampPageLsn` and `MarkFrameDirty` set `dirty`, `WriteBack` clears it
    // and `rec_lsn` with it - as well as
    // `CreateAtUnpinned`'s in-use test, `ScanRing::Fetch`, `AwaitWalGate`,
    // `Flush`, the dirty-page enumerations, `IsPinnedClass`,
    // `resident_pages()` and the test hooks. Step 3 takes them one at a
    // time; the erasers went first because a reader racing an erase is a
    // torn read, while an *eraser* racing a pin is a freed frame under a
    // live handle.
    // **`mutable`, because the const readers need it too** (AM-S2 step 3f).
    // `DirtyPageIds`, `DirtyPagesWithRecLsn` and `resident_pages()` walk or
    // measure the table and are `const` - const of the *logical* state,
    // which a latch does not change. This is the standard reason a mutex is
    // mutable, and the alternative was to un-const three accessors whose
    // callers correctly treat them as reads.
    mutable Latch frames_latch_;
    Latch* structure_latch() const noexcept { return latch_armed_ ? &frames_latch_ : nullptr; }

    // ---- The free map's latch, and the order (AM-S3) ---------------------
    //
    // **A second latch, not a second use of the first**, and the reason is
    // not contention - it is that the two are *nested*. `FetchPinned`'s hit
    // path holds the frame table across `Resolve`, and `Resolve` begins with
    // `IsAllocated`; `ResidentBytes` under the same hold asks
    // `IsHeaderless`. Both are free-map reads, so guarding the map with
    // `frames_latch_` would be a second acquisition on one thread, which
    // `base/latch.hpp` says plainly is a hang. The AM-S2 review already
    // named the shape: *structure -> allocation, allocation the inner
    // leaf*. This is that leaf.
    //
    // **The declared order is `frames_latch_` then `map_latch_`, never the
    // reverse** (`docs/rules/rules.md` §3). Nothing takes the frame table
    // while holding the map: the two allocation paths end their map hold
    // before calling `InsertFrame`, which is the edge AM-S2's review moved
    // for exactly this reason, and `EnsureRegionResidentLocked` does its
    // device work outside both. `AssertOrderBeforeFrames` is the debug
    // enforcement, so the order is a checked statement rather than a
    // comment.
    //
    // What it guards: `map_regions_` itself - a `std::map` that region
    // creation *inserts* into while other cores are inside `find` - and
    // every `MapRegion` in it, including the lazily installed
    // `headerless_map` pointer and the `dirty` flag.
    mutable Latch map_latch_;
    Latch* map_latch() const noexcept { return latch_armed_ ? &map_latch_ : nullptr; }

    // ---- AM-S2 step 2b: the loading set ---------------------------------
    //
    // The page ids a fault is in flight for, and the only reason the miss
    // path can drop the structure latch before the device read.
    //
    // **A set of ids rather than a flag on `Frame`, because of where the
    // bytes live.** `EnsureResident` reads into a standalone
    // `unique_ptr<Page>` and calls `InsertFrame` only afterwards, so there
    // is no frame to mark while the read runs - a placeholder frame would
    // have to be invented, with invalid bytes that every other reader of the
    // table would then have to be taught to skip. An id here is invisible to
    // all of them.
    //
    // **What it buys is one thing, and the second thing it was written to
    // buy stopped being true when the erasers took the latch.** What it buys
    // is the dedup: a second core missing the same page **waits** instead of
    // issuing a duplicate read whose `InsertFrame` would race the first.
    //
    // What it no longer buys - and the note is here because the argument was
    // load-bearing and is now inverted - is cover for the inline sweep. That
    // sweep used to run *after* `InsertFrame` returned and outside every
    // hold, taking a hand-pin on the fresh frame that raced any latched pin
    // on the same counter; keeping a second core off the frame through
    // `loading_` was how that race was survived. The sweep runs inside
    // `InsertFrame`'s own hold now (`InsertFrame`'s `sweep`), so there is no
    // unlatched increment to race and no window in which the fresh frame is
    // unprotected. The deadlock that shape was avoiding is avoided by
    // `EvictColdFramesLocked` instead, which is the honest place for it:
    // `base/latch.hpp` is not recursive, so a body that may be reached with
    // the hold already taken has to say so in its name.
    std::unordered_set<PageId> loading_;
    // Broadcast when a load finishes, either way. Waiters re-check their own
    // page and sleep again if it was not theirs: loads are rare against
    // hits, so spurious wakes are cheaper than a condition variable per
    // frame - which would also make `Frame` non-movable, and the table
    // moves frames in.
    std::condition_variable loading_done_;
    // See SetStampSuppressed. Off everywhere but inside a single-stream
    // mount pass.
    bool stamp_suppressed_ = false;

    // FM2: the resident map pages, keyed by region (free_map.hpp's
    // placement arithmetic). Ordered rather than hashed so a flush writes
    // regions in ascending id order - deterministic, and the order
    // WriteBack's run coalescing would want if map pages ever joined it.
    //
    // **Not frames.** D4 of docs/inflight/in-progress/workplan-multi-free-map.md keeps them
    // store-owned, which is what makes FlushMaps' write-maps-after-data
    // ordering trivially true and keeps them out of the checkpoint dirty
    // table and away from PageRef. The cost accepted is this second
    // caching mechanism inside the store.
    //
    // Every region the device holds is loaded at mount, so "resident" and
    // "exists" are the same set and a missing key means the region was
    // never created. That equivalence is what the const accessors above
    // rest on, and it is why they may answer from an empty page instead of
    // faulting - a `const noexcept` predicate cannot read a device.
    std::map<std::uint32_t, MapRegion> map_regions_;

    // FM6's whole-instance fast path: false until some region holds a
    // headerless bitmap. IsHeaderless sits on the fault path, the
    // write-back path and the WAL gate, and for every database with no
    // Waystone directory this answers all three with no lookup at all.
    // Seeded at mount from what loaded, and moved by the one writer that
    // can change it.
    bool any_headerless_ = false;

    // D8(a): the instance's allocated-page count, maintained rather than
    // swept. Seeded at mount - which already reads every region, so the
    // seed is free - and moved by each of the sites that sets a free-map
    // bit. O(1) at every print, where the sweep it replaced was O(regions)
    // and is printed at mount, at shutdown and by SHOW META.
    std::uint32_t allocated_pages_ = 0;
    PageId next_new_page_id_;

    // First id that may ever be evicted; everything below is resident by
    // class (EV3).
    //
    // **0 means nothing has been declared resident**, which is the honest
    // default rather than a gap: this layer must not know the catalog's page
    // layout, so it cannot invent the boundary, and a structure's real
    // protection is its *pin* and not its class. `Expeditor::Open` installs
    // it, as the first id that is not a fixed system structure.
    PageId first_evictable_page_id_ = 0;

    // §4's dirty queue: what the sweep found dirty at usage zero. Drained by
    // the writeback task (EVT03), which does not exist yet - so today it is
    // an accurate record of what a sweep *would* have had cleaned.
    std::vector<PageId> dirty_eviction_queue_;

    // The clock hand: where the next sweep resumes. An id rather than an
    // iterator, because `frames_` rehashes and an iterator would not
    // survive it - the sweep re-finds its position by ordering, which is
    // O(n log n) per pass over an unordered_map and is why the frame table
    // becomes open-addressed at EV05 (page.md §16-7).
    PageId clock_hand_ = 0;
    std::size_t frame_budget_ = 0;  // 0 = unbounded (pre-eviction behaviour)
    std::size_t live_pins_ = 0;
    // `kPinCeiling` scaled by how many threads may pin this store at once
    // (`SetLatchArmed`). One operation's bound times the operations in
    // flight; unscaled it is the per-operation number and the assert below
    // fires on correct traffic the moment two threads pin together.
    std::size_t pin_ceiling_ = kPinCeiling;
    std::size_t pin_high_water_ = 0;

    std::unordered_map<PageId, Frame> frames_;
};

}  // namespace kds::storage
