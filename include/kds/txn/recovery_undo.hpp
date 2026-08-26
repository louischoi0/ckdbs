#pragma once

#include <cstdint>

#include "kds/base/status.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/recovery.hpp"

// Recovery's undo phase (docs/spec/wal.md §12-3,
// docs/workplan-wal-recovery.md RC05): roll every loser back, so a restart
// yields exactly the acknowledged-commit state.
//
// ---- Why it lives in txn/ and not wal/ -----------------------------------
//
// `wal::UndoPhase` is the seam RC04a's driver calls; this is the
// implementation. It cannot live beside the interface: it needs `UndoLog`,
// `heap::PageView` and the undo record format, and `wal/` sits below `txn/`
// - `txn/undo_log.hpp` already includes `wal/manager.hpp`, so the reverse
// include would be a cycle.
//
// ---- What it walks, and why not the trail --------------------------------
//
// A **per-transaction chain**, newest to oldest, from the head the
// checkpoint recorded (RV10): `UndoVersion::txn_prev_undo_ptr`. Not the
// in-memory trail `TransactionManager::Abort` reads - that dies with the
// process - and **not the records in the replay range**, which is the
// enumeration gap §4b records: a page written back before a checkpoint has
// its recLSN cleared, so the redo start can advance past a still-
// uncommitted write and the record naming it is never scanned. Walking the
// chain reaches every record the transaction wrote, however far below the
// scan it lies.
//
// ---- The identity check, and why there is no locator ---------------------
//
// A btree leaf division moves tuples and renumbers slots, so a record's
// `(target_page_id, target_slot)` is where the row **was**. The live path
// handles this by asking `TransactionManager::RowLocator` for the row's
// current address; recovery cannot, because a locator needs `(rel_oid, pk)`
// and the undo record has no `rel_oid` - there is no room for one and
// `page.md` has no page->relation index to derive it from.
//
// So recovery takes the **no-locator branch** the live path already ships:
// check the pk at the address, and on a mismatch **fail the mount** rather
// than compensate a row this transaction never wrote. Narrow in practice -
// only a key named below a relation's high-water mark can divide a leaf
// mid-statement, and a heap relation refuses such a key
// (`docs/spec/heap-and-tuple.md` §4.1) - and it fails loudly rather than
// corrupting. Lifting it means putting `rel_oid` in the undo record, which
// is a format-version event and its own decision (§4a).
//
// ---- Crash-restartable, with no CLR --------------------------------------
//
// Compensations are ordinary logged mutations (RV6's surviving half), so a
// crash during undo leaves a prefix of them durable and the next recovery
// replays that prefix through the `page_lsn` gate before walking the chain
// again. Every compensation is therefore written to be a **no-op on
// re-application**: an overwrite puts back bytes already there, a cleared
// mark re-clears, and a retired slot is recognised as already retired
// rather than mistaken for a missing row.
//
// `TXN_ABORT` is what ends the loop for good: once it is durable, analysis
// classifies the transaction `kAborted` and undo is never asked again.
//
// Concurrency: core-local, like everything it touches.

namespace kds::txn {

// Chain steps before the walk gives up. The same bound and the same
// reasoning as `kMaxUndoChainLength` on the version chain: a cycle in a
// damaged chain must be a reported Corruption and never a hang.
inline constexpr std::size_t kMaxUndoTxnChainLength = 1u << 20;

class RecoveryUndo final : public wal::UndoPhase {
public:
    // `wal` may be null, which means compensations mutate pages without
    // being logged - the shape every socket-free test already uses. It is
    // sound only for a caller that will not crash again; a real mount
    // installs one, because an unlogged compensation is a rollback the next
    // recovery cannot see happened.
    RecoveryUndo(UndoLog& undo, wal::WalManager* wal = nullptr) noexcept
        : undo_(undo), wal_(wal) {}

    Status RollBack(storage::PageStore& store, const wal::AnalysisResult& analysis) override;

    // Compensations emitted, and transactions rolled back. For RC09's
    // report and for tests that assert work happened rather than was
    // skipped - a phase that silently did nothing is the failure mode this
    // whole plan is written against.
    std::uint64_t compensations() const noexcept { return compensations_; }
    std::uint64_t transactions() const noexcept { return transactions_; }
    std::uint64_t already_done() const noexcept { return already_done_; }

private:
    Status RollBackOne(storage::PageStore& store, std::uint64_t txn_id, std::uint64_t head);
    Status Compensate(storage::PageStore& store, std::uint64_t txn_id, const UndoVersion& rec);

    UndoLog& undo_;
    wal::WalManager* wal_;
    std::uint64_t compensations_ = 0;
    std::uint64_t transactions_ = 0;
    std::uint64_t already_done_ = 0;
};

}  // namespace kds::txn
