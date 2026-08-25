#include "kds/storage/extent_lease.hpp"

#include <string>

#include "kds/storage/device_page_store.hpp"

namespace kds::storage {

ExtentAllocator::ExtentAllocator(DevicePageStore& store, PageId first_new_page_id) noexcept
    : store_(&store), free_map_(store.free_map_bytes()), next_(first_new_page_id) {}

std::span<std::byte, kPageSize> ExtentAllocator::MapBytes() noexcept {
    // The store's accessor marks the map dirty on every take, which is the
    // whole reason it is taken per call and not once (the header).
    return store_ != nullptr ? store_->free_map_bytes() : free_map_;
}

Status ExtentAllocator::Persist() {
    return store_ != nullptr ? store_->PersistMaps() : Status::OK();
}

StatusOr<Extent> ExtentAllocator::Reserve(std::uint32_t count) {
    if (count == 0) {
        return Status::InvalidArgument("extent lease: a reservation of 0 pages is not a lease");
    }

    // Find the first run of `count` consecutive free ids at or above the
    // hint. The scan restarts from the id after a failed run rather than
    // from the next candidate, because a run that failed at position k means
    // k is allocated - so every start between the run's beginning and k is
    // equally doomed and re-testing them is wasted work.
    const std::span<std::byte, kPageSize> map = MapBytes();
    PageId candidate = next_;
    while (true) {
        auto found = FreeMapFindFirstFree(map, candidate);
        if (!found.has_value()) {
            return Status::OutOfSpace("extent lease: no free page id at or above " +
                                      std::to_string(candidate));
        }
        const PageId start = *found;
        if (start + count > kFreeMapBitsPerPage) {
            return Status::OutOfSpace(
                "extent lease: no run of " + std::to_string(count) +
                " contiguous free pages remains below the free map's coverage (" +
                std::to_string(kFreeMapBitsPerPage) + " ids)");
        }

        std::uint32_t run = 0;
        while (run < count && !FreeMapIsAllocated(map, start + run)) ++run;

        if (run == count) {
            // Marked here, not at first use: an id promised to one core must
            // never be found free by another, and the map is the only place
            // that fact can live.
            for (std::uint32_t i = 0; i < count; ++i) {
                FreeMapAllocate(map, start + i);
            }
            next_ = start + count;
            ++reservations_;
            return Extent{start, count};
        }

        // start + run is allocated, so the next possible start is past it.
        candidate = start + run + 1;
    }
}

StatusOr<PageId> LeasedIdSource::Next() {
    if (spent()) {
        // Retryable, and deliberately not OutOfSpace: the device may have
        // plenty of room and this core simply has no ids in hand. Conflating
        // the two would turn "ask core 0 for more" into "the database is
        // full". And TxnConflict rather than ResourceExhausted: status.hpp's
        // IsRetryable says why - the wire's bit follows one code.
        return Status::TxnConflict(
            "extent lease: this core's lease of " + std::to_string(current_.count) +
            " pages is spent; a refill must be granted before it can allocate again");
    }
    const PageId id = current_.first + issued_;
    ++issued_;
    ++issued_total_;
    return id;
}

void LeasedIdSource::Grant(Extent extent) {
    if (extent.empty()) return;

    current_ = extent;
    issued_ = 0;

    // Merge onto the tail when contiguous, which is the ordinary case - the
    // allocator carves sequentially, so a core allocating alone accumulates
    // one range rather than one entry per refill.
    if (!granted_.empty() && granted_.back().end() == extent.first) {
        granted_.back().count += extent.count;
        return;
    }
    granted_.push_back(extent);
}

bool LeasedIdSource::Owns(PageId page_id) const noexcept {
    for (const Extent& e : granted_) {
        if (e.Contains(page_id)) return true;
    }
    return false;
}

}  // namespace kds::storage
