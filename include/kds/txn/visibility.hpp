#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/txn/undo_log.hpp"

// The visibility predicate: which version of a tuple a read view is
// entitled to (docs/spec/txn.md section 4.3). It is the **first consumer of
// `Tuple::deleted`**, which the engine has set and never read.
//
// ---- Two phases, and the reason is structural ----------------------------
//
// docs/spec/parser-v2.md I15 R1 forbids a page-frame span being live across a
// nested page fetch, and stepping back an undo record *is* a page fetch.
// The step VM decodes under exactly such a span (`PageSpanGuard`), so a
// predicate that walked undo where it is called from would break R1 on
// every invisible row.
//
// So the predicate is split where the span is:
//
//   Classify()          no page fetch, safe under a live span. Answers
//                       outright for the common case and otherwise says
//                       "this needs the chain".
//   ResolveThroughUndo() the walk, run after the span is released.
//
// **The copy is only paid when the answer is not already known.** A tuple
// whose writer is visible - which is every row of a single-transaction
// workload, and every catalog row forever - is decided by Classify() with
// no copy and no fetch, so the ordinary read path costs one integer
// comparison. Only an invisible writer pays for the scratch copy, and the
// copy is a fixed number of bytes because invariant 13 makes a row's size a
// schema constant.
//
// ---- Every chain terminates -----------------------------------------------
//
// At an always-visible trx_id == 1 version, at undo_ptr == 0, or at a
// kInsert record. `UndoLog::Walk`'s kMaxUndoChainLength bound is the
// backstop for a damaged chain, not the normal exit.

namespace kds::txn {

enum class Visibility : std::uint8_t {
    // This version is the one the view sees; its payload is the tuple's
    // own bytes, untouched.
    kVisible,
    // No version of this tuple is visible to this view. The row does not
    // exist as far as the statement is concerned.
    kNoVersion,
    // Undecidable without the undo chain. The caller must release its page
    // span, copy the tuple out, and call ResolveThroughUndo().
    kNeedsUndoWalk,
};

// Phase 1 (txn.md section 4.3 step 2 and 3). Pure, no page fetch, safe to
// call with a page span live.
constexpr Visibility Classify(const ReadView& view, std::uint64_t trx_id, bool deleted,
                              std::uint64_t undo_ptr) noexcept {
    if (view.Visible(trx_id)) {
        // The version exists iff it is not delete-marked. A delete-mark by
        // a writer I can see is a row that is gone for me.
        return deleted ? Visibility::kNoVersion : Visibility::kVisible;
    }
    if (undo_ptr == kNoUndoPtr) {
        // An insert by a writer I cannot see. This is the whole reason an
        // INSERT writes no undo record (section 3.6): "invisible writer,
        // no predecessor" already means "no visible version", so the
        // record would carry no information.
        return Visibility::kNoVersion;
    }
    return Visibility::kNeedsUndoWalk;
}

// Convenience over a tuple the caller already read. Same phase-1 contract:
// no fetch, safe under a span.
inline Visibility Classify(const ReadView& view, const heap::PageView::Tuple& tuple) noexcept {
    return Classify(view, tuple.trx_id, tuple.deleted, tuple.undo_ptr);
}

// Phase 2 (txn.md section 4.3 step 4). Called only after Classify()
// answered kNeedsUndoWalk **and the caller's page span has been released**.
//
// `payload` is in/out: on entry it holds a copy of the tuple's own payload
// bytes; on return, if the result is kVisible, it holds the visible
// version's. It is the caller's reusable scratch buffer - a step VM
// keeps one across rows, so a scan that never walks allocates nothing.
//
// `trx_id`, `deleted` and `undo_ptr` are the tuple's, as Classify() saw
// them.
StatusOr<Visibility> ResolveThroughUndo(const ReadView& view, UndoLog& undo,
                                         std::uint64_t trx_id, bool deleted,
                                         std::uint64_t undo_ptr,
                                         std::vector<std::byte>& payload);

// ---- Check visibility (docs/spec/foreign-keys.md §4) ---------------------
//
// What a **constraint check** is entitled to see, which is not what a reader
// is. A foreign key check cannot read at the statement snapshot: a parent
// committed-deleted after that snapshot was taken must still fail the check
// (latest-state semantics, as in commercial engines), and an in-flight
// writer must be *seen* rather than looked through, so the check can fail
// fast instead of blocking (F3).
//
// **It never walks the undo chain, and that is what makes it small.** The
// latest state of a tuple is the version on its page; older versions are by
// definition not the latest. So this is a pure function of the three header
// fields, safe under a live page span, with no phase 2 and no scratch copy.
//
// It is *not* Classify with a different view, and the case that proves it is
// an in-flight **insert** of the parent: `undo_ptr == 0` under an invisible
// writer, which Classify answers kNoVersion - correct for a reader, since no
// version is visible - and which a check must answer kBusy, because the row
// may well be there a moment from now. That single disagreement is why this
// is a sibling function rather than a flag on the other one.
//
// The caller supplies a view minted **at check time**
// (TransactionManager::MintReadView), not the statement's. Under such a view
// "not visible" means "in flight right now", which is exactly the question
// F3 asks. A transaction's own uncommitted writes stay visible through
// `own_trx_id`, so a check inside a transaction judges by that
// transaction's own pending image with no special case here.
enum class CheckVerdict : std::uint8_t {
    // A row is there, at latest state.
    kLive,
    // No row is there: never inserted, or delete-marked by a writer this
    // view can see.
    kAbsent,
    // Another transaction is writing this row and has not resolved. The
    // answer depends on how it ends, so the check refuses now rather than
    // waiting - there is nothing to wait *on* under a cooperative
    // single-writer core.
    kBusy,
};

// `undo_ptr` is deliberately absent from the parameter list: latest state
// never consults it. See the note above.
constexpr CheckVerdict CheckVisibility(const ReadView& view, std::uint64_t trx_id,
                                       bool deleted) noexcept {
    if (!view.Visible(trx_id)) return CheckVerdict::kBusy;
    return deleted ? CheckVerdict::kAbsent : CheckVerdict::kLive;
}

inline CheckVerdict CheckVisibility(const ReadView& view,
                                    const heap::PageView::Tuple& tuple) noexcept {
    return CheckVisibility(view, tuple.trx_id, tuple.deleted);
}

// What a reader carries: the view, and the log it steps back through. One
// struct because the two are useless apart - a view with no log cannot
// resolve an invisible writer, and a log with no view has no question to
// answer.
//
// The default is the pre-MVCC engine exactly: every writer visible, no log
// needed, no copy ever taken. That is what lets the predicate be wired into
// the read path without changing a single existing reply.
struct Snapshot {
    ReadView view = ReadView::Everything();

    // Null is legal only while `view` admits every writer. A view that
    // needs the chain with no log to walk is a caller error, and the
    // reader below reports it rather than guessing.
    UndoLog* undo = nullptr;

    // True when this snapshot can never need the undo chain, so a reader
    // may skip the classification entirely. Not an optimization the reader
    // depends on - Classify() is one comparison - but it makes "nothing
    // changed for a non-transactional caller" checkable in one place.
    bool sees_everything() const noexcept {
        return view.up_to_trx_id == UINT64_MAX && view.in_flight_count == 0;
    }
};

}  // namespace kds::txn
