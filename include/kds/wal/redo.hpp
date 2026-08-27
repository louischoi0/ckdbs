#pragma once

#include <cstdint>
#include <map>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/wal/analysis.hpp"
#include "kds/wal/log_device.hpp"

// Recovery's redo phase (docs/spec/wal.md §12-2,
// docs/workplan-wal-recovery.md RC03): replay the log forward from the
// redo start so the pages hold exactly what they held at the crash -
// **uncommitted changes and undo pages included**, because that state is
// what the undo phase then rolls back from.
//
// ---- The one rule that makes this safe ----------------------------------
//
// RV5: a record is applied iff `record.lsn > page_lsn` of the page it
// names, and applying it stamps `page_lsn = record.lsn`. That is the whole
// of idempotence - no second mechanism, no per-record sequence numbers -
// and it is what lets recovery be interrupted and re-run at any point,
// including by a crash during recovery itself.
//
// Every applier is additionally written to be a no-op on re-application at
// the page level, so the property holds even where the gate would not save
// it (a page whose LSN was lost, a hand-built test). That is deliberate
// belt-and-braces: `page_lsn` is one uint64 in a header that a torn write
// can damage, and the FPI heals the page but the gate is what decides
// whether to reach for it.
//
// ---- Redo places at NAMED positions, never at "wherever" ----------------
//
// Every applier writes at the slot or offset the record names, because
// those positions are **durably referenced elsewhere**: an undo record
// names `(target_page_id, target_slot)`, a tuple's spilled cell names
// `(page_id, slot)` in the var-heap, and a trail entry names
// `(page_id, slot)` advisorily. A replay that re-derived positions by
// appending "wherever the page ends" would reproduce the bytes and break
// every one of those references.
//
// The heap and the var-heap had no such primitive and gained one for this
// (`PageView::RedoWriteTuple`, `varheap::PageWriteAt`); the undo page
// already had `UndoPageWriteAt`, written when the undo format was - that
// path anticipated recovery and the other two did not.
//
// ---- What this phase deliberately does not do ---------------------------
//
// It does not decide visibility, roll anything back, or touch an advisory
// structure. Waystone pages are headerless and are never a replay target
// (`page.md` §1); Cabins declare themselves unobserved after a restart
// (`cabin.md` §9); both are RV7. The assertion records are recognised
// and **deferred, not skipped silently** - rebuilding a Bound Cabin's group
// directory is RC07's, and it is blocked on the checkpoint-genesis
// decision (`assertion.md` §7). They are counted so the deferral is
// visible rather than assumed.

namespace kds::wal {

struct RedoStats {
    std::uint64_t records = 0;        // records the scan handed to redo
    std::uint64_t applied = 0;        // records whose effect was written
    std::uint64_t skipped_by_lsn = 0; // already reflected on the page (RV5)
    // The PW1c-2 not-dirty filter (redo.cpp carries the argument). Zero
    // on any stream with no PAGE_HANDOFF.
    std::uint64_t skipped_not_dirty = 0;
    std::uint64_t page_images = 0;    // FULL_PAGE_IMAGEs restored
    std::uint64_t pages_created = 0;  // pages the log named and the store lacked
    std::uint64_t deferred_assertions = 0;  // RC07's, counted not applied
    std::uint64_t no_page = 0;        // records with no page target (txn, checkpoint)

    // Pages that failed to load with a checksum error and were healed by a
    // later FPI. `page.md` §10's division of labour, counted: the checksum
    // detects, the image heals.
    std::uint64_t pages_healed = 0;
};

// Replays `analysis`'s range onto `store`.
//
// Fails with Corruption when a record cannot be applied to the page it
// names - a slot past a dense directory, a var-heap value changing length,
// an index entry that does not land where the record says. Each of those
// means the log and the store are not describing the same database, which
// is a mount to refuse rather than a prefix to accept (`txn.md` §8).
//
// A page that fails to load with a checksum error is **held** rather than
// failed on: its records are skipped until an FPI restores it, and only if
// the scan ends with one still poisoned is that Corruption. That ordering
// is why the FPI exists.
StatusOr<RedoStats> Redo(LogDevice& device, std::uint32_t core_id, storage::PageStore& store,
                         const AnalysisResult& analysis);

}  // namespace kds::wal
