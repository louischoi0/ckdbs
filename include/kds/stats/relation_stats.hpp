#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

#include "kds/base/latch.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/stats/per_core_ring.hpp"

// **`sys.relation_stats`' activity columns: the rows each relation's writes
// reported** (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md`
// §3.2, BO-S6). Memory-resident (BO-Q5 (b)): emptied by a restart and by a
// restore, never a catalog page. No switch (BO-Q8).
//
// ---- What a count is -------------------------------------------------------
//
// The rows a write statement that succeeded reported - `INSERT`'s, a KWP
// load chunk's, `UPDATE`'s, `DELETE`'s - and the keys a `PURGE` retired,
// since the instance started. A statement is counted when its handler
// answers, not when its transaction decides: rows an explicit transaction
// later rolls back stay counted, as the statement reported them, and so do
// an autocommit statement's whose deferred commit a fail-stopped log then
// aborts (`FinishDeferredCommits`). A `PURGE`'s keys are
// counted whatever its decide does, its records being no transaction's
// (PU6). Leaf splits, spills, page counts and delete-marked slots are not
// here (BO-Q13).
//
// ---- Where the rows live, and the latch protocol ---------------------------
//
// One part per core under a latch of its own (`LatchKind::kStatsRing`): the
// core a statement ran on is the only writer of its part, once per write
// statement at its end; a reader on any core takes each part's latch in turn
// and sums the parts by oid. No cross-core write is added to a statement's
// path.
//
// **A write never fails, delays or reorders the statement it describes.** A
// new relation past `kRelationsPerCore` is dropped and counted (a `sys.meta`
// field, SV3); one that cannot be allocated is dropped the same way. A
// dropped relation's counters stay in the parts - its oid is never reissued,
// and only the owning core writes a part - and the view, which lists the
// catalog's relations, never shows them. **So the bound counts every
// relation a core has written since the instance started, dropped ones
// included**: past 4096 on one core, that core's writes of a relation it has
// not written before are dropped and counted, and the view under-counts the
// relation - `relation_stats_dropped` above 0 is what says so.

namespace kds::stats {

enum class RelationWrite : std::uint8_t { kInsert, kUpdate, kDelete, kPurge };

struct RelationActivity {
    std::uint64_t inserts = 0;
    std::uint64_t updates = 0;
    std::uint64_t deletes = 0;
    std::uint64_t purged = 0;
};

class RelationStats {
public:
    static constexpr std::size_t kRelationsPerCore = 4096;  // BO-Q7 (a)'s shape bound

    // One write statement's rows, at its end. A count of 0 records nothing.
    static void Record(std::uint32_t core, catalog::Oid oid, RelationWrite kind,
                       std::uint64_t rows) noexcept;

    // Every relation's counts, the cores' parts summed.
    static std::unordered_map<catalog::Oid, RelationActivity> Snapshot();

    // New relations no part could take, over every core.
    static std::uint64_t Dropped() noexcept;

    // Empties every part; a test's alone.
    static void ResetForTest() noexcept;

private:
    struct Part {
        Latch latch{LatchKind::kStatsRing};
        std::unordered_map<catalog::Oid, RelationActivity> relations;
        std::uint64_t dropped = 0;
    };
    static std::array<Part, kStatsParts>& parts() noexcept;
};

}  // namespace kds::stats
