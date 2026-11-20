#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/lock_table.hpp"

// An index entry purge (DD, `instructions/v3.2.0/workorder-dd-index-entry-
// purge.md`): the entries of a live secondary index that no reader can
// reach and no writer can need, taken out of the tree in place.
//
// ---- The protocol ---------------------------------------------------------
//
// One attempt walks a bounded run of leaves. Per leaf, three phases, as the
// backfill works (DD-R5, `parser-v2.md` I15's R1):
//
//   1. copy the leaf's entries out under a shared hold, and drop it;
//   2. for each tuple key the copy names, take that row's tuple unit in `U`,
//      try-only (a refusal skips the row), and judge it with no
//      page held: `OwedIndexEntries`, the rebuild's own walk (DD-R1). An
//      entry naming the row that serves no owed entry (`SameOwedEntry`) is
//      dead;
//   3. take the dead entries out (`IndexRemoveEntries`: a write descent per
//      leaf, the leaf held exclusive, a leaf's first entry never removed),
//      each rewritten leaf logged as one `FULL_PAGE_IMAGE` at `kNoTxnId`
//      under the hold that changed it (DD-R6), then give the units back.
//
// **The fence** (DD-Q2 (a), DD-R3): every writer that puts a version of a
// row on a page holds the row's tuple unit in `U` or `X`, or a range or
// relation unit over it, from before its page write to its decide, and a
// rollback compensates before it releases. So while the purge holds a row's
// `U` no version of it is written, and no append of its - including one
// `IndexInsert` absorbs as a byte-identical duplicate (§1.2 F) - can rely
// on an entry being removed. The caller holds the relation's `IX` across
// the attempt (DD-R10).
//
// **The horizon** (DD-R2): a version is owed nothing only once its
// superseding writer is resolved for every reader, and every view an index
// is probed under is registered with the horizon or reads latest state
// (DD-S0's census).

namespace kds::txn {
class TransactionManager;
}
namespace kds::wal {
class WalManager;
}

namespace kds::exec {

// One attempt's run of leaves (DD §2): a walk's slice (`sched.md` §3), so
// an attempt's relation `IX` is held for a bounded time (DD-R10). CLA's
// proposal; DD-S2 measures the longest attempt against the 1 s fault net.
inline constexpr std::uint32_t kPurgeIndexLeavesPerAttempt = 64;

struct IndexPurgeAttempt {
    std::uint64_t removed = 0;   // entries taken out
    std::uint64_t skipped = 0;   // rows whose unit was refused: their entries stay
    // The first entry of the leaf the next attempt starts at, or empty when
    // the walk reached the rightmost leaf. A leaf's first entry is never
    // removed, so the key still names a leaf when the next attempt seeks it.
    std::vector<std::byte> resume;
};

// One attempt over `ix` of relation `access`, from the leaf `from` seeks to
// (empty: the leftmost) across at most `max_leaves` leaves, by transaction
// `txn`, whose `holdings` already carry the relation's `IX`. Each row's
// tuple unit is asked in `U` without waiting, and every unit granted is given
// back before the next leaf. `out` counts what was done **whatever the
// answer**: a refusal part-way keeps every leaf already rewritten, and says
// how many entries went (DD-R9). `wal` null logs nothing (tests).
Status PurgeIndexLeaves(storage::PageStore& store, catalog::Catalog& catalog,
                        const catalog::TableAccess& access, const txn::TransactionManager* txns,
                        wal::WalManager* wal, txn::LockTable& locks, std::uint64_t txn,
                        txn::LockHoldings& holdings, const catalog::TableAccess::IndexRef& ix,
                        std::span<const std::byte> from, std::uint32_t max_leaves,
                        IndexPurgeAttempt& out);

}  // namespace kds::exec
