#include "kds/txn/recovery_undo.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/txn/varheap_release.hpp"  // a loser's spill is released, not retired
#include "kds/wal/payload.hpp"

namespace kds::txn {
namespace {

// The pk the page currently holds at `slot`, or nullopt when the slot holds
// no readable tuple - out of range, dead, or a payload too short to carry a
// Keystone word. The three are not distinguished here on purpose: the
// caller's next question is always "is this the row I meant", and every one
// of them answers no.
std::optional<std::uint64_t> PkAt(heap::PageView& view, std::uint16_t slot) {
    auto payload = view.PayloadAt(slot, view.slot_count());
    if (!payload.ok()) return std::nullopt;
    auto id = KeystoneIdOfPayload(payload.value());
    if (!id.ok()) return std::nullopt;
    return id.value();
}

}  // namespace

Status RecoveryUndo::Compensate(storage::PageStore& store, std::uint64_t txn_id,
                                const UndoVersion& rec) {
    // ---- The var-heap arm, before the page is read as a heap page --------
    //
    // A loser's spill: the record names a kVarHeap slot, not a row, so
    // there is no pk at the address and the identity check below neither
    // applies nor could run - `heap::PageView` over var-heap bytes reads a
    // slot directory that is not there. Releasing it is the whole
    // compensation.
    //
    // **This is what closes the orphan `rule-fixed-length-tuple.md` §5 used
    // to hand to a sweep**: a crash between VARHEAP_APPEND and the tuple
    // write leaves a value nothing points at, and the append is now a link
    // in the loser's own chain, so undo reaches it like any other write.
    if (rec.type == UndoRecordType::kVarHeapAppend) {
        auto released =
            ReleaseVarHeapSlot(store, wal_, txn_id, rec.target_page_id, rec.target_slot);
        if (!released.ok()) {
            return released.status().WithContext("undo: var-heap page " +
                                                 std::to_string(rec.target_page_id));
        }
        // **The redo that never ran.** This ordering is phase B's own:
        // the UNDO_WRITE is written before the PAGE_INIT/VARHEAP_APPEND
        // that fill the slot, so a log whose readable prefix ends between
        // them leaves this chain naming a slot redo never created. An
        // append that was not redone has nothing to undo, and refusing the
        // mount over it would fail a recovery the WAL is behaving
        // correctly in - which is the same reading kInsert's branch below
        // gives to a slot that reads back nothing. **One rule, two record
        // types** (H1/CH1, 2026-08-29): this arm was fixed first and the
        // reviewer who fixed it named `kInsert` as the same shape; it is,
        // and both now answer a missing page and a missing slot the same
        // way.
        if (released.value() == ReleaseOutcome::kNothingToRelease) {
            ++already_done_;
            return Status::OK();
        }
        ++compensations_;
        return Status::OK();
    }

    auto bytes = store.Get(rec.target_page_id);
    if (!bytes.ok()) {
        // **A page redo never created** (H1). Same reading as the slot
        // below and as the var-heap arm above: the UNDO_WRITE precedes the
        // PAGE_INIT that would make this page exist, so a log whose
        // readable prefix ends between them names a page nothing wrote.
        // An insert that was not redone has nothing to retire, and
        // refusing the mount over it would fail a recovery the WAL is
        // behaving correctly in. `NotFound` alone - any other failure of
        // the store is a real one and is reported.
        if (rec.type == UndoRecordType::kInsert &&
            bytes.status().code() == StatusCode::kNotFound) {
            ++already_done_;
            return Status::OK();
        }
        return bytes.status().WithContext("undo: page " + std::to_string(rec.target_page_id) +
                                          " named by an undo record");
    }
    heap::PageView view(bytes.value().bytes());
    const std::optional<std::uint64_t> here = PkAt(view, rec.target_slot);

    // ---- Nothing to retire? ---------------------------------------------
    //
    // Only kInsert can look like this, and **the one rule covers two
    // causes** - which is the point of stating it as one (H1/CH1, and the
    // rule `kVarHeapAppend` above already holds). Either the previous undo
    // attempt retired the slot, after which no pk is readable there; or
    // redo never created it, because the UNDO_WRITE is written *before*
    // the HEAP_INSERT it can undo and the log's readable prefix ended
    // between the two. The engine cannot tell them apart at this point and
    // does not need to: for a record that says "this transaction inserted
    // here", a slot holding no row is a slot with nothing to retire.
    //
    // **The `slot < slot_count()` clause is gone deliberately.** It made
    // the first cause the only admitted one, so a slot *past* the
    // directory - the second cause's exact signature, since a slot redo
    // never appended is past its end - fell through to the identity check
    // and refused the mount naming a row that was never written. `PkAt`
    // already answers nullopt for out-of-range, dead and too-short alike,
    // and its own comment says why it does not distinguish them: every one
    // of them answers "not the row I meant".
    if (rec.type == UndoRecordType::kInsert && !here.has_value()) {
        ++already_done_;
        return Status::OK();
    }

    // ---- The identity check (§4a) ----------------------------------------
    if (!here.has_value() || here.value() != rec.pk) {
        // Reported, never guessed - the live path's no-locator branch, and
        // recovery has no locator at all (recovery_undo.hpp). Compensating
        // here would write a row this transaction never touched.
        return Status::Corruption(
            "undo: row id " + std::to_string(rec.pk) + " is no longer at page " +
            std::to_string(rec.target_page_id) + " slot " + std::to_string(rec.target_slot) +
            " (found " + (here.has_value() ? std::to_string(here.value()) : std::string("nothing")) +
            "); a leaf division moved it and recovery cannot re-locate a row - "
            "docs/spec/wal.md");
    }

    switch (rec.type) {
        case UndoRecordType::kInsert: {
            if (Status s = view.RetireSlot(rec.target_slot); !s.ok()) return s;
            if (wal_ == nullptr) break;
            std::array<std::byte, wal::kSlotRetirePayloadSize> buf{};
            const wal::SlotRetirePayload fields{rec.target_slot};
            if (auto n = wal::EncodeSlotRetire(buf, fields); !n.ok()) return n.status();
            // **The aborting transaction's id, not kNoTxnId** - the same
            // amendment the live compensation carries: kNoTxnId would hide
            // the rollback from the next analysis phase.
            auto out = wal_->Append(
                wal::RecordSpec{wal::RecordType::kSlotRetire, txn_id, rec.target_page_id}, buf);
            if (!out.ok()) return out.status();
            if (Status s = store.StampPageLsn(rec.target_page_id, out.value()); !s.ok()) return s;
            break;
        }

        case UndoRecordType::kOverwrite: {
            if (Status s = view.OverwriteTuple(rec.target_slot, rec.image, rec.prior_trx_id,
                                               rec.prior_undo_ptr);
                !s.ok()) {
                return s;
            }
            if (wal_ == nullptr) break;
            std::vector<std::byte> buf(wal::kHeapWriteFixedSize + rec.image.size());
            const wal::HeapWritePayload fields{rec.prior_trx_id, rec.prior_undo_ptr,
                                               rec.target_slot,
                                               static_cast<std::uint16_t>(rec.image.size())};
            auto n = wal::EncodeHeapWrite(buf, fields, rec.image);
            if (!n.ok()) return n.status();
            auto out = wal_->Append(
                wal::RecordSpec{wal::RecordType::kHeapOverwrite, txn_id, rec.target_page_id},
                std::span(buf).first(n.value()));
            if (!out.ok()) return out.status();
            if (Status s = store.StampPageLsn(rec.target_page_id, out.value()); !s.ok()) return s;
            break;
        }

        case UndoRecordType::kDeleteMark: {
            if (Status s = view.ClearDeleteMark(rec.target_slot, rec.prior_trx_id,
                                                rec.prior_undo_ptr);
                !s.ok()) {
                return s;
            }
            if (wal_ == nullptr) break;
            std::array<std::byte, wal::kDeleteUnmarkPayloadSize> buf{};
            const wal::HeapDeleteUnmarkPayload fields{rec.prior_trx_id, rec.prior_undo_ptr,
                                                      rec.target_slot};
            if (auto n = wal::EncodeHeapDeleteUnmark(buf, fields); !n.ok()) return n.status();
            auto out = wal_->Append(
                wal::RecordSpec{wal::RecordType::kHeapDeleteUnmark, txn_id, rec.target_page_id},
                buf);
            if (!out.ok()) return out.status();
            if (Status s = store.StampPageLsn(rec.target_page_id, out.value()); !s.ok()) return s;
            break;
        }

        case UndoRecordType::kInvalid:
        default:
            return Status::Corruption("undo: record at page " +
                                      std::to_string(rec.target_page_id) + " has type " +
                                      std::to_string(static_cast<int>(rec.type)));
    }

    ++compensations_;
    return Status::OK();
}

Status RecoveryUndo::RollBackOne(storage::PageStore& store, std::uint64_t txn_id,
                                 std::uint64_t head) {
    std::uint64_t ptr = head;
    std::size_t steps = 0;
    while (ptr != kNoUndoPtr) {
        if (++steps > kMaxUndoTxnChainLength) {
            return Status::Corruption("undo: transaction " + std::to_string(txn_id) +
                                      "'s chain exceeded " +
                                      std::to_string(kMaxUndoTxnChainLength) +
                                      " records; it does not terminate");
        }
        auto rec = undo_.Read(ptr);
        if (!rec.ok()) {
            return rec.status().WithContext("undo: transaction " + std::to_string(txn_id) +
                                            "'s chain at " + std::to_string(ptr));
        }
        if (Status s = Compensate(store, txn_id, rec.value()); !s.ok()) {
            return s;
        }
        // Read before the next step, because the record is copied out and
        // the page it came from may be re-fetched underneath us.
        ptr = rec.value().txn_prev_undo_ptr;
    }

    // **Last, and after every compensation.** A durable TXN_ABORT is what
    // makes the next analysis call this transaction `kAborted` instead of a
    // loser, so writing it early would strand any compensation a crash cut
    // short - the transaction would look finished and never be revisited.
    if (wal_ != nullptr) {
        auto out = wal_->Append(wal::RecordSpec{wal::RecordType::kTxnAbort, txn_id,
                                                kInvalidPageId},
                                {});
        if (!out.ok()) return out.status();
    }
    ++transactions_;
    return Status::OK();
}

Status RecoveryUndo::RollBack(storage::PageStore& store, const wal::AnalysisResult& analysis) {
    // Ordered iteration, because `AnalysisResult::transactions` is a
    // std::map and sched.md §8 wants a recovery whose work order is a
    // function of its input alone. Losers do not interact - each owns its
    // own writes - so the order is a reproducibility property rather than a
    // correctness one, which is exactly why it must not be left to a hash.
    for (const auto& [txn_id, state] : analysis.transactions) {
        if (state.outcome != wal::TxnOutcome::kLoser) {
            continue;
        }
        // A loser that wrote nothing owes nothing. It still gets its
        // TXN_ABORT, so a second recovery stops calling it a loser.
        if (Status s = RollBackOne(store, txn_id, state.last_undo_ptr); !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

}  // namespace kds::txn
