#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/durability.hpp"

// Buffer pool / page manager: a fixed-capacity table of frames tracking
// which pages are resident, with pin counts and dirty flags. Deliberately
// a first cut - no eviction (a full pool is OutOfSpace) and no
// dirty-list/checkpointer integration beyond exposing
// is_dirty()/MarkClean(). Both are follow-ups; the frame-reclamation
// policy is an open decision in CLAUDE.md.
//
// **There is no copy here, and that is the thing to understand about this
// file.** A buffer pool normally caches disk pages into RAM frames, which
// means a real memory-to-memory copy in each direction. The PageStore this
// pool sits over (InMemoryPageStore) already *is* RAM, so there is no
// second medium to copy between: Frame::bytes() is a view straight into
// the backing store's own memory, not a frame-owned copy.
//
// What the pool therefore contributes is only what is independent of the
// storage medium - pin/unpin (so nothing frees or reuses a page a caller
// still holds), dirty tracking (the integration point for a flush and
// checkpointer), and bounded resident-page admission control (the fixed
// frame count). Once a disk-backed PageStore is the norm here, Flush()
// becomes real I/O instead of bookkeeping, and the API shape is meant to
// survive that unchanged.
//
// Concurrency: single-core only, matching rules.md #3 (thread-per-core,
// shared-nothing) - pin counts and frame state are plain (non-atomic)
// fields. A cross-core buffer pool would need the message/queue interface
// rules.md #3 requires for cross-core communication, which does not exist
// yet; this class is not it.
//
// ---- The WAL gate (page.md section 8, wal.md section 8-1) ---------------
//
// WAL-before-data is enforced here, in code, rather than by asking callers
// to remember it: a dirty frame reaches the backing store only through
// Flush(), Flush() will not write a frame until the log records describing
// it are durable, and MarkClean() is private - the only way a frame goes
// clean is by having been flushed. That is what "unbypassable" means in
// page.md section 8, and page.md section 18-4 is the test for it.
//
// The wait is currently a synchronous EnsureDurable() on the injected
// WalDurability seam rather than a task suspension: every I/O path in the
// engine is still synchronous, so syncing the log inline *is* the wait,
// and the flushing task has nothing to suspend into. When the reactor and
// an async backend land, this becomes a suspension; the gate's position in
// the code does not move. Time spent there is counted (stats().wal_waits)
// because page.md section 11 wants flush stall attributable to WAL waits
// as a product metric.
//
// A pool with no seam injected may only flush pages that were never logged
// (page_lsn 0). A dirty frame carrying a real page_lsn with no WAL to ask
// is refused, not written - a gate that disappears when unconfigured is
// not a gate.
//
// ---- Logging (component tags "buffer" and "page") ----------------------
//
// Two questions get asked of a page cache after the fact - "who dirtied
// that page" and "why did the flush stall" - and neither is answerable
// from counters alone, so both are logged here rather than left to
// stats(). The levels follow the volume:
//
//   Trace  per-page events: a frame going dirty, a page loaded on a miss,
//          a page allocated. One line per page touch, so a Trace-level log
//          is a page-modification journal and is far too loud for a
//          running server (log.hpp's cost note).
//   Debug  per-batch events: flush batches and the WAL waits they paid.
//   Warn   frame exhaustion - the pool is full and there is no eviction,
//          so the next allocation fails a caller.
//   Error  the gate refusing a write, and a barrier that failed. Both mean
//          data did not reach the store; neither has a background caller
//          that would otherwise report it.
//
// A null logger (the default) makes every one of these a single pointer
// comparison, and a level below the message's costs one more - the message
// strings are built inside the guard, never before it.

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

    // Records that this frame was mutated by the WAL record at
    // `record_lsn`, which must already be in the log (wal.md section 6-1:
    // append first, then mutate). Stamps the on-page page_lsn field and
    // the frame's mirror of it together, so the two cannot drift - the
    // mirror exists only to keep the flush path from decoding a header.
    //
    // There is deliberately no no-argument overload. Dirtying a headered
    // page without a log record is the exact mistake the gate below
    // exists to prevent, and an overload that let a caller do it silently
    // would be the hole in it. Pages created but never formatted are the
    // one unlogged-dirty case, and the pool sets that state itself.
    //
    // This is the page-modification event: at Trace level it emits one
    // line per dirtied page ("page N dirty lsn=..."), which is the record
    // of what changed the database. Still noexcept - exceptions are
    // disabled engine-wide (rules.md #1), and below Trace the call builds
    // no string at all.
    void MarkDirty(wal::Lsn record_lsn) noexcept;

    // LSN of the last WAL record applied to this frame; 0 = never logged.
    // The flush gate waits on this one.
    wal::Lsn page_lsn() const noexcept { return page_lsn_; }

    // First record that would have to be replayed to rebuild this frame -
    // the recovery LSN CHECKPOINT_BEGIN records for it (wal.md section
    // 11-1). 0 means "nothing to replay", which is the state of a clean
    // frame and of a page that was created but never logged; it does not
    // mean "the head of the log".
    wal::Lsn rec_lsn() const noexcept { return rec_lsn_; }

    std::int64_t pin_count() const noexcept { return pin_count_; }

private:
    friend class BufferPool;

    PageId page_id_ = kInvalidPageId;
    std::span<std::byte> bytes_{};
    // The pool's logger, copied in at registration so a frame can report
    // its own mutation without carrying a back-pointer to the pool.
    Logger* log_ = nullptr;
    bool dirty_ = false;
    wal::Lsn page_lsn_ = 0;
    wal::Lsn rec_lsn_ = 0;
    std::int64_t pin_count_ = 0;
};

class BufferPool final : public PageStore {
public:
    static constexpr std::uint32_t kDefaultNrFrames = 4096;

    // `backing` provides the actual page bytes/persistence and must
    // outlive this pool. `nr_frames` bounds how many distinct pages can
    // be resident at once - a constructor parameter rather than a
    // compile-time constant, mainly so tests can exercise the "pool full"
    // path without needing 4096 pages.
    explicit BufferPool(PageStore& backing, std::uint32_t nr_frames = kDefaultNrFrames);

    // Injects the WAL-before-data seam. Null until the WAL is wired up on
    // a given path, in which case the pool may only flush never-logged
    // pages (see the file comment). `wal` must outlive the pool.
    void SetWalDurability(wal::WalDurability* wal) noexcept { wal_ = wal; }
    wal::WalDurability* wal_durability() const noexcept { return wal_; }

    // Diagnostic log, null (discard) by default so the pool is usable
    // without one. Applies to frames registered after this call *and* to
    // the ones already resident, so wiring a logger up after open still
    // reports the pages the pool is already holding. `log` must outlive
    // the pool.
    void SetLogger(Logger* log) noexcept;

    // Cache-hit-only lookup; does not touch `backing`. Pins the frame
    // before returning (caller must Unpin() exactly once). NotFound on a
    // cache miss.
    StatusOr<Frame*> Lookup(PageId page_id) noexcept;

    // Looks up page_id; on miss, loads it via backing_.Get() into a fresh
    // frame slot. Fails with OutOfSpace if every frame slot is already in
    // use (no eviction is implemented), or whatever error backing_.Get()
    // reports (e.g. NotFound if page_id was never created).
    StatusOr<Frame*> LookupOrLoad(PageId page_id);

    // Registers a brand-new page (via backing_.CreateAt()) into a fresh
    // frame slot, pinned and marked dirty. Pinning matters even without a
    // real disk to flush to: a caller-visible new page must not be
    // silently reclaimable before anything has looked at it. Fails with
    // AlreadyExists if page_id is already resident (a caller bug), or
    // OutOfSpace if the pool is full.
    StatusOr<Frame*> AllocNew(PageId page_id);

    void Pin(Frame& frame) noexcept { ++frame.pin_count_; }
    void Unpin(Frame& frame) noexcept { --frame.pin_count_; }

    // Writes one dirty frame back, and the only way a frame goes clean.
    //
    // Order is the contract, not an implementation detail: wait for the
    // log to be durable past this page's page_lsn, stamp the checksum
    // (page.md section 10 puts it last, after every other mutation), write
    // the bytes out, and only then clear the dirty flag. A failure at any
    // step leaves the frame dirty and the data file untouched.
    //
    // A clean frame is OK and does nothing.
    Status Flush(Frame& frame);

    // Flushes a named set of pages against a single log wait and a single
    // write-out barrier - what the checkpointer and the background writer
    // want (page.md sections 8 and 18-7). Per-frame Flush() in a loop is
    // correct but pays one barrier each; this pays one for the batch, and
    // orders the batch by page id so it becomes sequential I/O once there
    // is real I/O to order.
    //
    // Ids that are not resident, and resident frames that are already
    // clean, are skipped rather than refused: a checkpoint names the pages
    // that were dirty when it started, and by the time it gets to them the
    // background writer may have flushed them already.
    //
    // All-or-nothing: on failure nothing in the batch is marked clean.
    Status FlushPages(std::span<const PageId> page_ids);

    // FlushPages() over one id, for callers that hold an id rather than a
    // frame.
    Status FlushPage(PageId page_id);

    // FlushPages() over every dirty resident frame.
    Status FlushAll();

    struct DirtyPage {
        PageId page_id;
        // 0 = dirty but never logged (a created page nothing has written a
        // record for). Such an entry still has to be flushed, but it
        // contributes nothing to the redo start - a checkpoint computing
        // min(recLSN) must skip the zeroes rather than min() them in.
        wal::Lsn rec_lsn;
    };

    // {page_id -> recLSN} over the resident dirty frames, page-id sorted:
    // the dirty-page table CHECKPOINT_BEGIN carries (wal.md section 11-1),
    // and the input to the redo start min(recLSN).
    std::vector<DirtyPage> DirtyTable() const;

    struct Stats {
        std::uint32_t total;
        std::uint32_t free;
        std::uint32_t valid;

        // Flush-path counters (page.md section 11's product metrics).
        std::uint64_t flushes;  // frames actually written back
        std::uint64_t flush_barriers;  // write-out barriers those cost
        std::uint64_t wal_waits;  // flushes that had to wait on the log
        std::uint64_t unformatted_flushes;  // written without a checksum
    };
    Stats stats() const noexcept;

    // storage::PageStore overrides, so a BufferPool is itself a drop-in
    // PageStore for callers (e.g. kds::catalog::Catalog) that only need
    // page bytes and don't need pin/dirty tracking directly - all three
    // funnel through the same frame bookkeeping as AllocNew()/LookupOrLoad().
    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override;
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override;
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override;

    // Delegated, because the allocator is delegated: CreateNew() takes its
    // id from `backing_`, so the floor that constrains it is backing_'s.
    Status RaiseAllocationFloor(PageId first_allocatable_page_id) override {
        return backing_.RaiseAllocationFloor(first_allocatable_page_id);
    }

private:
    StatusOr<std::uint32_t> TakeFreeFrameSlot() noexcept;
    Frame& RegisterFrame(std::uint32_t slot, PageId page_id,
                         std::span<std::byte, kPageSize> bytes, bool initial_dirty);

    // The gate itself: returns only once the log is durable past
    // `page_lsn`. Takes the LSN rather than the frame so a batch can wait
    // once on the highest LSN in it instead of once per frame.
    Status AwaitWalDurability(PageId page_id, wal::Lsn page_lsn);

    // The one write-back path every public flush entry point funnels
    // through: gate, stamp, one barrier, mark clean. `slots` must name
    // dirty frames only.
    Status FlushSlots(std::vector<std::uint32_t> slots);
    // Everything that must happen to a page's bytes before write-out.
    void PrepareForWriteOut(Frame& frame);

    // Private on purpose (page.md section 8: "MarkClean exists only as the
    // completion step of the flush path"). A public one is a way to tell
    // the pool a page is safe without it ever having been written, which
    // is precisely the loss the gate exists to prevent.
    //
    // Clearing recLSN is part of going clean: a frame that has been
    // written back needs no replaying from anywhere, and the next
    // mutation establishes a new recovery start.
    void MarkClean(Frame& frame) noexcept {
        frame.dirty_ = false;
        frame.rec_lsn_ = 0;
    }

    PageStore& backing_;
    wal::WalDurability* wal_ = nullptr;
    Logger* log_ = nullptr;
    std::vector<Frame> frames_;
    std::vector<std::uint32_t> free_slots_;
    std::unordered_map<PageId, std::uint32_t> page_to_slot_;

    std::uint64_t flushes_ = 0;
    std::uint64_t flush_barriers_ = 0;
    std::uint64_t wal_waits_ = 0;
    std::uint64_t unformatted_flushes_ = 0;
};

}  // namespace kds::storage
