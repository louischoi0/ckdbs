#include "kds/storage/device_page_store.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "kds/storage/free_map.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::storage {

DevicePageStore::DevicePageStore(PageDevice& device, PageId first_new_page_id,
                                 const Page& free_map, bool free_map_dirty) noexcept
    : device_(device),
      free_map_page_(free_map),
      free_map_dirty_(free_map_dirty),
      next_new_page_id_(first_new_page_id) {}

StatusOr<std::unique_ptr<DevicePageStore>> DevicePageStore::Open(PageDevice& device,
                                                                 PageId first_new_page_id) {
    Page free_map{};
    std::span<std::byte, kPageSize> view(free_map);

    // An all-zero page reads back as page_type kInvalid, which is what both
    // a sparse never-written page and a file abandoned before its first
    // flush look like - neither carries any allocation, so both are a fresh
    // database rather than an error.
    bool fresh = device.page_capacity() <= kFreeMapPageId;
    if (!fresh) {
        if (Status s = device.ReadPage(kFreeMapPageId, view); !s.ok()) return s;
        if (RawPageType(view) == static_cast<std::uint8_t>(PageType::kInvalid)) {
            fresh = true;
        } else if (Status s = ValidateFreeMapPage(view); !s.ok()) {
            return s;
        }
    }

    if (fresh) {
        if (Status s = device.EnsureCapacity(kFreeMapPageId + 1); !s.ok()) return s;
        FormatFreeMapPage(view);
        FreeMapAllocate(view, kFreeMapPageId);
    }

    return std::unique_ptr<DevicePageStore>(
        new DevicePageStore(device, first_new_page_id, free_map, fresh));
}

bool DevicePageStore::IsAllocated(PageId page_id) const noexcept {
    if (page_id >= kFreeMapBitsPerPage) return false;
    return FreeMapIsAllocated(free_map_bytes(), page_id);
}

std::uint32_t DevicePageStore::allocated_pages() const noexcept {
    return FreeMapCountAllocated(free_map_bytes());
}

Status DevicePageStore::EnsureAddressable(PageId page_id) {
    if (page_id < device_.page_capacity()) return Status::OK();
    return device_.EnsureCapacity(page_id + 1);
}

std::span<std::byte, kPageSize> DevicePageStore::InsertFrame(PageId page_id,
                                                             std::unique_ptr<Page> bytes) {
    std::span<std::byte, kPageSize> view(*bytes);
    frames_.insert_or_assign(page_id, Frame{std::move(bytes), /*dirty=*/true});
    return view;
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::ResidentBytes(PageId page_id) {
    if (auto it = frames_.find(page_id); it != frames_.end()) {
        it->second.dirty = true;
        return std::span<std::byte, kPageSize>(*it->second.bytes);
    }

    // The free map says this page exists; if the device cannot address it,
    // the two disagree and that is not a page to read.
    if (page_id >= device_.page_capacity()) {
        return Status::Corruption("DevicePageStore: page " + std::to_string(page_id) +
                                  " is allocated but beyond device capacity " +
                                  std::to_string(device_.page_capacity()));
    }

    auto bytes = std::make_unique<Page>();
    if (Status s = device_.ReadPage(page_id, std::span<std::byte, kPageSize>(*bytes)); !s.ok()) {
        return s;
    }

    // Verified on the miss path only, never on a hit (page.md section 10).
    // Every page this store writes was stamped in Flush(), so a mismatch
    // here is real damage, not an unstamped page.
    if (Status s = VerifyPageChecksum(std::span<const std::byte, kPageSize>(*bytes)); !s.ok()) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("pagestore", "corruption: page " + std::to_string(page_id) +
                                         " failed checksum verification on read: " + s.message());
        }
        return s;
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("pagestore", "read page=" + std::to_string(page_id) + " from device");
    }
    return InsertFrame(page_id, std::move(bytes));
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::CreateAt(PageId page_id) {
    if (page_id >= kFreeMapBitsPerPage) {
        return Status::OutOfRange("DevicePageStore: page id " + std::to_string(page_id) +
                                  " is beyond the single free-map page's coverage (" +
                                  std::to_string(kFreeMapBitsPerPage) + " ids)");
    }
    if (IsAllocated(page_id)) {
        return Status::AlreadyExists("page id already in use");
    }
    if (Status s = EnsureAddressable(page_id); !s.ok()) return s;

    FreeMapAllocate(free_map_bytes(), page_id);
    free_map_dirty_ = true;

    auto bytes = std::make_unique<Page>();
    bytes->fill(std::byte{0});
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("pagestore", "alloc page=" + std::to_string(page_id) + " (allocated=" +
                                     std::to_string(allocated_pages()) + ")");
    }
    return InsertFrame(page_id, std::move(bytes));
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> DevicePageStore::CreateNew() {
    auto found = FreeMapFindFirstFree(free_map_bytes(), next_new_page_id_);
    if (!found.has_value()) {
        return Status::OutOfSpace("DevicePageStore: no free page id at or above " +
                                  std::to_string(next_new_page_id_));
    }

    const PageId page_id = *found;
    auto created = CreateAt(page_id);
    if (!created.ok()) return created.status();

    next_new_page_id_ = page_id + 1;
    return std::make_pair(page_id, created.value());
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::Get(PageId page_id) {
    if (!IsAllocated(page_id)) {
        return Status::NotFound("page id not found");
    }
    return ResidentBytes(page_id);
}

Status DevicePageStore::Flush() {
    // Merging adjacent ids into one WritePageRun needs the frames to be
    // contiguous in memory - the preallocated slab of page.md section 9,
    // which arrives with the buffer pool, not here.
    std::vector<PageId> dirty;
    dirty.reserve(frames_.size());
    for (const auto& [page_id, frame] : frames_) {
        if (frame.dirty) dirty.push_back(page_id);
    }
    std::sort(dirty.begin(), dirty.end());

    for (const PageId page_id : dirty) {
        auto it = frames_.find(page_id);

        // The last thing that touches a page before it goes out (page.md
        // section 8): every other mutation has already happened.
        StampPageChecksum(std::span<std::byte, kPageSize>(*it->second.bytes));

        if (Status s = device_.WritePage(page_id, std::span<const std::byte, kPageSize>(
                                                      *it->second.bytes));
            !s.ok()) {
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore", "write failed for page " + std::to_string(page_id) +
                                             ": " + s.message());
            }
            return s;
        }
        it->second.dirty = false;
        if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("pagestore", "wrote page=" + std::to_string(page_id));
        }
    }

    if (free_map_dirty_) {
        StampPageChecksum(free_map_bytes());
        if (Status s = device_.WritePage(kFreeMapPageId, free_map_bytes()); !s.ok()) {
            // The map is what makes a page reachable after a restart, so
            // losing this write loses pages whose bytes did land.
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore", "free-map write failed: " + s.message());
            }
            return s;
        }
        free_map_dirty_ = false;
        if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("pagestore", "free map written, " + std::to_string(allocated_pages()) +
                                         " page(s) allocated");
        }
    }
    if (!dirty.empty() && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "flushed " + std::to_string(dirty.size()) + " dirty page(s)");
    }
    return Status::OK();
}

Status DevicePageStore::Sync() {
    if (Status s = Flush(); !s.ok()) return s;
    Status s = device_.Sync();
    if (log_ != nullptr) {
        if (!s.ok() && log_->enabled(LogLevel::kError)) {
            log_->Error("pagestore", "device sync failed: " + s.message());
        } else if (s.ok() && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("pagestore", "device synced, " + std::to_string(resident_pages()) +
                                         " page(s) resident");
        }
    }
    return s;
}

std::vector<PageId> DevicePageStore::DirtyPageIds() const {
    std::vector<PageId> dirty;
    dirty.reserve(frames_.size());
    for (const auto& [page_id, frame] : frames_) {
        if (frame.dirty) dirty.push_back(page_id);
    }
    std::sort(dirty.begin(), dirty.end());
    return dirty;
}

Status DevicePageStore::FlushPages(std::span<const PageId> page_ids) {
    std::vector<PageId> ordered(page_ids.begin(), page_ids.end());
    std::sort(ordered.begin(), ordered.end());

    bool wrote_any = false;
    for (const PageId page_id : ordered) {
        auto it = frames_.find(page_id);
        if (it == frames_.end() || !it->second.dirty) {
            continue;  // evicted, or already flushed by someone else
        }

        StampPageChecksum(std::span<std::byte, kPageSize>(*it->second.bytes));
        if (Status s = device_.WritePage(
                page_id, std::span<const std::byte, kPageSize>(*it->second.bytes));
            !s.ok()) {
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore", "checkpoint write failed for page " +
                                             std::to_string(page_id) + ": " + s.message());
            }
            return s;
        }
        it->second.dirty = false;
        wrote_any = true;
        if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("pagestore", "wrote page=" + std::to_string(page_id) + " (checkpoint)");
        }
    }

    // The free map goes out with them, and after them: a page is only
    // reachable once the map says its id is allocated, so publishing the
    // map first would let a crash expose a page whose bytes never landed.
    if (free_map_dirty_) {
        StampPageChecksum(free_map_bytes());
        if (Status s = device_.WritePage(kFreeMapPageId, free_map_bytes()); !s.ok()) {
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore", "checkpoint free-map write failed: " + s.message());
            }
            return s;
        }
        free_map_dirty_ = false;
        wrote_any = true;
    }

    if (!wrote_any) return Status::OK();  // nothing written, nothing to sync
    Status s = device_.Sync();
    if (log_ != nullptr) {
        if (!s.ok() && log_->enabled(LogLevel::kError)) {
            log_->Error("pagestore", "checkpoint sync failed: " + s.message());
        } else if (s.ok() && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("pagestore", "checkpoint wrote " + std::to_string(ordered.size()) +
                                         " named page(s) and synced");
        }
    }
    return s;
}

}  // namespace kds::storage
