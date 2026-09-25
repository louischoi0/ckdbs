#pragma once

#include <cstdint>
#include <functional>

#include "kds/base/latch.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/heap/heap_page.hpp"

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
// ---- One ceiling, a window per core (AT-S10b, AT-R4) -----------------------
//
// Every core carves its own block from the instance's one ceiling: each
// core's sequence is built over the same `SuperBlock`, the same `persist`
// and the same superblock latch, and `Carve()` reads and raises the ceiling
// inside that latch, which makes it the `fetch_add` AT-R4 names - two
// cores carving at once get two disjoint blocks. There is no refill
// protocol, no grant and no refusal: a spent window is another carve. Until
// AT-S10b only core 0 carved, and a peer drew windows from blocks core 0
// carved for it and sent over the ring (the transaction-id lease, PW1),
// because page 0 was core 0's; every core writes it since AT-S5, under the
// superblock latch since AT-S8.
//
// ---- Concurrency ----------------------------------------------------------
//
// A sequence's window (`next_`, `ceiling_`) is its core's own and is read
// and written by that core alone (rules.md section 3). The superblock it
// carves from is declared shared, under the superblock latch - `SetLatch`.

namespace kds::txn {

// Ids never wrap: the header field is 48 bits (invariant 12) and exhaustion
// is OutOfRange, exactly as the row-id sequence reports it.
inline constexpr std::uint64_t kMaxTrxId = heap::kMaxTrxId;

// How many ids one durable write reserves. `[PROPOSED]` - and the number is
// a **floor**, not a default: below ~4096 the durable write stops
// amortizing (see the header comment's measurements). Raising it costs only
// ids burned by a crash, which are free.
inline constexpr std::uint64_t kTrxIdBlockSize = 4096;

// A reserved run of ids: `[first, first + count)`.
struct TrxIdRange {
    std::uint64_t first = 0;
    std::uint64_t count = 0;

    bool empty() const noexcept { return count == 0; }
};

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

    // **The superblock latch** (AT-S8), null where one thread writes page 0.
    // `Carve` raises the ceiling in the one `SuperBlock` every core's
    // checkpoint anchor also mutates and encodes whole
    // (`server::SuperBlockCheckpointAnchor::SetLatch`), so the raise is held
    // under it; `persist` takes it again around its own encode. `latch` must
    // outlive this.
    void SetLatch(Latch* latch) noexcept { superblock_latch_ = latch; }

    // Issues the next id, reserving and persisting a new block when the
    // current one is spent. Fails with OutOfRange past kMaxTrxId - never
    // wrapped, because a wrapped id would make an old row's writer look
    // like a live one.
    StatusOr<std::uint64_t> Next();

    // Reserves `count` ids and makes the raised ceiling durable, **without
    // touching this sequence's own window**. The one place a block leaves
    // the superblock, for whichever core's sequence calls it - the read of
    // the ceiling and its raise are one step under the superblock latch.
    //
    // The range is durable before it is returned. That ordering is a
    // correctness statement rather than a preference:
    // a mount refuses a log that names an id above the superblock's
    // ceiling, so issuing before persisting would let a crash produce
    // exactly that log and refuse the mount of a database that did nothing
    // wrong.
    StatusOr<TrxIdRange> Carve(std::uint64_t count);

    // Ids left in the current window.
    std::uint64_t remaining() const noexcept { return next_ >= ceiling_ ? 0 : ceiling_ - next_; }

    // The next id this sequence would issue, without issuing it. For
    // minting a read view's high-water mark, which must be an *exclusive*
    // bound over ids already handed out.
    std::uint64_t peek() const noexcept { return next_; }

    // The durable ceiling. Everything in [peek(), ceiling()) is reserved
    // and unspent, and a crash burns it.
    std::uint64_t ceiling() const noexcept { return ceiling_; }

    // ---- Burning an unspent window on purpose (AN-R13) -------------------
    //
    // Discards `[peek(), ceiling())` and installs a fresh window, so
    // `peek()` jumps to the current high-water. **A crash and a stopped
    // core already do this** - the file's opening trade is that ids are
    // unique and monotonic and *never gapless*, and that a burned id costs
    // nothing - so this adds a caller, not a property.
    //
    // Why anyone would: `peek()` is published as this core's bound on the
    // instance's commit-order floor
    // (`instructions/v3.0.0/workorder-an-read-view.md` AN-R8), and it rises
    // only when this core issues. A core that stops running transactions
    // freezes it, and the floor - a minimum over cores - freezes with it,
    // so the commit window can never drop an entry again. Burning is how an
    // idle core stops holding the instance. It is a carve like any other,
    // on any core, since AT-S10b.
    Status BurnWindow();

private:
    Status ReserveBlock();
    void InstallWindow(TrxIdRange window) noexcept;

    server::SuperBlock& superblock_;
    std::function<Status()> persist_;
    // `next_` and `ceiling_` keep the offsets they had before PW1, ahead of
    // what it added. Reordering them measured inside `kds_txn_bench`'s own
    // noise floor either way, so this is free rather than proven.
    std::uint64_t next_;
    std::uint64_t ceiling_;
    Latch* superblock_latch_ = nullptr;  // SetLatch; last, off the hot offsets
};

}  // namespace kds::txn
