#include "kds/storage/device_page_store.hpp"

#include <algorithm>
#include <cstring>
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
    // would publish an allocated headerless page whose headerless bit had
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
    // A leased core's copy of the free map is the one it read at Open(),
    // and core 0 sets the bits for a lease when it *reserves* it - which
    // happens later, in core 0's copy. So this store's map cannot be asked
    // about this store's own ids, and the lease is the authority for them.
    //
    // Only an addition, never a subtraction: a bit the map does have still
    // counts. The two can only disagree in the direction of the map being
    // behind, because nothing ever frees.
    if (lease_ != nullptr && lease_->Owns(page_id)) return true;
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
                                                             std::unique_ptr<Page> bytes,
                                                             bool dirty) {
    std::span<std::byte, kPageSize> view(*bytes);
    frames_.insert_or_assign(page_id, Frame{std::move(bytes), dirty});
    return view;
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::ResidentBytes(PageId page_id,
                                                                         bool mark_dirty,
                                                                         bool bump_usage) {
#ifndef NDEBUG
    // The shared-nothing check (workplan-crosscore.md P2, guideline 1),
    // debug builds only. It sits here rather than in Get()/GetForRead()
    // because *faulting* is the act that makes a page this core's business;
    // a frame already resident was faulted through this same test.
    //
    // A hard failure rather than an assert: the caller has a Status channel,
    // and a test can assert on the code where it could not on a SIGABRT.
    if (!MayFault(page_id)) {
        return Status::InvalidArgument(
            "DevicePageStore: core " + std::to_string(core_id_) + " may not fault page " +
            std::to_string(page_id) + "; it belongs to another core");
    }
    // Reading a system page is how a peer reaches the catalog; *dirtying*
    // one would make it a second writer of a page with exactly one owner.
    if (mark_dirty && !MayWrite(page_id)) {
        return Status::InvalidArgument(
            "DevicePageStore: core " + std::to_string(core_id_) + " may not write page " +
            std::to_string(page_id) + "; the system range has one writer, the system core");
    }
#endif

    if (auto it = frames_.find(page_id); it != frames_.end()) {
        // Never clears the flag: a frame already dirty from an earlier
        // mutation stays dirty however many readers touch it afterwards.
        if (mark_dirty) it->second.dirty = true;
        // §3.1-2: a saturating bump on every hit, including a read -
        // "recently used" is about access, not about mutation. A *ring*
        // fetch is the one exception (§5): a scan's touch is not heat.
        if (bump_usage && it->second.usage < kClockUsageCap) ++it->second.usage;
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
    return InsertFrame(page_id, std::move(bytes), mark_dirty);
}

void DevicePageStore::ReleaseScanSlot(PageId page_id) noexcept {
    if (page_id == kInvalidPageId) return;
    auto it = frames_.find(page_id);
    if (it == frames_.end()) return;  // reclaimed by a sweep meanwhile: fine
    const Frame& frame = it->second;
    // The foreground got there: a dirty write must reach the device, a pin
    // is absolute, a usage bump means a foreground accessor touched it
    // (ring fetches never bump), and a pinned-class page is never dropped
    // by anyone. Each abandons the frame to ordinary pool life.
    if (frame.dirty || frame.pins > 0 || frame.usage > 0 || IsPinnedClass(page_id)) return;
    frames_.erase(it);
}

// The real ring (§5): fixed slots, cyclic reuse, drop-on-rotation unless
// the foreground claimed the frame. Nested so it can reach the frame
// table; handed out as the base-class ScanFetcher so callers stay
// concrete-store-blind.
class DevicePageStore::ScanRing final : public ScanFetcher {
public:
    ScanRing(DevicePageStore& store, std::size_t frames)
        : store_(store), slots_(frames == 0 ? 1 : frames, kInvalidPageId) {}

    ~ScanRing() override {
        // The scan is over: every slot the foreground did not claim goes
        // back to the device's keeping.
        for (const PageId id : slots_) store_.ReleaseScanSlot(id);
    }

    StatusOr<std::span<std::byte, kPageSize>> Fetch(PageId page_id) override {
        // In place when resident - the foreground's frame or one of this
        // ring's own slots - never bumping usage: §5's interaction rule in
        // one direction, and "a scan is not heat" in the other.
        if (auto it = store_.frames_.find(page_id); it != store_.frames_.end()) {
            return std::span<std::byte, kPageSize>(*it->second.bytes);
        }

        // Rotate: the slot's previous occupant is dropped unless the
        // foreground claimed it, then the new page faults in clean with
        // its usage untouched.
        store_.ReleaseScanSlot(slots_[hand_]);
        auto bytes = store_.ResidentBytes(page_id, /*mark_dirty=*/false, /*bump_usage=*/false);
        if (!bytes.ok()) return bytes.status();
        slots_[hand_] = page_id;
        hand_ = (hand_ + 1) % slots_.size();
        return bytes;
    }

private:
    DevicePageStore& store_;
    std::vector<PageId> slots_;
    std::size_t hand_ = 0;
};

std::unique_ptr<ScanFetcher> DevicePageStore::OpenScanRing(std::size_t frames) {
    return std::make_unique<ScanRing>(*this, frames);
}

bool DevicePageStore::MayFault(PageId page_id) const noexcept {
    // The system core owns every fixed structure, so it may reach anything.
    if (lease_ == nullptr) return true;
    // The fixed system range is readable by every core: the catalog lives
    // there, and a core that cannot read it cannot serve a statement (P6).
    if (page_id < system_page_limit_) return true;
    if (lease_->Owns(page_id)) return true;
    // CC7: pages of a relation the catalog assigns to this core, granted at
    // DDL publish. Read rights only - MayWrite never consults this list.
    for (const Extent& granted : fault_granted_) {
        if (granted.Contains(page_id)) return true;
    }
    return false;
}

void DevicePageStore::GrantFaultPages(Extent extent) {
    if (extent.empty()) return;
    for (Extent& granted : fault_granted_) {
        if (granted.end() == extent.first) {
            granted.count += extent.count;
            return;
        }
    }
    fault_granted_.push_back(extent);
}

bool DevicePageStore::MayWrite(PageId page_id) const noexcept {
    if (lease_ == nullptr) return true;
    // Read-only for a peer, deliberately: one writer per catalog page is
    // what makes a peer's stale view a retryable "not found" rather than a
    // torn read.
    if (page_id < system_page_limit_) return false;
    return lease_->Owns(page_id);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::CreateAt(PageId page_id) {
    if (lease_ != nullptr) {
        // Placing a page at a *chosen* id is a claim on the free map, and
        // this store does not own it (see SetCoreOwnership). Every caller of
        // CreateAt is bootstrap or a fixed system page, all of which are
        // core 0's by M5 - so this is unreachable rather than restrictive,
        // and it is here so that it stays that way.
        return Status::InvalidArgument(
            "DevicePageStore: core " + std::to_string(core_id_) +
            " may not place a page at a chosen id; the free map belongs to the system core");
    }
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
    // A brand-new page exists only in this frame until it is written back,
    // so it is dirty by definition.
    return InsertFrame(page_id, std::move(bytes), /*dirty=*/true);
}

StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> DevicePageStore::CreateNew() {
    if (lease_ != nullptr) {
        // A leased core takes its id from the run core 0 already reserved
        // for it, and touches no shared state to do it. The free-map bits
        // were set at reservation, so there is nothing to mark here - which
        // is exactly why this path needs no message and no suspension
        // (extent_lease.hpp).
        auto id = lease_->Next();
        if (!id.ok()) return id.status();
        if (Status s = EnsureAddressable(id.value()); !s.ok()) return s;

        auto bytes = std::make_unique<Page>();
        bytes->fill(std::byte{0});
        if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("pagestore", "alloc page=" + std::to_string(id.value()) + " from core " +
                                         std::to_string(core_id_) + "'s lease (" +
                                         std::to_string(lease_->remaining()) + " left)");
        }
        return std::make_pair(id.value(),
                              InsertFrame(id.value(), std::move(bytes), /*dirty=*/true));
    }

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
    return ResidentBytes(page_id, /*mark_dirty=*/true);
}

StatusOr<std::span<std::byte, kPageSize>> DevicePageStore::GetForRead(PageId page_id) {
    if (!IsAllocated(page_id)) {
        return Status::NotFound("page id not found");
    }
    return ResidentBytes(page_id, /*mark_dirty=*/false);
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
        // meaningless watermark - a page of 0xFF entry bytes reads as
        // 0xFFFF... - and EnsureDurable can only refuse it, which failed
        // every Flush() on any database with a dirty headerless page and so
        // made SYNC and the checkpointer unable to persist anything at all.
        // Sound only while headerless pages are unlogged. If a headerless
        // class becomes logged, its LSN has to reach the gate out of band
        // rather than through a field the format does not have.
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

StatusOr<std::size_t> DevicePageStore::WriteBack(std::span<const PageId> page_ids) {
    // Ascending and unique: id order is file order (page.md section 13),
    // and the queue this drains may name a page twice across sweeps.
    std::vector<PageId> ordered(page_ids.begin(), page_ids.end());
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

    // (1) durable: one gate call for the batch maximum, before any byte
    // moves - the whole of flush-before-evict.
    if (Status s = AwaitWalGate(ordered); !s.ok()) return s;

    std::size_t written = 0;
    std::vector<std::byte> scratch;
    for (std::size_t i = 0; i < ordered.size();) {
        auto it = frames_.find(ordered[i]);
        if (it == frames_.end() || !it->second.dirty) {
            ++i;  // evicted, or already written by someone else: not ours
            continue;
        }

        // Extend the run while the next ids are consecutive, resident and
        // dirty - the shape one WritePageRun can take.
        std::size_t run = 1;
        while (run < kWritebackRunPages && i + run < ordered.size() &&
               ordered[i + run] == ordered[i] + run) {
            auto next = frames_.find(ordered[i + run]);
            if (next == frames_.end() || !next->second.dirty) break;
            ++run;
        }

        // (2) checksum, the last thing that touches a page before it goes
        // out (page.md section 8) - skipped for a headerless page, which
        // has no field to put one in.
        for (std::size_t k = 0; k < run; ++k) {
            auto& frame = frames_.find(ordered[i + k])->second;
            StampIfHeadered(ordered[i + k], std::span<std::byte, kPageSize>(*frame.bytes));
        }

        // (3) write: one device call for a run, per page otherwise. The
        // run copies into scratch because frames are separate heap
        // allocations - bounded by kWritebackRunPages, and best-effort by
        // spec §4: a device without a real scatter write still sees the
        // pages land in file order.
        Status wrote = Status::OK();
        if (run > 1) {
            scratch.resize(run * kPageSize);
            for (std::size_t k = 0; k < run; ++k) {
                const auto& frame = frames_.find(ordered[i + k])->second;
                std::memcpy(scratch.data() + k * kPageSize, frame.bytes->data(), kPageSize);
            }
            wrote = device_.WritePageRun(ordered[i], static_cast<std::uint32_t>(run),
                                         std::span<const std::byte>(scratch));
        } else {
            const auto& frame = frames_.find(ordered[i])->second;
            wrote = device_.WritePage(ordered[i],
                                      std::span<const std::byte, kPageSize>(*frame.bytes));
        }
        if (!wrote.ok()) {
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("pagestore", "write failed for page " +
                                             std::to_string(ordered[i]) + " (run of " +
                                             std::to_string(run) + "): " + wrote.message());
            }
            return wrote;
        }

        // (4) clean, only now: a failure above leaves the frame dirty and
        // its recLSN intact, so the next writeback retries it.
        for (std::size_t k = 0; k < run; ++k) {
            auto& frame = frames_.find(ordered[i + k])->second;
            frame.dirty = false;
            frame.rec_lsn = wal::kNoLsn;  // clean: nothing to replay into it
            if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
                log_->Trace("pagestore", "wrote page=" + std::to_string(ordered[i + k]));
            }
        }
        written += run;
        i += run;
    }
    return written;
}

StatusOr<std::size_t> DevicePageStore::DrainDirtyEvictionQueue() {
    const std::vector<PageId> queued = TakeDirtyEvictionQueue();
    if (queued.empty()) return std::size_t{0};
    auto written = WriteBack(queued);
    if (written.ok() && written.value() > 0 && log_ != nullptr &&
        log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "writeback drained " + std::to_string(written.value()) +
                                     " queued dirty page(s)");
    }
    return written;
}

std::size_t DevicePageStore::MaintainFreeReserve(std::size_t pool_frames,
                                                 std::size_t watermark) {
    std::size_t reclaimed_total = 0;
    for (;;) {
        const std::size_t resident = frames_.size();
        const std::size_t free_frames = pool_frames > resident ? pool_frames - resident : 0;
        if (free_frames >= watermark) break;

        const std::size_t reclaimed = EvictColdFrames(watermark - free_frames);
        reclaimed_total += reclaimed;

        // Queued dirt is written clean here so the *next* rotation can
        // reclaim it - §4's "reclaim happens on the sweep's next visit".
        // A drain failure ends the loop rather than the world: the pages
        // stay dirty and queued facts are re-derived by the next sweep.
        auto drained = DrainDirtyEvictionQueue();
        const std::size_t cleaned = drained.ok() ? drained.value() : 0;

        if (reclaimed == 0 && cleaned == 0) break;  // a full rotation yielded nothing
    }
    return reclaimed_total;
}

Status DevicePageStore::Flush() {
    std::vector<PageId> dirty;
    dirty.reserve(frames_.size());
    for (const auto& [page_id, frame] : frames_) {
        if (frame.dirty) dirty.push_back(page_id);
    }

    auto written = WriteBack(dirty);
    if (!written.ok()) return written.status();

    if (Status s = FlushMaps(); !s.ok()) return s;
    if (written.value() > 0 && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore",
                    "flushed " + std::to_string(written.value()) + " dirty page(s)");
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

Status DevicePageStore::EvictClean(std::span<const PageId> page_ids) {
    // Checked before anything is dropped, so a bad call leaves the store
    // exactly as it was rather than half-evicted.
    for (const PageId id : page_ids) {
        auto it = frames_.find(id);
        if (it == frames_.end()) continue;
        if (it->second.dirty) {
            return Status::InvalidArgument(
                "DevicePageStore: page " + std::to_string(id) +
                " is dirty; evicting it would discard a write");
        }
        // A pinned frame is one somebody holds a live `PageRef` into, so
        // dropping it here is the use-after-free the handle exists to
        // prevent (docs/workplan-eviction.md EV01). This path predates pins
        // and its callers - a peer dropping stale catalog pages - never hold
        // one, so the check guards against a future caller rather than
        // against normal operation, exactly as the dirty check above does.
        if (it->second.pins != 0) {
            return Status::InvalidArgument(
                "DevicePageStore: page " + std::to_string(id) + " is pinned by " +
                std::to_string(it->second.pins) +
                " reference(s); evicting it would dangle them");
        }
    }
    for (const PageId id : page_ids) {
        frames_.erase(id);
    }
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "evicted " + std::to_string(page_ids.size()) +
                                     " page(s) for re-read on core " + std::to_string(core_id_));
    }
    return Status::OK();
}

Status DevicePageStore::FlushPages(std::span<const PageId> page_ids) {
    // The checkpointer's route through the one writeback primitive - §4's
    // "consumer of the machinery, not a parallel implementation".
    auto written = WriteBack(page_ids);
    if (!written.ok()) return written.status();
    bool wrote_any = written.value() > 0;

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
            log_->Debug("pagestore", "checkpoint wrote " + std::to_string(written.value()) +
                                         " named page(s) and synced");
        }
    }
    return s;
}



// ---- Frame reclamation (docs/workplan-eviction.md EV01-EV02) -------------

DevicePageStore::PageRef& DevicePageStore::PageRef::operator=(PageRef&& other) noexcept {
    if (this != &other) {
        Release();
        store_ = other.store_;
        page_id_ = other.page_id_;
        data_ = other.data_;
        // Emptied, not just copied from: the pin transfers, so the source
        // must not drop it on destruction.
        other.store_ = nullptr;
        other.page_id_ = kInvalidPageId;
        other.data_ = nullptr;
    }
    return *this;
}

DevicePageStore::PageRef::~PageRef() { Release(); }

void DevicePageStore::PageRef::Release() noexcept {
    if (store_ == nullptr) return;
    store_->UnpinFrame(page_id_);
    store_ = nullptr;
    page_id_ = kInvalidPageId;
    data_ = nullptr;
}

void DevicePageStore::PageRef::MarkDirty() noexcept {
    if (store_ != nullptr) store_->MarkFrameDirty(page_id_);
}

void DevicePageStore::UnpinFrame(PageId page_id) noexcept {
    auto it = frames_.find(page_id);
    if (it == frames_.end()) return;
    // Saturating rather than wrapping. An unpin with no pin is a defect in
    // the handle, not in the caller, and the two failure modes are not
    // symmetric: a floor leaves a frame resident forever (a leak, visible in
    // pinned_frames()), where an underflow makes it evictable while somebody
    // still holds it.
    if (it->second.pins != 0) --it->second.pins;
}

void DevicePageStore::MarkFrameDirty(PageId page_id) noexcept {
    auto it = frames_.find(page_id);
    if (it != frames_.end()) it->second.dirty = true;
}

StatusOr<DevicePageStore::PageRef> DevicePageStore::PinnedGet(PageId page_id) {
    auto bytes = ResidentBytes(page_id, /*mark_dirty=*/true);
    if (!bytes.ok()) return bytes.status();
    // The frame is resident because ResidentBytes just made it so, and the
    // pin is taken before anything can run between the two.
    ++frames_.find(page_id)->second.pins;
    return PageRef(this, page_id, bytes.value());
}

StatusOr<DevicePageStore::PageRef> DevicePageStore::PinnedGetForRead(PageId page_id) {
    auto bytes = ResidentBytes(page_id, /*mark_dirty=*/false);
    if (!bytes.ok()) return bytes.status();
    ++frames_.find(page_id)->second.pins;
    return PageRef(this, page_id, bytes.value());
}

bool DevicePageStore::IsPinnedClass(PageId page_id) const noexcept {
    // Half one: the reserved low ids. Needed because the fixed catalog pages
    // are formatted kHeap like any user relation, so the kind cannot tell
    // them apart - the finding recorded at the declaration and in
    // docs/workplan-eviction.md EVT01.
    if (page_id < first_evictable_page_id_) return true;

    // Half two: the page kind, which is what EV3 actually specifies and what
    // works for a class that has one of its own. A Bound Cabin's pages carry
    // the aggregate an admission check reads, so reclaiming one would take a
    // *constraint* out of memory - the definition of a class that is never a
    // candidate.
    //
    // Only asked of a resident frame: a page that is not in the pool cannot
    // be a sweep candidate anyway, and reading a header off the device to
    // answer would turn a skip test into an I/O.
    auto it = frames_.find(page_id);
    if (it == frames_.end()) return false;
    const PageHeaderFields header =
        ReadPageHeader(std::span<const std::byte, kPageSize>(*it->second.bytes));
    return header.page_type == static_cast<std::uint8_t>(PageType::kCabinBound);
}

std::vector<PageId> DevicePageStore::TakeDirtyEvictionQueue() {
    std::vector<PageId> out;
    out.swap(dirty_eviction_queue_);
    return out;
}

void DevicePageStore::SetResidentLimit(PageId first_evictable_page_id) noexcept {
    // Additive only - see the declaration. A limit that could fall would let
    // a structure be declared un-evictable and then be evicted.
    if (first_evictable_page_id > first_evictable_page_id_) {
        first_evictable_page_id_ = first_evictable_page_id;
    }
}

std::size_t DevicePageStore::pinned_frames() const noexcept {
    std::size_t pinned = 0;
    for (const auto& [id, frame] : frames_) {
        if (frame.pins != 0) ++pinned;
    }
    return pinned;
}

std::size_t DevicePageStore::EvictColdFrames(std::size_t budget) {
    if (budget == 0 || frames_.empty()) return 0;

    // The sweep order. `frames_` is an unordered_map, so "where the hand is"
    // cannot be an iterator - a rehash would invalidate it - and is instead a
    // page id the pass re-finds by ordering. That costs a sort per sweep and
    // is why page.md §16-7 has the frame table becoming open-addressed; it is
    // deliberately not fixed here, because a sweep nothing calls yet (EV7) is
    // not where to spend that change.
    std::vector<PageId> order;
    order.reserve(frames_.size());
    for (const auto& [id, frame] : frames_) order.push_back(id);
    std::sort(order.begin(), order.end());

    // Resume where the last pass stopped, so the hand advances around the
    // whole set rather than re-punishing the low ids every time.
    auto start = std::lower_bound(order.begin(), order.end(), clock_hand_);
    const std::size_t first = static_cast<std::size_t>(start - order.begin());

    std::size_t reclaimed = 0;
    // Enough laps for the highest usage counter to be walked down to zero
    // and then collected. Nothing bumps a counter while the sweep runs, so
    // one more lap than the cap is exactly sufficient and no rotation past
    // that can reclaim anything a previous one did not.
    const std::size_t steps = order.size() * (kClockUsageCap + 1);
    for (std::size_t step = 0; step < steps && reclaimed < budget; ++step) {
        const PageId id = order[(first + step) % order.size()];
        auto it = frames_.find(id);
        if (it == frames_.end()) continue;  // reclaimed earlier in this pass

        Frame& frame = it->second;

        // The three refusals, in the order they are cheapest to test. Each
        // is a guarantee something else depends on, not an optimization:
        //   pinned    - a live PageRef points into these bytes (EV01);
        //   resident  - the class is never a candidate at any pressure (EV3),
        //               which is what AST04's Bound Cabin rests on;
        //   dirty     - the flush it needs is WAL-gated and is EV04's, so
        //               dropping it here would lose a write (EV02's scope).
        if (frame.pins != 0) continue;
        if (IsPinnedClass(id)) continue;

        // §3.2's branches, in the specified order: a positive usage counter
        // is decremented and the frame survives this rotation.
        if (frame.usage != 0) {
            --frame.usage;
            continue;
        }

        // Usage zero and dirty: queued for writeback, **not** reclaimed
        // (§3.2's fourth branch, §4's queue). Reclaiming it would lose the
        // write, and the writeback that would clean it is EVT03's.
        if (frame.dirty) {
            if (std::find(dirty_eviction_queue_.begin(), dirty_eviction_queue_.end(), id) ==
                dirty_eviction_queue_.end()) {
                dirty_eviction_queue_.push_back(id);
            }
            continue;
        }

        frames_.erase(it);
        ++reclaimed;
        clock_hand_ = id + 1;
    }

    if (reclaimed != 0 && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("pagestore", "clock reclaimed " + std::to_string(reclaimed) +
                                     " frame(s) on core " + std::to_string(core_id_) + ", " +
                                     std::to_string(frames_.size()) + " resident");
    }
    return reclaimed;
}

}  // namespace kds::storage
