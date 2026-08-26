#pragma once

#include <cstdint>
#include <functional>

#include "kds/base/status.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/txn/trx_id_lease.hpp"

// The transaction id sequence (docs/spec/txn.md section 4.2, section 10-2).
//
// ---- Bump-ahead, and what it does and does not buy ------------------------
//
// Persisting a ceiling per id would be one durable write per transaction.
// `bench/results-keystone-alloc.md` measured that exact scheme for row ids:
// **2629x** the cost of an in-memory bump, against **1.24x** for a block of
// 4096 - and 43x for a block of 64, which is why the block size is a floor
// established by measurement rather than a preference. So a block is
// reserved and persisted once, ids are handed out of memory, and a crash
// burns the unspent remainder.
//
// What that buys: ids are **unique and monotonic**, never reissued across a
// restart. What it does not buy: gaplessness, which nothing needs, and
// crash-*safety* of the ceiling itself, which is the next paragraph.
//
// ---- The exposure this shares with the row-id allocator -------------------
//
// The superblock is **unlogged**. `Persist` writes the page and syncs it,
// so a clean shutdown and an explicit SYNC are covered - but a crash
// between the ceiling being raised in memory and the page reaching the
// platter loses the raise, and the next boot reissues the block. That is
// the same shape of exposure `docs/rules/keystoneid-k0-findings.md` records for
// `sys.tables.next_id`, and it closes the same way: logged catalog writes
// and recovery, neither of which exists. It is stated here rather than
// discovered later.
//
// It compounds with txn.md section 8's accepted gap rather than being
// separate from it - after a crash, an uncommitted row already reads as
// committed, and a reissued id would make two transactions' rows
// indistinguishable on top of that. Both close with recovery and neither
// closes without it.
//
// ---- Two consumers of one ceiling (PW1) -----------------------------------
//
// Since `docs/inflight/in-progress/workplan-peer-writer.md` PW1 the superblock's ceiling has two
// consumers: core 0's own sequence, and the blocks core 0 carves for peers
// that may not write page 0. `Carve()` below is the single place either one
// takes a block from, which is what keeps them from colliding - the same
// arrangement `Catalog::AllocateRowIdRange()` already has for row ids, where
// the bulk-INSERT path and the grant handler share one carve.
//
// A peer's sequence is given a `TrxIdLease*` instead (`SetLeaseSource`), and
// draws its window from grants rather than from the page. It never carves,
// and `persist` stays installed as the backstop that says so.
//
// ---- Concurrency ----------------------------------------------------------
//
// Core-local, like everything else (rules.md section 3). One sequence per
// core; the cross-core protocol is the lease above, and a *shared* sequence
// is still [OPEN] (txn.md section 9) and still assumed by nothing here.

namespace kds::txn {

// Ids never wrap: the header field is 48 bits (invariant 12) and exhaustion
// is OutOfRange, exactly as the row-id sequence reports it.
inline constexpr std::uint64_t kMaxTrxId = heap::kMaxTrxId;

// How many ids one durable write reserves. `[PROPOSED]` - and the number is
// a **floor**, not a default: below ~4096 the durable write stops
// amortizing (see the header comment's measurements). Raising it costs only
// ids burned by a crash, which are free.
inline constexpr std::uint64_t kTrxIdBlockSize = 4096;

class TrxIdSequence {
public:
    // `persist` makes the superblock's raised ceiling durable. It is a
    // callback rather than a PageStore reference because *what* durable
    // means here belongs to whoever owns the superblock page - bootstrap
    // holds it, the expeditor writes it - and this class must not acquire
    // an opinion about page layout to do arithmetic.
    //
    // A null `persist` means the ceiling is raised in memory only: the
    // unlogged path every socket-free test runs on, and the same shape as
    // CommandDispatcher's optional WalManager. Ids stay unique within the
    // run and may be reissued after a restart, which is exactly what the
    // pre-MVCC dispatcher already did.
    TrxIdSequence(server::SuperBlock& superblock, std::function<Status()> persist = nullptr)
        : superblock_(superblock),
          persist_(std::move(persist)),
          next_(superblock.next_trx_id()),
          ceiling_(superblock.next_trx_id()) {}

    // Issues the next id, reserving and persisting a new block when the
    // current one is spent. Fails with OutOfRange past kMaxTrxId - never
    // wrapped, because a wrapped id would make an old row's writer look
    // like a live one. On a leased sequence a spent window fails with
    // **TxnConflict** instead: the one code `IsRetryable` admits, because
    // the grant that fixes it is already on its way (status.hpp's IsRetryable).
    StatusOr<std::uint64_t> Next();

    // Reserves `count` ids and makes the raised ceiling durable, **without
    // touching this sequence's own window**. The one place a block leaves
    // the superblock: `ReserveBlock()` calls it for this core, and core 0's
    // `kTrxIdLease` handler calls it for a peer's.
    //
    // The range is durable before it is returned. That ordering is a
    // correctness statement rather than a preference:
    // `CoreRuntime::Open` refuses a mount whose peer stream names an id
    // above the superblock's ceiling, so granting before persisting would
    // let a crash produce exactly that log and refuse the mount of a
    // database that did nothing wrong.
    StatusOr<TrxIdRange> Carve(std::uint64_t count);

    // Draws this sequence's windows from `lease` instead of from the
    // superblock. A peer's wiring, and `Catalog::SetRowIdLeases`'s shape
    // for the same reason. `lease` must outlive this sequence; null
    // restores the carving path.
    void SetLeaseSource(TrxIdLease* lease) noexcept { lease_ = lease; }

    // Ids left in the current window. A pending grant is **not** counted:
    // it is not issuable until the window is spent and `ReserveBlock()`
    // takes it.
    std::uint64_t remaining() const noexcept { return next_ >= ceiling_ ? 0 : ceiling_ - next_; }

    // Whether it is time to ask for another block. A leased core must ask
    // **before** the window is spent - `Next()` is called from inside a
    // statement and cannot await a grant, which is
    // `storage/extent_lease.hpp`'s rule and its quarter-window threshold. A
    // sequence holding nothing at all reads as low, so a peer's first tick
    // asks.
    //
    // **A grant already in hand counts, even though `remaining()` cannot
    // see it**, and this is the one point where the lease may not simply
    // copy `LeasedIdSource`. That one installs the extent when the grant
    // arrives, so its low-water mark falls with the grant; this one parks
    // the block until the window is spent. Asking on the window alone would
    // therefore stay true across the whole refill and `MaybeRefillTrxIds()`
    // would ask again on every tick - a superblock write and a full `Sync()`
    // per millisecond on core 0, and a block of ids burned with each.
    bool low_water() const noexcept {
        const std::uint64_t held =
            remaining() + (lease_ != nullptr ? lease_->pending_count() : 0);
        return held == 0 || held <= window_ / 4;
    }

    // The next id this sequence would issue, without issuing it. For
    // minting a read view's high-water mark, which must be an *exclusive*
    // bound over ids already handed out.
    std::uint64_t peek() const noexcept { return next_; }

    // The durable ceiling. Everything in [peek(), ceiling()) is reserved
    // and unspent, and a crash burns it.
    std::uint64_t ceiling() const noexcept { return ceiling_; }

private:
    Status ReserveBlock();
    void InstallWindow(TrxIdRange window) noexcept;

    server::SuperBlock& superblock_;
    std::function<Status()> persist_;
    // `next_` and `ceiling_` keep the offsets they had before PW1, ahead of
    // what it added. Reordering them measured inside `kds_txn_bench`'s own
    // noise floor either way, so this is free rather than proven -
    // `docs/inflight/in-progress/workplan-peer-writer.md` carries the numbers and the null
    // control that made them unusable.
    std::uint64_t next_;
    std::uint64_t ceiling_;
    TrxIdLease* lease_ = nullptr;
    // The size of the window `next_`/`ceiling_` came from, so `low_water()`
    // measures against what was actually granted rather than against a
    // constant a smaller grant would sit permanently below.
    std::uint64_t window_ = 0;
};

}  // namespace kds::txn
