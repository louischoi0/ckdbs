#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/storage/page_device.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/durability.hpp"

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
// Every *headered* page it writes is stamped with a CRC32C over the common
// page header (page.md sections 8 and 10) and verified when it is read back
// on a miss - never on a hit. Corruption is therefore detected at load; the
// FULL_PAGE_IMAGE that heals it is the WAL's job (wal.md section 10).
//
// ---- Headerless pages ---------------------------------------------------
//
// A page class whose payload tiles 8 KiB exactly - an array of fixed-size
// entries sized to a power of two, addressed by shift and mask - has no
// room for the common header, and no caller wants one: a header would cost
// an entry and break the addressing that is the point of such a layout.
// Stamping a checksum into one at offset 4 would overwrite data, and
// verifying one on read would reject it.
//
// CreateNewHeaderless() marks a page as such, and the mark is **durable**,
// in a second bitmap page of the same shape as the free map
// (kHeaderlessMapPageId). It cannot be an in-memory side table: this store
// never evicts, so a page comes off the device exactly once, on first touch
// after open - which is exactly when a verify would reject it. It cannot be
// recomputed from the catalog either, because the store is opened before
// bootstrap has a catalog to ask.
//
// The cost is that these pages carry no damage detection at all, so the
// mechanism is only appropriate for a structure whose corruption is
// *survivable* - one a reader validates against an authoritative source
// and can fall back from. Exactly one page class qualifies today: the
// interior pages of the waystone directory (stats/waystone_dir.hpp), whose
// 2048 child ids tile the page exactly and whose damage costs a lookup that
// falls through to the authoritative path. A database with no waystone
// directory pays one reserved page for the bitmap and nothing else.
//
// Not here, deliberately:
//   - No eviction. Everything touched stays resident, as InMemoryPageStore
//     already did. Clock eviction needs PageRef (page.md section 3) and
//     the frame-reclamation policy is an open decision in CLAUDE.md.
//   - Dirty tracking is by which accessor the caller chose, not by what it
//     actually wrote: Get() marks the frame dirty, GetForRead() leaves it
//     alone, and both hand out the same raw mutable span. A reader that
//     calls Get() costs a needless write-back; a writer that calls
//     GetForRead() loses its write. PageRef (page.md section 3) is what
//     replaces the convention with a type.
//   - One free-map page, so coverage is kFreeMapBitsPerPage ids; beyond
//     that is OutOfRange, not silently unmapped.
//
// ---- The WAL gate (page.md section 8, wal.md section 8-1) ---------------
//
// A dirty page may reach the device only once the log records describing
// its modifications are durable. That rule is enforced here rather than
// asked of callers: SetWalGate() installs a WalDurability, and every write
// path below (Flush, Sync, FlushPages) first calls EnsureDurable() on the
// highest page_lsn among the pages it is about to write. With no gate
// installed the store behaves exactly as it did before one existed, which
// is what the WAL-free unit tests and the simulator rely on - and which is
// sound only for a caller that logs nothing.
//
// The gate is one call per flush batch, not per page: EnsureDurable() is a
// no-op once the watermark has passed, so gating on the batch maximum
// costs at most one sync for the whole batch instead of one per page.
//
// StampPageLsn() is the other half. A mutation path appends its record,
// then calls it with the record's LSN; that both records the page_lsn the
// gate reads and captures the frame's **recLSN** - the LSN of the first
// record to dirty the frame since it was last clean, which is what a
// checkpoint's dirty table must carry for recovery's redo start to be
// correct (wal.md section 11-1). A frame keeps that value until it is
// written back, then drops it.
//
// Concurrency: core-local, no internal synchronization (rules.md #3).
//
// Logging (component tag "pagestore"): allocation and write-back are the
// two things that change what is on disk, so both are logged - allocation
// and per-page write-back at Trace (one line per page), batch write-back
// and sync at Debug, and every device-level failure at Error. A failed
// checksum verify is Error and says "corruption" in as many words: it is
// the one thing this class can detect that no caller can diagnose from a
// Status alone, and it names damage rather than a policy refusal.

namespace kds::storage {

// Fixed home of the free-map page, in the reserved sub-128 system range
// alongside the superblock (0) and the catalog's fixed pages (4-8).
inline constexpr PageId kFreeMapPageId = 1;

// The headerless bitmap (PageType::kHeaderlessMap), immediately after it
// in the same reserved sub-128 range. One bit per page id: set means the
// page carries no common header, so it is neither checksum-stamped on the
// way out nor verified on the way in.
inline constexpr PageId kHeaderlessMapPageId = 2;

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

    // Get() that leaves the frame clean (page_store.hpp). Without it a
    // read-only statement dirties every page it touches and the next
    // checkpoint writes the whole working set back with nothing changed.
    StatusOr<std::span<std::byte, kPageSize>> GetForRead(PageId page_id) override;

    // CreateNew() for a page that will carry no common header - see the
    // note above. The whole 8 KiB is the caller's, and this store will
    // neither stamp nor verify a checksum on it, now or after a reopen.
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewHeaderless() override;

    // Whether `page_id` was created headerless. False for an id that does
    // not exist, which is the safe answer: an unknown page is treated as
    // headered and therefore verified.
    bool IsHeaderless(PageId page_id) const noexcept;

    // Writes dirty frames back in page-id order, which is file order
    // (page.md section 13), then the free map. Data pages go first so a
    // crash between them can only orphan a page, never publish one whose
    // bytes never landed. Not durable on its own - Sync() adds the fsync.
    Status Flush();
    Status Sync() override;

    // Installs the WAL-before-data gate described above. Null (the
    // default) disables it. `gate` must outlive the store.
    void SetWalGate(wal::WalDurability* gate) noexcept { wal_gate_ = gate; }

    // Records that the record at `lsn` modified `page_id`: stamps the
    // page header's page_lsn and, if this is the first record to dirty the
    // frame since it was last written back, adopts `lsn` as its recLSN.
    //
    // Call it *after* appending the record and *before* returning to the
    // client. Fails with NotFound if the page is not resident, and with
    // InvalidArgument for lsn 0 (kNoPageLsn means "never logged", so it
    // cannot also mean "logged at 0"; a real record LSN is never 0 because
    // offset 0 is the segment header - record.hpp).
    Status StampPageLsn(PageId page_id, std::uint64_t lsn) override;

    // Page ids currently dirty, in ascending order.
    std::vector<PageId> DirtyPageIds() const;

    // The same set with each frame's recLSN attached - what a checkpoint
    // snapshots (wal.md section 11-1). A frame dirtied by something that
    // logged nothing reports 0, which the checkpointer reads as "nothing
    // to replay for this page"; that is accurate for the unlogged paths
    // (catalog writes, bootstrap) and wrong for a logged one, which is why
    // every logged mutation must call StampPageLsn().
    std::vector<std::pair<PageId, wal::Lsn>> DirtyPagesWithRecLsn() const;

    // Writes back exactly these pages and syncs. Ids that are not dirty,
    // or not resident, are skipped rather than treated as an error -
    // something else may have flushed them since the caller's snapshot.
    Status FlushPages(std::span<const PageId> page_ids);

    // Diagnostic log, null (discard) by default. Set after Open(), since
    // the store has to exist before a server has anything to log about;
    // `log` must outlive the store.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    std::size_t resident_pages() const noexcept { return frames_.size(); }
    std::uint32_t allocated_pages() const noexcept;
    bool IsAllocated(PageId page_id) const noexcept;

private:
    using Page = std::array<std::byte, kPageSize>;

    struct Frame {
        std::unique_ptr<Page> bytes;
        bool dirty = false;
        // First log record to dirty this frame since it was last written
        // back; 0 when nothing logged touched it. See StampPageLsn().
        wal::Lsn rec_lsn = 0;
    };

    DevicePageStore(PageDevice& device, PageId first_new_page_id, const Page& free_map,
                    const Page& headerless_map, bool maps_dirty) noexcept;

    // Stamps a checksum unless the page is headerless. The one place that
    // decision is made, so no write path can forget it.
    void StampIfHeadered(PageId page_id, std::span<std::byte, kPageSize> page) const;

    // Writes back whichever of the two bitmap pages are dirty, after the
    // data pages they describe. Same ordering rule the free map always
    // followed: a page is only reachable once the map says so.
    Status FlushMaps();

    // Waits for the log records of `page_ids` to be durable before any of
    // them is written. A no-op with no gate installed or no logged page in
    // the batch.
    Status AwaitWalGate(std::span<const PageId> page_ids);

    // `mark_dirty` is false only for a read-only fetch: a frame faulted in
    // by a reader has not been modified, so it enters the map clean and
    // nothing writes it back.
    StatusOr<std::span<std::byte, kPageSize>> ResidentBytes(PageId page_id, bool mark_dirty);
    std::span<std::byte, kPageSize> InsertFrame(PageId page_id, std::unique_ptr<Page> bytes,
                                                bool dirty);
    Status EnsureAddressable(PageId page_id);

    std::span<std::byte, kPageSize> free_map_bytes() noexcept {
        return std::span<std::byte, kPageSize>(free_map_page_);
    }
    std::span<const std::byte, kPageSize> free_map_bytes() const noexcept {
        return std::span<const std::byte, kPageSize>(free_map_page_);
    }
    std::span<std::byte, kPageSize> headerless_map_bytes() noexcept {
        return std::span<std::byte, kPageSize>(headerless_map_page_);
    }
    std::span<const std::byte, kPageSize> headerless_map_bytes() const noexcept {
        return std::span<const std::byte, kPageSize>(headerless_map_page_);
    }

    PageDevice& device_;
    Logger* log_ = nullptr;
    wal::WalDurability* wal_gate_ = nullptr;
    Page free_map_page_;
    Page headerless_map_page_;
    // One flag for both maps: they are written together, in the same
    // order, at the same points, and a separate flag per map would only
    // create a state where one is on disk and the other is not.
    bool maps_dirty_;
    PageId next_new_page_id_;
    std::unordered_map<PageId, Frame> frames_;
};

}  // namespace kds::storage
