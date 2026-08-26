#pragma once

#include <cstdint>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/analysis.hpp"

// Recovery's high-water repair (docs/workplan-wal-recovery.md RV4/RC04):
// after analysis has said which page ids and which transaction ids the log
// names, move the two allocators past all of them - **before the store can
// serve an allocation.**
//
// ---- The hazard, and a correction to how RV4 states it -------------------
//
// RV4 words this as "the superblock's high-water mark is unlogged". The
// superblock holds no such mark: `server/superblock.hpp` says in as many
// words that it "deliberately does not hold allocation state - no page-id
// counter, no total/free page counts", and that which ids exist is answered
// by `DevicePageStore`'s free-map page. The *hazard* survives that
// correction unchanged, because the free map is unlogged too:
//
//   1. a page is allocated - a bit set in the in-memory free map;
//   2. its PAGE_INIT and its writes are logged, and the log is synced;
//   3. the crash lands before the map page reaches the platter.
//
// The stream still names the page. The map does not. Nothing about that
// state is detectable from the map alone, and the next `CreateNew()` hands
// the id straight back out - over bytes that redo has by then rewritten,
// and that a durable undo record still points at by `(page_id, slot)`. It
// is the difference between a leaked page and a corrupted one.
//
// Where the correction *does* bite is on how much is left to repair.
// Redo's own `CreateAt()` re-establishes the bit for every page it writes
// through a PAGE_INIT or a full page image, so the common case is already
// closed by the phase before this one. What is not closed, and what makes
// this task a fix rather than a formality:
//
//   - **a page the log names that redo never visits.** A CHECKPOINT_BEGIN
//     dirty-page entry with a recLSN of 0 is dirty-but-described-by-no-
//     record (wal.md §11-3), so no applier ever runs for it and no bit is
//     ever set - and analysis still counts it in `max_page_id`, correctly,
//     because the log named it.
//   - **the floor itself.** `DevicePageStore::Open` starts every mount at
//     the same constant `first_new_page_id`, which is a property of the
//     build and not of the database in front of it. Safety today rests on
//     the free map being complete; this makes it rest on the log instead,
//     which is the one record a crash cannot revert.
//
// ---- What it costs -------------------------------------------------------
//
// Ids below the mark are skipped for the life of the instance. Nothing
// frees a page (page.md §5) and `CreateNew()` hands out the lowest free id,
// so in practice there are almost no gaps below the mark to skip - and the
// alternative to skipping them is re-issuing one the log names.
//
// ---- The transaction-id half ---------------------------------------------
//
// `txn::TrxIdSequence` reserves a block of ids, persists the ceiling into
// the superblock, and hands ids out of memory (`txn/trx_id.hpp`) - so a
// crash between the raise and the page reaching the platter reissues the
// block, which is the same shape of exposure by the same unlogged-page
// mechanism. `AnalysisResult::max_txn_id` exists for it, and this function
// computes the ceiling it implies. It does **not** apply it: the superblock
// belongs to `server/`, which sits above `wal/`, and analysis.hpp already
// declined to reach up for the same reason. The caller owes exactly one
// call, `SuperBlock::SetNextTrxId(repair.next_trx_id)`, which already
// refuses to lower a ceiling.

namespace kds::wal {

// What the repair established. Reported rather than assumed, because two
// consumers outside this function need the numbers: the superblock owes the
// transaction ceiling (above), and `storage::ExtentAllocator` - core 0's
// carver of per-core page extents (`storage/extent_lease.hpp`) - must start
// its search above `page_floor`, since a granted extent covering a page the
// log names is this same hazard wearing the multicore shape.
struct HighWaterRepair {
    // First page id an allocation may hand out: one past the largest id any
    // record in the replayed range named. `kInvalidPageId` when the log
    // named no page at all, in which case nothing was raised.
    PageId page_floor = kInvalidPageId;

    // Whether the log named a page. False for a stream of nothing but
    // transaction and checkpoint records, which is a legal empty recovery
    // rather than a repair that failed.
    bool page_floor_raised = false;

    // The transaction-id ceiling the superblock must not sit below: one
    // past the largest id the log names. 0 when the log names none.
    // **Computed here, applied by the caller** - see the header note.
    std::uint64_t next_trx_id = 0;
};

// Runs the repair against `store`. Call it after redo and before anything
// can allocate (RV1: recovery completes before the listener binds).
//
// Fails with whatever `PageStore::RaiseAllocationFloor` reports - a store
// that cannot raise its floor answers Unsupported, and a floor past the
// store's addressable range answers OutOfRange. Either refuses the mount,
// which is the point: a recovery that could not close this hazard must not
// serve the database (docs/spec/txn.md §8).
//
// Fails with Corruption when the log names `kInvalidPageId - 1`, whose
// successor is the reserved invalid id (invariant 1). No page can legally
// live there, so the stream is not describing a database this build wrote.
StatusOr<HighWaterRepair> RaiseHighWater(storage::PageStore& store,
                                         const AnalysisResult& analysis);

}  // namespace kds::wal
