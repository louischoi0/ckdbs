#pragma once

#include <array>
#include <cassert>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// Abstract "give me the bytes for page_id" seam: every store - the
// bounded-pool DevicePageStore, InMemoryPageStore, page_mgr's BufferPool -
// implements it, and no caller knows which it holds.
//
// **The pin model (page.md §3; background: workplan-pageref.md MG01).** The
// public accessors return a `PageRef` - a move-only handle whose lifetime IS
// the validity of the bytes: construction pins the frame, destruction unpins
// it, and a store that never evicts implements pin/unpin as no-ops, which for
// it is a true statement rather than a stub. There is deliberately **no
// implicit conversion from PageRef to a span**: `auto s =
// store.Get(id).value()` would dangle the moment the temporary unpinned.
// Access is `ref.bytes()`, on a named handle whose lifetime is visible.
//
// The `*Unpinned` accessors are the implementation seam only: protected
// since MG06, so engine code cannot name them.

namespace kds::storage {

// Which of PageStore::Get()/GetForRead() a page walk should fetch through.
// Named rather than a bool because it appears at call sites far from the
// declaration, and because picking the wrong one on a mutating walk loses
// the write silently - see GetForRead() below.
enum class PageAccess {
    kRead,   // the visitor will not write through the page
    kWrite,  // the visitor may modify tuples in place
    // A read, for the outermost read walk of a SELECT (BE-R5): a page it
    // faults in enters cold, so a scan larger than the pool does not
    // displace the working set; a page it finds resident warms as any hit.
    kScan,
};

// How a fetch counts toward its frame's CLOCK usage (`eviction.md` §3.1).
// Two answers, where a flag gave one: whether a fault enters warm, and
// whether a hit bumps.
enum class FetchHeat : std::uint8_t {
    kWarm,  // a fault enters at usage 1, a hit bumps: every ordinary fetch
    kScan,  // a fault enters at usage 0, a hit bumps: `PageAccess::kScan` (BE-R5)
    kRing,  // a fault enters at usage 0, a hit never bumps: the scan ring (§5)
};

// How a bulk sequential reader fetches pages (docs/spec/eviction.md §5).
// A scan that faults every page of a large relation
// through the ordinary path floods the pool with frames it will touch
// exactly once; ring mode reuses a small fixed set of frames cyclically
// and never bumps usage counters, so a scan's touch does not look like
// heat and the foreground working set stays where it was.
//
// The seam is a virtual fetcher rather than a store method because the
// callers that need it - the relayout planner's survey and the cabin
// optimizer's builds (PO4 makes ring routing mandatory there), which are
// the two the tree has - hold a `PageStore&` and must not know which
// concrete store is under them (aggregate scans and the CREATE ASSERTION
// builder open none). The base-class ring is
// deliberately plain: a store that never evicts has no pool to protect, so
// fetching through `GetForRead` *is* its correct ring, and only
// `DevicePageStore` overrides with the real cyclic one.
class ScanFetcher {
public:
    virtual ~ScanFetcher() = default;

    // Read-only by contract, exactly as GetForRead(): the span is mutable
    // for the same mechanical reason and writing through it is the same
    // defect. Valid until the next Fetch() on this fetcher - ring rotation
    // may drop the previous page's frame - which is a *stricter* lifetime
    // than the ordinary accessors give, and the reason a ring consumer
    // finishes each page before fetching the next.
    //
    // **And does not suspend while it holds one** (AM-R8c, AR2 R2). A
    // fetcher holds a pin, and DevicePageStore's ring the page latch with
    // it, from the Fetch that returned the span until the next one; a
    // ring's span is not a PageRef, so no destructor enforces this.
    virtual StatusOr<std::span<std::byte, kPageSize>> Fetch(PageId page_id) = 0;
};

// Frames one ring holds (`kds.scan_ring_frames`, spec §6). `[PROPOSED]`
// 32; a parameter of OpenScanRing rather than a behavior anything may
// depend on.
inline constexpr std::size_t kScanRingFrames = 32;

class PageStore;

// The move-only pinned-page handle (page.md §3).
// Construction pins, destruction unpins; the bytes are valid exactly as
// long as the handle is. Copy is deleted rather than refcounted: two
// handles to one frame is not a thing any caller needs, and making it
// expressible would make "who unpins" a question.
class PageRef {
public:
    PageRef() noexcept = default;
    PageRef(const PageRef&) = delete;
    PageRef& operator=(const PageRef&) = delete;
    PageRef(PageRef&& other) noexcept { *this = std::move(other); }
    inline PageRef& operator=(PageRef&& other) noexcept;
    ~PageRef() { Release(); }

    bool valid() const noexcept { return store_ != nullptr; }
    PageId page_id() const noexcept { return page_id_; }

    // Valid exactly as long as this handle is. Taking a copy of the span
    // and dropping the handle is the use-after-free eviction introduces -
    // which is why no implicit conversion to a span exists.
    std::span<std::byte, kPageSize> bytes() const noexcept {
        return std::span<std::byte, kPageSize>(data_, kPageSize);
    }

    // Marks the frame dirty after the fact, for a handle taken through
    // GetForRead() that turned out to write. Cheaper than a second fetch
    // and honest about which frames are written back.
    inline void MarkDirty() noexcept;

    // Drops the pin early. Idempotent; the destructor calls it.
    inline void Release() noexcept;

private:
    friend class PageStore;
    friend class PageClaims;
    PageRef(PageStore* store, PageId page_id, std::span<std::byte, kPageSize> bytes) noexcept
        : store_(store), page_id_(page_id), data_(bytes.data()) {}

    PageStore* store_ = nullptr;
    PageId page_id_ = kInvalidPageId;
    // A pointer rather than a span, because `std::span<T, N>` with a fixed
    // extent has no empty state - and a moved-from or released handle
    // needs one.
    std::byte* data_ = nullptr;
};

// The mode a pinned accessor takes its frame in (AM-S1, the page latch).
// `GetForRead` is shared; `Get` and every `Create*` are exclusive. A store
// without a latch ignores it; `DevicePageStore` latches the frame in this
// mode when armed (its header says when that is). Deliberately not
// `PageLatchMode` itself: this header is the interface every store
// implements and must not depend on the latch primitive, and the two name
// different things - the accessor's intent here, the word's state there -
// that happen to coincide today.
//
// `kUnlatched` is a pin and no latch (`UnlatchedPin`).
enum class PinMode : std::uint8_t { kShared, kExclusive, kUnlatched };

// **Creations claimed ahead of the writes they serve** (`PageStore::ClaimNew`).
// A split writes a page and then creates the next one, and a creation can
// fail for the device - a file that cannot grow, a free-map region that
// cannot be read - as well as for the pool; failing after the first write
// leaves the structure torn (`docs/inflight/bugs/a-fetch-refused-after-a-
// page-write-leaves-the-mutation-half-done.md`). A claim does everything a
// creation can fail at - the frame slot, the id, the device's room for it -
// so `CreateNew` here only publishes a claimed page. Claims not used go
// back when the handle dies. Past its claims - a store that claims nothing,
// having no device to fail - `CreateNew` is the store's own.
class PageClaims {
public:
    PageClaims() = default;
    PageClaims(PageClaims&& other) noexcept
        : store_(std::exchange(other.store_, nullptr)), ids_(std::move(other.ids_)),
          next_(other.next_) {}
    PageClaims& operator=(PageClaims&&) = delete;
    PageClaims(const PageClaims&) = delete;
    inline ~PageClaims();

    // The next claimed page, created zeroed and pinned exclusive.
    inline StatusOr<std::pair<PageId, PageRef>> CreateNew();

private:
    friend class PageStore;
    PageStore* store_ = nullptr;
    std::vector<PageId> ids_;
    std::size_t next_ = 0;
};

// A pin with no latch (`PageStore::PinUnlatched`): the frame stays in the
// pool and nothing more, so a later `Get` of it is a hit neither the pool nor
// the device can refuse, and the holder takes no place in any latch order.
// A split that grows a root pins the anchor it will re-publish through this
// way before it writes anything, and takes the anchor's latch only where it
// always did, after every tree page - holding the anchor exclusive across
// the split instead would put it ahead of another core's index pages, and a
// page latch has no timeout (`docs/spec/page.md` §3). Move-only; the pin
// drops with the handle, and an empty one holds nothing.
class UnlatchedPin {
public:
    UnlatchedPin() noexcept = default;
    UnlatchedPin(UnlatchedPin&& other) noexcept
        : store_(std::exchange(other.store_, nullptr)), page_id_(other.page_id_) {}
    inline UnlatchedPin& operator=(UnlatchedPin&& other) noexcept;
    UnlatchedPin(const UnlatchedPin&) = delete;
    UnlatchedPin& operator=(const UnlatchedPin&) = delete;
    ~UnlatchedPin() { Release(); }
    inline void Release() noexcept;

private:
    friend class PageStore;
    UnlatchedPin(PageStore* store, PageId page_id) noexcept : store_(store), page_id_(page_id) {}
    PageStore* store_ = nullptr;
    PageId page_id_ = kInvalidPageId;
};

class PageStore {
public:
    virtual ~PageStore() = default;

    // ---- The pinned accessors: the engine's API ------------------------
    //
    // Non-virtual on purpose: each is `FetchPinned`/`CreatePinned` plus a
    // handle; the seam a store implements is the raw fetch (below) plus
    // the frame hooks.

    // ---- The fetch-and-pin pair (AM-S2) -------------------------------
    //
    // **One overridable operation, because under the shared pool the fetch
    // and the pin must be atomic and cannot be made so from here**:
    // DevicePageStore latches its frame table across the pair. The default
    // is plain fetch-then-pin, true for a store with no eviction
    // (`InMemoryPageStore`, `page_mgr`'s `BufferPool`). The create accessors
    // have their own (`CreatePinned`, below).
    //
    // `for_read` picks which raw fetch runs: the two differ only in dirty
    // marking, and an override needs to know which it is servicing. `heat` says how the fetch counts toward the frame's usage
    // (`FetchHeat`); a store with no reclaim has no counter to bump.
    virtual StatusOr<std::span<std::byte, kPageSize>> FetchPinned(PageId page_id, PinMode mode,
                                                                 bool for_read,
                                                                 FetchHeat /*heat*/) {
        auto bytes = for_read ? GetForReadUnpinned(page_id) : GetUnpinned(page_id);
        if (!bytes.ok()) return bytes.status();
        PinFrame(page_id, mode);
        return bytes.value();
    }

    // Fetches an already-created page, pinned, for read or in-place
    // mutation. Fails with NotFound if page_id was never created.
    StatusOr<PageRef> Get(PageId page_id) {
        auto bytes = FetchPinned(page_id, PinMode::kExclusive, /*for_read=*/false,
                                 FetchHeat::kWarm);
        if (!bytes.ok()) return bytes.status();
        return PageRef(this, page_id, bytes.value());
    }

    // The accessor a page walk's `PageAccess` names, so a visitor does not
    // spell the mapping itself: `kWrite` is `Get`, `kRead` is `GetForRead`,
    // and `kScan` is `GetForRead` with a fault entering cold (BE-R5).
    StatusOr<PageRef> Fetch(PageId page_id, PageAccess access) {
        if (access == PageAccess::kWrite) return Get(page_id);
        auto bytes = FetchPinned(page_id, PinMode::kShared, /*for_read=*/true,
                                 access == PageAccess::kScan ? FetchHeat::kScan
                                                             : FetchHeat::kWarm);
        if (!bytes.ok()) return bytes.status();
        return PageRef(this, page_id, bytes.value());
    }

    // Get() for a caller that will not write through the page. The bytes
    // are still mutable and the promise is by contract, not by type
    // (GetForReadUnpinned's note); a read fetch that turns out to write
    // calls MarkDirty() on the handle.
    StatusOr<PageRef> GetForRead(PageId page_id) { return Fetch(page_id, PageAccess::kRead); }

    // **The create half of the fetch-and-pin pair** (AM-S2). Same obligation
    // as `FetchPinned` and the same default - create, then pin.
    //
    // `which` picks the raw create rather than three virtuals: the three
    // differ only in how the id is chosen and whether a header is stamped,
    // and a store overriding this needs to know which it is servicing.
    // `kClaimed` publishes an id `ClaimNew` claimed.
    enum class CreateKind : std::uint8_t { kAt, kNew, kNewHeaderless, kClaimed };

    virtual StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreatePinned(
        CreateKind which, PageId page_id) {
        if (which == CreateKind::kAt || which == CreateKind::kClaimed) {
            auto bytes = which == CreateKind::kAt ? CreateAtUnpinned(page_id)
                                                  : CreateClaimedUnpinned(page_id);
            if (!bytes.ok()) return bytes.status();
            PinFrame(page_id, PinMode::kExclusive);
            return std::pair<PageId, std::span<std::byte, kPageSize>>(page_id, bytes.value());
        }
        auto made = which == CreateKind::kNew ? CreateNewUnpinned() : CreateNewHeaderlessUnpinned();
        if (!made.ok()) return made.status();
        PinFrame(made.value().first, PinMode::kExclusive);
        return made;
    }

    // Creates a brand-new page at exactly `page_id`, zero-initialized and
    // pinned. Fails with AlreadyExists if that id is already in use.
    StatusOr<PageRef> CreateAt(PageId page_id) {
        auto made = CreatePinned(CreateKind::kAt, page_id);
        if (!made.ok()) return made.status();
        return PageRef(this, made.value().first, made.value().second);
    }

    // Creates a brand-new page at an id the store chooses, zero-
    // initialized and pinned. Fails with OutOfSpace when out of ids.
    StatusOr<std::pair<PageId, PageRef>> CreateNew() {
        auto made = CreatePinned(CreateKind::kNew, kInvalidPageId);
        if (!made.ok()) return made.status();
        return std::pair<PageId, PageRef>(
            made.value().first, PageRef(this, made.value().first, made.value().second));
    }

    // Faults `page_id` in and pins it with no latch (`UnlatchedPin`);
    // `kInvalidPageId` pins nothing and answers an empty handle.
    StatusOr<UnlatchedPin> PinUnlatched(PageId page_id) {
        if (page_id == kInvalidPageId) return UnlatchedPin();
        auto bytes = FetchPinned(page_id, PinMode::kUnlatched, /*for_read=*/true,
                                 FetchHeat::kWarm);
        if (!bytes.ok()) return bytes.status();
        return UnlatchedPin(this, page_id);
    }

    // Claims `n` creations ahead (`PageClaims`): every way a creation can
    // fail happens here, before the caller writes anything.
    StatusOr<PageClaims> ClaimNew(std::size_t n) {
        PageClaims claims;
        claims.store_ = this;
        for (std::size_t i = 0; i < n; ++i) {
            auto id = ClaimNewId();
            if (!id.ok()) return id.status();  // the claims so far go back
            if (id.value() == kInvalidPageId) break;  // a store that claims nothing
            claims.ids_.push_back(id.value());
        }
        return claims;
    }

    // CreateNew() for a page with no common header - see
    // CreateNewHeaderlessUnpinned() for who needs that and why.
    StatusOr<std::pair<PageId, PageRef>> CreateNewHeaderless() {
        auto made = CreatePinned(CreateKind::kNewHeaderless, kInvalidPageId);
        if (!made.ok()) return made.status();
        return std::pair<PageId, PageRef>(
            made.value().first, PageRef(this, made.value().first, made.value().second));
    }

    // ---- The frame hooks: what a pooled store overrides ---------------
    //
    // Defaults are no-ops, and for every store without eviction that is
    // the *true* implementation, not a stub: a frame that can never be
    // reclaimed needs no pin to stay resident. DevicePageStore overrides
    // all three onto its Frame metadata (pins, dirty, and the latch word
    // when armed - the mode is what tells it shared from exclusive).
    virtual void PinFrame(PageId page_id, PinMode mode) noexcept {
        (void)page_id;
        (void)mode;
    }
    virtual void UnpinFrame(PageId page_id) noexcept { (void)page_id; }
    // A `kUnlatched` pin's release: the pin alone, there being no latch.
    virtual void UnpinUnlatchedFrame(PageId page_id) noexcept { (void)page_id; }
    virtual void MarkFrameDirty(PageId page_id) noexcept { (void)page_id; }

    // Outstanding pins across the store, for the suspend audit
    // (exec::InstallSuspendAudit): "a coroutine may not suspend holding a
    // pin" is mechanically `live_pins() == 0` at the suspension point. A
    // store whose PinFrame is the no-op above counts nothing and answers
    // 0, which is honest - it has no reclaim for a pin to hold off.
    virtual std::size_t live_pins() const noexcept { return 0; }

    // ---- FM10: what the allocation map costs -------------------------
    //
    // Reported by SHOW META, which holds a PageStore and stays blind to
    // which concrete store it has. Defaulted to a map of nothing, which is
    // the truth for every store that has no free map - the in-memory one.
    struct MapResidency {
        // Regions loaded or created; one per 65,280 ids of file.
        std::size_t regions = 0;
        // Bitmap pages held in memory: one per region, plus one more for
        // each region that has a headerless bitmap. **Resident is
        // in-existence** for the free map - every region the device holds
        // is loaded at mount - so the gap between this and 2x regions is
        // exactly what FM6's deferred headerless bitmaps save.
        std::size_t resident_pages = 0;
        // Ids the resident maps can answer about. Not the file's size: the
        // top region is partial.
        std::uint64_t coverage_ids = 0;
        // Whether any headerless page exists anywhere. False is the common
        // case and is what lets IsHeaderless answer with no lookup.
        bool has_headerless = false;
    };
    virtual MapResidency map_residency() const noexcept { return {}; }

    // BF-R11's store half, for `SHOW META`: pages allocated now, pages
    // `FreePage` has freed, and creations served from its free list rather
    // than the cursor. Zero for a store that cannot free.
    struct AllocationCounters {
        std::uint64_t allocated = 0;
        std::uint64_t freed = 0;
        std::uint64_t reused = 0;
    };
    virtual AllocationCounters allocation_counters() const noexcept { return {}; }
    // The buffer pool's counters (BE-R2, `eviction.md` EV9), for `SHOW
    // META`. Monotonic since the store opened. A store with no pool answers
    // zeros.
    struct PoolCounters {
        std::uint64_t budget = 0;         // frames the pool may hold
        std::uint64_t resident = 0;       // frames holding a page now
        std::uint64_t slots = 0;          // slots that exist
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;         // device reads and creations
        std::uint64_t reclaimed_inline = 0;
        std::uint64_t reclaimed_background = 0;
        std::uint64_t batches_inline = 0;
        std::uint64_t batches_background = 0;
        std::uint64_t batches_partial = 0;  // inline batches that freed less than asked
        std::uint64_t batch_steps = 0;      // slots the inline batches walked
        // Slots the tick's batches walked. With `batch_steps`, every step the
        // hand takes in a running server - over `slots`, its laps (BG-S1).
        std::uint64_t batch_steps_background = 0;
        std::uint64_t dirty_queued = 0;     // dirty victims queued for writeback
        std::uint64_t dirty_drained = 0;    // queued pages the drain wrote clean
        std::uint64_t refused = 0;          // reservations refused at the cap
    };
    virtual PoolCounters pool_counters() const { return {}; }

    // ---- Where a full pool may refuse (BE-R4, BE-Q11) -------------------
    //
    // A pool at its cap with nothing reclaimable refuses a fault or a
    // creation `ResourceExhausted`. That is safe only where the mutation
    // has written nothing yet: a refusal after a page write leaves the
    // write with nothing that undoes it (`docs/inflight/bugs/a-fetch-refused-
    // after-a-page-write-leaves-the-mutation-half-done.md`). So each
    // mutation that writes and then fetches opens a **no-refuse window**
    // before its first write. Opening it takes a share of the pool for the
    // window, and **that** is where a full pool refuses. Inside, fills draw
    // on the share, and a mutation that outgrows it stops the instance
    // rather than return an error into a half-done write. Windows nest; the
    // outermost one holds the share. Per thread, and sound because no window
    // holds a suspension point: every window is opened and closed inside one
    // synchronous call.
    //
    // **Drain mode** is for the passes that are not a row's mutation and
    // hold no page latch between their steps - recovery's redo and undo, a
    // rollback, an assertion's commit and abort loops. At the cap, a fill on
    // such a thread holding no pin writes the dirty queue back and retries,
    // which is sound there and nowhere else (`device_page_store.hpp`'s
    // durability-wait bullet), instead of being refused.
    //
    // A store with no pool does nothing for either.
    virtual Status BeginNoRefuseWindow(std::size_t frames) {
        (void)frames;
        return Status::OK();
    }
    virtual void EndNoRefuseWindow() noexcept {}
    virtual void BeginDrainOnPressure() noexcept {}
    virtual void EndDrainOnPressure() noexcept {}

    // ---- The raw seam: protected since MG06 ----------------------------
    //
    // What a store implements - and, since MG06, *only* what a store
    // implements: engine code cannot name these through the base class, so
    // "no caller holds a raw span" is enforced by access control rather
    // than by review. A concrete store may re-publish its own overrides
    // (InMemoryPageStore does, for the forwarding test doubles); a store
    // with real eviction (DevicePageStore) keeps them out of reach. The
    // returned span models no pin: it is valid only while nothing evicts,
    // which is the defect the pinned accessors above exist to close.

protected:
    virtual StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) = 0;

    // A claim's two halves and its return (`PageClaims`). The default claims
    // nothing - `kInvalidPageId` - and is the true implementation for a
    // store with no device and no pool, whose creation cannot fail after a
    // write any more than before it.
    virtual StatusOr<PageId> ClaimNewId() { return kInvalidPageId; }
    virtual StatusOr<std::span<std::byte, kPageSize>> CreateClaimedUnpinned(PageId page_id) {
        return Status::InvalidArgument("page " + std::to_string(page_id) +
                                       " was not claimed from this store");
    }
    virtual void ReleaseClaimedId(PageId page_id) noexcept { (void)page_id; }
    friend class PageClaims;

    virtual StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() = 0;

public:
    // Raises the floor CreateNew() allocates from: after an OK return, no
    // CreateNew() may hand out an id below `first_allocatable_page_id`.
    //
    // **Monotonic.** A floor at or below the current one is an accepted
    // no-op, never a lowering - a floor that could fall would re-open the
    // hazard it exists to close, and every caller so far only ever raises.
    //
    // Its one production caller is recovery (docs/spec/wal.md §12): the durable record of which
    // ids exist is unlogged, so a crash can revert it while the log still
    // names pages above it, and an allocation afterwards would hand out a
    // page redo has already written. Raising the floor past every id the
    // log names is what makes that impossible.
    //
    // The cost is ids: the cursor skips every clear bit below the floor for
    // the rest of the run. That is what a high-water mark means, and it is
    // cheap because CreateNew() hands out the lowest free id, so few gaps
    // lie below the mark. **A clear bit below the floor is not proof that an
    // id is free** - the log may name it - which is why a page freed during
    // the run comes back through `DevicePageStore`'s free list rather than
    // by lowering the cursor: the list holds only ids a reclaim checked and
    // gated (BF-R6), and the floor does not gate it. After a crash the list
    // is gone and the floor hides the run's frees below it until a clean
    // restart.
    //
    // **The default refuses**, and does not quietly succeed. A store that
    // ignored the raise would let recovery report a repair that did not
    // happen, which is worse than a mount that fails naming the store -
    // RV1's rule that a failed recovery refuses the mount rather than
    // serving a partial database.
    virtual Status RaiseAllocationFloor(PageId first_allocatable_page_id) {
        (void)first_allocatable_page_id;
        return Status::Unsupported(
            "PageStore: this store cannot raise its allocation floor, so recovery cannot "
            "guarantee it will not re-issue a page id the log names");
    }

    // ---- Freeing a page (BF-R5) -----------------------------------------
    //
    // **Gives `page_id` back to the allocator**: its resident frame, dirty or
    // not, is discarded and its allocation bit cleared, in one hold, and the
    // id becomes one a later creation may hand out again. The caller owns
    // the precondition, which nothing here can check: no record that names
    // the page can be replayed again, and no reader can reach it
    // (`instructions/v3.0.0/workorder-bf-drop-table-page-reclaim.md` BF-R4,
    // BF-R8, BF-R9). The page's bytes are dead once that holds, so writing a
    // dirty frame back would cost I/O for nothing and could land over a
    // reuse.
    //
    // `kDeferred` changes nothing: the frame is pinned, latched, claimed by a
    // writeback, or a fault or a creation of the id is in flight, and the
    // caller asks again later. `NotFound` is an id already free, which a
    // re-driven reclaim reads as done. `Corruption` is an id no relation
    // owns - the system range, a map page, a headerless page or a Bound
    // Cabin page: a walk that reached one has read a page that is not the
    // relation's.
    //
    // **The default refuses `NotImplemented`**, so a store that cannot free
    // reports a leak rather than a free that never happened. Not
    // `Unsupported`, as `RaiseAllocationFloor`'s default is: a store that
    // cannot free is one a later release could teach to, with no change to
    // the architecture (`status.hpp`'s test).
    enum class FreeOutcome : std::uint8_t { kFreed, kDeferred };
    virtual StatusOr<FreeOutcome> FreePage(PageId page_id) {
        (void)page_id;
        return Status::NotImplemented("PageStore: this store cannot free a page");
    }

    // ---- A var-heap chain's last tail (BN-R8,
    // `workorder-bn-opt-read-update-path.md`) --------------------------
    //
    // `varheap::ChainAppend` remembers here the page it appended into, so
    // the next append to the same chain starts its walk there rather than
    // at the root (`varheap.cpp` says why that is safe). A slot two roots
    // hash to keeps whichever wrote last, and the other's next append walks
    // from its root, as every append did before BN. One table per store;
    // relaxed atomics, because a hint is only ever a start the walk then
    // re-checks link by link, so no ordering with any other write is owed.
    PageId VarHeapTailHint(PageId root) const noexcept {
        const std::uint64_t slot =
            varheap_tail_hints_[TailHintIndex(root)].load(std::memory_order_relaxed);
        return static_cast<PageId>(slot >> 32) == root ? static_cast<PageId>(slot)
                                                       : kInvalidPageId;
    }
    void SetVarHeapTailHint(PageId root, PageId tail) noexcept {
        varheap_tail_hints_[TailHintIndex(root)].store(
            (static_cast<std::uint64_t>(root) << 32) | tail, std::memory_order_relaxed);
    }

private:
    // 2^10 = 1,024 chains before two share a slot (BN-R8); 8 KiB a store.
    // A slot is `root << 32 | tail`, so a lookup tells its own root from
    // another's; an empty slot reads as root 0, a page id no chain is rooted
    // at. The index is the top `kTailHintBits` of a Fibonacci hash.
    static constexpr unsigned kTailHintBits = 10;
    static constexpr std::size_t kVarHeapTailHints = std::size_t{1} << kTailHintBits;
    static std::size_t TailHintIndex(PageId root) noexcept {
        return (static_cast<std::uint64_t>(root) * 0x9E3779B97F4A7C15ull) >> (64 - kTailHintBits);
    }
    std::array<std::atomic<std::uint64_t>, kVarHeapTailHints> varheap_tail_hints_{};

protected:
    // Fetches an already-created page's bytes for reading or in-place
    // mutation. Fails with NotFound if page_id was never created.
    virtual StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) = 0;

    // GetUnpinned() for a caller that will not write through the returned
    // span. A store that tracks dirty frames may use this to leave the
    // frame clean; one that does not is already correct doing nothing,
    // which is why the default is plain GetUnpinned().
    //
    // The span is still mutable, and the promise is by contract rather
    // than by type: the type-safe shape is a const page view, which is a
    // mechanical refactor across every page layer (heap, btree)
    // and is deliberately not attempted here. Writing through this span is
    // a defect - the write lands in the frame and may never reach the
    // device.
    //
    // The opt-in direction is the safe one. A caller that forgets to use
    // this pays an unnecessary write-back; the inverse design - a fetch
    // that leaves frames clean plus an explicit MarkDirty() - loses data
    // the first time someone forgets the call.
    virtual StatusOr<std::span<std::byte, kPageSize>> GetForReadUnpinned(PageId page_id) {
        return GetUnpinned(page_id);
    }

    // CreateNewUnpinned() for a page that carries no common page header -
    // the whole 8 KiB belongs to the caller. For a payload that tiles the
    // page exactly: a power-of-two entry array a header would cost an
    // entry of, breaking the shift/mask addressing that is the point of
    // it. Its one caller is the waystone directory's interior pages
    // (stats/waystone_dir.hpp) - see DevicePageStore's header for what
    // giving up the header costs.
    //
    // The default is plain CreateNewUnpinned(), which is correct for any
    // store that neither stamps nor verifies a page checksum - there is
    // nothing to opt out of. A store that does (DevicePageStore)
    // overrides it to record the fact durably, because getting this wrong
    // writes a checksum over live data at byte 4.
    virtual StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>>
    CreateNewHeaderlessUnpinned() {
        return CreateNewUnpinned();
    }

public:
    // Records that the WAL record at `lsn` modified `page_id`: stamps the
    // page header's page_lsn, which is what a store's write-back path
    // compares against the log's durable watermark (wal.md section 8-1).
    // A logged mutation calls this after appending its record and before
    // acknowledging the client.
    //
    // The default does exactly the stamp and nothing else, which is the
    // whole of the obligation for a store with no stable storage under it:
    // there is no write-back to order against the log. A store that does
    // write back overrides this to also track the frame's recLSN and to
    // hold the gate (DevicePageStore). No store stamps a stream: a page
    // carries none since BP-S3 (one log for the instance).
    virtual Status StampPageLsn(PageId page_id, std::uint64_t lsn) {
        auto page = Get(page_id);
        if (!page.ok()) return page.status();
        SetPageLsn(page.value().bytes(), lsn);
        return Status::OK();
    }

    // Makes everything written through this store durable: after an OK
    // return, the state survives the process dying by any means.
    //
    // Not pure virtual, and the default is OK because it is *true* for a
    // store with no stable storage under it - an InMemoryPageStore has
    // nothing that could outlive the process, so there is nothing it could
    // fail to persist. That keeps callers from having to ask which kind of
    // store they hold.
    //
    // A whole-store durability point; a commit's durability is the WAL's
    // (docs/spec/wal.md), not this call's.
    virtual Status Sync() { return Status::OK(); }

    // A scan fetcher for one bulk sequential read (see ScanFetcher above).
    // The default is the plain pass-through - correct for any store with
    // no pool to protect; DevicePageStore overrides with the real ring.
    virtual std::unique_ptr<ScanFetcher> OpenScanRing(std::size_t frames = kScanRingFrames);
};

// The pass-through fetcher: GetForRead, page by page. What ring mode
// *means* on a store that never evicts.
//
// Holds the previous fetch's PageRef, which implements the ScanFetcher
// lifetime contract *exactly*: the span is valid until the next Fetch(),
// because that is when the previous pin drops.
class PlainScanFetcher final : public ScanFetcher {
public:
    explicit PlainScanFetcher(PageStore& store) noexcept : store_(store) {}
    StatusOr<std::span<std::byte, kPageSize>> Fetch(PageId page_id) override {
        auto page = store_.GetForRead(page_id);
        if (!page.ok()) return page.status();
        last_ = std::move(page.value());
        return last_.bytes();
    }

private:
    PageStore& store_;
    PageRef last_;
};

inline std::unique_ptr<ScanFetcher> PageStore::OpenScanRing(std::size_t /*frames*/) {
    return std::make_unique<PlainScanFetcher>(*this);
}

inline UnlatchedPin& UnlatchedPin::operator=(UnlatchedPin&& other) noexcept {
    if (this != &other) {
        Release();
        store_ = std::exchange(other.store_, nullptr);
        page_id_ = other.page_id_;
    }
    return *this;
}

inline void UnlatchedPin::Release() noexcept {
    if (store_ != nullptr) std::exchange(store_, nullptr)->UnpinUnlatchedFrame(page_id_);
}

inline PageClaims::~PageClaims() {
    if (store_ == nullptr) return;
    // Highest first, so a store's cursor steps back over every id it gave.
    for (std::size_t i = ids_.size(); i-- > next_;) store_->ReleaseClaimedId(ids_[i]);
}

inline StatusOr<std::pair<PageId, PageRef>> PageClaims::CreateNew() {
    if (next_ == ids_.size()) {
        // Only a store that claims nothing gets here: a claiming store run
        // past its claims is a caller that counted its creations wrong.
        assert(ids_.empty() && "a claiming store ran past its claims");
        return store_->CreateNew();
    }
    const PageId id = ids_[next_++];
    auto made = store_->CreatePinned(PageStore::CreateKind::kClaimed, id);
    if (!made.ok()) return made.status();
    return std::pair<PageId, PageRef>(id, PageRef(store_, id, made.value().second));
}

inline PageRef& PageRef::operator=(PageRef&& other) noexcept {
    if (this != &other) {
        Release();
        store_ = other.store_;
        page_id_ = other.page_id_;
        data_ = other.data_;
        other.store_ = nullptr;
        other.page_id_ = kInvalidPageId;
        other.data_ = nullptr;
    }
    return *this;
}

inline void PageRef::Release() noexcept {
    if (store_ != nullptr) {
        store_->UnpinFrame(page_id_);
        store_ = nullptr;
        data_ = nullptr;
    }
}

inline void PageRef::MarkDirty() noexcept {
    if (store_ != nullptr) store_->MarkFrameDirty(page_id_);
}

// The window's share, in frames (BE-Q11): what one row's mutation is
// allowed to fault or create after its first write - eight times
// `kPinCeiling`'s per-operation bound, so a clustered split, an index
// split per index, an undo page and a spill fit with room to spare.
inline constexpr std::size_t kWindowFrames = 64;

// A no-refuse window, held for one mutation (`PageStore`'s note). `Open`
// is the refusal point: it fails `ResourceExhausted` when the pool cannot
// promise the share, and the caller has written nothing yet.
class NoRefuseWindow {
public:
    [[nodiscard]] static StatusOr<NoRefuseWindow> Open(PageStore& store,
                                                       std::size_t frames = kWindowFrames) {
        if (Status s = store.BeginNoRefuseWindow(frames); !s.ok()) return s;
        return NoRefuseWindow(&store);
    }
    NoRefuseWindow(NoRefuseWindow&& other) noexcept : store_(std::exchange(other.store_, nullptr)) {}
    NoRefuseWindow& operator=(NoRefuseWindow&&) = delete;
    NoRefuseWindow(const NoRefuseWindow&) = delete;
    ~NoRefuseWindow() {
        if (store_ != nullptr) store_->EndNoRefuseWindow();
    }

private:
    explicit NoRefuseWindow(PageStore* store) noexcept : store_(store) {}
    PageStore* store_;
};

// Whether this thread is inside a no-refuse window on any store - what the
// executor's suspend audit asks, since no window may hold a suspension point.
bool NoRefuseWindowOpenOnThisThread() noexcept;

// Drain mode for the life of the guard (`PageStore`'s note).
class DrainOnPressure {
public:
    explicit DrainOnPressure(PageStore& store) noexcept : store_(store) {
        store_.BeginDrainOnPressure();
    }
    ~DrainOnPressure() { store_.EndDrainOnPressure(); }
    DrainOnPressure(const DrainOnPressure&) = delete;
    DrainOnPressure& operator=(const DrainOnPressure&) = delete;

private:
    PageStore& store_;
};

}  // namespace kds::storage
