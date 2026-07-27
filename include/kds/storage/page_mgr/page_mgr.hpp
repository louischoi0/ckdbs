#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "kds/storage/page_store.hpp"

// Buffer pool / page manager, ported from the legacy kernel engine's
// page_mgr.c/kds_page_mgr.h: a fixed-capacity table of frames tracking
// which pages are resident, with pin counts and dirty flags. Scope
// matches the legacy "first cut" exactly - no eviction (pool full =>
// OutOfSpace, same as the legacy ENOSPC) and no dirty-list/checkpointer
// integration beyond exposing is_dirty()/MarkClean(). Both are separate,
// documented follow-ups there too.
//
// Divergence from the legacy design, and why: the legacy buffer pool
// exists to cache disk pages (a genuinely separate medium, block device
// sectors) into RAM frames - kds_frame_read_from_disk() does a real
// memory-to-memory copy, and kds_frame_flush() writes a frame's RAM copy
// back out to disk. This project's current storage::PageStore backing a
// BufferPool (InMemoryPageStore) already *is* RAM - there is no second
// medium to copy between yet (that only arrives once a real disk-backed
// PageStore exists, the still-open "I/O backend abstraction" item in
// CLAUDE.md). So Frame::bytes() here is a view directly into the backing
// store's own memory, not a separate frame-owned copy: what this file
// keeps from the legacy design is the part that's independent of the
// storage medium - pin/unpin (so nothing frees/reuses a page a caller is
// still using), dirty tracking (an integration point for a future
// flush/checkpointer), and bounded resident-page admission control (the
// fixed frame count). Once a real disk-backed PageStore exists, Flush()
// becomes meaningful I/O instead of bookkeeping; the API shape here is
// meant to not need to change when that happens.
//
// Concurrency: single-core only, matching rules.md #3 (thread-per-core,
// shared-nothing) - pin counts and frame state are plain (non-atomic)
// fields. A cross-core buffer pool would need the message/queue interface
// rules.md #3 requires for cross-core communication, which does not exist
// yet; this class is not it.

namespace kds::storage {

class BufferPool;

class Frame {
public:
    PageId page_id() const noexcept { return page_id_; }

    // A view directly into the backing PageStore's memory for this page -
    // see the file-level comment on why this isn't a separate copy. Held
    // internally with dynamic extent only because a fixed-extent span
    // has no default state (needed for Frame to sit in a std::vector
    // before it's ever registered); reconstructed as fixed-extent here
    // since every registered frame's span is always exactly kPageSize.
    std::span<std::byte, kPageSize> bytes() const noexcept {
        return std::span<std::byte, kPageSize>(bytes_.data(), kPageSize);
    }

    bool is_dirty() const noexcept { return dirty_; }
    void MarkDirty() noexcept { dirty_ = true; }

    std::int64_t pin_count() const noexcept { return pin_count_; }

private:
    friend class BufferPool;

    PageId page_id_ = kInvalidPageId;
    std::span<std::byte> bytes_{};
    bool dirty_ = false;
    std::int64_t pin_count_ = 0;
};

class BufferPool final : public PageStore {
public:
    // Matches the legacy engine's KDS_BUF_NR_FRAMES.
    static constexpr std::uint32_t kDefaultNrFrames = 4096;

    // `backing` provides the actual page bytes/persistence and must
    // outlive this pool. `nr_frames` bounds how many distinct pages can
    // be resident at once - unlike the legacy fixed compile-time
    // constant, this is a constructor parameter (mainly so tests can
    // exercise the "pool full" path without needing 4096 pages).
    explicit BufferPool(PageStore& backing, std::uint32_t nr_frames = kDefaultNrFrames);

    // Cache-hit-only lookup; does not touch `backing`. Pins the frame
    // before returning (caller must Unpin() exactly once). NotFound on a
    // cache miss.
    StatusOr<Frame*> Lookup(PageId page_id) noexcept;

    // Looks up page_id; on miss, loads it via backing_.Get() into a fresh
    // frame slot. Fails with OutOfSpace if every frame slot is already in
    // use (no eviction implemented, same as the legacy engine) or
    // whatever error backing_.Get() reports (e.g. NotFound if page_id was
    // never created).
    StatusOr<Frame*> LookupOrLoad(PageId page_id);

    // Registers a brand-new page (via backing_.CreateAt()) into a fresh
    // frame slot, pinned and marked dirty (the legacy engine's rationale
    // still applies here even without a real disk to flush to yet: a
    // caller-visible new page must not be silently reclaimable before
    // anything has looked at it). Fails with AlreadyExists if page_id is
    // already resident (mirrors the legacy "caller bug" rejection) or
    // OutOfSpace if the pool is full.
    StatusOr<Frame*> AllocNew(PageId page_id);

    void Pin(Frame& frame) noexcept { ++frame.pin_count_; }
    void Unpin(Frame& frame) noexcept { --frame.pin_count_; }

    // Clears a frame's dirty flag. Callers should only do this once its
    // bytes are actually durable - today that's a no-op-adjacent
    // bookkeeping step (see file comment); once a real disk-backed
    // PageStore exists, callers would do so right after a real flush.
    void MarkClean(Frame& frame) noexcept { frame.dirty_ = false; }

    struct Stats {
        std::uint32_t total;
        std::uint32_t free;
        std::uint32_t valid;
    };
    Stats stats() const noexcept;

    // storage::PageStore overrides, so a BufferPool is itself a drop-in
    // PageStore for callers (e.g. kds::catalog::Catalog) that only need
    // page bytes and don't need pin/dirty tracking directly - all three
    // funnel through the same frame bookkeeping as AllocNew()/LookupOrLoad().
    StatusOr<std::span<std::byte, kPageSize>> CreateAt(PageId page_id) override;
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNew() override;
    StatusOr<std::span<std::byte, kPageSize>> Get(PageId page_id) override;

private:
    StatusOr<std::uint32_t> TakeFreeFrameSlot() noexcept;
    Frame& RegisterFrame(std::uint32_t slot, PageId page_id,
                         std::span<std::byte, kPageSize> bytes, bool initial_dirty);

    PageStore& backing_;
    std::vector<Frame> frames_;
    std::vector<std::uint32_t> free_slots_;
    std::unordered_map<PageId, std::uint32_t> page_to_slot_;
};

}  // namespace kds::storage
