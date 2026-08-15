#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/txn/visibility.hpp"
#include "kds/wal/checkpointer.hpp"
#include "kds/wal/manager.hpp"

// The transaction manager (docs/txn.md sections 1, 5, 6).
//
// ---- What it does not have ------------------------------------------------
//
// **No lock manager, no waiting, no deadlock detection**, and the Keystone
// lock byte stays unused. A write conflict is detected from the tuple
// header alone and the loser is aborted retryably. That is stricter than
// PostgreSQL's READ COMMITTED, which re-reads; it is a deliberate
// simplification, and the whole reason there is nothing to wait on.
//
// **No reader registration.** Nothing records that a snapshot exists, which
// is what makes undo retention a non-goal and SnapshotTooOld structurally
// unreachable (section 4.1). It is also exactly what has to change when
// purge lands.
//
// **No cross-core protocol.** One manager, one core, one WAL stream; a
// transaction spanning cores is not representable and `wal.md` section 3
// says not to design it yet.
//
// ---- The trail, and the undo chain beside it -------------------------------
//
// Every write appends to an in-memory trail, and Abort walks it in reverse
// emitting compensations. **The trail is the live path's and does not
// survive a crash** - which is why there is a second, durable record of the
// same writes.
//
// Every write also appends an undo record, and since RV10
// (`docs/workplan-wal-recovery.md` §4b) that includes an **insert**.
// Section 3.6's reasoning was about *visibility* and remains true there: a
// tuple with `undo_ptr == kNoUndoPtr` whose writer is invisible already
// means "no visible version", so reading an insert needs no record, and
// `AppendUndo` deliberately does not stamp the tuple. What needs the record
// is recovery: each one links to the transaction's previous through
// `txn_prev_undo_ptr`, and an insert that wrote none would break that chain
// and orphan everything the transaction did before it.
//
// So the two structures are kept in step by one function - `AppendUndo`
// advances the chain, the `Note*` calls extend the trail, and both are
// called from the same three write paths.
//
// ---- Concurrency -----------------------------------------------------------
//
// Core-local (rules.md section 3). "Concurrent" here means interleaved
// cooperative tasks on one core, which is what makes first-updater-wins
// checkable without a latch: nothing suspends between reading a tuple's
// header and overwriting it.

namespace kds::txn {

enum class IsolationLevel : std::uint8_t {
    // Default. A read view per **statement**: a statement sees everything
    // committed before it began. Chosen over REPEATABLE READ because under
    // first-updater-wins with no waiting, holding one view for a whole
    // transaction converts more concurrent writes into retryable aborts
    // (section 1). Matches PostgreSQL and Oracle, differs from InnoDB.
    kReadCommitted = 1,
    // A read view per **transaction**, taken at BEGIN.
    kRepeatableRead = 2,
};

const char* IsolationLevelName(IsolationLevel level) noexcept;

// Parses "read committed" / "repeatable read" case-insensitively, plus the
// hyphen and underscore spellings a config file invites. Anything else is
// InvalidArgument naming what was given - the same shape
// wal::ParseDurabilityClass has.
StatusOr<IsolationLevel> ParseIsolationLevel(std::string_view text);

// What a transaction did, in the order it did it. One entry per page
// mutation, carrying exactly what the compensation needs and nothing else.
enum class TrailAction : std::uint8_t {
    kInsert = 1,      // compensated by RetireSlot
    kOverwrite = 2,   // compensated by putting `image` back
    kDeleteMark = 3,  // compensated by clearing the mark
};

struct TrailEntry {
    TrailAction action = TrailAction::kInsert;
    std::uint32_t rel_oid = 0;
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    std::uint64_t pk = 0;

    // The header to restore. For an insert these are meaningless and the
    // slot is retired instead.
    std::uint64_t prior_trx_id = 0;
    std::uint64_t prior_undo_ptr = kNoUndoPtr;

    // kOverwrite only: the bytes to put back. Held in memory rather than
    // read from the undo record the write already wrote, because rollback
    // must work whether or not that record is reachable - and because the
    // trail is what an insert has instead of a record at all.
    std::vector<std::byte> image;
};

class Transaction {
public:
    std::uint64_t id() const noexcept { return id_; }
    IsolationLevel isolation() const noexcept { return isolation_; }
    bool active() const noexcept { return active_; }

    // The view this transaction reads through *right now*. Under
    // REPEATABLE READ it was taken at BEGIN and never moves; under READ
    // COMMITTED the manager re-mints it at each statement.
    const ReadView& view() const noexcept { return view_; }

    const std::vector<TrailEntry>& trail() const noexcept { return trail_; }

    // The newest undo record this transaction wrote - the head of its
    // `txn_prev_undo_ptr` chain (RV10, `undo_page.hpp`). `kNoUndoPtr` until
    // it writes one.
    //
    // **This is what a checkpoint persists** so recovery can walk the chain
    // after a crash. The trail above is the live rollback path's and does
    // not survive one; this pointer is the durable equivalent, and the two
    // are kept in step by `TransactionManager::AppendUndo` being the only
    // thing that advances either.
    std::uint64_t last_undo_ptr() const noexcept { return last_undo_ptr_; }

private:
    friend class TransactionManager;

    std::uint64_t id_ = 0;
    IsolationLevel isolation_ = IsolationLevel::kReadCommitted;
    ReadView view_;
    std::vector<TrailEntry> trail_;
    std::uint64_t last_undo_ptr_ = kNoUndoPtr;
    bool active_ = false;
};

// Implements `wal::ActiveTransactions`, which is how a checkpoint asks for
// the live set. That interface's own comment says "implemented by the
// transaction manager when it exists" - this is it. Before this, the
// expeditor passed `NoActiveTransactions`, which was a *correct* answer
// only while no transaction could be live; with real transactions an empty
// CHECKPOINT_BEGIN table would tell recovery's analysis phase that nothing
// was in flight when something was, and every uncommitted row of those
// transactions would be treated as a winner.
class TransactionManager final : public wal::ActiveTransactions {
public:
    // `wal` may be null - the unlogged path the socket-free tests run on.
    TransactionManager(TrxIdSequence& ids, UndoLog& undo, storage::PageStore& store,
                       wal::WalManager* wal = nullptr) noexcept
        : ids_(ids), undo_(undo), store_(store), wal_(wal) {}

    // Starts a transaction and takes its first read view. Fails with
    // OutOfSpace past kMaxTrackedLiveTxns live transactions - the bound
    // ReadView documents, surfaced rather than silently dropping an id,
    // because a dropped in-flight id makes an uncommitted row visible.
    StatusOr<Transaction*> Begin(IsolationLevel isolation);

    // Re-mints the read view at a statement boundary. A no-op under
    // REPEATABLE READ, which is the entire difference between the two
    // levels (section 1) - stated as one branch in one function so it
    // cannot drift.
    Status StartStatement(Transaction& txn);

    // Ends the transaction. The trail is dropped: a committed write needs
    // no compensation, and the undo records stay for readers whose
    // snapshots predate it.
    //
    // Appends TXN_COMMIT and applies `durability` when a WalManager was
    // given. Nothing here waits on a group commit - the caller owes the
    // client that acknowledgement and owns the wait, exactly as the
    // dispatcher's INSERT path already does.
    StatusOr<wal::Lsn> Commit(Transaction& txn, wal::DurabilityClass durability);

    // ---- Re-locating a row whose address moved --------------------------
    //
    // A trail entry records where a row *was* - `(page_id, slot)` - because
    // that is what rollback needs to reach it, and for most of this engine's
    // life a row's address was stable for life (row_codec.hpp). A btree leaf
    // division breaks that: it moves half a leaf's tuples to another page and
    // renumbers the slots of the ones that stay
    // (`docs/heap-and-tuple.md` §4.1). Only a kExplicit relation can trigger
    // one mid-statement, but when it does, every entry this transaction
    // recorded earlier names a slot that is out of range or holds a
    // different row - and compensating that blindly is not a failed
    // rollback, it is a rollback that corrupts a row it never wrote.
    //
    // So compensation checks identity before it acts, and asks for the row's
    // current address when the check fails. The lookup lives outside this
    // class because finding a row by `(rel_oid, pk)` needs the catalog and
    // the relation's storage form, and `txn` deliberately knows neither -
    // `CommandDispatcher::LocateByPk` is the implementation, installed here.
    //
    // Unset is a valid configuration: without it a moved row is reported
    // rather than compensated wrongly, which is the safe half of the same
    // rule.
    struct RowLocation {
        PageId page_id = kInvalidPageId;
        std::uint16_t slot = 0;
    };
    // Passed to Abort rather than stored on the manager: the implementation
    // belongs to the dispatcher, which is shorter-lived than the manager it
    // borrows, so a stored callback capturing it would outlive its captures.
    using RowLocator = std::function<StatusOr<RowLocation>(std::uint32_t rel_oid,
                                                            std::uint64_t pk)>;

    // Walks the trail **in reverse** emitting each compensation as an
    // ordinary logged page mutation (section 6), then TXN_ABORT with no
    // durability wait - a transaction whose abort record did not survive is
    // a transaction with no commit record, which recovery rolls back
    // anyway. Undo pages are not freed.
    Status Abort(Transaction& txn, const RowLocator& locate_row = {});

    // ---- First-updater-wins (section 5) --------------------------------
    //
    // The verdict for writer `txn` over a tuple whose current header
    // carries `cur`:
    //
    //   kAlwaysVisibleTrxId  proceed - pre-existing or catalog-stamped row
    //   txn itself           proceed - my own earlier write; the new undo
    //                        record links to the old one, so rollback
    //                        unwinds both and lands on the original
    //   visible to my view   proceed - it committed before my read view, so
    //                        I am the first updater since it
    //   otherwise            **conflict** - still in flight, or committed
    //                        after my read view
    //
    // `pk` only shapes the message, which is part of the wire contract
    // (section 5): "ERR TXN_CONFLICT retryable=1 row id=42 was written by
    // transaction 118".
    Status CheckWriteConflict(const Transaction& txn, std::uint64_t cur,
                              std::uint64_t pk) const;

    // ---- The one place an undo record is appended (RV10) ----------------
    //
    // Sets `fields.txn_prev_undo_ptr` from `txn`, sets `fields.pk`, appends
    // through the log, and on success advances the transaction's head.
    //
    // It exists so no call site can forget the chain link. There are three
    // of them - overwrite, delete-mark and, since RV10, insert - and a
    // record written with a zero `txn_prev_undo_ptr` would silently
    // terminate the chain early, orphaning every record the transaction
    // wrote before it. That failure is invisible until a crash, which is
    // exactly the kind this engine centralizes rather than documents.
    //
    // `pk` is passed separately rather than pre-set on `fields` for the
    // same reason: it is required for every type (`undo_page.hpp` - two of
    // the three carry no image to recover it from) and a parameter cannot
    // be left at its default by accident.
    //
    // **Does not stamp the tuple.** Whether the new record joins the
    // *version* chain is the caller's, and for `kInsert` it must not:
    // `txn.md` §3.6's visibility rule is that `undo_ptr == kNoUndoPtr`
    // means "inserted", so an insert's record is reachable through the
    // transaction chain alone.
    StatusOr<std::uint64_t> AppendUndo(Transaction& txn, UndoRecordFields fields,
                                       std::uint64_t pk, std::span<const std::byte> image);

    // Records one write for rollback. Never fails: a trail entry is memory,
    // and a write that could not be recorded is a write that cannot be
    // undone - which is why the caller records *before* it mutates.
    void NoteInsert(Transaction& txn, std::uint32_t rel_oid, PageId page_id, std::uint16_t slot,
                    std::uint64_t pk);
    void NoteOverwrite(Transaction& txn, std::uint32_t rel_oid, PageId page_id,
                       std::uint16_t slot, std::uint64_t pk, std::uint64_t prior_trx_id,
                       std::uint64_t prior_undo_ptr, std::span<const std::byte> image);
    void NoteDeleteMark(Transaction& txn, std::uint32_t rel_oid, PageId page_id,
                        std::uint16_t slot, std::uint64_t pk, std::uint64_t prior_trx_id,
                        std::uint64_t prior_undo_ptr);

    // The snapshot a reader hands the step VM.
    //
    // Qualified deliberately: `wal::ActiveTransactions::Snapshot()` is a
    // member function of this class, so inside it the bare name `Snapshot`
    // resolves to that function rather than to the type. The two meanings
    // are unrelated - one is a live-transaction list for a checkpoint, the
    // other is a read view plus its undo log - and the qualification is
    // what keeps them apart.
    txn::Snapshot SnapshotFor(const Transaction& txn) noexcept {
        txn::Snapshot snap;
        snap.view = txn.view();
        snap.undo = &undo_;
        return snap;
    }

    // A read view for a statement outside any transaction - autocommit's
    // read path, and the one every SELECT takes today.
    StatusOr<ReadView> MintReadView(std::uint64_t own_trx_id);

    // Frees a transaction the caller is done holding. Separate from
    // Commit/Abort on purpose: those end the transaction and leave the
    // object standing, so reading its id afterwards - which the reply to
    // the client does - is defined. Releasing one that is still active is
    // a no-op, because freeing it would dangle the holder's own pointer.
    void Release(Transaction& txn);

    // wal::ActiveTransactions. The ids in flight *right now*, for a
    // checkpoint's CHECKPOINT_BEGIN table (wal.md sections 11, 12).
    std::vector<wal::CheckpointActiveTxn> Snapshot() const override;

    UndoLog& undo() noexcept { return undo_; }

    // Transactions still running. An ended-but-unreleased one is not
    // counted, and is already invisible to every read view.
    std::size_t ActiveCount() const noexcept;

private:
    Status Compensate(const TrailEntry& entry, std::uint64_t trx_id,
                      const RowLocator& locate_row);

    TrxIdSequence& ids_;
    UndoLog& undo_;
    storage::PageStore& store_;
    wal::WalManager* wal_;

    // Live transactions, in id order. A vector because the set is bounded
    // by kMaxTrackedLiveTxns and a scan over 64 entries beats a hash on
    // every read view minted - which under READ COMMITTED is every
    // statement.
    std::vector<std::unique_ptr<Transaction>> live_;
};

// The view a statement outside any transaction reads at: everything
// committed on this manager's core, nothing in flight. The free-function
// sibling of `SnapshotFor(const Transaction&)` above, and it lives here
// for the same reason that one does - two callers mint it (the
// dispatcher's autocommit arm and every cross-core pipeline stage), and
// spelling six lines twice is how the two drift.
//
// A null manager answers the default snapshot: every writer visible, no
// undo log, which is the pre-MVCC engine exactly and what a caller
// without a manager got before.
inline StatusOr<Snapshot> AutocommitSnapshot(TransactionManager* manager) {
    if (manager == nullptr) return Snapshot{};
    auto view = manager->MintReadView(kNoTrxId);
    if (!view.ok()) return view.status();
    Snapshot snap;
    snap.view = view.value();
    snap.undo = &manager->undo();
    return snap;
}

}  // namespace kds::txn
