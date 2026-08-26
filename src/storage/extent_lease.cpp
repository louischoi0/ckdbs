#include "kds/storage/extent_lease.hpp"

#include <string>

#include "kds/storage/device_page_store.hpp"

namespace kds::storage {

ExtentAllocator::ExtentAllocator(DevicePageStore& store, PageId first_new_page_id) noexcept
    : store_(&store), next_(first_new_page_id) {}

StatusOr<std::span<std::byte, kPageSize>> ExtentAllocator::MapBytes(std::uint32_t region) {
    // The store's accessor marks the region dirty on every take, which is
    // the whole reason it is taken per call and not held (the header).
    if (store_ != nullptr) return store_->FreeMapBytesForRegion(region);
    // The bare-bytes form is one page and therefore one region, and it is
    // region 0 by construction. Anything else has no bytes to answer from,
    // which is the same OutOfSpace the single-page allocator reported when
    // it ran off the end of its map.
    if (region != 0) {
        return Status::OutOfSpace(
            "extent lease: this allocator holds one free-map page, covering " +
            std::to_string(kFreeMapBitsPerPage) + " ids (0.." +
            std::to_string(kFreeMapBitsPerPage - 1) + ")");
    }
    return std::span<std::byte, kPageSize>(bare_map_, kPageSize);
}

Status ExtentAllocator::Persist() {
    return store_ != nullptr ? store_->PersistMaps() : Status::OK();
}

StatusOr<Extent> ExtentAllocator::Reserve(std::uint32_t count) {
    if (count == 0) {
        return Status::InvalidArgument("extent lease: a reservation of 0 pages is not a lease");
    }
    // A run longer than a region can never be placed, whatever the hint:
    // D3 refuses to straddle, so the region *is* the largest reservation.
    // Reported before the search rather than after it walks the whole id
    // space finding nothing.
    if (count > kFreeMapBitsPerPage) {
        return Status::OutOfSpace(
            "extent lease: a run of " + std::to_string(count) +
            " pages cannot be placed; a reservation may not cross a free-map region, so " +
            std::to_string(kFreeMapBitsPerPage) + " ids is the longest possible");
    }

    // Find the first run of `count` consecutive free ids at or above the
    // hint. The scan restarts from the id after a failed run rather than
    // from the next candidate, because a run that failed at position k means
    // k is allocated - so every start between the run's beginning and k is
    // equally doomed and re-testing them is wasted work.
    //
    // FM4: the walk is per region now. A region is asked for only when the
    // search enters it, which is also what creates it (FM5) - so the
    // allocator grows the map by walking off the end of one, with no
    // separate growth call and nothing to keep in step.
    PageId candidate = next_;
    while (candidate < kMaxPageCount) {
        const std::uint32_t region = FreeMapRegionOf(candidate);
        const PageId base = FreeMapRegionBase(region);
        auto map = MapBytes(region);
        if (!map.ok()) return map.status();

        auto found = FreeMapFindFirstFree(map.value(), FreeMapBitIndexOf(candidate));
        if (!found.has_value()) {
            candidate = FreeMapRegionBase(region + 1);  // this region is full
            continue;
        }
        const std::uint32_t start = *found;
        // A bitmap id is occupied whatever its bit says: under FM6 a
        // region's headerless map carries no bit until the page is placed,
        // and handing that id out would put a data page where the bitmap
        // must go. Arithmetic, for the reason CreateNew gives.
        if (IsMapPageId(base + start)) {
            candidate = base + start + 1;
            continue;
        }

        // **D3(a): a reservation never straddles a region.** A run that
        // would cross abandons the tail of this region and restarts in the
        // next, wasting at most count-1 ids per boundary - under 0.1% at
        // the default extent size. What it buys is that a reservation
        // dirties one map page, lands in one crash window, and needs one
        // region resident. Those ids are never reclaimed (nothing frees),
        // so the waste is permanent and small rather than temporary.
        if (start + count > kFreeMapBitsPerPage) {
            candidate = FreeMapRegionBase(region + 1);
            continue;
        }
        // The design ceiling bounds the last region, which is partial:
        // Extent::end() is PageId arithmetic and must not reach past the
        // id space (docs/inflight/in-progress/workplan-multi-free-map.md §4, finding 1).
        if (static_cast<std::uint64_t>(base) + start + count > kMaxPageCount) {
            return Status::OutOfSpace(
                "extent lease: no run of " + std::to_string(count) +
                " contiguous free pages remains below the " + std::to_string(kMaxPageCount) +
                "-page design ceiling");
        }

        std::uint32_t run = 0;
        while (run < count && !FreeMapIsAllocated(map.value(), start + run) &&
               !IsMapPageId(base + start + run)) {
            ++run;
        }

        if (run == count) {
            // Marked here, not at first use: an id promised to one core must
            // never be found free by another, and the map is the only place
            // that fact can live.
            for (std::uint32_t i = 0; i < count; ++i) {
                FreeMapAllocate(map.value(), start + i);
            }
            // Every one of those bits was proved clear by the probe above,
            // so the store's maintained count moves by exactly `count`
            // (D8(a); DevicePageStore::NoteAllocated says why it may add
            // rather than re-scan).
            if (store_ != nullptr) store_->NoteAllocated(count);
            next_ = base + start + count;
            ++reservations_;
            return Extent{base + start, count};
        }

        // start + run is allocated, so the next possible start is past it.
        candidate = base + start + run + 1;
    }

    return Status::OutOfSpace("extent lease: no free page id at or above " +
                              std::to_string(next_));
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
