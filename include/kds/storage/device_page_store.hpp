#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/storage/extent_lease.hpp"
#include "kds/storage/free_map.hpp"
#include "kds/storage/page_device.hpp"
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
// Concurrency: core-local, no internal synchronization (rules.md #3).
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
// alongside the superblock (0) and the catalog's fixed pages (4-8).
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
    // Refuses with Unsupported on a **leased** store. A leased core takes
    // its ids from the extent core 0 reserved for it and never consults
    // this floor (SetCoreOwnership), so raising it here would be a silent
    // no-op - the shape of repair that reports success and changes nothing.
    // The leased core's equivalent is that core 0's ExtentAllocator must
    // start above the recovered high-water, which is the grant path's and
    // not this store's. Recovery runs at mount, before any lease is
    // installed (RV1), so the refusal is a guard against this being called
    // somewhere it does not belong rather than a restriction on recovery.
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
    bool IsHeaderless(PageId page_id) const noexcept;

    // Writes dirty frames back in page-id order, which is file order
    // (page.md section 13), then the free map. Data pages go first so a
    // crash between them can only orphan a page, never publish one whose
    // bytes never landed. Not durable on its own - Sync() adds the fsync.
    Status Flush();
    Status Sync() override;

    // The two map pages written back if dirty, then the device synced - the
    // maps alone, not the frames. What an extent grant needs before it
    // leaves core 0 (extent_lease.hpp, `ExtentAllocator::Persist`): a run of
    // ids a peer may write into must be allocated on the device before the
    // peer can hold committed rows in it, or a crash frees the run for the
    // next mount's allocator to hand out over them. On a leased store the
    // map write is FlushMaps' guarded no-op and only the sync runs.
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
    // the whole of pin-safety. A page already resident - foreground's or a
    // ring slot's - is used in place, with no usage bump and no rotation.
    //
    // What this bounds: a full scan grows residency by at most the ring's
    // size, whatever the relation's. The frames are ordinary frames in the
    // page table while resident, so a foreground hit on one behaves as a
    // hit anywhere - only the lifecycle differs.
    std::unique_ptr<ScanFetcher> OpenScanRing(std::size_t frames = kScanRingFrames) override;

    // Installs the WAL-before-data gate described above. Null (the
    // default) disables it. `gate` must outlive the store.
    void SetWalGate(wal::WalDurability* gate) noexcept { wal_gate_ = gate; }

    // ---- Core ownership (docs/inflight/in-progress/workplan-crosscore.md M5, P2) -------------
    //
    // Binds this store to `core_id` and, for a core that is not the system
    // core, to the lease it allocates from (storage/extent_lease.hpp).
    //
    // **With a lease installed this store never touches the free map.** That
    // is the whole point: the map is one durable page and M5 gives it to
    // core 0, so a second writer would be shared mutable state between cores
    // - which workplan guideline 1 forbids outright. `CreateNew()` takes its
    // ids from the lease instead, and `CreateAt()` becomes unavailable,
    // since placing a page at a *chosen* id is a claim on the map that only
    // its owner can make.
    //
    // `system_page_limit` is the first id that is *not* a fixed system
    // structure - the superblock, the two bitmaps and the catalog's pages
    // all sit below it. A peer may **read** those and may never write one
    // (see MayFault/MayWrite), which is what lets a non-zero core resolve a
    // relation while the single-writer property survives.
    //
    // It is a parameter rather than a constant because the boundary is
    // `server::kFirstUserPageId` and this layer must not know the catalog's
    // page layout; 0 means "no readable system range", which is what every
    // caller predating multicore gets.
    //
    // `lease` must outlive the store. Null (the default) is core 0's
    // arrangement and behaves exactly as this class always has.
    void SetCoreOwnership(std::uint32_t core_id, LeasedIdSource* lease,
                          PageId system_page_limit = 0) noexcept {
        core_id_ = core_id;
        lease_ = lease;
        system_page_limit_ = system_page_limit;
        // The same boundary by the same definition, so it is adopted rather
        // than restated: everything below `system_page_limit` is a fixed
        // system structure, and a fixed system structure is resident by
        // class (docs/inflight/in-progress/workplan-eviction.md EV3).
        SetResidentLimit(system_page_limit);
    }

    // The identity half of the above, on its own. `StampPageLsn` writes
    // `core_id + 1` into the page's PL-C stream stamp (PL §9 rule 4), and
    // mount-time recovery stamps pages *before* the lease may be installed
    // (server/core_runtime.cpp says why) - so a peer needs its id early or
    // it stamps core 0's onto its own pages. Sets nothing else: the lease
    // is what MayFault/MayWrite/CreateAt key on, and it stays absent.
    void SetStreamCoreId(std::uint32_t core_id) noexcept { core_id_ = core_id; }

    std::uint32_t core_id() const noexcept { return core_id_; }

    // Whether this store's core may **read** `page_id`.
    //
    // Core 0 may reach anything - it owns the superblock, the free map, the
    // headerless map and the catalog pages. Any other core may reach ids
    // inside an extent it was granted, plus the fixed system range
    // read-only: the catalog lives there, and a core that cannot read the
    // catalog cannot resolve a relation and so cannot serve a statement at
    // all (workplan-crosscore.md P6).
    //
    // Enforced in the frame-load path **in debug builds only** (P2's
    // "ownership-violation assert"), so release pays nothing. It is a
    // mechanical check on shared-nothing rather than a security boundary:
    // what it catches is a core reaching for a page that is not its own,
    // which is a defect however it got there.
    //
    // A page this core may write, it may read: the write-rights set
    // (grants and stamp claims, below) is consulted here too, so a write
    // grant carries its own fault rights (the 95b45e8 review's C2) and a
    // page claimed from its stamp is readable by the claim alone.
    bool MayFault(PageId page_id) const noexcept;

    // Whether this store's core may **write** `page_id` - i.e. take a frame
    // it is allowed to dirty.
    //
    // Strictly narrower than MayFault: the system range is readable by
    // every core and writable only by core 0. That asymmetry is the whole
    // of P6's soundness. Catalog pages have exactly one writer, so a peer's
    // view can be stale (which is a retryable "not found", crosscore.md §5)
    // but never torn by a second writer.
    //
    // Three sources say yes for a leased store: the lease, a write grant
    // (PW1c-4), and a **stamp claim** (PW1c-7) - a page whose PL-C stream
    // stamp names this core, found on the frame-load path when neither of
    // the first two covered it. The claim is what makes ownership survive
    // a restart: leases and grants are memory-resident, the stamp is the
    // page's own durable statement of the same fact, and PL §9 rule 6
    // guarantees no page leaves a stream without being restamped.
    bool MayWrite(PageId page_id) const noexcept override;

    // Fault rights over a page range this core does **not** own by lease
    // (crosscore.md CC7, workplan P6b): a relation the catalog assigns to
    // this core is built from another core's allocations, and this is how
    // page ownership becomes a function of the catalog. MayFault consults
    // these; MayWrite deliberately never does - a grant is read-only, and
    // the write path arrives only with statement dispatch.
    //
    // Held as bits beside the write-rights bitmap (the PW1c-7 review's S1;
    // it was a vector of extents merged where contiguous), so a
    // re-delivered grant that repeats its extent sets what is already set
    // and MayFault stays one bit test whatever was granted. Nothing revokes
    // a grant: revocation is ownership rebalance, which M3 keeps out of v1.
    void GrantFaultPages(Extent extent);

    // Write rights over **exact pages** this core does not own by lease
    // (PW1c-4, `docs/inflight/in-progress/workplan-peer-writer.md` §8 rule 1): the pages core 0
    // formatted for a relation this core owns, granted at DDL publish only
    // after their handoff records are durable (PL §9 rule 1's ordering,
    // the sender's to keep). Exact-page and never extent-granular,
    // deliberately - the superset that is safe to fault is not safe to
    // write, which is the precise objection that rules out widening
    // GrantFaultPages instead. MayWrite and MayFault consult this set;
    // nothing revokes an entry, GrantFaultPages' rule. Ids beyond the free
    // map's coverage hold no bit and are never writable, the store's
    // existing ceiling.
    void GrantWritePages(std::span<const PageId> pages);

    // Pages this store admitted from their stream stamp alone (PW1c-7).
    // A diagnostic, and what a test reads to see that a claim happened.
    std::uint64_t stamp_claims() const noexcept { return stamp_claims_; }

    // How many times this store adopted the device's free map because its
    // own copy called a page absent (AdoptDeviceMapOnMiss). A diagnostic,
    // and what a test reads to see that the adoption happened.
    std::uint64_t map_refreshes_on_miss() const noexcept { return map_refreshes_on_miss_; }

    // Re-reads the free-map page from the device into this store's copy
    // (peers only - the system core's copy IS the authority). The fix for
    // the 95b45e8 review's C1: a peer's snapshot is taken at Open(), so a
    // relation created *after* the peer started has no bit in it, and
    // every granted page answered "page id not found". Called by the grant
    // receivers, ordered correctly by construction: core 0 flushes the
    // maps inside the publish flush before any grant leaves. Reads into a
    // scratch page, validates whole, then **unions** into the copy: a
    // failed or torn read (core 0 flushes this page concurrently, no
    // latch) leaves the copy intact for the next grant to retry, and
    // union - never replacement - preserves the bits redo's mount-time
    // CreateAt set in this copy alone. Does not mark the maps dirty: this
    // adopts truth, it does not create peer writes.
    Status RefreshFreeMapFromDevice();

    // The live free-map page **of one region**, for the one caller that
    // carves extents out of it (storage/extent_lease.hpp's ExtentAllocator).
    // Exposed rather than wrapped because reservation is *policy* about who
    // gets which ids, which is not this class's business - what is its
    // business is that the bytes have a single owner, and handing out a
    // mutable view of them is exactly as narrow as that ownership.
    //
    // FM4: a region, not "the map". The allocator can no longer hold one
    // span for its lifetime, because the id space is no longer one page -
    // it asks per region, and the bit indices it passes are within-page
    // (FreeMapBitIndexOf), never absolute ids.
    //
    // FM5: **this is the growth point.** A region the device does not hold
    // yet is formatted here and its own two bits marked, so an allocator
    // that walks off the end of region N simply asks for region N+1 and
    // gets one. Fails rather than returns bytes, which is why it is a
    // StatusOr where the old accessor was infallible: creating a region
    // touches the device's capacity, and a peer may not create one at all.
    //
    // Marks the region dirty on every take, exactly as the single-page
    // accessor did - the reason it is taken per call and never cached (the
    // PW3b finding at extent_lease.hpp).
    //
    // **The system core only.** A leased store never allocates from the map
    // (see SetCoreOwnership), so calling this on one is a defect.
    StatusOr<std::span<std::byte, kPageSize>> FreeMapBytesForRegion(std::uint32_t region);

    // The other half of that seam, and the reason it is public: the
    // allocator sets bits through the span above, so it is the only writer
    // the maintained allocated-page count (D8(a)) cannot see. It reports
    // the run it just marked.
    //
    // Narrow on purpose. `pages` is the length of a run every one of whose
    // bits Reserve() proved clear before setting it, so this can be a plain
    // add rather than a re-scan - and the proof is next door, in the loop
    // that probes the whole run before marking any of it.
    void NoteAllocated(std::uint32_t pages) noexcept { allocated_pages_ += pages; }

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
    // `SetCoreOwnership` already raises it, since its `system_page_limit` is
    // the same boundary by the same definition; this is for a store that
    // never calls that, and for AST04, whose Bound Cabin pages are resident
    // by class and are allocated above the system range.
    void SetResidentLimit(PageId first_evictable_page_id) noexcept;

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

    std::size_t EvictColdFrames(std::size_t budget);

    // Pages the sweep found dirty at usage zero, in the order it found them
    // - §4's dirty queue, populated here and drained by EVT03's writeback
    // task. Cleared by `TakeDirtyEvictionQueue()`.
    std::vector<PageId> TakeDirtyEvictionQueue();

    std::size_t resident_pages() const noexcept { return frames_.size(); }

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
    static constexpr std::size_t kPinCeiling = 8;

    std::uint32_t allocated_pages() const noexcept;
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
    StatusOr<std::span<std::byte, kPageSize>> Resolve(PageId page_id, bool mark_dirty);

    // A leased core's free-map copy is a **mount-time snapshot**, and the
    // only thing that advanced it was a relation fault/write grant
    // (core_runtime.cpp's two grant receivers). Every page core 0 allocates
    // between grants is therefore invisible to a peer - a *catalog* page
    // above all, which a peer must read to resolve any relation of its own
    // - and the fault seam turned that staleness into a permanent
    // `NotFound`, because nothing on the failing path ever asked the device
    // again: it left a peer-owned relation unwritable for the rest of the
    // mount (`docs/inflight/known-gaps.md`, G1).
    //
    // So: before a leased store calls a page absent, it adopts the device's
    // truth once - the same operation the grant receivers perform, at the
    // one seam where "stale" and "absent" are otherwise indistinguishable.
    // Returns whether the page is allocated after the adoption.
    //
    // The cost is a device read per resident region **on a miss**. That is
    // an error path everywhere but one: `command_dispatcher.cpp`'s
    // speculative fault of a creation page while write rights are pending
    // *expects* the miss, so a client retrying a `RelationWriteRightsPending`
    // refusal pays one adoption per statement until the grant lands. It
    // buys what that site is for - `TryClaimByStamp` sits behind this same
    // gate and could never run while the map was stale.
    //
    // A page still absent afterwards is one core 0 has allocated but not
    // yet flushed, or one that genuinely does not exist; the refusal is the
    // same for both, but the next statement re-adopts, so the first clears
    // itself.
    bool AdoptDeviceMapOnMiss(PageId page_id);
    void LogAdoptionFailure(PageId page_id, const Status& why) const;

    struct Frame {
        std::unique_ptr<Page> bytes;
        bool dirty = false;
        // First log record to dirty this frame since it was last written
        // back; 0 when nothing logged touched it. See StampPageLsn().
        wal::Lsn rec_lsn = 0;

        // ---- Reclamation state (docs/inflight/in-progress/workplan-eviction.md EV01) --------
        //
        // How many live `PageRef`s hold this frame. **Plain, non-atomic,
        // core-local** - `page.md` §6 makes that the model rather than a
        // shortcut: one pool per core, and cross-core page access does not
        // exist. A frame with pins > 0 is never a victim, at any pressure
        // (EV4 answers OutOfSpace instead of waiting).
        std::uint32_t pins = 0;

        // The CLOCK usage counter (`docs/spec/eviction.md` EV1 / §3.1-2):
        // **saturating on access, decremented by the sweep, reclaimed at
        // zero.** A counter rather than a single reference bit, so a page
        // touched five times outlives one touched once - which a bit cannot
        // express, and which is the whole of EV1's "no LRU lists".
        std::uint8_t usage = 0;
    };

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

    // One region's two grant bitmaps (CC7's fault grants, PW1c-4's
    // exact-page write grants and PW1c-7's stamp claims). Each null until
    // something is granted into this region.
    struct RightsRegion {
        std::unique_ptr<Page> fault;
        std::unique_ptr<Page> write;
    };

    DevicePageStore(PageDevice& device, PageId first_new_page_id) noexcept;

    // The all-zero page a region that does not exist reads as. Shared and
    // never written - the const accessors hand out a read-only view of it.
    static const Page& AbsentRegionPage() noexcept;

    const MapRegion* FindRegion(std::uint32_t region) const noexcept {
        auto it = map_regions_.find(region);
        return it == map_regions_.end() ? nullptr : &it->second;
    }

    // Loads `region` from the device if it holds one, formats a fresh one
    // if it does not (FM5's growth), and returns it resident either way.
    // A torn or wrong-typed map page is Corruption and refuses, the rule
    // Open has always applied to region 0 and RV3 applied to the catalog.
    StatusOr<MapRegion*> EnsureRegionResident(std::uint32_t region);

    // Loads `region` only if the device already holds it, leaving it
    // absent otherwise. What mount walks: a region the file is not large
    // enough to hold, or whose map page reads as never-written, does not
    // exist and must not be created by the act of looking.
    Status LoadRegionIfPresent(std::uint32_t region);

    // Whether any resident region has unwritten bits.
    bool maps_dirty() const noexcept;

public:
    // FM10. Resident is in-existence for the free map (see map_regions_),
    // so `regions` counts what the file holds and the gap between
    // `resident_pages` and twice that is what FM6's deferred headerless
    // bitmaps save.
    MapResidency map_residency() const noexcept override {
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
    // (Declared after the public block above; still private.) For the
    // one path that changes bits without going through a site that can
    // report them: RefreshFreeMapFromDevice unions in whatever core 0 has
    // published since, and how many bits that added is not knowable
    // without counting. O(regions) and rare, where the count it maintains
    // is O(1) and printed on three paths.
    void RecountAllocatedPages() noexcept;

    // Stamps a checksum unless the page is headerless. The one place that
    // decision is made, so no write path can forget it.
    void StampIfHeadered(PageId page_id, std::span<std::byte, kPageSize> page) const;

    // Writes back whichever of the two bitmap pages are dirty, after the
    // data pages they describe. Same ordering rule the free map always
    // followed: a page is only reachable once the map says so.
    Status FlushMaps();

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

    // PW1c-7's claim, run by ResidentBytes for a leased store on a page
    // outside every granted set: reads the page's stream stamp - off the
    // resident frame when redo left one, else off the device, checksum
    // verified - and admits the page to the write-rights set when the
    // stamp names this core. A device read it made is handed back through
    // `prefetched` so the miss path does not read twice. Anything else
    // (foreign stamp, unstamped, headerless, unreadable, beyond the bitmap)
    // leaves the rights exactly as they were: the checks after it refuse.
    void TryClaimByStamp(PageId page_id, std::unique_ptr<Page>& prefetched);

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

    class ScanRing;  // the ScanFetcher over this store; defined in the .cpp

    // PageRef's callbacks, overriding the base no-ops onto Frame metadata
    // (MG01). Private on purpose - overriding with narrower access is legal,
    // and a pin is only ever taken and dropped by a handle through the base
    // interface: a caller that could unpin by hand could unpin someone
    // else's frame.
    void PinFrame(PageId page_id) noexcept override;
    void UnpinFrame(PageId page_id) noexcept override;
    void MarkFrameDirty(PageId page_id) noexcept override;
    std::span<std::byte, kPageSize> InsertFrame(PageId page_id, std::unique_ptr<Page> bytes,
                                                bool dirty, bool warm = true);
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
    // One region's rights bitmaps, created on first grant into that region
    // (D10(a)). Null until then, which is the overwhelmingly common state:
    // a peer holds grants in the handful of regions its relations live in,
    // not across the id space.
    RightsRegion& RightsFor(PageId page_id) {
        return rights_regions_[FreeMapRegionOf(page_id)];
    }
    const RightsRegion* FindRights(PageId page_id) const noexcept {
        auto it = rights_regions_.find(FreeMapRegionOf(page_id));
        return it == rights_regions_.end() ? nullptr : &it->second;
    }
    // The bit tests, range-checked because the bitmap helper reads an id
    // beyond its coverage as set - stated once, here.
    // The bit tests. **No id ceiling any more** (D10(a)): these were single
    // pages indexed by absolute page id, so they capped at one bitmap
    // page's 65,280 ids and a peer could hold no grant above region 0 -
    // the free map's ceiling lifting did not lift theirs, because D1's
    // placement never reached them. They are keyed by region now, like the
    // map, and a region with no grants answers no without allocating one.
    bool HasFaultRight(PageId page_id) const noexcept {
        const RightsRegion* rights = FindRights(page_id);
        return rights != nullptr && rights->fault != nullptr &&
               FreeMapIsAllocated(std::span<const std::byte, kPageSize>(*rights->fault),
                                  FreeMapBitIndexOf(page_id));
    }
    bool HasWriteRight(PageId page_id) const noexcept {
        const RightsRegion* rights = FindRights(page_id);
        return rights != nullptr && rights->write != nullptr &&
               FreeMapIsAllocated(std::span<const std::byte, kPageSize>(*rights->write),
                                  FreeMapBitIndexOf(page_id));
    }

    PageDevice& device_;
    Logger* log_ = nullptr;
    wal::WalDurability* wal_gate_ = nullptr;

    // Core ownership (see SetCoreOwnership). The defaults are core 0's, so
    // every construction site that predates multicore keeps its behaviour.
    std::uint32_t core_id_ = 0;
    LeasedIdSource* lease_ = nullptr;
    // The two rights sets a leased store holds beyond its lease, one bit
    // per page id in the free map's layout (the headerless map's
    // precedent: identical addressing, different meaning): CC7's fault
    // grants, and the write rights - PW1c-4's exact-page grants plus
    // PW1c-7's stamp claims. Bitmaps rather than the extent vector and
    // sorted page vector that preceded them, because the claims grew the
    // write population from a handful of creation pages to every page of
    // this core's relations touched since mount, re-delivery repeats
    // grants, and both checks sit on the frame-load path. Never persisted:
    // the durable form of the same fact is each page's own stamp, which is
    // what refills them. Core 0 (no lease) never consults either. 16 KiB
    // per store.
    // D10(a): keyed by region, mirroring map_regions_, and each half built
    // only when something is granted into it. Never persisted - the durable
    // form of the same fact is each page's own stream stamp, which is what
    // refills them - so unlike the free map there is no format question and
    // no migration, which is what made growing them the cheap answer.
    std::map<std::uint32_t, RightsRegion> rights_regions_;
    std::uint64_t stamp_claims_ = 0;
    std::uint64_t map_refreshes_on_miss_ = 0;
    // First non-system page id; 0 means no readable system range. See
    // SetCoreOwnership.
    PageId system_page_limit_ = 0;

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
    // layout (see SetCoreOwnership), so it cannot invent the boundary, and a
    // structure's real protection is its *pin* and not its class. A server
    // sets it through SetCoreOwnership, whose `system_page_limit` is already
    // exactly this boundary - "the first id that is not a fixed system
    // structure".
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
    std::size_t pin_high_water_ = 0;

    std::unordered_map<PageId, Frame> frames_;
};

}  // namespace kds::storage
