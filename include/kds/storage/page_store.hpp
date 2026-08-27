#pragma once

#include <memory>
#include <span>
#include <utility>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// Abstract "give me the bytes for page_id" seam. This exists so that
// higher layers (catalog/) can be written and tested today against
// something that behaves like storage, without waiting on the real
// buffer pool - which needs its own not-yet-decided eviction policy and
// I/O backend (both open items in CLAUDE.md). Swapping in the real
// buffer pool later means implementing this interface, not rewriting
// every caller.
//
// **The pin model (docs/workplan-pageref.md MG01).** The public accessors
// return a `PageRef` - a move-only handle whose lifetime IS the validity of
// the bytes: construction pins the frame, destruction unpins it, and a store
// that never evicts implements pin/unpin as no-ops, which for it is a true
// statement rather than a stub. There is deliberately **no implicit
// conversion from PageRef to a span**: `auto s = store.Get(id).value()`
// followed by using `s` as bytes would compile and dangle the moment the
// temporary unpinned - the exact bug eviction introduces. Access is
// `ref.bytes()`, on a named handle whose lifetime is visible in the code.
//
// The `*Unpinned` accessors below are the migration-era escape hatch and
// the implementation seam: they are what MG02/MG03 convert call sites off
// of, and MG06 deletes them. New code must not call them.

namespace kds::storage {

// Which of PageStore::Get()/GetForRead() a page walk should fetch through.
// Named rather than a bool because it appears at call sites far from the
// declaration, and because picking the wrong one on a mutating walk loses
// the write silently - see GetForRead() below.
enum class PageAccess {
    kRead,   // the visitor will not write through the page
    kWrite,  // the visitor may modify tuples in place
};

// How a bulk sequential reader fetches pages (docs/spec/eviction.md §5,
// workplan EVT06). A scan that faults every page of a large relation
// through the ordinary path floods the pool with frames it will touch
// exactly once; ring mode reuses a small fixed set of frames cyclically
// and never bumps usage counters, so a scan's touch does not look like
// heat and the foreground working set stays where it was.
//
// The seam is a virtual fetcher rather than a store method because the
// callers that need it - the relayout planner's survey, the cabin
// optimizer's builds (PO4 makes ring routing mandatory there), aggregate
// full scans - hold a `PageStore&` and must not know which concrete store
// is under them. The base-class ring is deliberately plain: a store that
// never evicts has no pool to protect, so fetching through `GetForRead`
// *is* its correct ring, and only `DevicePageStore` overrides with the
// real cyclic one.
class ScanFetcher {
public:
    virtual ~ScanFetcher() = default;

    // Read-only by contract, exactly as GetForRead(): the span is mutable
    // for the same mechanical reason and writing through it is the same
    // defect. Valid until the next Fetch() on this fetcher - ring rotation
    // may drop the previous page's frame - which is a *stricter* lifetime
    // than the ordinary accessors give, and the reason a ring consumer
    // finishes each page before fetching the next.
    virtual StatusOr<std::span<std::byte, kPageSize>> Fetch(PageId page_id) = 0;
};

// Frames one ring holds (`kds.scan_ring_frames`, spec §6). `[PROPOSED]`
// 32; a parameter of OpenScanRing rather than a behavior anything may
// depend on.
inline constexpr std::size_t kScanRingFrames = 32;

class PageStore;

// The move-only pinned-page handle (page.md S2, workplan-pageref.md).
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
    PageRef(PageStore* store, PageId page_id, std::span<std::byte, kPageSize> bytes) noexcept
        : store_(store), page_id_(page_id), data_(bytes.data()) {}

    PageStore* store_ = nullptr;
    PageId page_id_ = kInvalidPageId;
    // A pointer rather than a span, because `std::span<T, N>` with a fixed
    // extent has no empty state - and a moved-from or released handle
    // needs one.
    std::byte* data_ = nullptr;
};

class PageStore {
public:
    virtual ~PageStore() = default;

    // ---- The pinned accessors: the engine's API ------------------------
    //
    // Non-virtual on purpose: each is the raw fetch plus the pin, and the
    // seam a store implements is the raw fetch (below) plus the three
    // frame hooks. Nothing can evict between the fetch and the pin - a
    // core is single-threaded and nothing here suspends - so the pair is
    // atomic in the only sense that matters.

    // Fetches an already-created page, pinned, for read or in-place
    // mutation. Fails with NotFound if page_id was never created.
    StatusOr<PageRef> Get(PageId page_id) {
        auto bytes = GetUnpinned(page_id);
        if (!bytes.ok()) return bytes.status();
        PinFrame(page_id);
        return PageRef(this, page_id, bytes.value());
    }

    // Get() for a caller that will not write through the page. The bytes
    // are still mutable and the promise is by contract, not by type
    // (GetForReadUnpinned's note); a read fetch that turns out to write
    // calls MarkDirty() on the handle.
    StatusOr<PageRef> GetForRead(PageId page_id) {
        auto bytes = GetForReadUnpinned(page_id);
        if (!bytes.ok()) return bytes.status();
        PinFrame(page_id);
        return PageRef(this, page_id, bytes.value());
    }

    // Creates a brand-new page at exactly `page_id`, zero-initialized and
    // pinned. Fails with AlreadyExists if that id is already in use.
    StatusOr<PageRef> CreateAt(PageId page_id) {
        auto bytes = CreateAtUnpinned(page_id);
        if (!bytes.ok()) return bytes.status();
        PinFrame(page_id);
        return PageRef(this, page_id, bytes.value());
    }

    // Creates a brand-new page at an id the store chooses, zero-
    // initialized and pinned. Fails with OutOfSpace when out of ids.
    StatusOr<std::pair<PageId, PageRef>> CreateNew() {
        auto made = CreateNewUnpinned();
        if (!made.ok()) return made.status();
        PinFrame(made.value().first);
        return std::pair<PageId, PageRef>(
            made.value().first, PageRef(this, made.value().first, made.value().second));
    }

    // CreateNew() for a page with no common header - see
    // CreateNewHeaderlessUnpinned() for who needs that and why.
    StatusOr<std::pair<PageId, PageRef>> CreateNewHeaderless() {
        auto made = CreateNewHeaderlessUnpinned();
        if (!made.ok()) return made.status();
        PinFrame(made.value().first);
        return std::pair<PageId, PageRef>(
            made.value().first, PageRef(this, made.value().first, made.value().second));
    }

    // ---- The frame hooks: what a pooled store overrides ---------------
    //
    // Defaults are no-ops, and for every store without eviction that is
    // the *true* implementation, not a stub: a frame that can never be
    // reclaimed needs no pin to stay resident. DevicePageStore overrides
    // all three onto its Frame metadata (pins, dirty).
    virtual void PinFrame(PageId page_id) noexcept { (void)page_id; }
    virtual void UnpinFrame(PageId page_id) noexcept { (void)page_id; }
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

    virtual StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() = 0;

public:
    // Raises the floor CreateNew() allocates from: after an OK return, no
    // CreateNew() may hand out an id below `first_allocatable_page_id`.
    //
    // **Monotonic.** A floor at or below the current one is an accepted
    // no-op, never a lowering - a floor that could fall would re-open the
    // hazard it exists to close, and every caller so far only ever raises.
    //
    // Its one production caller is recovery (docs/spec/wal.md §12,
    // docs/workplan-wal-recovery.md RV4/RC04): the durable record of which
    // ids exist is unlogged, so a crash can revert it while the log still
    // names pages above it, and an allocation afterwards would hand out a
    // page redo has already written. Raising the floor past every id the
    // log names is what makes that impossible.
    //
    // The cost is ids: every free id below the floor is skipped for the
    // life of the instance. That is what a high-water mark means, and it is
    // cheap here because nothing frees a page (page.md §5) and CreateNew()
    // hands out the lowest free id, so there are almost no gaps below the
    // mark to skip.
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
    // Whether this store's core may **write** `page_id`. True by default:
    // a store with no core ownership is the system core's arrangement and
    // may write anything. DevicePageStore overrides it with the lease, the
    // grants and the stamp claims (PW1c-4, PW1c-7); it is on the interface
    // so a dispatcher can ask before a write reaches the frame-load path's
    // refusal, and answer retryably with the relation's name.
    virtual bool MayWrite(PageId /*page_id*/) const noexcept { return true; }

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
    // hold the gate (DevicePageStore) - and, since PW1c-3, to stamp the
    // owning stream (PL §9 rule 4). The default deliberately does not: a
    // store with no stable storage never survives into a recovery where
    // streams could be confused, and it knows no core to stamp.
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
    // This is a whole-store durability point, which is all there is until
    // the WAL lands (docs/spec/wal.md): with one, durability becomes
    // per-transaction (group commit, and KWP/1's per-transaction
    // durability class, docs/spec/protocol.md) and calling this per statement
    // stops being the right shape.
    virtual Status Sync() { return Status::OK(); }

    // A scan fetcher for one bulk sequential read (see ScanFetcher above).
    // The default is the plain pass-through - correct for any store with
    // no pool to protect - so every existing store and test behaves
    // exactly as before; DevicePageStore overrides with the real ring.
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

}  // namespace kds::storage
