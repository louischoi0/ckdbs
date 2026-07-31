#include "kds/storage/device_page_store.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "kds/storage/free_map.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::storage {

DevicePageStore::DevicePageStore(PageDevice& device, PageId first_new_page_id,
                                 const Page& free_map, const Page& headerless_map,
                                 bool maps_dirty) noexcept
    : device_(device),
      free_map_page_(free_map),
      headerless_map_page_(headerless_map),
      maps_dirty_(maps_dirty),
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

    Page headerless_map{};
    std::span<std::byte, kPageSize> hview(headerless_map);
    if (fresh) {
        if (Status s = device.EnsureCapacity(kHeaderlessMapPageId + 1); !s.ok()) return s;
        FormatFreeMapPage(view);
        FreeMapAllocate(view, kFreeMapPageId);
        FreeMapAllocate(view, kHeaderlessMapPageId);
        FormatFreeMapPage(hview, PageType::kHeaderlessMap);
    } else {
        // A database written before the headerless map existed has nothing
        // at that id, which reads as kInvalid. Treated as "no headerless
        // pages" rather than an error: that is exactly true of such a
        // database, since it predates the only thing that creates them.
        if (device.page_capacity() > kHeaderlessMapPageId) {
            if (Status s = device.ReadPage(kHeaderlessMapPageId, hview); !s.ok()) return s;
        }
        if (RawPageType(hview) == static_cast<std::uint8_t>(PageType::kInvalid)) {
            if (Status s = device.EnsureCapacity(kHeaderlessMapPageId + 1); !s.ok()) return s;
            FormatFreeMapPage(hview, PageType::kHeaderlessMap);
            FreeMapAllocate(view, kHeaderlessMapPageId);
        } else if (Status s = ValidateFreeMapPage(hview, PageType::kHeaderlessMap); !s.ok()) {
            return s;
        }
    }

    return std::unique_ptr<DevicePageStore>(
        new DevicePageStore(device, first_new_page_id, free_map, headerless_map, fresh));
}

bool DevicePageStore::IsHeaderless(PageId page_id) const noexcept {
    return FreeMapIsAllocated(headerless_map_bytes(), page_id) && IsAllocated(page_id);
}

void DevicePageStore::StampIfHeadered(PageId page_id,
                                      std::span<std::byte, kPageSize> page) const {
    // The one place the decision is made. A write path that stamped
    // directly would have to remember to ask, and the failure mode of
    // forgetting is silent data corruption in a page nobody checksums.
    if (IsHeaderless(page_id)) return;
    StampPageChecksum(page);
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>>
DevicePageStore::CreateNewHeaderless() {
    auto created = CreateNew();
    if (!created.ok()) return created.status();

    // Marked after the allocation succeeds and before the caller writes a
    // byte, so no flush can ever see the page headered.
    FreeMapAllocate(headerless_map_bytes(), created.value().first);
    maps_dirty_ = true;
    if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
        log_->Trace("pagestore",
                    "alloc headerless page=" + std::to_string(created.value().first));
    }
    return created;
}

Status DevicePageStore::FlushMaps() {
    if (!maps_dirty_) return Status::OK();

    // The headerless map first, the free map second. Both orderings are
    // safe, but this one is safe for a reason worth writing down: the free
    // map is what makes a page id *exist*, so a crash between the two
    // leaves a headerless bit set for an id nothing allocated - harmless,
    // since IsHeaderless() also requires allocation. The reverse order
    // would publish an allocated Waystone page whose headerless bit had
    // not landed, and the next read of it would verify a checksum that was
    // never written and call the page corrupt.
    StampPageChecksum(headerless_map_bytes());
    if (Status s = device_.WritePage(kHeaderlessMapPageId, headerless_map_bytes()); !s.ok()) {
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("pagestore", "headerless-map write failed: " + s.message());
        }
        return s;
    }

    StampPageChecksum(free_map_bytes());
    if (Status s = device_.WritePage(kFreeMapPageId, free_map_bytes()); !s.ok()) {
        // The map is what makes a page reachable after a restart, so
        // losing this write loses pages whose bytes did land.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("pagestore", "free-map write failed: " + s.message());
        }
        return s;
    }

    maps_dirty_ = false;
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "maps written, " + std::to_string(allocated_pages()) +
                                     " page(s) allocated");
    }
    return Status::OK();
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
    // Every *headered* page this store writes was stamped in Flush(), so a
    // mismatch here is real damage. A headerless page carries no checksum
    // by construction and is skipped - which is why the headerless map has
    // to be durable: this is the moment an in-memory-only set would have
    // already been lost.
    if (!IsHeaderless(page_id)) {
        if (Status s = VerifyPageChecksum(std::span<const std::byte, kPageSize>(*bytes));
            !s.ok()) {
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore",
                            "corruption: page " + std::to_string(page_id) +
                                " failed checksum verification on read: " + s.message());
            }
            return s;
        }
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
    maps_dirty_ = true;

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

Status DevicePageStore::StampPageLsn(PageId page_id, std::uint64_t lsn) {
    if (lsn == wal::kNoLsn) {
        return Status::InvalidArgument(
            "DevicePageStore: page_lsn 0 means 'never logged' and cannot be stamped");
    }
    auto it = frames_.find(page_id);
    if (it == frames_.end()) {
        return Status::NotFound("DevicePageStore: page " + std::to_string(page_id) +
                                " is not resident, so its page_lsn cannot be stamped");
    }

    SetPageLsn(std::span<std::byte, kPageSize>(*it->second.bytes), lsn);
    it->second.dirty = true;
    // First record since the frame was last written back wins: recLSN is
    // the *oldest* LSN redo must replay to make the page whole, so a later
    // record must never overwrite it (wal.md section 11-1).
    if (it->second.rec_lsn == wal::kNoLsn) it->second.rec_lsn = lsn;
    return Status::OK();
}

Status DevicePageStore::AwaitWalGate(std::span<const PageId> page_ids) {
    if (wal_gate_ == nullptr) return Status::OK();

    // One EnsureDurable for the batch maximum, not one per page: the call
    // is a no-op once the watermark is past, so the highest page_lsn in
    // the batch subsumes every other.
    wal::Lsn highest = wal::kNoLsn;
    for (const PageId page_id : page_ids) {
        auto it = frames_.find(page_id);
        if (it == frames_.end() || !it->second.dirty) continue;
        // Skipped for the same reason the stamping loop in Flush() skips it
        // (StampIfHeadered): a headerless page has no page_lsn field, so the
        // bytes at that offset are entry data. Reading them yields a
        // meaningless watermark - a Waystone directory page reads as
        // 0xFFFF... - and EnsureDurable can only refuse it, which failed
        // every Flush() on any database with a dirty Waystone page and so
        // made SYNC and the checkpointer unable to persist anything at all.
        // Correct today because Waystone pages are unlogged (CLAUDE.md open
        // decision: persistence class), so the gate has nothing to wait for.
        // If they become logged, their LSN has to reach the gate out of
        // band rather than through a field the format does not have.
        if (IsHeaderless(page_id)) continue;
        const std::uint64_t page_lsn =
            GetPageLsn(std::span<const std::byte, kPageSize>(*it->second.bytes));
        if (page_lsn > highest) highest = page_lsn;
    }
    if (highest == wal::kNoLsn) return Status::OK();  // nothing logged in this batch

    if (Status s = wal_gate_->EnsureDurable(highest); !s.ok()) {
        // Refusing the flush is the whole point: writing the page anyway
        // would put data on disk ahead of the log that describes it.
        if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
            log_->Error("pagestore", "WAL gate refused a flush up to page_lsn " +
                                         std::to_string(highest) + ": " + s.message());
        }
        return s;
    }
    return Status::OK();
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

    if (Status s = AwaitWalGate(dirty); !s.ok()) return s;

    for (const PageId page_id : dirty) {
        auto it = frames_.find(page_id);

        // The last thing that touches a page before it goes out (page.md
        // section 8): every other mutation has already happened. Skipped
        // for a headerless page, which has no field to put it in.
        StampIfHeadered(page_id, std::span<std::byte, kPageSize>(*it->second.bytes));

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
        it->second.rec_lsn = wal::kNoLsn;  // clean: nothing to replay into it
        if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("pagestore", "wrote page=" + std::to_string(page_id));
        }
    }

    if (Status s = FlushMaps(); !s.ok()) return s;
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

std::vector<std::pair<PageId, wal::Lsn>> DevicePageStore::DirtyPagesWithRecLsn() const {
    std::vector<std::pair<PageId, wal::Lsn>> dirty;
    dirty.reserve(frames_.size());
    for (const auto& [page_id, frame] : frames_) {
        if (frame.dirty) dirty.emplace_back(page_id, frame.rec_lsn);
    }
    std::sort(dirty.begin(), dirty.end());
    return dirty;
}

Status DevicePageStore::FlushPages(std::span<const PageId> page_ids) {
    std::vector<PageId> ordered(page_ids.begin(), page_ids.end());
    std::sort(ordered.begin(), ordered.end());

    if (Status s = AwaitWalGate(ordered); !s.ok()) return s;

    bool wrote_any = false;
    for (const PageId page_id : ordered) {
        auto it = frames_.find(page_id);
        if (it == frames_.end() || !it->second.dirty) {
            continue;  // evicted, or already flushed by someone else
        }

        StampIfHeadered(page_id, std::span<std::byte, kPageSize>(*it->second.bytes));
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
        it->second.rec_lsn = wal::kNoLsn;
        wrote_any = true;
        if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("pagestore", "wrote page=" + std::to_string(page_id) + " (checkpoint)");
        }
    }

    // The maps go out with them, and after them: a page is only reachable
    // once the map says its id is allocated, so publishing the map first
    // would let a crash expose a page whose bytes never landed.
    if (maps_dirty_) {
        if (Status s = FlushMaps(); !s.ok()) return s;
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
