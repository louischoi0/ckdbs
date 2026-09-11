#include "kds/txn/manager.hpp"

#include <optional>

#include <algorithm>
#include <bit>
#include <limits>
#include <cctype>
#include <string>

#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/txn/varheap_release.hpp"  // a rolled-back spill is released, not retired
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
        // Out of scope and **not** [OPEN] (section 1). **Both halves of
        // the reason this used to give are now false** (AR0-M1 flagged the
        // second; AO-S3 makes the first stale): readers *are* registered
        // (`txn.md` §4.1), and a lock manager exists
        // (`txn/lock_table.hpp`). What still holds is narrower and is what
        // the refusal now says - the lock family has no **predicate** lock.
        // Its finest fence is a slice, a bounded key interval, which
        // cannot express "every row a `WHERE` matches" over a non-key
        // column, and SSI's alternative needs per-row read tracking that
        // nothing records. AO-S8 owes the same correction to `txn.md` §1's
        // four SR texts.
        return Status::Unsupported(
            "SERIALIZABLE is out of scope: it needs predicate locking or read-tracking, and this "
            "engine's lock family fences key intervals rather than predicates and tracks no "
            "row-level reads");
    }
    return Status::InvalidArgument("unknown isolation level '" + std::string(text) +
                                   "'; expected 'read committed' or 'repeatable read'");
}

ReadView TransactionManager::MintView(std::uint64_t own_trx_id, bool held) noexcept {
    ReadView view;
    // **A held snapshot is announced before it is taken** (AN-8 §8.5 C1,
    // `instance_visibility.hpp`'s "reclamation" note). Between this mint
    // and the publication that follows it - after a WAL append in `Begin`,
    // inside `RegisterReader` for an autocommit statement - a pass on
    // another core would otherwise see no reader on this core, drop an
    // entry above the ceiling this is about to read, and raise the floor
    // past it; the view would then answer that transaction committed at an
    // LSN above its own snapshot. Lowering the slot to a value at or below
    // the ceiling first closes that: a pass that misses this store is
    // ordered before it, and the ceiling read below is ordered after, so
    // whatever that pass dropped the ceiling already covers. The next
    // `PublishCoreBounds` recomputes the slot exactly.
    if (held) visibility_->LowerSnapshotBound(core_, visibility_->SnapshotCeiling());
    // The ceiling every commit at or below which is already in the window
    // (AN-R9): one load, and the whole of what a snapshot is.
    view.snapshot_lsn = visibility_->SnapshotCeiling();
    view.own_trx_id = own_trx_id;
    view.visibility = visibility_;
    // The Cabin's bit (cabin.md section 6a), and the one reason this still
    // walks `live_`: a set may not be banked from a view that could not see
    // a transaction in flight on this core. `Begin` mints before it pushes,
    // so the owner is never its own contemporary here.
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (t->active_ && t->id_ != own_trx_id) {
            view.in_flight_at_mint = true;
            break;
        }
    }
    return view;
}

ReadView TransactionManager::MintReadView(std::uint64_t own_trx_id) noexcept {
    return MintView(own_trx_id, /*held=*/true);
}

ReadView TransactionManager::MintCheckView(std::uint64_t writer_trx_id) noexcept {
    return MintView(writer_trx_id, /*held=*/false);
}

StatusOr<Transaction*> TransactionManager::Begin(IsolationLevel isolation) {
    auto id = ids_.Next();
    if (!id.ok()) return id.status();

    auto txn = std::make_unique<Transaction>();
    txn->id_ = id.value();
    txn->isolation_ = isolation;
    txn->active_ = true;
    txn->view_ = MintReadView(id.value());

    if (wal_ != nullptr) {
        if (auto begun = wal_->Append(wal::RecordSpec{wal::RecordType::kTxnBegin, id.value()});
            !begun.ok()) {
            // The mint lowered this core's slot for a view nothing will
            // hold; recomputed here rather than left stale-low until the
            // next publication, which on an idle core could be a while.
            PublishCoreBounds();
            return begun.status();
        }
    }

    live_.push_back(std::move(txn));
    // After the push: `OldestActiveTrxId()` and `LocalSnapshotBound()` read
    // `live_`, and this transaction is exactly what may have lowered both.
    PublishCoreBounds();
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

    txn.view_ = MintReadView(txn.id_);
    // The view this transaction held is gone, and it may have been the one
    // holding this core's snapshot bound down. A bound left stale-low costs
    // retention, never correctness, but an RC session that never ends would
    // then pin the instance at its first statement's snapshot for its life.
    PublishCoreBounds();
    return Status::OK();
}

Status TransactionManager::AdoptSnapshot(Transaction& txn, std::uint64_t snapshot_lsn) {
    if (!txn.active_) {
        return Status::InvalidArgument("transaction " + std::to_string(txn.id_) +
                                       " is no longer active");
    }
    if (snapshot_lsn > txn.view_.snapshot_lsn) {
        return Status::InvalidArgument(
            "cross-owner transaction: the coordinator's snapshot " +
            std::to_string(snapshot_lsn) + " is above this core's ceiling " +
            std::to_string(txn.view_.snapshot_lsn) +
            ", which a snapshot minted earlier on one commit order cannot be");
    }
    // The slot first, the view second, the publication third. The
    // publication alone would cover a transaction that is in `live_` - it
    // recomputes the slot over every live view, this one's included - and
    // nothing on this core runs between the two lines; the lowering is
    // what covers the snapshot whatever the caller holds, and it costs one
    // store. `in_flight_at_mint` is left as the mint stamped it: moving the
    // snapshot back makes it stale in principle (a transaction live at the
    // adopted instant and since committed is invisible to this view and
    // absent from `live_`), and it costs nothing because the Cabin's
    // banking gate refuses on `own_trx_id` first, which an enrolled context
    // always carries.
    visibility_->LowerSnapshotBound(core_, snapshot_lsn);
    txn.view_.snapshot_lsn = snapshot_lsn;
    PublishCoreBounds();
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

Status TransactionManager::NoteVarHeapAppend(Transaction& txn, std::uint32_t rel_oid,
                                             PageId page_id, std::uint16_t slot,
                                             std::uint64_t pk) {
    // No prior version and no image: an append supersedes nothing, and
    // there is nothing to restore - releasing the slot is the whole
    // compensation. The record exists to be a link in the transaction's
    // chain, which is RV10's argument for kInsert verbatim
    // (undo_page.hpp).
    UndoRecordFields rec{};
    rec.prior_trx_id = kNoTrxId;
    rec.prior_undo_ptr = kNoUndoPtr;
    rec.target_page_id = page_id;  // a kVarHeap page, not a heap one
    rec.target_slot = slot;
    rec.type = static_cast<std::uint8_t>(UndoRecordType::kVarHeapAppend);
    if (auto ptr = AppendUndo(txn, rec, pk, {}); !ptr.ok()) return ptr.status();

    TrailEntry entry;
    entry.action = TrailAction::kVarHeapAppend;
    entry.rel_oid = rel_oid;
    entry.page_id = page_id;
    entry.slot = slot;
    entry.pk = pk;
    txn.trail_.push_back(std::move(entry));
    return Status::OK();
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

void TransactionManager::MoveSchemaWordIfCatalogWriter(const Transaction& txn) noexcept {
    if (txn.wrote_catalog_ && schema_word_ != nullptr) {
        schema_word_->fetch_add(1, std::memory_order_acq_rel);
    }
}

StatusOr<wal::Lsn> TransactionManager::Commit(Transaction& txn,
                                              wal::DurabilityClass durability) {
    if (!txn.active_) {
        return Status::InvalidArgument("transaction " + std::to_string(txn.id_) +
                                       " is no longer active");
    }
    wal::Lsn lsn = wal::kNoLsn;
    // **Before the append** (AN-R9, `instance_visibility.hpp`'s "snapshot
    // ceiling" note): from here until the marker is released, no snapshot
    // minted on any core covers the LSN this commit is about to be
    // assigned. Set before rather than after the append, because the gap
    // between the append returning and a marker being stored is exactly
    // the interval AN-Q3 names, and a marker stored inside it closes
    // nothing. Released on every exit, including a throw: a marker left
    // set caps every mint on the instance for good.
    std::optional<InstanceVisibility::PendingCommit> pending;
    pending.emplace(*visibility_, core_);
    if (wal_ != nullptr) {
        auto committed = wal_->Commit(txn.id_, durability);
        // **The borrows stay held here, and that is the contract, not an
        // oversight** (AO-R6). The append failed, so this transaction is
        // still active and may yet be rolled back; releasing its tenancies
        // now would admit another writer to rows this one can still undo.
        // The caller owes an `Abort`, which releases them -
        // `command_dispatcher.cpp`'s failed-commit path does exactly that
        // and says why. The autocommit write scope does **not**, and
        // already leaks the `Transaction` itself; AO-S3 inherits that as a
        // borrow leak and owns fixing it.
        if (!committed.ok()) return committed.status();
        lsn = committed.value();
    }

    // Dropped, not kept: a committed write needs no compensation, and the
    // undo records stay behind for readers whose snapshots predate it.
    //
    // Nothing is said to the undo log. Its pages are shared by every
    // transaction (undo_log.hpp), so a transaction ending releases nothing
    // and reserves nothing to release - what it does do is stop bounding
    // the floor, which is what lets a later growth recycle its pages.
    txn.trail_.clear();

    // **The window entry goes in before the in-flight set lets go**
    // (AN-R2, AN-R9): there is no instant where the transaction is in
    // neither record, so where a map insert sits cannot move visibility on
    // its own. On the unlogged path `lsn` is `kNoLsn`, which the window
    // reads as "the next position in commit order" - an unlogged instance
    // has no record to take an order from and this is that order's only
    // home. The marker lifts with `pending` at the end of this function,
    // after the entry and the ceiling are both in.
    visibility_->PublishCommit(txn.id_, lsn);
    txn.active_ = false;
    PublishCoreBounds();
    // **The marker lifts before the borrows go** (the AT-S5e review's C8),
    // the order AO-R6 below states and the scope used to invert: a waiter
    // this release wakes on another core can mint a view at once - a
    // `CREATE ASSERTION` granted the relation mints its build's - and with
    // the marker still set that view is capped below this commit, reading
    // the rows it just made visible as not yet committed. The entry and the
    // ceiling are both in by here, which is all the marker was for.
    pending.reset();
    // And the schema word, when this transaction wrote the catalog (AT-S5e):
    // before the release, so a writer it wakes cannot re-run through a
    // boundary that predates the decide.
    MoveSchemaWordIfCatalogWriter(txn);
    // **The borrows go last** (AO-R6). After the window entry and after
    // `active_` falls, so a waiter this wakes re-checks against a
    // transaction it can already see as committed. Releasing first would
    // let the woken writer read a holder that is decided in fact and
    // in-flight to every observer, and refuse itself for a conflict that no
    // longer exists.
    if (locks_ != nullptr) locks_->Release(txn.id_, txn.borrows_);
    return lsn;
}

Status TransactionManager::Compensate(const TrailEntry& entry, std::uint64_t trx_id,
                                      const RowLocator& locate_row) {
    // ---- The var-heap arm, before everything else ------------------------
    //
    // A spilled value is compensated by releasing its slot, and **nothing
    // below applies to it**: the address names a kVarHeap page, so there is
    // no Keystone word to probe, no row that could have moved, and no
    // `heap::PageView` that may legally be built over those bytes. Handled
    // here rather than as a fourth case in the switch, because the switch
    // is already past two things this action must not do.
    //
    // Idempotent, like every compensation: `PageRelease` answers OK on a
    // slot already released, so a crash mid-rollback replays this safely
    // (recovery_undo.hpp - no CLR).
    if (entry.action == TrailAction::kVarHeapAppend) {
        // `trx_id`, not kNoTxnId, on SLOT_RETIRE's argument (txn.md §6): a
        // rollback compensation is owned by the aborting transaction and
        // must be visible to recovery's analysis phase.
        auto released = ReleaseVarHeapSlot(store_, wal_, trx_id, entry.page_id, entry.slot);
        if (!released.ok()) return released.status();
        // **A live rollback cannot legitimately find the slot missing.**
        // This process appended it moments ago, so its absence is a defect
        // here and not, as it is under recovery, a redo that never ran.
        // Reported rather than skipped, for the reason the no-locator
        // branch below reports: a compensation that silently does nothing
        // is indistinguishable from one that worked.
        if (released.value() == ReleaseOutcome::kNothingToRelease) {
            return Status::Corruption("rollback: var-heap slot " + std::to_string(entry.slot) +
                                      " on page " + std::to_string(entry.page_id) +
                                      " is not there to release");
        }
        return Status::OK();
    }

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
    // **No window entry** (AN-R2). A loser is invisible by absence, and its
    // page changes have just been physically undone above - which is the
    // same fact the floor rests on, so nothing is owed here beyond letting
    // the transaction stop holding the floor down.
    PublishCoreBounds();
    // The schema word before the borrows, as at commit: the compensations
    // above put a rolled-back DDL's catalog rows back, and a writer this
    // release wakes must re-run through a boundary that sees them - a
    // `DROP INDEX` undone here restores an index a peer's cache had left
    // out while the drop was open (the AT-S5e review's C1).
    MoveSchemaWordIfCatalogWriter(txn);
    // And the borrows, last and for the same reason as at commit: the
    // compensations above have already put every page back, so a waiter
    // woken here reads the prior version rather than a half-undone one.
    if (locks_ != nullptr) locks_->Release(txn.id_, txn.borrows_);
    // Undo pages are **not** freed; purge is a non-goal (section 9). Nor is
    // this transaction's undo separable from anyone else's - one page holds
    // many transactions' records (undo_log.hpp).
    return first_failure;
}

TransactionManager::BurnOutcome TransactionManager::MaybeBurnIdleBlock() {
    // Idle since the previous tick. Checked before anything that touches
    // the shared structure, so a busy core pays one load of its own
    // sequence and returns.
    const std::uint64_t cursor = ids_.peek();
    const bool idle = cursor == last_burn_cursor_;
    last_burn_cursor_ = cursor;
    if (!idle) return BurnOutcome::kNotNeeded;

    // Not what the floor is waiting on, or the only core attached - in
    // which case the floor tracks this cursor and the window drains on its
    // own (`instance_visibility.hpp`).
    if (!visibility_->PinsFloor(core_)) return BurnOutcome::kNotNeeded;

    // Pinned, but the window has not grown enough to pay for the burn. An
    // instance can sit here indefinitely and lose nothing: the floor is
    // held, and what the floor holds is memory nobody is short of yet.
    if (visibility_->window_size() < kBurnWindowThreshold) return BurnOutcome::kNotNeeded;

    if (!ids_.can_burn()) return BurnOutcome::kNeedsBlock;
    if (Status s = ids_.BurnWindow(); !s.ok()) {
        // The window is untouched on a failure, so this core is exactly
        // where it was and the next tick tries again. Nothing is reported:
        // a burn that did not happen costs memory, never a wrong answer,
        // and a caller that could act on the failure would only retry.
        return BurnOutcome::kNotNeeded;
    }

    PublishCoreBounds();
    // **And spend it, in the same call.** The gate above reads
    // `window_size()`, and the window only ever shrinks inside `Reclaim()` -
    // which runs from `PublishCommit` and from an attach, and from nowhere
    // else. So a burn that only raises the cursor leaves the gate open on
    // exactly the instance it was built for: with the commits stopped
    // nothing ever calls `Reclaim()`, `window_size()` never falls, and every
    // idle core that pins burns again on its next tick - a superblock carve
    // and `Sync()` per tick on core 0 and a lease round trip per two ticks
    // on a peer, for the life of the process, on an instance doing nothing.
    // Reclaiming here is what makes the mechanism terminate: each burn puts
    // one core's cursor above every id ever issued, so after at most one
    // burn per attached core the candidate passes the whole window, the
    // window drains and the gate closes. Same reason the constructor
    // reclaims - a bound moved, so the pass that reads it runs.
    visibility_->Reclaim();
    last_burn_cursor_ = ids_.peek();
    return BurnOutcome::kBurned;
}

void TransactionManager::PublishCoreBounds() noexcept {
    // The horizon's term first. It has no ordering relation to the pair
    // below - a stale-high read of it is covered by AN-R1's ceiling
    // argument, not by store order - so it goes where a new snapshot lowers
    // it soonest.
    visibility_->PublishSnapshotBound(core_, LocalSnapshotBound());
    // **The unresolved bound before the cursor, and the order is the
    // contract, not a preference.** `Begin` moves the two in opposite
    // directions in one step: it lowers this core's oldest unresolved id to
    // the id it has just issued, and raises the cursor past that id. A
    // concurrent `Reclaim()` on another core that read the *raised* cursor
    // beside the *old* unresolved bound would compute a floor above a
    // transaction that is live, and the floor's branch would then answer
    // "committed" for it - H2 reintroduced through the publication order
    // rather than through the predicate.
    //
    // Storing the bound that moves *down* first closes it. Both stores are
    // release and both loads are acquire, so a reader that sees the new
    // cursor sees the new bound with it; and a reader that sees the old
    // cursor is bounded by the old cursor, which is the id just issued
    // (`peek()` is exclusive) and so still at or below every live id.
    // `PublishBounds` is that order made one call.
    visibility_->PublishBounds(core_, OldestActiveTrxId(), ids_.peek());
}

std::uint64_t TransactionManager::LocalSnapshotBound() const noexcept {
    std::uint64_t bound = kUnboundedBound;
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (!t->active_) continue;
        if (t->view_.snapshot_lsn < bound) bound = t->view_.snapshot_lsn;
    }
    for (std::size_t word = 0; word < reader_used_.size(); ++word) {
        std::uint64_t used = reader_used_[word];
        while (used != 0) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(used));
            used &= used - 1;
            const std::uint64_t held = reader_slots_[word * 64 + bit];
            if (held < bound) bound = held;
        }
    }
    return bound;
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

wal::Lsn TransactionManager::OldestPreparedLsn() const {
    // R6-4. The oldest live prepare pins the checkpoint's redo start, so
    // the record that says "this transaction is not mine to decide" stays
    // inside every replay range until it is decided. 0 when nothing here is
    // prepared, which is every core that is not a participant in a
    // cross-owner transaction right now.
    wal::Lsn oldest = 0;
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (!t->active_ || t->prepare_lsn_ == 0) continue;
        if (oldest == 0 || t->prepare_lsn_ < oldest) oldest = t->prepare_lsn_;
    }
    return oldest;
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

bool TransactionManager::IsInDoubt(std::uint64_t trx_id) const noexcept {
    for (const std::unique_ptr<Transaction>& t : live_) {
        if (t->id_ == trx_id) return t->active_ && t->prepared_;
    }
    return false;
}

StatusOr<ReaderLease> TransactionManager::RegisterReader(const ReadView& view) {
    for (std::size_t word = 0; word < reader_used_.size(); ++word) {
        if (reader_used_[word] == ~std::uint64_t{0}) continue;
        const unsigned bit = static_cast<unsigned>(std::countr_one(reader_used_[word]));
        const std::uint32_t slot = static_cast<std::uint32_t>(word * 64 + bit);
        reader_used_[word] |= std::uint64_t{1} << bit;
        // Stored as-is, zero included: a fresh instance that has published
        // no commit mints `snapshot_lsn == 0`, and a bound of 0 simply
        // holds the horizon below every commit - which is what that view
        // means.
        reader_slots_[slot] = view.snapshot_lsn;
        // Published before the lease is handed back, so the slot is never
        // held by a reader the instance cannot see. AN-R1 says why the gap
        // between the mint and this store is harmless anyway.
        PublishCoreBounds();
        return ReaderLease(this, slot);
    }
    return Status::OutOfSpace("more than " + std::to_string(kMaxRegisteredReaders) +
                              " registered readers on this core");
}

void TransactionManager::UnregisterReader(std::uint32_t slot) noexcept {
    reader_used_[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
    PublishCoreBounds();
}

std::uint64_t TransactionManager::ReadHorizon() const noexcept {
    return visibility_->HorizonLsn();
}

bool TransactionManager::ResolvedForEveryReader(std::uint64_t trx_id) const {
    if (trx_id == kAlwaysVisibleTrxId) return true;
    if (trx_id < visibility_->Floor()) return true;
    const InstanceVisibility::CommitLookup found = visibility_->LookupCommit(trx_id);
    if (trx_id < found.floor) return true;
    if (found.commit_lsn == kNoCommitLsn) return false;
    // **The bound after the lookup, and the order is the whole argument.**
    // What may still need this deleter invisible is a snapshot below its
    // commit: one published (the horizon), one a commit in flight may cap
    // a future mint to (the pending-commit markers), or one minted and not
    // yet published - which lowered its core's slot *before* it read the
    // ceiling (`MintView`). Read after the lookup, the bound is ordered
    // after the commit's publication; a mint that this read does not see
    // stored its slot after this read, and so read the ceiling after the
    // publication too, and covers the commit. Read *before* the lookup the
    // same mint could sit between the two reads and take a ceiling below
    // the commit while this sweep, having seen no reader, retires the
    // mark: a row physically gone from a snapshot that was owed it.
    //
    // This is `ReadView::Visible`'s branch 4 with the bound in place of
    // the snapshot, and it is not written as one because `Visible` reads
    // the snapshot first - which is right for a fixed snapshot and wrong
    // for a bound read live.
    const std::uint64_t bound =
        std::min(visibility_->HorizonLsn(), visibility_->PendingCommitBound());
    return found.commit_lsn <= bound;
}

}  // namespace kds::txn
