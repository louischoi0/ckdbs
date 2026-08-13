#pragma once

#include <array>
#include <memory>
#include <unordered_map>

#include "kds/storage/page_store.hpp"

// A PageStore backed by plain heap-allocated pages in an unordered_map -
// no disk, no eviction, everything lives in process memory for as long as
// the store does. Useful today for catalog/ tests and early bring-up;
// replace with a real buffer-pool-backed PageStore once one exists.

namespace kds::storage {

class InMemoryPageStore final : public PageStore {
public:
    // `first_new_page_id` is the id CreateNew() hands out first (and then
    // increments from). Fixed-id callers (CreateAt) are unaffected by it,
    // but pick a value above any ids you plan to CreateAt (e.g.
    // kds::server::kFirstUserPageId, 128, matches the catalog's fixed
    // pages living below that) - CreateNew does not skip over collisions,
    // it just reports AlreadyExists like CreateAt would.
    explicit InMemoryPageStore(PageId first_new_page_id = 1) noexcept;

    // Public on purpose where the base is protected: this store never
    // evicts, so its raw spans are permanently valid, and the forwarding
    // test doubles (mount_recovery, page_mgr_wal_gate, ...) reach the raw
    // seam through this concrete type rather than through the base.
    // The inherited defaults re-published alongside the overrides below:
    // this store never evicts, so its raw seam is permanently safe, and the
    // forwarding test doubles reach it through this concrete type - the
    // base keeps it protected (MG06).
    using PageStore::CreateNewHeaderlessUnpinned;
    using PageStore::GetForReadUnpinned;

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override;
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override;
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override;

    // The whole of the allocator here is `next_new_page_id_`, so raising
    // the floor is raising it - and this store needs the call more than a
    // durable one does, because CreateNew() does not skip over ids that
    // already exist (see the constructor's note). It does not even walk
    // past them: a failed CreateAt() returns before the increment, so a
    // replay that created the page at `next_new_page_id_` wedges every
    // later allocation at AlreadyExists permanently. Only a raise unwedges
    // it.
    Status RaiseAllocationFloor(PageId first_allocatable_page_id) override;

    // Pages created so far. Nothing evicts, so this is both the resident
    // count and the allocated one. Exposed because "how many pages did
    // that touch" is the assertion sparse-allocation tests are made of,
    // and only a page count can show it.
    std::size_t page_count() const noexcept { return pages_.size(); }

private:
    using Page = std::array<std::byte, kPageSize>;

    std::unordered_map<PageId, std::unique_ptr<Page>> pages_;
    PageId next_new_page_id_;
};

}  // namespace kds::storage
