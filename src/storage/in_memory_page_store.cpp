#include "kds/storage/in_memory_page_store.hpp"

#include <cstring>

namespace kds::storage {

InMemoryPageStore::InMemoryPageStore(PageId first_new_page_id) noexcept
    : next_new_page_id_(first_new_page_id) {}

StatusOr<std::span<std::byte, kPageSize>> InMemoryPageStore::CreateAtUnpinned(PageId page_id) {
    if (pages_.contains(page_id)) {
        return Status::AlreadyExists("page id already in use");
    }

    auto page = std::make_unique<Page>();
    page->fill(std::byte{0});
    std::span<std::byte, kPageSize> view(*page);
    pages_.emplace(page_id, std::move(page));
    return view;
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> InMemoryPageStore::CreateNewUnpinned() {
    // Skip ids `CreateAt` already placed. The cursor is where to *start*
    // looking, not a claim that everything from it upward is free, and
    // nothing advances it when a page is placed by id.
    //
    // DevicePageStore gets this for free: its CreateAt marks the free map
    // and its CreateNew searches that map, so a placed page is simply never
    // offered. This store has no map, so the skip has to be written out -
    // and without it the two stores disagree about a sequence every recovery
    // test performs, `CreateAt` some pages then `CreateNew` another. The
    // failure is not subtle but it is misleading: CreateNew reports "page id
    // already in use" about the page it is standing on.
    while (pages_.contains(next_new_page_id_)) {
        ++next_new_page_id_;
    }
    PageId id = next_new_page_id_;
    auto created = CreateAt(id);
    if (!created.ok()) {
        return created.status();
    }
    ++next_new_page_id_;
    return std::make_pair(id, created.value().bytes());
}

Status InMemoryPageStore::RaiseAllocationFloor(PageId first_allocatable_page_id) {
    if (first_allocatable_page_id > next_new_page_id_) {
        next_new_page_id_ = first_allocatable_page_id;
    }
    return Status::OK();
}

StatusOr<std::span<std::byte, kPageSize>> InMemoryPageStore::GetUnpinned(PageId page_id) {
    auto it = pages_.find(page_id);
    if (it == pages_.end()) {
        return Status::NotFound("page id not found");
    }
    return std::span<std::byte, kPageSize>(*it->second);
}

}  // namespace kds::storage
