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
// ---- One chain per transaction -------------------------------------------
//
// A transaction's undo pages form a chain through `prev_page_id`, newest
// first, and every page on it carries that transaction's id in
// `owner_trx_id`. Two transactions never share a page. That is what lets a
// future purge pass free a whole transaction's undo with no side table -
// and purge is the *only* reason the per-transaction chain exists, because
// a reader never follows it: a reader follows `undo_ptr`, which names a
// page and an offset directly.
//
// So there are two chains here and they are not the same chain:
//
//   prev_page_id     page -> page, one transaction's pages, for reclamation
//   prior_undo_ptr   record -> record, one tuple's versions, for reading
//
// A version chain crosses transactions freely; a page chain never does.
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
    // a page when the transaction has none or its tail is full.
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

    // Pages this transaction has allocated, for tests and inspection.
    StatusOr<std::uint32_t> PageCountFor(std::uint64_t trx_id);

    // Forgets a committed or aborted transaction's tail. The pages stay
    // allocated and readable - an older snapshot may still walk into them
    // through a tuple's undo_ptr - this only stops the *next* transaction
    // with the same id from appending to them, which cannot happen anyway
    // since ids are never reissued. It is here so the tail table does not
    // grow without bound over a long-running process.
    void Forget(std::uint64_t trx_id);

private:
    // The tail undo page of each live transaction. Small and core-local:
    // one entry per transaction currently holding undo, dropped at
    // Forget(). A vector rather than a map because the live set is bounded
    // by kMaxTrackedLiveTxns (read_view.hpp) and a linear scan over 64
    // entries beats a hash on every append.
    struct Tail {
        std::uint64_t trx_id;
        PageId page_id;
    };

    StatusOr<PageId> TailFor(std::uint64_t trx_id, std::size_t need);
    Status LogPageInit(std::uint64_t trx_id, PageId page_id);
    Status LogUndoWrite(std::uint64_t trx_id, PageId page_id, std::uint16_t offset,
                        const UndoRecordFields& fields, std::span<const std::byte> image);

    storage::PageStore& store_;
    wal::WalManager* wal_;
    std::vector<Tail> tails_;
};

}  // namespace kds::txn
