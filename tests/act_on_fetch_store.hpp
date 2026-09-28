#pragma once

// A store that runs one action on the `nth` fetch of a chosen page, in
// either mode, and forwards everything else. It is the barrier a
// cross-core window needs in a single-threaded cell: the action runs after
// the fetch before it and ahead of the one it is attached to, which is
// exactly where a second core's divide lands. `index_tree_test.cpp` (AT-S15)
// and `btree_test.cpp` (AT-S16) both drive their windows through it.

#include <functional>
#include <span>
#include <utility>

#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::testing_race {

class ActOnFetchStore final : public storage::PageStore {
public:
    explicit ActOnFetchStore(storage::InMemoryPageStore& inner) : inner_(inner) {}

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        Tick(page_id);
        return inner_.GetUnpinned(page_id);
    }
    StatusOr<std::span<std::byte, kPageSize>> GetForReadUnpinned(PageId page_id) override {
        Tick(page_id);
        return inner_.GetForReadUnpinned(page_id);
    }

    void OnFetch(PageId page_id, int nth, std::function<void()> action) {
        watched_ = page_id;
        nth_ = nth;
        fetches_ = 0;
        action_ = std::move(action);
    }
    bool fired() const noexcept { return fired_; }

private:
    void Tick(PageId page_id) {
        if (action_ && page_id == watched_ && ++fetches_ == nth_) {
            auto action = std::move(action_);  // disarmed first: the action fetches too
            action_ = nullptr;
            fired_ = true;
            action();
        }
    }

    storage::InMemoryPageStore& inner_;
    PageId watched_ = kInvalidPageId;
    int nth_ = 0;
    int fetches_ = 0;
    bool fired_ = false;
    std::function<void()> action_;
};

}  // namespace kds::testing_race
