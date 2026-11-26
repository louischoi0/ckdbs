#pragma once

// A page store that records which pages of one relation a statement pinned
// exclusive, and can fail one fetch. **A write has no `ANALYZE` of its
// leaves**, so what tells a write's descent or bounded walk from its walk of
// the whole relation is the hold: a write walk takes every leaf of the
// relation exclusive, a descent the leaf its key is in. Shared by the suites
// that pin which pages a write holds (`key_set_test.cpp`,
// `keyed_write_test.cpp`).

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <utility>

#include "kds/base/status.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::server {

class LeafWatchingStore final : public storage::PageStore {
public:
    explicit LeafWatchingStore(storage::InMemoryPageStore& inner) : inner_(inner) {}

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        if (Fails()) return Status::IoError("a fetch the test failed");
        return inner_.GetUnpinned(page_id);
    }
    StatusOr<std::span<std::byte, kPageSize>> GetForReadUnpinned(PageId page_id) override {
        if (Fails()) return Status::IoError("a fetch the test failed");
        return inner_.GetForReadUnpinned(page_id);
    }
    Status RaiseAllocationFloor(PageId first_allocatable_page_id) override {
        return inner_.RaiseAllocationFloor(first_allocatable_page_id);
    }
    void PinFrame(PageId page_id, storage::PinMode mode) noexcept override {
        if (watched_oid_ == 0 || mode != storage::PinMode::kExclusive) return;
        auto bytes = inner_.GetForReadUnpinned(page_id);
        if (bytes.ok() && storage::GetOwnerOid(bytes.value()) == watched_oid_) held_.insert(page_id);
    }

    // From now on, the pages `oid` owns that are pinned exclusive.
    void Watch(std::uint64_t oid) {
        watched_oid_ = oid;
        held_.clear();
    }
    std::size_t held() const noexcept { return held_.size(); }
    void FailNextFetch() noexcept { fail_next_ = true; }
    int failed() const noexcept { return failed_; }

private:
    bool Fails() noexcept {
        if (!fail_next_) return false;
        fail_next_ = false;
        ++failed_;
        return true;
    }

    storage::InMemoryPageStore& inner_;
    std::unordered_set<PageId> held_;
    std::uint64_t watched_oid_ = 0;
    bool fail_next_ = false;
    int failed_ = 0;
};

}  // namespace kds::server
