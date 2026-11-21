#pragma once

#include <cstdint>
#include <unordered_map>

#include "kds/catalog/oid.hpp"
#include "kds/sched/wait_state.hpp"
#include "kds/stats/oid_parts.hpp"

// **How often a tree's leaves divide, and how often one is compacted in
// place** (`instructions/v3.2.0/workorder-df-statistics-coverage.md` §2 part
// D, DF-Q9 (a)): `sys.relation_stats`' `leaf_divisions` and
// `leaf_compactions`, `sys.indexes`' `leaf_divisions`. Memory-resident
// (DF-R1): since the process started. No switch (DF-R9). Nothing in the
// engine reads a count (DF-R3).
//
// ---- What a count is --------------------------------------------------------
//
// Counted in the storage layer where the act is done, under the leaf's
// exclusive hold, by the oid the tree's pages are owned by - a relation's
// for its clustered tree, an index's for its own:
//
//   divisions    a full leaf divided: half its rows moved to a new leaf
//                (`btree.cpp`'s `SplitLeafAndInsert`, `index_tree.cpp`'s leaf
//                split). **An append is not a division**: a clustered leaf
//                grown by a fresh leaf to its right, nothing moved - what a
//                rising key produces - is not counted. An index leaf has no
//                append shape: every split of one divides it.
//   compactions  a full clustered leaf rebuilt in place over the slots a
//                rollback or a `PURGE` retired, in place of a division
//                (`CompactLeafAndInsert`). An index leaf is never compacted:
//                no index entry is removed.
//
// A clustered division and a compaction are counted once the act has
// returned; an index's once its entries have moved to the new leaf, before
// the entry is placed and the separator promoted. Nothing that left its
// page as it was is counted - a stale descent, a refused parent or claim, a
// split its page refused. A mount's replay of a `BTREE_SPLIT` applies
// images and counts nothing, so the counts are this run's acts alone.
//
// ---- Where the counts live ---------------------------------------------------
//
// The process's, as the BO stores are: the storage layer has no instance to
// ask. `OidParts` (`stats/oid_parts.hpp`), the part chosen by the calling
// thread's core (`CurrentCore()`). **A count is taken under a page's
// exclusive hold and takes nothing itself** (`docs/rules/rules.md` §3a;
// DL-S2): relaxed single-writer words in a latch-free part, so a split
// never waits for a view's reader with its pages held. **The one
// allocation is a part's first division of any tree**, under that hold
// (DF-Q22 (a): DF-R2's first-sight exception, read to reach this store);
// it cannot refuse the split - a failure drops the count.

namespace kds::stats {

struct LeafActivity {
    sched::RelaxedU64 divisions;
    sched::RelaxedU64 compactions;

    // A reader's merge of two cores' parts.
    LeafActivity& operator+=(const LeafActivity& o) noexcept {
        divisions.add(o.divisions);
        compactions.add(o.compactions);
        return *this;
    }
};

class LeafStats {
public:
    // Relations and indexes together; `sys.meta`'s `leaf_stats_dropped`
    // counts the trees past it.
    static constexpr std::size_t kOwnersPerCore = 8192;

    // `owner_oid` is the tree's pages' owner; 0, a page no object owns,
    // counts nothing.
    static void CountDivision(catalog::Oid owner_oid) noexcept;
    static void CountCompaction(catalog::Oid owner_oid) noexcept;

    // Every tree's counts, the cores' parts summed.
    static std::unordered_map<catalog::Oid, LeafActivity> Snapshot();

    // New trees no part could take, over every core.
    static std::uint64_t Dropped() noexcept;

    // Empties every part; a test's alone.
    static void ResetForTest() noexcept;

private:
    using Parts = OidParts<LeafActivity, kOwnersPerCore>;
    static Parts& parts() noexcept;
};

}  // namespace kds::stats
