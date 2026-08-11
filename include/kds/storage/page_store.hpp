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
// This deliberately does not attempt to model pinning, dirty tracking, or
// eviction - it only promises that bytes written through a returned span
// are visible to a later Get() for the same page_id. Those concerns
// belong to whatever the real buffer-pool-backed implementation adds.

namespace kds::storage {

// Which of PageStore::Get()/GetForRead() a page walk should fetch through.
// Named rather than a bool because it appears at call sites far from the
// declaration, and because picking the wrong one on a mutating walk loses
// the write silently - see GetForRead() below.
enum class PageAccess {
    kRead,   // the visitor will not write through the page
    kWrite,  // the visitor may modify tuples in place
};

// How a bulk sequential reader fetches pages (docs/spec-eviction.md §5,
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

class PageStore {
public:
    virtual ~PageStore() = default;

    // Creates a brand-new page at exactly `page_id`, zero-initialized.
    // Fails with AlreadyExists if that id is already in use. For callers
    // that need a page at a specific, well-known id (e.g. the catalog's
    // fixed bootstrap pages) rather than whatever id the store would
    // pick.
    virtual StatusOr<std::span<std::byte, kPageSize>> CreateAt(PageId page_id) = 0;

    // Creates a brand-new page at an id the store chooses, zero-
    // initialized. Fails with OutOfSpace if the store has no more ids to
    // hand out.
    virtual StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNew() = 0;

    // Raises the floor CreateNew() allocates from: after an OK return, no
    // CreateNew() may hand out an id below `first_allocatable_page_id`.
    //
    // **Monotonic.** A floor at or below the current one is an accepted
    // no-op, never a lowering - a floor that could fall would re-open the
    // hazard it exists to close, and every caller so far only ever raises.
    //
    // Its one production caller is recovery (docs/wal.md §12,
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

    // Fetches an already-created page's bytes for reading or in-place
    // mutation. Fails with NotFound if page_id was never created.
    virtual StatusOr<std::span<std::byte, kPageSize>> Get(PageId page_id) = 0;

    // Get() for a caller that will not write through the returned span.
    // A store that tracks dirty frames may use this to leave the frame
    // clean; one that does not is already correct doing nothing, which is
    // why the default is plain Get().
    //
    // The span is still mutable, and the promise is by contract rather
    // than by type: the type-safe shape is a const page view, which is a
    // mechanical refactor across every page layer (heap, btree)
    // and is deliberately not attempted here. Writing through this span is
    // a defect - the write lands in the frame and may never reach the
    // device.
    //
    // The opt-in direction is the safe one. A caller that forgets to use
    // this pays an unnecessary write-back; the inverse design - a Get()
    // that leaves frames clean plus an explicit MarkDirty() - loses data
    // the first time someone forgets the call.
    virtual StatusOr<std::span<std::byte, kPageSize>> GetForRead(PageId page_id) {
        return Get(page_id);
    }

    // CreateNew() for a page that carries no common page header - the
    // whole 8 KiB belongs to the caller. For a payload that tiles the page
    // exactly: a power-of-two entry array a header would cost an entry of,
    // breaking the shift/mask addressing that is the point of it. Its one
    // caller is the waystone directory's interior pages
    // (stats/waystone_dir.hpp) - see DevicePageStore's header for what
    // giving up the header costs.
    //
    // The default is plain CreateNew(), which is correct for any store
    // that neither stamps nor verifies a page checksum - there is nothing
    // to opt out of. A store that does (DevicePageStore) overrides it to
    // record the fact durably, because getting this wrong writes a
    // checksum over live data at byte 4.
    virtual StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewHeaderless() {
        return CreateNew();
    }

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
    // hold the gate (DevicePageStore).
    virtual Status StampPageLsn(PageId page_id, std::uint64_t lsn) {
        auto page = Get(page_id);
        if (!page.ok()) return page.status();
        SetPageLsn(page.value(), lsn);
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
    // the WAL lands (docs/wal.md): with one, durability becomes
    // per-transaction (group commit, and KWP/1's per-transaction
    // durability class, docs/protocol.md) and calling this per statement
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
class PlainScanFetcher final : public ScanFetcher {
public:
    explicit PlainScanFetcher(PageStore& store) noexcept : store_(store) {}
    StatusOr<std::span<std::byte, kPageSize>> Fetch(PageId page_id) override {
        return store_.GetForRead(page_id);
    }

private:
    PageStore& store_;
};

inline std::unique_ptr<ScanFetcher> PageStore::OpenScanRing(std::size_t /*frames*/) {
    return std::make_unique<PlainScanFetcher>(*this);
}

}  // namespace kds::storage
