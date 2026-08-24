#include "kds/wal/analysis.hpp"

#include <algorithm>
#include <string>

#include "kds/wal/log_scanner.hpp"

namespace kds::wal {

const char* TxnOutcomeName(TxnOutcome outcome) noexcept {
    switch (outcome) {
        case TxnOutcome::kWinner: return "winner";
        case TxnOutcome::kAborted: return "aborted";
        case TxnOutcome::kLoser: return "loser";
    }
    return "?";
}

Lsn RedoStartFrom(Lsn floor_lsn, const std::map<PageId, Lsn>& dirty_pages) noexcept {
    Lsn out = floor_lsn;
    for (const auto& [page_id, rec_lsn] : dirty_pages) {
        (void)page_id;
        // Zero means "dirty but described by no record" - skipped, never
        // min()ed in (wal.md §11-3).
        if (rec_lsn != 0) {
            out = std::min(out, rec_lsn);
        }
    }
    return out;
}

Lsn RedoStartFrom(Lsn floor_lsn, std::span<const CheckpointDirtyPage> dirty_pages) noexcept {
    Lsn out = floor_lsn;
    for (const CheckpointDirtyPage& page : dirty_pages) {
        if (page.rec_lsn != 0) {
            out = std::min(out, page.rec_lsn);
        }
    }
    return out;
}

StatusOr<AnalysisResult> Analyze(LogDevice& device, std::uint32_t core_id,
                                 const AnalysisStart& start) {
    AnalysisResult out;
    out.scan_start_lsn = start.redo_start_lsn;

    // Seen as a loser until a terminal record says otherwise. Never
    // downgrades an outcome: a transaction that committed does not become
    // a loser because a later record still names it.
    const auto note_txn = [&out](std::uint64_t txn_id, TxnOutcome outcome) {
        if (txn_id == kNoTxnId) {
            return;
        }
        out.max_txn_id = std::max(out.max_txn_id, txn_id);
        auto [it, inserted] = out.transactions.emplace(txn_id, TxnState{outcome, 0});
        if (!inserted && outcome != TxnOutcome::kLoser) {
            it->second.outcome = outcome;
        }
    };

    // RV10's chain head. Always overwritten rather than kept, because the
    // scan is forward and the newest record this transaction wrote is the
    // one undo must start from - the checkpoint's value is a seed for
    // whatever it wrote *before* the scan, not a better answer than a
    // record inside it.
    const auto note_undo_head = [&out](std::uint64_t txn_id, std::uint64_t ptr) {
        if (txn_id == kNoTxnId || ptr == 0) {
            return;
        }
        out.transactions[txn_id].last_undo_ptr = ptr;
    };

    const auto visit = [&](const DecodedRecord& record) -> Status {
        // Every record whose envelope names a transaction implies that
        // transaction existed, even if its TXN_BEGIN is below the scan
        // start or its id came from a checkpoint's active list.
        note_txn(record.header.txn_id, TxnOutcome::kLoser);

        // Non-transactional by contract (a handoff is an ownership event,
        // log_page_handoff.hpp) - a nonzero txn_id has already minted a
        // phantom loser on the line above, so it is refused as the
        // corruption it is, never interpreted. Keyed on the *type* alone
        // and placed here rather than inside the page branch below: the
        // hazard is the transaction id, not the page, so a handoff naming
        // no page - itself malformed, since the page is the record's whole
        // subject - would otherwise carry the phantom through unrefused.
        if (record.type() == RecordType::kPageHandoff && record.header.txn_id != kNoTxnId) {
            return Status::Corruption(
                "analysis: PAGE_HANDOFF at lsn " + std::to_string(record.header.lsn) +
                " carries txn " + std::to_string(record.header.txn_id) +
                "; a handoff is an ownership event and belongs to no transaction");
        }

        // A page mutation dirties its page as of *this* LSN, unless the
        // page is already known dirty from earlier - the recLSN is the
        // oldest record that must be replayed, so the first one wins.
        //
        // PAGE_HANDOFF is the *removal* (PW1c-2, PL §9 rule 3): the page
        // left this stream at this LSN, and everything this stream logged
        // for it before is already in the durable image (rule 1a's flush),
        // so this stream's redo has nothing left to contribute - a
        // checkpoint-seeded entry included. A page that later comes back
        // re-enters through its re-acquirer's ordinary records, so erase
        // followed by first-wins emplace gives the post-return recLSN.
        //
        // The erase is positional: a later CHECKPOINT_BEGIN whose dirty
        // table still lists the page re-seeds it at a pre-handoff recLSN.
        // Sound only because the pool's dirty table keys off the frame's
        // dirty bit - so PW1c-4's rule-1a flush must clear the pool's
        // dirty entry, not merely write the bytes (the d3a8b08 review's
        // stated precondition on that unbuilt task).
        //
        // max_page_id takes the handoff's page too, deliberately: the
        // durable record of which pages exist is the unlogged free map,
        // so "the page existed here" is exactly what may not survive the
        // crash - if this stream's ordinary records for it fell below the
        // scan start and the incoming core never wrote it, this record is
        // the one durable proof the id is in use, and the high-water must
        // rise past it (RV4's hazard, reopened for exactly the handed-off
        // page; raising it is monotone and free).
        if (record.header.page_id != kInvalidPageId) {
            if (record.type() == RecordType::kPageHandoff) {
                out.dirty_pages.erase(record.header.page_id);
            } else {
                out.dirty_pages.emplace(record.header.page_id, record.header.lsn);
            }
            out.max_page_id = (out.max_page_id == kInvalidPageId)
                                  ? record.header.page_id
                                  : std::max(out.max_page_id, record.header.page_id);
        }

        // An undo record this transaction just wrote becomes the head of
        // its chain. The pointer is (page_id, offset) packed the way
        // `txn::EncodeUndoPtr` packs it - reproduced here rather than
        // called, because `wal/` sits below `txn/` and this is the one
        // place the layering costs a duplicated shift. The shape is pinned
        // by a static_assert-style test in wal_analysis_test.cpp.
        if (record.type() == RecordType::kUndoWrite) {
            auto decoded = DecodeUndoWrite(record.payload);
            if (!decoded.ok()) {
                return decoded.status();
            }
            note_undo_head(record.header.txn_id,
                           (static_cast<std::uint64_t>(record.header.page_id)
                            << kAnalysisUndoPtrPageIdShift) |
                               decoded.value().fields.offset);
        }

        switch (record.type()) {
            case RecordType::kTxnCommit:
                note_txn(record.header.txn_id, TxnOutcome::kWinner);
                break;
            case RecordType::kTxnAbort:
                // Already compensated by records this scan has passed, and
                // redo will replay them. Not a loser (analysis.hpp).
                note_txn(record.header.txn_id, TxnOutcome::kAborted);
                break;
            case RecordType::kCheckpointBegin: {
                // The seed: both tables as they stood when the checkpoint
                // began. This is why the record is written at all, and why
                // analysis takes it from the scan rather than from a
                // separate read - the scan starts at or before it.
                auto decoded = DecodeCheckpointBegin(record.payload);
                if (!decoded.ok()) {
                    return decoded.status();
                }
                for (const CheckpointActiveTxn& t : decoded.value().active_txns) {
                    note_txn(t.txn_id, TxnOutcome::kLoser);
                    note_undo_head(t.txn_id, t.last_undo_ptr);
                }
                for (const CheckpointDirtyPage& page : decoded.value().dirty_pages) {
                    out.dirty_pages.emplace(page.page_id, page.rec_lsn);
                    out.max_page_id = (out.max_page_id == kInvalidPageId)
                                          ? page.page_id
                                          : std::max(out.max_page_id, page.page_id);
                }
                break;
            }
            default:
                break;
        }
        return Status::OK();
    };

    auto scan = ScanLog(device, core_id, start.redo_start_lsn, visit);
    if (!scan.ok()) {
        return scan.status();
    }

    out.end_lsn = scan.value().end_lsn;
    out.stopped_early = scan.value().stopped_early;
    out.records = scan.value().records;

    // The honesty check (analysis.hpp): the anchor was published with the
    // stream's durable end, so a scan that ends before it has lost records
    // the anchor depends on. Refuse the mount rather than replay a prefix.
    if (start.anchor_durable_lsn != 0 && out.end_lsn < start.anchor_durable_lsn) {
        return Status::Corruption(
            "wal analysis: core " + std::to_string(core_id) + "'s stream ends at lsn " +
            std::to_string(out.end_lsn) + ", before the durable point " +
            std::to_string(start.anchor_durable_lsn) +
            " its checkpoint anchor was published with - records the anchor depends on are "
            "missing");
    }

    // The floor is the durable end, which is an *upper* bound rather than
    // a lower one - and that is the difference between this caller and the
    // checkpointer.
    //
    // The checkpointer computes the redo start forward, from a table whose
    // recLSNs all predate the checkpoint it is writing, so flooring at the
    // checkpoint's own LSN is what picks the oldest of them. Analysis
    // recomputes it backward, from a table whose recLSNs are all at or
    // after the point the scan began - so flooring at the scan start would
    // return the scan start every time, and the number would say nothing.
    //
    // Flooring at the end instead makes the answer "the oldest page any
    // record in this range must be replayed from", and "nothing to redo"
    // exactly when no page was dirtied. Same shared rule about skipping a
    // recLSN of 0; different bound, which is what the parameter is for.
    out.redo_start_lsn = RedoStartFrom(out.end_lsn, out.dirty_pages);

    for (const auto& [txn_id, state] : out.transactions) {
        (void)txn_id;
        switch (state.outcome) {
            case TxnOutcome::kWinner: ++out.winners; break;
            case TxnOutcome::kAborted: ++out.aborted; break;
            case TxnOutcome::kLoser: ++out.losers; break;
        }
    }
    return out;
}

}  // namespace kds::wal
