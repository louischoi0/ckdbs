#include "kds/txn/manager.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <cctype>
#include <string>

#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

namespace kds::txn {

namespace {

std::string Folded(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '-' || c == '_') c = ' ';
    }
    // Collapse runs of spaces, so "repeatable  read" parses like the
    // one-space spelling. A config file is written by hand.
    std::string collapsed;
    bool in_space = false;
    for (char c : out) {
        if (c == ' ') {
            if (!in_space && !collapsed.empty()) collapsed.push_back(' ');
            in_space = true;
            continue;
        }
        in_space = false;
        collapsed.push_back(c);
    }
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
    return collapsed;
}

}  // namespace

const char* IsolationLevelName(IsolationLevel level) noexcept {
    switch (level) {
        case IsolationLevel::kReadCommitted:
            return "read committed";
        case IsolationLevel::kRepeatableRead:
            return "repeatable read";
    }
    return "unknown";
}

StatusOr<IsolationLevel> ParseIsolationLevel(std::string_view text) {
    const std::string folded = Folded(text);
    if (folded == "read committed" || folded == "rc") return IsolationLevel::kReadCommitted;
    if (folded == "repeatable read" || folded == "rr") return IsolationLevel::kRepeatableRead;
    if (folded == "serializable") {
        // Out of scope and **not** [OPEN] (section 1): it needs predicate
        // locking or SSI read-tracking, neither of which fits a design with
        // no lock manager and no reader registration. Named explicitly so
        // the refusal says why rather than "unknown level".
        return Status::Unsupported(
            "SERIALIZABLE is out of scope: it needs predicate locking or read-tracking, and "
            "this engine has neither a lock manager nor row-level read tracking");
    }
    return Status::InvalidArgument("unknown isolation level '" + std::string(text) +
                                   "'; expected 'read committed' or 'repeatable read'");
}

StatusOr<ReadView> TransactionManager::MintReadView(std::uint64_t own_trx_id) {
    ReadView view;
    // Exclusive high-water mark over ids **already handed out**: an id the
    // sequence has not issued cannot have written anything.
    view.up_to_trx_id = ids_.peek();
    view.own_trx_id = own_trx_id;
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (!t->active_) continue;
        if (t->id_ == own_trx_id) continue;  // my own writes are always mine
        if (Status s = view.AddInFlight(t->id_); !s.ok()) return s;
    }
    return view;
}

StatusOr<Transaction*> TransactionManager::Begin(IsolationLevel isolation) {
    if (ActiveCount() >= kMaxTrackedLiveTxns) {
        return Status::OutOfSpace("more than " + std::to_string(kMaxTrackedLiveTxns) +
                                  " live transactions; a read view cannot track them");
    }
    auto id = ids_.Next();
    if (!id.ok()) return id.status();

    auto txn = std::make_unique<Transaction>();
    txn->id_ = id.value();
    txn->isolation_ = isolation;
    txn->active_ = true;

    // Minted *after* the transaction is registered nowhere yet, so it does
    // not appear in its own in-flight set - which it must not, because a
    // transaction always sees its own writes.
    auto view = MintReadView(id.value());
    if (!view.ok()) return view.status();
    txn->view_ = view.value();

    if (wal_ != nullptr) {
        if (auto begun = wal_->Append(wal::RecordSpec{wal::RecordType::kTxnBegin, id.value()});
            !begun.ok()) {
            return begun.status();
        }
    }

    live_.push_back(std::move(txn));
    return live_.back().get();
}

Status TransactionManager::StartStatement(Transaction& txn) {
    if (!txn.active_) {
        return Status::InvalidArgument("transaction " + std::to_string(txn.id_) +
                                       " is no longer active");
    }
    // **The entire difference between the two levels**, in one branch:
    // REPEATABLE READ holds the view it took at BEGIN, READ COMMITTED takes
    // a fresh one so the statement sees everything committed before it
    // began.
    if (txn.isolation_ == IsolationLevel::kRepeatableRead) return Status::OK();

    auto view = MintReadView(txn.id_);
    if (!view.ok()) return view.status();
    txn.view_ = view.value();
    return Status::OK();
}

Status TransactionManager::CheckWriteConflict(const Transaction& txn, std::uint64_t cur,
                                              std::uint64_t pk) const {
    if (cur == kAlwaysVisibleTrxId) return Status::OK();
    if (cur == txn.id_) {
        // My own earlier write. The new undo record links to the old one,
        // so a rollback unwinds both and lands on the original.
        return Status::OK();
    }
    if (txn.view_.Visible(cur)) return Status::OK();

    // Either still in flight, or committed after my read view. Under
    // REPEATABLE READ this is exactly first-updater-wins; under READ
    // COMMITTED the arm can still fire in the narrow window between a
    // statement's snapshot and its write, and KDS aborts retryably rather
    // than re-reading (section 5).
    //
    // The message is part of the wire contract, not a diagnostic.
    return Status::TxnConflict("row id=" + std::to_string(pk) + " was written by transaction " +
                               std::to_string(cur));
}

void TransactionManager::NoteInsert(Transaction& txn, std::uint32_t rel_oid, PageId page_id,
                                    std::uint16_t slot, std::uint64_t pk) {
    TrailEntry entry;
    entry.action = TrailAction::kInsert;
    entry.rel_oid = rel_oid;
    entry.page_id = page_id;
    entry.slot = slot;
    entry.pk = pk;
    txn.trail_.push_back(std::move(entry));
}

void TransactionManager::NoteOverwrite(Transaction& txn, std::uint32_t rel_oid, PageId page_id,
                                       std::uint16_t slot, std::uint64_t pk,
                                       std::uint64_t prior_trx_id, std::uint64_t prior_undo_ptr,
                                       std::span<const std::byte> image) {
    TrailEntry entry;
    entry.action = TrailAction::kOverwrite;
    entry.rel_oid = rel_oid;
    entry.page_id = page_id;
    entry.slot = slot;
    entry.pk = pk;
    entry.prior_trx_id = prior_trx_id;
    entry.prior_undo_ptr = prior_undo_ptr;
    entry.image.assign(image.begin(), image.end());
    txn.trail_.push_back(std::move(entry));
}

void TransactionManager::NoteDeleteMark(Transaction& txn, std::uint32_t rel_oid, PageId page_id,
                                        std::uint16_t slot, std::uint64_t pk,
                                        std::uint64_t prior_trx_id,
                                        std::uint64_t prior_undo_ptr) {
    TrailEntry entry;
    entry.action = TrailAction::kDeleteMark;
    entry.rel_oid = rel_oid;
    entry.page_id = page_id;
    entry.slot = slot;
    entry.pk = pk;
    entry.prior_trx_id = prior_trx_id;
    entry.prior_undo_ptr = prior_undo_ptr;
    txn.trail_.push_back(std::move(entry));
}

StatusOr<wal::Lsn> TransactionManager::Commit(Transaction& txn,
                                              wal::DurabilityClass durability) {
    if (!txn.active_) {
        return Status::InvalidArgument("transaction " + std::to_string(txn.id_) +
                                       " is no longer active");
    }
    wal::Lsn lsn = wal::kNoLsn;
    if (wal_ != nullptr) {
        auto committed = wal_->Commit(txn.id_, durability);
        if (!committed.ok()) return committed.status();
        lsn = committed.value();
    }

    // Dropped, not kept: a committed write needs no compensation, and the
    // undo records stay behind for readers whose snapshots predate it.
    //
    // Nothing is said to the undo log. Its pages are shared by every
    // transaction (undo_log.hpp), so a transaction ending releases nothing
    // and reserves nothing to release - what it does do is stop bounding
    // the horizon, which is what lets a later growth recycle its pages.
    txn.trail_.clear();
    txn.active_ = false;
    return lsn;
}

Status TransactionManager::Compensate(const TrailEntry& entry, std::uint64_t trx_id,
                                      const RowLocator& locate_row) {
    // ---- Where the row is *now* -----------------------------------------
    //
    // The trail recorded an address, and a btree leaf division can have
    // moved the row since (manager.hpp's RowLocator). So the address is
    // checked against the identity it is supposed to reach before a single
    // byte is written: `pk` is the row, `(page_id, slot)` is only where it
    // was last seen.
    //
    // The check is one payload read on a page already in hand, and it is
    // paid on every compensation rather than only on relations that can
    // move rows - a rollback is rare, and a check that runs only where a
    // bug is expected is a check nobody trusts.
    PageId page_id = entry.page_id;
    std::uint16_t slot = entry.slot;
    {
        auto probe = store_.Get(page_id);
        if (!probe.ok()) return probe.status();
        heap::PageView view(probe.value().bytes());
        bool matches = false;
        if (auto payload = view.PayloadAt(slot, view.slot_count()); payload.ok()) {
            if (auto id = KeystoneIdOfPayload(payload.value()); id.ok()) {
                matches = id.value() == entry.pk;
            }
        }
        if (!matches) {
            if (!locate_row) {
                // Reported, never guessed. Compensating here would retire or
                // overwrite whichever row now occupies the slot - a write to
                // a row this transaction never touched, which is worse than
                // an unwound rollback that says so.
                return Status::Corruption(
                    "row id " + std::to_string(entry.pk) + " of relation oid " +
                    std::to_string(entry.rel_oid) + " is no longer at page " +
                    std::to_string(page_id) + " slot " + std::to_string(slot) +
                    ", and no row locator is installed to find it");
            }
            auto found = locate_row(entry.rel_oid, entry.pk);
            if (!found.ok()) return found.status();
            page_id = found.value().page_id;
            slot = found.value().slot;
        }
    }

    auto bytes = store_.Get(page_id);
    if (!bytes.ok()) return bytes.status();
    heap::PageView page(bytes.value().bytes());

    // ---- Catalog pages are compensated, and never logged ----------------
    //
    // A transactional DDL registers its catalog rows here so a rollback can
    // undo them (workplan-ddl-transactional.md DT3a/DT5). The *forward*
    // writes that put those rows on the page are unlogged - catalog writes
    // have no WAL records and the catalog is not recovered (known-gaps.md
    // RV3) - so a compensation record for one would be the only record in
    // the stream naming that page, and recovery would try to apply it to a
    // page image that never saw the write it is undoing.
    //
    // That is not a lost update, it is a **failed mount**: a SLOT_RETIRE or
    // DELETE_UNMARK naming a slot the on-disk page does not have yet is
    // `NotFound` from the applier, and redo reports rather than skips
    // (wal/redo.cpp). Undoing the page and saying nothing is the only
    // reading consistent with the forward write, which said nothing either.
    //
    // Keyed on the page rather than on a flag the caller passes, so a DDL
    // added later inherits it instead of remembering it.
    const bool unlogged_page = page_id < catalog::kCatalogOverflowLimit;

    switch (entry.action) {
        case TrailAction::kInsert: {
            if (Status s = page.RetireSlot(slot); !s.ok()) return s;
            if (wal_ == nullptr || unlogged_page) return Status::OK();
            std::array<std::byte, wal::kSlotRetirePayloadSize> buf{};
            const wal::SlotRetirePayload fields{slot};
            if (auto n = wal::EncodeSlotRetire(buf, fields); !n.ok()) return n.status();
            // **The aborting transaction's id, not kNoTxnId.** payload.hpp
            // says no transaction owns a SLOT_RETIRE; that is true of a
            // purge pass and false of a rollback compensation, and stamping
            // kNoTxnId would hide the rollback from recovery's analysis
            // phase (section 6's amendment).
            auto rec = wal_->Append(
                wal::RecordSpec{wal::RecordType::kSlotRetire, trx_id, page_id}, buf);
            if (!rec.ok()) return rec.status();
            return store_.StampPageLsn(page_id, rec.value());
        }

        case TrailAction::kOverwrite: {
            if (Status s = page.OverwriteTuple(slot, entry.image, entry.prior_trx_id,
                                                entry.prior_undo_ptr);
                !s.ok()) {
                return s;
            }
            if (wal_ == nullptr || unlogged_page) return Status::OK();
            std::vector<std::byte> buf(wal::kHeapWriteFixedSize + entry.image.size());
            const wal::HeapWritePayload fields{entry.prior_trx_id, entry.prior_undo_ptr,
                                               slot,
                                               static_cast<std::uint16_t>(entry.image.size())};
            if (auto n = wal::EncodeHeapWrite(buf, fields, entry.image); !n.ok()) {
                return n.status();
            }
            auto rec = wal_->Append(
                wal::RecordSpec{wal::RecordType::kHeapOverwrite, trx_id, page_id}, buf);
            if (!rec.ok()) return rec.status();
            return store_.StampPageLsn(page_id, rec.value());
        }

        case TrailAction::kDeleteMark: {
            if (Status s = page.ClearDeleteMark(slot, entry.prior_trx_id,
                                                 entry.prior_undo_ptr);
                !s.ok()) {
                return s;
            }
            if (wal_ == nullptr || unlogged_page) return Status::OK();
            // **HEAP_DELETE_UNMARK, not HEAP_DELETE_MARK.** This logged the
            // mark record until 2026-08-11, which redo replays by *setting*
            // a mark - so a crash after this rollback brought the row back
            // deleted, the abort undone by its own compensation. The two
            // directions are two record types (`record.hpp`).
            std::array<std::byte, wal::kDeleteUnmarkPayloadSize> buf{};
            const wal::HeapDeleteUnmarkPayload fields{entry.prior_trx_id, entry.prior_undo_ptr,
                                                      slot};
            if (auto n = wal::EncodeHeapDeleteUnmark(buf, fields); !n.ok()) return n.status();
            auto rec = wal_->Append(
                wal::RecordSpec{wal::RecordType::kHeapDeleteUnmark, trx_id, page_id}, buf);
            if (!rec.ok()) return rec.status();
            return store_.StampPageLsn(page_id, rec.value());
        }
    }
    return Status::Corruption("trail entry with an unknown action");
}

Status TransactionManager::Abort(Transaction& txn, const RowLocator& locate_row) {
    if (!txn.active_) return Status::OK();  // aborting twice is not an error

    // **In reverse**, and as ordinary logged page mutations - the shape
    // wal.md section 12-3 asks for, so recovery-driven rollback later
    // reuses this path verbatim.
    Status first_failure = Status::OK();
    for (std::size_t i = txn.trail_.size(); i > 0; --i) {
        if (Status s = Compensate(txn.trail_[i - 1], txn.id_, locate_row); !s.ok()) {
            // Keep unwinding. A compensation that fails leaves one row
            // wrong; stopping here would leave every earlier row wrong too,
            // and there is nothing to retry into.
            if (first_failure.ok()) first_failure = s;
        }
    }

    if (wal_ != nullptr) {
        // No durability wait: a transaction whose abort record did not
        // survive is a transaction with no commit record, which recovery
        // rolls back anyway. The record exists to save recovery the work,
        // not to make the abort true.
        if (auto aborted = wal_->Abort(txn.id_); !aborted.ok() && first_failure.ok()) {
            first_failure = aborted.status();
        }
    }

    txn.trail_.clear();
    txn.active_ = false;
    // Undo pages are **not** freed; purge is a non-goal (section 9). Nor is
    // this transaction's undo separable from anyone else's - one page holds
    // many transactions' records (undo_log.hpp).
    return first_failure;
}

std::vector<wal::CheckpointActiveTxn> TransactionManager::Snapshot() const {
    // Active only: a transaction that has ended but whose handle the caller
    // has not dropped yet is not in flight, and listing it would make
    // recovery roll back writes that were committed.
    //
    // Each carries the head of its undo chain (RV10). That pointer is the
    // whole reason a checkpoint is sufficient for undo: it lets recovery
    // walk a loser's records backwards from here, however far below the
    // redo start they were written.
    std::vector<wal::CheckpointActiveTxn> out;
    out.reserve(live_.size());
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (t->active_) out.push_back({t->id_, t->last_undo_ptr_});
    }
    return out;
}

StatusOr<std::uint64_t> TransactionManager::AppendUndo(Transaction& txn,
                                                       UndoRecordFields fields, std::uint64_t pk,
                                                       std::span<const std::byte> image) {
    // Both set here and nowhere else - manager.hpp says why.
    fields.txn_prev_undo_ptr = txn.last_undo_ptr_;
    fields.pk = pk;

    auto ptr = undo_.Append(txn.id_, fields, image);
    if (!ptr.ok()) return ptr.status();

    // Advanced only on success. A failed append wrote no record, so leaving
    // the head where it was keeps the chain a chain - the next record links
    // past the gap rather than to a pointer nothing backs.
    txn.last_undo_ptr_ = ptr.value();
    return ptr.value();
}

void TransactionManager::Release(Transaction& txn) {
    // Ending a transaction does **not** free it: the caller holds a
    // pointer, and Commit/Abort deliberately leave the object standing so
    // that reading its id or its level afterwards is defined. This is the
    // separate step that frees it, called when the holder drops the handle.
    //
    // An inactive transaction is already invisible to MintReadView, so a
    // handle held past its end costs memory and never correctness.
    if (txn.active_) return;  // still running; freeing it now would dangle
    live_.erase(std::remove_if(live_.begin(), live_.end(),
                               [&](const std::unique_ptr<Transaction>& t) {
                                   return t.get() == &txn;
                               }),
                live_.end());
}

std::size_t TransactionManager::ActiveCount() const noexcept {
    std::size_t n = 0;
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (t->active_) ++n;
    }
    return n;
}

std::uint64_t TransactionManager::OldestActiveTrxId() const noexcept {
    std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (t->active_ && t->id_ < oldest) oldest = t->id_;
    }
    return oldest;
}

bool TransactionManager::IsInFlight(std::uint64_t trx_id) const noexcept {
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (t->id_ == trx_id) return t->active_;
    }
    return false;
}

StatusOr<ReaderLease> TransactionManager::RegisterReader(const ReadView& view) {
    for (std::size_t word = 0; word < reader_used_.size(); ++word) {
        if (reader_used_[word] == ~std::uint64_t{0}) continue;
        const unsigned bit = static_cast<unsigned>(std::countr_one(reader_used_[word]));
        const std::uint32_t slot = static_cast<std::uint32_t>(word * 64 + bit);
        reader_used_[word] |= std::uint64_t{1} << bit;
        // Stored as-is, zero included: a core whose id sequence has issued
        // nothing mints `up_to_trx_id == 0`, and a bound of 0 simply holds
        // the horizon below every real id - which is what that view means.
        reader_slots_[slot] = view.MinVisibleBound();
        return ReaderLease(this, slot);
    }
    return Status::OutOfSpace("more than " + std::to_string(kMaxRegisteredReaders) +
                              " registered readers on this core");
}

void TransactionManager::UnregisterReader(std::uint32_t slot) noexcept {
    reader_used_[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
}

std::uint64_t TransactionManager::ReadHorizon() const noexcept {
    std::uint64_t horizon = std::numeric_limits<std::uint64_t>::max();
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (!t->active_) continue;
        // One term, not two: the view's bound already folds in the owner's
        // own id - Begin and StartStatement always mint with it - so a
        // separate `t->id_` comparison could never lower this further.
        const std::uint64_t bound = t->view_.MinVisibleBound();
        if (bound < horizon) horizon = bound;
    }
    for (std::size_t word = 0; word < reader_used_.size(); ++word) {
        std::uint64_t used = reader_used_[word];
        while (used != 0) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(used));
            used &= used - 1;
            const std::uint64_t bound = reader_slots_[word * 64 + bit];
            if (bound < horizon) horizon = bound;
        }
    }
    return horizon;
}

}  // namespace kds::txn
