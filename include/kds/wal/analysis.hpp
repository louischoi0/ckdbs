#pragma once

#include <cstdint>
#include <map>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"

// Recovery's analysis phase (docs/spec/wal.md §12-1,
// docs/workplan-wal-recovery.md RC02): one forward scan of a core's stream
// that answers the two questions redo and undo are about to ask - **which
// pages need replaying, and which transactions were left unfinished.**
//
// It reads the log and nothing else. No page is fetched, no catalog is
// opened, nothing is written: analysis is a pure function of the bytes on
// the device plus the anchor it starts from, which is what lets RC02's
// tests be scripted logs rather than crashed databases.
//
// ---- Where a scan starts, and the check that it is honest ---------------
//
// From the core's superblock anchor (`server::WalAnchorFields`), which
// carries `redo_start_lsn` and, published with it, the `durable_lsn` the
// stream had reached at the time. Analysis takes both, and **requires the
// scan to reach that durable point**. That check is the difference between
// two states a scan cannot otherwise tell apart:
//
//   - a clean shutdown right after a checkpoint, which yields no records
//     and is perfectly normal;
//   - a log that has *lost* the records the anchor depends on, which
//     yields no records and is a database that must not open.
//
// Without the second, recovery's quietest failure mode is a silent empty
// replay onto a database that needed one - `txn.md` §8's partial recovery,
// arrived at by omission. An anchor of all zeroes means no checkpoint was
// ever published, so there is nothing to fall short of and the scan starts
// at the beginning of the stream (superblock.hpp says why zero is safe as
// a sentinel: no record ever has LSN 0).
//
// ---- The three outcomes, and why abort is not a loser -------------------
//
// `docs/spec/txn.md` §6 writes rollback's compensations as **ordinary logged
// page mutations**, and appends `TXN_ABORT` after them. A stream is a
// sequential prefix - if the abort record is durable, every compensation
// before it is too - so a transaction with a durable `TXN_ABORT` has
// already been undone *by records redo will replay*. Undoing it a second
// time from its undo chain would be work, not correctness.
//
// So the split is three ways and not two. A transaction with no terminal
// record at all is the loser: its writes are in the log, its compensations
// are not, and undo must produce them.

namespace kds::wal {

// What analysis concluded about one transaction. Ordered so the widest
// obligation sorts last, which is only a convenience for reporting.
enum class TxnOutcome : std::uint8_t {
    // A durable TXN_COMMIT. Its writes stand; redo replays them.
    kWinner = 0,
    // A durable TXN_ABORT: already compensated in the log (see above).
    // Distinct from kWinner because its *effects* are gone, and from
    // kLoser because undo must not run for it.
    kAborted = 1,
    // No terminal record. Undo owes this transaction a rollback.
    kLoser = 2,
};

const char* TxnOutcomeName(TxnOutcome outcome) noexcept;

// What analysis knows about one transaction: its verdict, and - for a loser
// - where its undo chain starts.
//
// The head is RV10's whole point (`docs/workplan-wal-recovery.md` §4b).
// Enumerating a loser's writes from the records in the replay range is
// **incomplete**: a page written back before a checkpoint has its recLSN
// cleared, so the redo start can advance past a still-uncommitted write and
// the record naming it is never scanned. Walking `txn_prev_undo_ptr`
// backwards from this pointer reaches every record the transaction wrote,
// however far below the scan it lies.
//
// Two sources, and the later always wins because the scan is forward:
// `CHECKPOINT_BEGIN`'s active table seeds it for a transaction that was
// already running, and each `UNDO_WRITE` this transaction appends replaces
// it. `kNoUndoPtr` means the transaction wrote nothing, and undo owes it
// no compensation.
// `txn::kUndoPtrPageIdShift`, duplicated deliberately. `wal/` sits below
// `txn/` and `txn/undo_log.hpp` already includes `wal/manager.hpp`, so
// reaching back for the real constant would be a cycle. This is the one
// place the layering costs a repeated literal; `wal_analysis_test.cpp`
// builds a pointer both ways and compares, so a change to either is caught.
inline constexpr int kAnalysisUndoPtrPageIdShift = 16;

struct TxnState {
    TxnOutcome outcome = TxnOutcome::kLoser;
    std::uint64_t last_undo_ptr = 0;  // txn::kNoUndoPtr, not named here: wal/ sits below txn/
};

struct AnalysisResult {
    // Where the scan began and where the stream actually ended.
    Lsn scan_start_lsn = 0;
    Lsn end_lsn = 0;
    bool stopped_early = false;   // a torn tail, metered (log_scanner.hpp)
    std::uint64_t records = 0;

    // page -> the LSN at which it first became dirty in this scan's range,
    // seeded from CHECKPOINT_BEGIN's table. Redo replays a page from its
    // recLSN; the page_lsn gate does the rest (wal.md §9).
    //
    // A PAGE_HANDOFF removes its page (PW1c-2, spec-page-lsn-cross-stream
    // §9 rule 3): from that LSN the page belongs to another stream and
    // this stream's redo never touches it - sound because the handoff's
    // flush put everything logged before it in the durable image. A page
    // that returns is durably restamped at re-acquisition (§9 rule 6), so
    // it re-enters here through its post-return records with a page_lsn
    // already in this stream's space - no per-page departure memory is
    // needed, and none is kept (the f19ead1 review's C2: a durable fact
    // must not be keyed to one scan window).
    //
    // Ordered containers throughout, not hashed ones: sched.md §8 requires
    // deterministic iteration, and a recovery whose page visit order
    // varies between runs cannot be replayed from a seed.
    std::map<PageId, Lsn> dirty_pages;

    std::map<std::uint64_t, TxnState> transactions;

    // The oldest recLSN in `dirty_pages`, or `end_lsn` when nothing was
    // dirtied - "the earliest point redo must actually touch", which is at
    // or after the scan start rather than at it.
    //
    // Computed by `RedoStartFrom`, shared with the checkpointer, but with
    // the **opposite bound**: the checkpointer floors at its own
    // CHECKPOINT_BEGIN LSN because its recLSNs all predate it, and analysis
    // floors at the durable end because its recLSNs all follow the scan
    // start. Flooring analysis at the scan start would answer the scan
    // start every time. The shared part is §11-3's real rule - skip a
    // recLSN of 0, take the minimum - which is the part that drifts.
    Lsn redo_start_lsn = 0;

    // The largest page id any replayed record names, or kInvalidPageId
    // when the range named none. RV4's input, and the repair reads it
    // through `wal/high_water.hpp`.
    //
    // The hazard: the durable record of which page ids exist is unlogged -
    // the free-map page, **not** the superblock, which holds no allocation
    // state at all (server/superblock.hpp; RV4 misnames it and
    // high_water.hpp corrects it). A crash can revert that map while the
    // log still names pages above it, after which an allocation would hand
    // out a page redo has already written. Recovery raises the store's
    // allocation floor past this before it can serve one.
    PageId max_page_id = kInvalidPageId;

    // Largest transaction id seen, so the id sequence can be advanced past
    // anything the log names (the trx-id half of the same hazard).
    std::uint64_t max_txn_id = 0;

    // Counts, for RC09's report and for tests that assert work was done
    // rather than skipped.
    std::uint64_t winners = 0;
    std::uint64_t aborted = 0;
    std::uint64_t losers = 0;
};

// wal.md §11-3's rule, in one place: the redo start is the checkpoint's own
// LSN, lowered by every *nonzero* recLSN in the dirty table. A recLSN of 0
// means "dirty but described by no record" and is **skipped, never
// min()ed in** - taking it would drag the redo start to zero and replay
// the whole stream.
//
// Shared by the checkpointer, which computes it forward from its own
// snapshot, and by analysis, which recomputes it backward from the table it
// rebuilt. Two copies of this rule is two chances to lose the skip.
Lsn RedoStartFrom(Lsn floor_lsn, const std::map<PageId, Lsn>& dirty_pages) noexcept;
Lsn RedoStartFrom(Lsn floor_lsn, std::span<const CheckpointDirtyPage> dirty_pages) noexcept;

// Where analysis starts, taken from the core's superblock anchor. Passed as
// plain LSNs rather than as `server::WalAnchorFields` on purpose: `wal/`
// sits below `server/`, and the layer that owns the superblock is the one
// that should read it (RC06 does).
struct AnalysisStart {
    // The anchor's `redo_start_lsn`. Zero means no checkpoint was ever
    // published - scan from the beginning of the stream.
    Lsn redo_start_lsn = 0;
    // The anchor's `durable_lsn`: how far the stream had reached when the
    // anchor was published. The scan must reach it. Zero disables the
    // check, which is the same "no anchor yet" case.
    Lsn anchor_durable_lsn = 0;
};

// One forward pass. Fails with Corruption when the stream ends before the
// anchor's durable point (above), and passes through the scanner's own
// failures - a foreign segment header, a device error - unchanged. A torn
// tail is not a failure; it is `stopped_early` on the result.
StatusOr<AnalysisResult> Analyze(LogDevice& device, std::uint32_t core_id,
                                 const AnalysisStart& start);

}  // namespace kds::wal
