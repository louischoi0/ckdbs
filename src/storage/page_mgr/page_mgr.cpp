#include "kds/storage/page_mgr/page_mgr.hpp"

namespace kds::storage {

BufferPool::BufferPool(PageStore& backing, std::uint32_t nr_frames) : backing_(backing) {
    frames_.resize(nr_frames);
    free_slots_.reserve(nr_frames);
    for (std::uint32_t i = 0; i < nr_frames; ++i) {
        free_slots_.push_back(i);
    }
}

StatusOr<std::uint32_t> BufferPool::TakeFreeFrameSlot() noexcept {
    if (free_slots_.empty()) {
        return Status::OutOfSpace("buffer pool has no free frames (no eviction implemented)");
    }
    std::uint32_t slot = free_slots_.back();
    free_slots_.pop_back();
    return slot;
}

Frame& BufferPool::RegisterFrame(std::uint32_t slot, PageId page_id,
                                  std::span<std::byte, kPageSize> bytes, bool initial_dirty) {
    Frame& f = frames_[slot];
    f.page_id_ = page_id;
    f.bytes_ = bytes;
    f.dirty_ = initial_dirty;
    f.pin_count_ = 0;
    page_to_slot_[page_id] = slot;
    return f;
}

StatusOr<Frame*> BufferPool::Lookup(PageId page_id) noexcept {
    auto it = page_to_slot_.find(page_id);
    if (it == page_to_slot_.end()) {
        return Status::NotFound("page not resident in buffer pool");
    }
    Frame& f = frames_[it->second];
    Pin(f);
    return &f;
}

StatusOr<Frame*> BufferPool::LookupOrLoad(PageId page_id) {
    auto hit = Lookup(page_id);
    if (hit.ok()) return hit;

    auto slot = TakeFreeFrameSlot();
    if (!slot.ok()) return slot.status();

    auto bytes = backing_.Get(page_id);
    if (!bytes.ok()) {
        free_slots_.push_back(slot.value());
        return bytes.status();
    }

    Frame& f = RegisterFrame(slot.value(), page_id, bytes.value(), /*initial_dirty=*/false);
    Pin(f);
    return &f;
}

StatusOr<Frame*> BufferPool::AllocNew(PageId page_id) {
    // Up-front check: reject if already resident (caller bug), mirroring
    // the legacy engine's kds_buf_alloc_new().
    auto existing = Lookup(page_id);
    if (existing.ok()) {
        Unpin(*existing.value());
        return Status::AlreadyExists("page id already resident in buffer pool");
    }

    auto slot = TakeFreeFrameSlot();
    if (!slot.ok()) return slot.status();

    auto created = backing_.CreateAt(page_id);
    if (!created.ok()) {
        free_slots_.push_back(slot.value());
        return created.status();
    }

    // Marked dirty up front: a caller-visible new page must be guaranteed
    // to reach persistence even if the caller never touches it again
    // before eviction - same rationale as the legacy engine's DIRTY flag
    // on kds_buf_alloc_new(), even though there is no real disk to flush
    // to yet (see file-level comment).
    Frame& f = RegisterFrame(slot.value(), page_id, created.value(), /*initial_dirty=*/true);
    Pin(f);
    return &f;
}

BufferPool::Stats BufferPool::stats() const noexcept {
    Stats s{};
    s.total = static_cast<std::uint32_t>(frames_.size());
    s.free = static_cast<std::uint32_t>(free_slots_.size());
    s.valid = s.total - s.free;
    return s;
}

StatusOr<std::span<std::byte, kPageSize>> BufferPool::CreateAt(PageId page_id) {
    auto frame = AllocNew(page_id);
    if (!frame.ok()) return frame.status();
    auto bytes = frame.value()->bytes();
    Unpin(*frame.value());
    return bytes;
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> BufferPool::CreateNew() {
    auto slot = TakeFreeFrameSlot();
    if (!slot.ok()) return slot.status();

    auto created = backing_.CreateNew();
    if (!created.ok()) {
        free_slots_.push_back(slot.value());
        return created.status();
    }
    auto [new_id, bytes] = created.value();

    RegisterFrame(slot.value(), new_id, bytes, /*initial_dirty=*/true);
    return std::make_pair(new_id, bytes);
}

StatusOr<std::span<std::byte, kPageSize>> BufferPool::Get(PageId page_id) {
    auto frame = LookupOrLoad(page_id);
    if (!frame.ok()) return frame.status();
    auto bytes = frame.value()->bytes();
    Unpin(*frame.value());
    return bytes;
}

}  // namespace kds::storage
