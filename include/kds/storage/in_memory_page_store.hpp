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

    StatusOr<std::span<std::byte, kPageSize>> CreateAt(PageId page_id) override;
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNew() override;
    StatusOr<std::span<std::byte, kPageSize>> Get(PageId page_id) override;

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
