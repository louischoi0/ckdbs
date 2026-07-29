#pragma once

#include <span>
#include <utility>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

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
