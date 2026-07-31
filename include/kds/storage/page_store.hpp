#pragma once

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

    // Fetches an already-created page's bytes for reading or in-place
    // mutation. Fails with NotFound if page_id was never created.
    virtual StatusOr<std::span<std::byte, kPageSize>> Get(PageId page_id) = 0;

    // CreateNew() for a page that carries no common page header - the
    // whole 8 KiB belongs to the caller. Waystone's entry and directory
    // pages are the only ones today (docs/waystone-concpets.md §§5-6):
    // their tilings are exact powers of two and a header would cost a slot
    // and break the shift/mask addressing.
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
};

}  // namespace kds::storage
