#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/undo_page.hpp"
#include "kds/wal/manager.hpp"

// The undo log: where before-images go, and how a reader walks back to the
// version its snapshot is entitled to (docs/txn.md section 3).
//
// ---- One current page, shared by every transaction -----------------------
//
// The log keeps a single current page and appends every transaction's
// records to it until it fills, then chains a new one behind it through
// `prev_page_id`. **A page is not owned by a transaction.**
//
// It was, until 2026-08-05, and the cost of that is what changed it: an
// autocommitted statement *is* a transaction, so a page per transaction is
// a fresh 8 KB page per `UPDATE` holding one ~88-byte record - measured at
// 132 MB of data file for 16,414 updates, and at the instance's whole
// ~510 MB page-id space exhausted after ~65,000 of them, at which point
// every further write failed (bench/results-txn-layer-budget.md §3).
// Sharing puts ~92 records on a page instead of one.
//
// Nothing was relying on the exclusivity. A reader follows `undo_ptr`,
// which names a page and an offset directly; rollback replays the
// transaction's in-memory trail (manager.hpp) and never walks undo pages;
// redo names each record's offset explicitly, so interleaved writers replay
// in LSN order onto the same page correctly. The only thing exclusivity
// would have bought is a purge that could free a transaction's pages
// without a side table - and purge does not exist, cannot exist without
// reader registration (§9), and would in any case need a per-page horizon
// rather than an owner, because a page's records outlive their writer.
//
// So there are **three** chains here and no two of them are the same chain:
//
//   prev_page_id       page -> page, the log's pages in creation order
//   prior_undo_ptr     record -> record, one tuple's versions, for reading
//   txn_prev_undo_ptr  record -> record, one *transaction's* records, for
//                      recovery's undo phase (RV10, added 2026-08-11)
//
// A version chain crosses transactions freely, and so does a page. The
// third exists because neither of the first two answers "what did this
// transaction write", and after a crash that is the only question undo
// asks. Its head per active transaction is durable in `CHECKPOINT_BEGIN`,
// so walking it does not depend on the redo start - which is what the WAL
// alone could not give (docs/workplan-wal-recovery.md §4b).
//
// ---- Allocation ----------------------------------------------------------
//
// `PageStore::CreateNew()` plus `FormatUndoPage()`, which stamps the common
// page header as `kUndo`. **`CreateNewHeaderless()` must never be used**
// (txn.md section 3.1): the page needs the checksum and, more importantly,
// the `page_lsn` the WAL-before-data gate reads, because undo writes are
// themselves WAL-logged.
//
// ---- WAL (txn.md section 3.5) --------------------------------------------
//
// Page creation logs `PAGE_INIT{min_key = 0, page_type = kUndo}`; each
// append logs `UNDO_WRITE` whose envelope `page_id` is the **undo** page,
// not the heap page the record describes. `lower` and `nr_records` are
// derivable by replaying a page's `UNDO_WRITE`s in LSN order, so there is
// no undo-page-header record type and none is needed.
//
// **No `FULL_PAGE_IMAGE` for undo pages.** One is fully reconstructible
// from its `PAGE_INIT` plus its `UNDO_WRITE`s, which makes it FPI-exempt on
// the merits rather than by omission - unlike a heap page, whose link edits
// no record type describes.
//
// ---- Nothing is ever freed ------------------------------------------------
//
// There is no `Free()` and no reuse. Purge is a non-goal (txn.md section 9)
// because readers are deliberately not registered, so nothing can know a
// version is unreachable. A write-heavy relation therefore grows undo
// monotonically - the same trade `heap_chain.hpp` already documents for
// deleted heap space, recorded here rather than discovered later.
//
// ---- Concurrency ----------------------------------------------------------
//
// Core-local, like every other subsystem (rules.md section 3). Holds a
// reference to a store and a manager and no lock of its own; the caller
// holds whatever pin the pages need.

namespace kds::txn {

// The ceiling on a version-chain walk. Exceeding it is Corruption, not a
// hang - the same guard `kMaxChainPages` provides for the heap chain. A
// legitimate chain this long would mean 65,536 uncommitted versions of one
// tuple, which no transaction this engine can express produces.
inline constexpr std::uint32_t kMaxUndoChainLength = 1u << 16;

// One version-chain step, as `Walk` reports it. The image is **copied**,
// not viewed: the next step fetches another page, and a store is free to
// move its frames when it hands one out. That copy is also what keeps the
// nested-access rule (docs/parser-v2.md I15 R1) satisfiable on the read
// path - see visibility.hpp, which is the walk's one production caller.
struct UndoVersion {
    UndoRecordType type = UndoRecordType::kInvalid;
    std::uint64_t prior_trx_id = 0;
    std::uint64_t prior_undo_ptr = kNoUndoPtr;
    std::vector<std::byte> image;
};

class UndoLog {
public:
    // `wal` may be null, which means undo pages are mutated without being
    // logged - the unlogged path every pre-existing socket-free test runs
    // on, matching CommandDispatcher's own optional WalManager.
    UndoLog(storage::PageStore& store, wal::WalManager* wal = nullptr) noexcept
        : store_(store), wal_(wal) {}

    // Appends one before-image on behalf of `trx_id` and returns the
    // `undo_ptr` to stamp into the tuple that now supersedes it. Allocates
    // a page only when the log has none or the current one is full - a
    // transaction is not entitled to a page of its own.
    //
    // `fields.prior_undo_ptr` is the *tuple's* current undo_ptr - the
    // version chain's next link - and `fields.prior_trx_id` its current
    // writer. Both come off the tuple header the caller is about to
    // overwrite, which is the only place they exist.
    //
    // Fails with InvalidArgument for an image the codec refuses, and with
    // whatever the store or the log reports otherwise. **A failure here
    // must abort the write**: a tuple stamped with an undo_ptr whose record
    // was never written is a version chain that dead-ends in garbage.
    StatusOr<std::uint64_t> Append(std::uint64_t trx_id, const UndoRecordFields& fields,
                                    std::span<const std::byte> image);

    // Reads the record `ptr` names, copying its image out. Fails with
    // Corruption for an implausible pointer or a damaged record - undo is
    // authoritative data, so a bad pointer is an error and never a miss
    // (invariant 8 governs Waystone, not this).
    StatusOr<UndoVersion> Read(std::uint64_t ptr);

    // Walks a version chain newest -> oldest from `ptr`, calling `fn` for
    // each version until it returns false, the chain ends at kNoUndoPtr, or
    // kMaxUndoChainLength steps have been taken - the last of which is
    // Corruption.
    //
    // A caller that only needs one step calls Read(); this exists for
    // rollback and for tests that assert a whole chain.
    Status Walk(std::uint64_t ptr, const std::function<bool(const UndoVersion&)>& fn);

    // How many undo pages this log has allocated, walked off the page chain
    // rather than counted in memory, so it reports what is on the device.
    // For tests and inspection; it is O(pages) and not for a hot path.
    //
    // It replaced `PageCountFor(trx_id)`, which sharing makes unanswerable:
    // a page holds records from many transactions and none of them owns it.
    StatusOr<std::uint32_t> PageCount();

    // There is no Forget(). It existed to drop a finished transaction's tail
    // from a per-transaction table, and there is no such table now - one
    // current page serves everyone, and a transaction ending changes nothing
    // about it.

private:
    StatusOr<PageId> TailFor(std::uint64_t trx_id, std::size_t need);
    Status LogPageInit(std::uint64_t trx_id, PageId page_id);
    Status LogUndoWrite(std::uint64_t trx_id, PageId page_id, std::uint16_t offset,
                        const UndoRecordFields& fields, std::span<const std::byte> image);

    storage::PageStore& store_;
    wal::WalManager* wal_;
    // The log's current page - one integer, where a per-transaction table
    // used to be. kInvalidPageId until the first append, and after a restart:
    // a page from a previous run is still readable through a tuple's
    // undo_ptr, but nothing appends to it, because its free space is not
    // recorded anywhere this log reads at startup.
    PageId tail_ = kInvalidPageId;
};

}  // namespace kds::txn
