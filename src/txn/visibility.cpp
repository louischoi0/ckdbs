#include "kds/txn/visibility.hpp"

#include <string>

namespace kds::txn {

StatusOr<Visibility> ResolveThroughUndo(const ReadView& view, UndoLog& undo,
                                         std::uint64_t trx_id, bool deleted,
                                         std::uint64_t undo_ptr,
                                         std::vector<std::byte>& payload) {
    // The loop is txn.md section 4.3 step 4, and it re-enters at step 2
    // every time - which is why Classify() is called on the *stepped-back*
    // state rather than the predicate being reimplemented here.
    std::uint32_t steps = 0;
    while (undo_ptr != kNoUndoPtr) {
        if (++steps > kMaxUndoChainLength) {
            return Status::Corruption("undo chain exceeds " +
                                      std::to_string(kMaxUndoChainLength) +
                                      " versions; treating it as a cycle");
        }

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
    }

    // Unreachable through Classify(), which answers kNoVersion for a
    // kNoUndoPtr chain end. Stated rather than assumed: reaching here means
    // the loop's exit condition and the predicate disagree.
    return Visibility::kNoVersion;
}

}  // namespace kds::txn
