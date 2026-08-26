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
// version its snapshot is entitled to (docs/spec/txn.md section 3).
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
// without a side table - and the purge that now exists (below) confirms
// the analysis: it frees by a per-page bound rather than by an owner,
// because a page's records outlive their writer.
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
// ---- The purge: pages recycle within the log ------------------------------
//
// (docs/inflight/in-progress/workplan-undo-purge.md, D1-D3 ratified 2026-08-19; this section
// replaced "Nothing is ever freed" when the reader-registration
// prerequisite fell.)
//
// **The reachability fact**: a record holds the version its writer
// *replaced*, and a reader walks into undo only while writers are
// invisible - so a record whose writer is below the manager's
// ReadHorizon() is unreachable by every live and future traversal, and
// recovery's undo phase cannot need it either (an active transaction
// bounds the horizon at or below its own id).
//
// The purge unit is a whole page (records are addressed by byte offset;
// nothing smaller can be reclaimed), the bound is a per-page maximum
// writer id kept **in memory** beside the chain - a record does not store
// its own writer, and this run's chain is the only one the log appends
// to, so the side table needs no on-disk home - and the trigger is
// growth: TailFor reclaims settled pages from the old end before it
// allocates, and allocation prefers the recycle list to CreateNew(). A
// freed page's stale `undo_ptr`s are harmless by the fact above; its
// stale `prev_page_id` links are why LivePages() counts the in-memory
// chain and no device walk remains.
//
// **Disarmed by default**: with no horizon source installed the log
// behaves exactly as before - nothing is freed. TransactionManager
// installs its ReadHorizon() on construction, so every manager-owned log
// purges; a log built bare (tests, tools) does not. A crash forgets the
// recycle list and this run's chain alike; the next run starts a fresh
// chain, so the old pages leak exactly as they always have - the
// mount-time reclaim of prior-run pages is UP4, not built.
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
// nested-access rule (docs/spec/parser-v2.md I15 R1) satisfiable on the read
// path - see visibility.hpp, which is the walk's one production caller.
struct UndoVersion {
    UndoRecordType type = UndoRecordType::kInvalid;
    std::uint64_t prior_trx_id = 0;
    std::uint64_t prior_undo_ptr = kNoUndoPtr;
    std::vector<std::byte> image;

    // ---- What the *version* walk does not need, and undo does -----------
    //
    // A reader stepping back through versions has the tuple in hand, so it
    // needs only "what did it look like before" - the three fields above.
    // Recovery's undo phase arrives from the other end: it has a chain of
    // records and no idea which tuple any of them is about, so it needs
    // every one of these.
    PageId target_page_id = kInvalidPageId;
    std::uint16_t target_slot = 0;
    std::uint64_t txn_prev_undo_ptr = kNoUndoPtr;  // the transaction chain (RV10)
    std::uint64_t pk = 0;                          // the identity check (§4a)
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

    // There is no PageCount() either: it walked the on-disk prev chain,
    // which reuse made partly historical, and its honest replacement is
    // LivePages() below. (Its own predecessor, `PageCountFor(trx_id)`,
    // fell to sharing - a page holds records from many transactions and
    // none of them owns it.)

    // There is no Forget(). It existed to drop a finished transaction's tail
    // from a per-transaction table, and there is no such table now - one
    // current page serves everyone, and a transaction ending changes nothing
    // about it.

    // ---- The purge (header note above; docs/inflight/in-progress/workplan-undo-purge.md) -----

    // Arms purge-on-growth: `horizon` answers the manager's ReadHorizon()
    // when called. Null disarms (the default, and the pre-purge behaviour).
    // The source is called only from inside Append(), so a caller whose
    // lambda captures an object need only guarantee that object is alive
    // while appends run - which TransactionManager, the sole appender,
    // does trivially for itself. It uninstalls on destruction anyway,
    // because "the only caller keeps the rule" is the assumption the
    // reader-registration review saw break once already (its B1).
    void SetHorizonSource(std::function<std::uint64_t()> horizon) {
        horizon_ = std::move(horizon);
    }

    // This run's live chain length and lifetime recycle count, for
    // SHOW META. Plateauing live pages under a write-heavy loop is the
    // whole feature; the recycle count is what proves the plateau came
    // from reuse rather than from idleness.
    std::uint32_t LivePages() const noexcept {
        return static_cast<std::uint32_t>(pages_.size());
    }
    std::uint64_t PagesRecycled() const noexcept { return pages_recycled_; }

private:
    StatusOr<PageId> TailFor(std::uint64_t trx_id, std::size_t need);
    // Appends the PAGE_INIT for a new or reclaimed page and returns its
    // LSN - `wal::kNoLsn` on the unlogged path. **Stamping is separate**
    // because FormatUndoPage memsets the frame, `page_lsn` included, so
    // the stamp must follow the format while on a reclaimed page the
    // format must follow the append (TailFor says why).
    StatusOr<wal::Lsn> LogPageInit(std::uint64_t trx_id, PageId page_id);
    Status StampInit(PageId page_id, wal::Lsn lsn);
    Status LogUndoWrite(std::uint64_t trx_id, PageId page_id, std::uint16_t offset,
                        const UndoRecordFields& fields, std::span<const std::byte> image);
    // Moves every settled page - maximum writer below the horizon - from
    // the old end of the chain to the recycle list. Stops at the first
    // page that fails (append order is not id order, so later pages may
    // be settled too; skipping them is conservative and bounds the pass),
    // and never takes the tail, whose free space is in use.
    void ReclaimSettledPages();

    storage::PageStore& store_;
    wal::WalManager* wal_;
    // The log's current page - one integer, where a per-transaction table
    // used to be. kInvalidPageId until the first append, and after a restart:
    // a page from a previous run is still readable through a tuple's
    // undo_ptr, but nothing appends to it, because its free space is not
    // recorded anywhere this log reads at startup.
    PageId tail_ = kInvalidPageId;

    // This run's chain, oldest first, each page with the largest writer id
    // ever appended to it - the purge bound, kept here because a record
    // does not store its own writer and the page header has no 48-bit
    // field free. `pages_.back().page_id == tail_` whenever non-empty.
    struct TrackedPage {
        PageId page_id = kInvalidPageId;
        std::uint64_t max_trx_id = 0;
    };
    std::vector<TrackedPage> pages_;
    std::vector<PageId> recycle_;
    std::function<std::uint64_t()> horizon_;
    std::uint64_t pages_recycled_ = 0;
};

}  // namespace kds::txn
