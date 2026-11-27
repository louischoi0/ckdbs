#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/index_purge.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/lock_table.hpp"

// **The unasked index purge** (DW, `instructions/v3.2.0/workorder-dw-btree-
// purge-driver.md`): what runs `PURGE INDEX`'s own pass over a relation's
// indexes with no statement asking. It adds the choice of what to run and
// when, and nothing else: the judge, the fence, the slices and the record are
// `PurgeIndexLeaves`' (DW-R1), so an entry leaves only if a rebuild would not
// append it.
//
// ---- What it runs on --------------------------------------------------------
//
// **Two counts of `stats::RelationStats`, cumulative**, each a write that
// left the tree a dead entry more:
//
//   - `deletes`: a deleted row's entry, one an index. On a heap it does not
//     count - the slot release purges for a heap's deletes
//     (`heap_release.hpp`) - which the driver reads at the relation's first
//     pass, a relation's kind being fixed for the life of its oid.
//   - `key_updates` (DW-Q7 (b)): the rows an `UPDATE` wrote with a new
//     entry appended to some index - a key or a covered value moved, and the
//     row's old entry is the dead one, on a btree and on a heap alike; no
//     slot is retired by an `UPDATE`, so nothing else purges it. Counted on
//     the write path, at the row, and only where the driver is switched on.
//     An `UPDATE` that moves nothing an index carries is not counted, and
//     nor is one back to a value whose entry is still there: it kills one
//     entry and revives another.
//
// A relation is queued once its writes since it was last queued reach its
// threshold, and a pass is every index of the relation walked once
// (`RelationIndexPurge`). No page is read to choose.
//
// **The threshold follows what a pass costs**: a pass reads every entry of
// every index, so the writes that buy one are at least `kMinWrites` and at
// least one `kLiveShare`th of the entries an index kept at the last pass -
// the rows, near enough. A pass an older view makes fruitless (its rows are
// still owed their entries) has spent the writes that bought it, and the
// next is bought at the same price.
//
// ---- Protocol ---------------------------------------------------------------
//
// One thread: core 0's `system` tick offers, steps and reads the counts, and
// nothing here is latched. A step is one `PurgeIndexLeaves` attempt under the
// relation's `IX`, which **the caller** takes as a read holder, try-only, and
// gives back when the step returns (`CommandDispatcher::StepIndexPurge`), so
// nothing is held between two steps (DW-R2) and a relation `X` asked on any
// core is granted in the gap. The step makes the catalog memo current under
// that `IX` before it reads the relation's indexes, as the heap release's
// does (`heap_release.cpp`): a tick is no statement boundary.
//
// **No statement epoch is held, and none is owed, because the step is core
// 0's and keeps no page id between steps**: a drop of the relation or of an
// index needs the `X` the step's `IX` excludes, the page reclaim that frees a
// dropped tree runs on this same reactor and so never inside a step, and what
// a pass keeps across steps is an index's oid and an entry, sought again from
// the slot's root. A driver moved off core 0 would owe the epoch.
//
// **A heap's slot release and this driver may be on one relation at once**,
// a step of each by turns on the one tick. Both only remove entries, by the
// one judge, each seeking its leaf again at every step; the release's verify
// then reads what either left.
//
// **Switched off, none exists** (DW-R3): the dispatcher builds one only where
// `index_purge_driver` is on, and with none nothing is offered, counted or
// stepped - `key_updates` included.
namespace kds::exec {

struct IndexPurgeDriverCounts {
    std::uint64_t passes = 0;   // relations whose pass ended, however it ended
    std::uint64_t read = 0;     // entries the walked leaves held
    std::uint64_t removed = 0;  // entries taken out
    std::uint64_t skipped = 0;  // rows whose unit was refused
};

class IndexPurgeDriver {
public:
    // 4,096 writes: under it a pass is not worth a walk of the smallest
    // index. One fifth of an index's kept entries: the share of a table
    // PostgreSQL's autovacuum waits for by default. [PROPOSED], both.
    static constexpr std::uint64_t kMinWrites = 4096;
    static constexpr std::uint64_t kLiveShare = 5;

    explicit IndexPurgeDriver(std::uint64_t min_writes = kMinWrites) : min_writes_(min_writes) {}

    // A relation's cumulative deletes and key-moving updates, as the
    // statistics count them now: queued once those since it was last queued
    // reach its threshold.
    void Offer(catalog::Oid oid, std::uint64_t deletes, std::uint64_t key_updates);

    // The relation the next step is on - the running pass's, else the next
    // queued one's, whose pass this begins - or none.
    std::optional<catalog::Oid> Current();

    // One attempt of at most `max_leaves` leaves of `Current()`'s pass, by
    // `holder`, whose `holdings` carry the relation's `IX`. A relation dropped
    // or a catalog chain ends its pass OK, and a refusal ends it too.
    Status Step(storage::PageStore& store, catalog::Catalog& catalog,
                const txn::TransactionManager* txns, wal::WalManager* wal, txn::LockTable& locks,
                std::uint64_t holder, txn::LockHoldings& holdings, std::uint32_t max_leaves);

    // A pass running or queued.
    bool owed() const noexcept { return pass_.has_value() || !queue_.empty(); }
    std::size_t queued() const noexcept { return queue_.size(); }
    const IndexPurgeDriverCounts& counts() const noexcept { return counts_; }

private:
    struct Relation {
        std::uint64_t deletes = 0;      // the counts it was last queued at
        std::uint64_t key_updates = 0;
        std::uint64_t threshold = 0;    // a share of its last whole pass; `Offer` floors it
        bool heap = false;              // read at its first pass
        bool queued = false;
    };
    struct Pass {
        catalog::Oid oid = 0;
        RelationIndexPurge walk;
        std::uint64_t indexes = 1;  // the relation's, as the last step read them
        std::uint64_t read = 0;
        std::uint64_t removed = 0;
    };

    void EndPass();

    std::uint64_t min_writes_;
    std::unordered_map<catalog::Oid, Relation> relations_;
    std::deque<catalog::Oid> queue_;
    std::optional<Pass> pass_;
    IndexPurgeDriverCounts counts_;
};

}  // namespace kds::exec
