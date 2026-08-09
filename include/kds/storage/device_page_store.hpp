#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/storage/extent_lease.hpp"
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

    StatusOr<std::span<std::byte, kPageSize>> CreateAt(PageId page_id) override;
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNew() override;
    StatusOr<std::span<std::byte, kPageSize>> Get(PageId page_id) override;

    // Get() that leaves the frame clean (page_store.hpp). Without it a
    // read-only statement dirties every page it touches and the next
    // checkpoint writes the whole working set back with nothing changed.
    StatusOr<std::span<std::byte, kPageSize>> GetForRead(PageId page_id) override;

    // CreateNew() for a page that will carry no common header - see the
    // note above. The whole 8 KiB is the caller's, and this store will
    // neither stamp nor verify a checksum on it, now or after a reopen.
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewHeaderless() override;

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

    // ---- The writeback primitive (docs/spec-eviction.md §4, EVT03) ------
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

    // Installs the WAL-before-data gate described above. Null (the
    // default) disables it. `gate` must outlive the store.
    void SetWalGate(wal::WalDurability* gate) noexcept { wal_gate_ = gate; }

    // ---- Core ownership (docs/workplan-crosscore.md M5, P2) -------------
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
        // class (docs/workplan-eviction.md EV3).
        SetResidentLimit(system_page_limit);
    }

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
    bool MayFault(PageId page_id) const noexcept;

    // Whether this store's core may **write** `page_id` - i.e. take a frame
    // it is allowed to dirty.
    //
    // Strictly narrower than MayFault: the system range is readable by
    // every core and writable only by core 0. That asymmetry is the whole
    // of P6's soundness. Catalog pages have exactly one writer, so a peer's
    // view can be stale (which is a retryable "not found", crosscore.md §5)
    // but never torn by a second writer.
    bool MayWrite(PageId page_id) const noexcept;

    // The live free-map page, for the one caller that carves extents out of
    // it (storage/extent_lease.hpp's ExtentAllocator). Exposed rather than
    // wrapped because reservation is *policy* about who gets which ids,
    // which is not this class's business - what is its business is that the
    // bytes have a single owner, and handing out a mutable view of them is
    // exactly as narrow as that ownership.
    //
    // **The system core only.** A leased store never allocates from the map
    // (see SetCoreOwnership), so calling this on one is a defect.
    std::span<std::byte, kPageSize> free_map_bytes() noexcept {
        maps_dirty_ = true;
        return std::span<std::byte, kPageSize>(free_map_page_);
    }

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
    // (docs/workplan-crosscore.md P6). A peer holds catalog pages this
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

    // ---- Frame reclamation (docs/workplan-eviction.md EV01-EV02) -------
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
    class PageRef {
    public:
        PageRef() noexcept = default;
        PageRef(const PageRef&) = delete;
        PageRef& operator=(const PageRef&) = delete;
        PageRef(PageRef&& other) noexcept { *this = std::move(other); }
        PageRef& operator=(PageRef&& other) noexcept;
        ~PageRef();

        bool valid() const noexcept { return store_ != nullptr; }
        PageId page_id() const noexcept { return page_id_; }

        // Valid exactly as long as this handle is. Taking a copy of the
        // span and dropping the handle is the use-after-free eviction
        // introduces - see EV03's note on why no implicit conversion to a
        // span exists.
        std::span<std::byte, kPageSize> bytes() const noexcept {
            return std::span<std::byte, kPageSize>(data_, kPageSize);
        }

        // Marks the frame dirty after the fact, for a handle taken through
        // `PinnedGetForRead` that turned out to write. Cheaper than a
        // second fetch and honest about which frames are written back.
        void MarkDirty() noexcept;

        // Drops the pin early. Idempotent; the destructor calls it.
        void Release() noexcept;

    private:
        friend class DevicePageStore;
        PageRef(DevicePageStore* store, PageId page_id,
                std::span<std::byte, kPageSize> bytes) noexcept
            : store_(store), page_id_(page_id), data_(bytes.data()) {}

        DevicePageStore* store_ = nullptr;
        PageId page_id_ = kInvalidPageId;
        // A pointer rather than a span, because `std::span<T, N>` with a
        // fixed extent has no empty state - and a moved-from or released
        // handle needs one.
        std::byte* data_ = nullptr;
    };

    // `Get()` / `GetForRead()` returning a pinned handle. Same fetch, same
    // faulting, same dirty semantics - the only difference is that the
    // frame cannot be reclaimed while the handle lives.
    StatusOr<PageRef> PinnedGet(PageId page_id);
    StatusOr<PageRef> PinnedGetForRead(PageId page_id);

    // Whether `page_id` belongs to a **pinned class**
    // (`docs/spec-eviction.md` EV3): never a sweep candidate at any
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
    // The CLOCK usage counter's ceiling (`docs/spec-eviction.md` EV1,
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

    std::uint32_t allocated_pages() const noexcept;
    bool IsAllocated(PageId page_id) const noexcept;

private:
    using Page = std::array<std::byte, kPageSize>;

    struct Frame {
        std::unique_ptr<Page> bytes;
        bool dirty = false;
        // First log record to dirty this frame since it was last written
        // back; 0 when nothing logged touched it. See StampPageLsn().
        wal::Lsn rec_lsn = 0;

        // ---- Reclamation state (docs/workplan-eviction.md EV01) --------
        //
        // How many live `PageRef`s hold this frame. **Plain, non-atomic,
        // core-local** - `page.md` §6 makes that the model rather than a
        // shortcut: one pool per core, and cross-core page access does not
        // exist. A frame with pins > 0 is never a victim, at any pressure
        // (EV4 answers OutOfSpace instead of waiting).
        std::uint32_t pins = 0;

        // The CLOCK usage counter (`docs/spec-eviction.md` EV1 / §3.1-2):
        // **saturating on access, decremented by the sweep, reclaimed at
        // zero.** A counter rather than a single reference bit, so a page
        // touched five times outlives one touched once - which a bit cannot
        // express, and which is the whole of EV1's "no LRU lists".
        std::uint8_t usage = 0;
    };

    DevicePageStore(PageDevice& device, PageId first_new_page_id, const Page& free_map,
                    const Page& headerless_map, bool maps_dirty) noexcept;

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
    // nothing writes it back.
    StatusOr<std::span<std::byte, kPageSize>> ResidentBytes(PageId page_id, bool mark_dirty);

    // PageRef's two callbacks. Private because a pin is only ever taken and
    // dropped by a handle - a caller that could unpin by hand could unpin
    // someone else's frame.
    void UnpinFrame(PageId page_id) noexcept;
    void MarkFrameDirty(PageId page_id) noexcept;
    std::span<std::byte, kPageSize> InsertFrame(PageId page_id, std::unique_ptr<Page> bytes,
                                                bool dirty);
    Status EnsureAddressable(PageId page_id);

    std::span<const std::byte, kPageSize> free_map_bytes() const noexcept {
        return std::span<const std::byte, kPageSize>(free_map_page_);
    }
    std::span<std::byte, kPageSize> headerless_map_bytes() noexcept {
        return std::span<std::byte, kPageSize>(headerless_map_page_);
    }
    std::span<const std::byte, kPageSize> headerless_map_bytes() const noexcept {
        return std::span<const std::byte, kPageSize>(headerless_map_page_);
    }

    PageDevice& device_;
    Logger* log_ = nullptr;
    wal::WalDurability* wal_gate_ = nullptr;

    // Core ownership (see SetCoreOwnership). The defaults are core 0's, so
    // every construction site that predates multicore keeps its behaviour.
    std::uint32_t core_id_ = 0;
    LeasedIdSource* lease_ = nullptr;
    // First non-system page id; 0 means no readable system range. See
    // SetCoreOwnership.
    PageId system_page_limit_ = 0;

    Page free_map_page_;
    Page headerless_map_page_;
    // One flag for both maps: they are written together, in the same
    // order, at the same points, and a separate flag per map would only
    // create a state where one is on disk and the other is not.
    bool maps_dirty_;
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

    std::unordered_map<PageId, Frame> frames_;
};

}  // namespace kds::storage
