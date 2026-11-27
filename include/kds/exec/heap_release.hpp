#pragma once

#include <cstdint>
#include <set>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/heap_store.hpp"
#include "kds/exec/index_purge.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/lock_table.hpp"

namespace kds::txn {
class TransactionManager;
}
namespace kds::wal {
class WalManager;
}

// **A heap's slot release** (DN-R2, DN-D1; DD-S4's driver, ruled into DN-S2
// by the operator on 2026-11-21): the one thing that frees a deleted or
// rolled-back row's slot, so an insert can take it again. A slot is freed
// only once **no entry of any index of the relation names its rowid** - an
// entry that outlived its slot would name the next row there.
//
// One job is one relation, in four phases, each a bounded step at a time:
//
//   1. **Collect** (data pages): every retired slot not yet free is a
//      candidate. On a hidden-Keystone heap a delete-marked row whose deleter
//      every reader has passed is retired first, its spills released, at
//      `kNoTxnId` - `PURGE`'s act, safe there because no named key at or
//      below the mark is admitted (DN-Q14 (a)), so the key is never bound
//      again. A declared-Keystone heap keeps its tombstones: their Keystone
//      entries are what refuse the key named again, until a `PURGE`.
//      A slot already free is handed to `HeapSpace` as a hint.
//   2. **Purge** (every index, DD's `PurgeIndexLeaves`): the entries no
//      version is owed are removed - a rolled-back or retired row is owed
//      none. An online build's index is not purged (DD-R8).
//   3. **Verify** (every index, read only): each entry's rowid is struck
//      from the candidates. An index's entries move only rightward, never
//      leftward, so a left-to-right walk sees every entry that existed when
//      it began; and no statement writes an entry naming a candidate after
//      the collect, since a retired slot is written by no one. **An online
//      build's backfill does** - it copies a page, then appends, holding
//      nothing between - so an index still building ends the job unfreed,
//      queued again. What survives - a leaf's first entry (DD-Q3 (a)), a
//      row whose unit the purge was refused - keeps its candidate unfreed.
//   4. **Free**: every remaining candidate, still retired and not free under
//      its page's exclusive hold, is freed and logged (SLOT_FREE), a page
//      at a time.
//
// Each step takes the relation's `IX` as a read holder, never waiting, as
// `PURGE INDEX`'s slices do; a step refused it does nothing. Under it the
// step makes the catalog memo current, so it reads the relation's indexes as
// they stand. The job holds no page and no lock between steps, and a
// relation dropped under it ends it. A job that left a slot owed for a
// reason that passes - a deleter in flight or still seen by a reader, an
// index building - queues its relation again when it ends.
namespace kds::exec {

struct HeapReleaseCounts {
    std::uint64_t retired = 0;   // tombstones retired (phase 1)
    std::uint64_t removed = 0;   // index entries removed (phase 2)
    std::uint64_t named = 0;     // candidates an entry still named (phase 3)
    std::uint64_t freed = 0;     // slots freed (phase 4)
};

struct HeapReleaseContext {
    storage::PageStore& store;
    catalog::Catalog& catalog;
    const txn::TransactionManager* txns;
    wal::WalManager* wal;
    txn::LockTable& locks;
    std::uint64_t holder;
    txn::LockHoldings& holdings;  // the relation's `IX`, taken by the caller
    HeapSpace& space;
};

class HeapSlotRelease {
public:
    explicit HeapSlotRelease(catalog::Oid rel_oid) : rel_oid_(rel_oid) {}

    catalog::Oid rel_oid() const noexcept { return rel_oid_; }
    bool done() const noexcept { return phase_ == Phase::kDone; }
    const HeapReleaseCounts& counts() const noexcept { return counts_; }

    // At most `budget` pages or leaves of work. A relation no longer there
    // ends the job OK.
    Status Step(HeapReleaseContext& cx, std::uint32_t budget);

private:
    enum class Phase : std::uint8_t { kCollect, kPurge, kVerify, kFree, kDone };

    Status Collect(HeapReleaseContext& cx, const catalog::TableAccess& access,
                   std::uint32_t budget);
    Status Purge(HeapReleaseContext& cx, const catalog::TableAccess& access,
                 std::uint32_t budget);
    Status Verify(HeapReleaseContext& cx, const catalog::TableAccess& access,
                  std::uint32_t budget);
    Status Free(HeapReleaseContext& cx, const catalog::TableAccess& access,
                std::uint32_t budget);

    catalog::Oid rel_oid_;
    Phase phase_ = Phase::kCollect;
    PageId page_ = kInvalidPageId;  // the collect's next data page
    bool started_ = false;
    std::set<std::uint64_t> candidates_;  // rowids
    RelationIndexPurge purge_;  // phase 2's place
    // The index the verify is on, by oid - an index created or dropped
    // between steps moves no position - and its next leaf. 0: the next index
    // after `done_oid_`.
    std::uint64_t index_oid_ = 0;
    std::uint64_t done_oid_ = 0;
    PageId leaf_ = kInvalidPageId;
    // A slot left owed for a reason that passes: the job queues its
    // relation again when it ends.
    bool deferred_ = false;
    HeapReleaseCounts counts_;
};

}  // namespace kds::exec
