#include "kds/txn/visibility.hpp"

#include <string>

#include "kds/base/cycle_guard.hpp"

namespace kds::txn {

StatusOr<Visibility> ResolveThroughUndo(const ReadView& view, UndoLog& undo,
                                         std::uint64_t trx_id, bool deleted,
                                         std::uint64_t undo_ptr,
                                         std::vector<std::byte>& payload) {
    // The loop is txn.md section 4.3 step 4, and it re-enters at step 2
    // every time - which is why Classify() is called on the *stepped-back*
    // state rather than the predicate being reimplemented here. Bounded by a
    // cycle check, not a length (the audit's G21; `UndoLog::Walk` says why
    // the check is sound here).
    CycleGuard<std::uint64_t> guard(undo_ptr);
    while (undo_ptr != kNoUndoPtr) {
        auto version = undo.Read(undo_ptr);
        if (!version.ok()) return version.status();

        switch (version.value().type) {
            case UndoRecordType::kOverwrite:
                // The record carries the bytes as they were before this
                // version overwrote them.
                payload.assign(version.value().image.begin(), version.value().image.end());
                deleted = false;
                break;
            case UndoRecordType::kDeleteMark:
                // **Keep the current payload.** A delete-mark changes no
                // tuple bytes, and if a later overwrite changed them, the
                // newer undo record already restored them on the way down.
                deleted = false;
                break;
            case UndoRecordType::kInsert:
                // The version did not exist before this record. Defined
                // and never written today (section 3.6), handled here so
                // that persisting the insert trail later needs no change
                // to the reader.
                return Visibility::kNoVersion;
            case UndoRecordType::kInvalid:
                return Status::Corruption("undo record at " + std::to_string(undo_ptr) +
                                          " has the invalid type");
        }

        trx_id = version.value().prior_trx_id;
        undo_ptr = version.value().prior_undo_ptr;
        const Visibility verdict = Classify(view, trx_id, deleted, undo_ptr);
        if (verdict != Visibility::kNeedsUndoWalk) return verdict;

        // **Only a link the walk follows is asked of the guard** (EP): a
        // link whose writer the view sees may be stale, even self-naming
        // (`undo_log.hpp`'s purge note).
        if (guard.Revisits(undo_ptr)) {
            return Status::Corruption("undo chain revisits the record at " +
                                      std::to_string(undo_ptr) + "; the links are cyclic");
        }
    }

    // Unreachable through Classify(), which answers kNoVersion for a
    // kNoUndoPtr chain end. Stated rather than assumed: reaching here means
    // the loop's exit condition and the predicate disagree.
    return Visibility::kNoVersion;
}

}  // namespace kds::txn
