#pragma once

#include <cstdint>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/manager.hpp"

// Releasing one spilled value, logged - the step every death of a var-heap
// value goes through (`instructions/v2.5.0/varchar-char-architecture.md` §4).
//
// **It lives here rather than at its callers because there are three of
// them and they must not drift.** A live rollback
// (`TransactionManager::Compensate`), recovery's undo phase
// (`txn::RecoveryUndo`), and - when it lands - the deferred-release drain
// all do the same five steps: fetch the page, tombstone the slot, encode
// the record, append it under the right envelope, stamp the page. Written
// out three times, the page-type guard or the stamp would eventually be
// missing from one of them, which is the failure `exec::LogSlotRetire`
// exists to prevent for its own record and which this file copies.
//
// The envelope's `txn_id` is the caller's, and the split is `txn.md` §6's:
// a **rollback compensation** carries the aborting transaction's id,
// because analysis must see the rollback; a **purge drain** carries
// `wal::kNoTxnId`, because no transaction owns it.

namespace kds::txn {

// What a release found when it got there.
enum class ReleaseOutcome : std::uint8_t {
    // The slot held a value and now holds a tombstone, or already held one
    // (`varheap::PageRelease` is idempotent, so the two are one answer).
    kReleased = 0,

    // **There was nothing to release**, because the append this would undo
    // was never applied to the page: the page does not exist, or the slot
    // is past its directory.
    //
    // Reachable only from recovery, and by a route phase B's own ordering
    // opens. An `UNDO_WRITE{kVarHeapAppend}` is written *before* the
    // `PAGE_INIT`/`VARHEAP_APPEND` that fill the slot, so a log whose
    // readable prefix ends between them leaves the loser's chain naming a
    // slot redo never created. The WAL rule guarantees the converse only -
    // a flushed page image implies its records are durable - so this is a
    // real state and not a torn-write hypothesis.
    //
    // A caller that can reach it must treat it as work already done: an
    // append that was never redone has nothing to undo. A caller that
    // cannot must treat it as Corruption, because in-process the slot was
    // written moments ago and its absence is a defect, not a crash.
    kNothingToRelease = 1,
};

// Tombstones `slot` on `page_id` and logs a `VARHEAP_RELEASE` for it,
// stamping the page behind the record. `wal` may be null, which skips the
// record exactly as every other unlogged path does.
//
// Fails with Corruption for a page that is not a kVarHeap page - that check
// is `varheap::PageRelease`'s and is load-bearing, since a heap page holds
// `nr_slots` at the same body offset.
StatusOr<ReleaseOutcome> ReleaseVarHeapSlot(storage::PageStore& store, wal::WalManager* wal,
                                            std::uint64_t txn_id, PageId page_id,
                                            std::uint16_t slot);

}  // namespace kds::txn
