#pragma once

#include <cstdint>
#include <functional>

#include "kds/base/status.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/heap/heap_page.hpp"

// The transaction id sequence (docs/txn.md section 4.2, section 10-2).
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
// the same shape of exposure `docs/keystoneid-k0-findings.md` records for
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
// ---- Concurrency ----------------------------------------------------------
//
// Core-local, like everything else (rules.md section 3). One sequence per
// instance; a cross-core protocol is [OPEN] (txn.md section 9) and nothing
// here assumes one.

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
    // like a live one.
    StatusOr<std::uint64_t> Next();

    // The next id this sequence would issue, without issuing it. For
    // minting a read view's high-water mark, which must be an *exclusive*
    // bound over ids already handed out.
    std::uint64_t peek() const noexcept { return next_; }

    // The durable ceiling. Everything in [peek(), ceiling()) is reserved
    // and unspent, and a crash burns it.
    std::uint64_t ceiling() const noexcept { return ceiling_; }

private:
    Status ReserveBlock();

    server::SuperBlock& superblock_;
    std::function<Status()> persist_;
    std::uint64_t next_;
    std::uint64_t ceiling_;
};

}  // namespace kds::txn
