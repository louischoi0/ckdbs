#include "kds/storage/in_memory_page_store.hpp"

#include <cstring>

namespace kds::storage {

InMemoryPageStore::InMemoryPageStore(PageId first_new_page_id) noexcept
    : next_new_page_id_(first_new_page_id) {}

StatusOr<std::span<std::byte, kPageSize>> InMemoryPageStore::CreateAt(PageId page_id) {
    if (pages_.contains(page_id)) {
        return Status::AlreadyExists("page id already in use");
    }

    auto page = std::make_unique<Page>();
    page->fill(std::byte{0});
    std::span<std::byte, kPageSize> view(*page);
    pages_.emplace(page_id, std::move(page));
    return view;
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> InMemoryPageStore::CreateNew() {
    PageId id = next_new_page_id_;
    auto created = CreateAt(id);
    if (!created.ok()) {
        return created.status();
    }
    ++next_new_page_id_;
    return std::make_pair(id, created.value());
}

Status InMemoryPageStore::RaiseAllocationFloor(PageId first_allocatable_page_id) {
    if (first_allocatable_page_id > next_new_page_id_) {
        next_new_page_id_ = first_allocatable_page_id;
    }
    return Status::OK();
}

StatusOr<std::span<std::byte, kPageSize>> InMemoryPageStore::Get(PageId page_id) {
    auto it = pages_.find(page_id);
    if (it == pages_.end()) {
        return Status::NotFound("page id not found");
    }
    return std::span<std::byte, kPageSize>(*it->second);
}

}  // namespace kds::storage
