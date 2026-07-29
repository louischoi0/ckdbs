#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "kds/storage/page_device.hpp"
#include "kds/storage/page_store.hpp"

// The disk-backed PageStore: the same three-operation contract catalog,
// bootstrap and the dispatcher already speak, served out of a PageDevice.
// On a FilePageDevice a database survives a restart; on a MemoryPageDevice
// the identical code runs under the simulator with fault injection.
//
// Two durable facts make that work, and this class owns both: which page
// ids exist (a free-map bitmap page at kFreeMapPageId, so Get() still
// answers NotFound after a reopen - which is what bootstrap reads to
// decide whether to run Catalog::Bootstrap()), and which page bytes are
// current (resident frames, written back by Flush()).
//
// Every page it writes is stamped with a CRC32C over the common page
// header (page.md sections 8 and 10) and verified when it is read back on
// a miss - never on a hit. Corruption is therefore detected at load; the
// FULL_PAGE_IMAGE that heals it is the WAL's job (wal.md section 10).
//
// Not here, deliberately:
//   - No eviction. Everything touched stays resident, as InMemoryPageStore
//     already did. Clock eviction needs PageRef (page.md section 3) and
//     the frame-reclamation policy is an open decision in CLAUDE.md.
//   - No WAL gate (page.md section 8): there is no WAL to wait on, so this
//     is restart-durable, not crash-durable.
//   - Every page handed out is marked dirty, because PageStore v1 hands
//     out a raw mutable span with no MarkDirty(). PageRef fixes that.
//   - One free-map page, so coverage is kFreeMapBitsPerPage ids; beyond
//     that is OutOfRange, not silently unmapped.
//
// Concurrency: core-local, no internal synchronization (rules.md #3).

namespace kds::storage {

// Fixed home of the free-map page, in the reserved sub-128 system range
// alongside the superblock (0) and the catalog's fixed pages (4-8).
inline constexpr PageId kFreeMapPageId = 1;

class DevicePageStore final : public PageStore {
public:
    // `device` must outlive the store. A device with no pages, or one whose
    // free-map page was never written, is initialized as a fresh database;
    // otherwise the existing free map is loaded, and a bad checksum or
    // header is Corruption rather than a guess.
    //
    // `first_new_page_id` is where CreateNew() starts looking; pick a value
    // above any id that gets CreateAt'ed.
    static StatusOr<std::unique_ptr<DevicePageStore>> Open(PageDevice& device,
                                                           PageId first_new_page_id = 1);

    StatusOr<std::span<std::byte, kPageSize>> CreateAt(PageId page_id) override;
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNew() override;
    StatusOr<std::span<std::byte, kPageSize>> Get(PageId page_id) override;

    // Writes dirty frames back in page-id order, which is file order
    // (page.md section 13), then the free map. Data pages go first so a
    // crash between them can only orphan a page, never publish one whose
    // bytes never landed. Not durable on its own - Sync() adds the fsync.
    Status Flush();
    Status Sync() override;

    std::size_t resident_pages() const noexcept { return frames_.size(); }
    std::uint32_t allocated_pages() const noexcept;
    bool IsAllocated(PageId page_id) const noexcept;

private:
    using Page = std::array<std::byte, kPageSize>;

    struct Frame {
        std::unique_ptr<Page> bytes;
        bool dirty = false;
    };

    DevicePageStore(PageDevice& device, PageId first_new_page_id, const Page& free_map,
                    bool free_map_dirty) noexcept;

    StatusOr<std::span<std::byte, kPageSize>> ResidentBytes(PageId page_id);
    std::span<std::byte, kPageSize> InsertFrame(PageId page_id, std::unique_ptr<Page> bytes);
    Status EnsureAddressable(PageId page_id);

    std::span<std::byte, kPageSize> free_map_bytes() noexcept {
        return std::span<std::byte, kPageSize>(free_map_page_);
    }
    std::span<const std::byte, kPageSize> free_map_bytes() const noexcept {
        return std::span<const std::byte, kPageSize>(free_map_page_);
    }

    PageDevice& device_;
    Page free_map_page_;
    bool free_map_dirty_;
    PageId next_new_page_id_;
    std::unordered_map<PageId, Frame> frames_;
};

}  // namespace kds::storage
