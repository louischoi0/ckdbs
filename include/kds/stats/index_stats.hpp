#pragma once

#include <cstdint>
#include <unordered_map>

#include "kds/catalog/oid.hpp"
#include "kds/stats/oid_parts.hpp"

// **`sys.indexes`' usage columns: how often each secondary index served a
// read** (`instructions/v3.2.0/workorder-df-statistics-coverage.md` §2 part
// C). Memory-resident (DF-R1). No switch (DF-R9). Nothing in the engine
// reads a count (DF-R3): no advisor, no automatic drop.
//
// ---- What a count is --------------------------------------------------------
//
// Folded from the executor's per-step counters where a chain's run is
// recorded (`CommandDispatcher::RecordExecution`): for every step an index
// served - `kIndexProbe` or `kIndexRange` - the times the step walked the
// index between one pair of bounds (`probes`, `ranges`:
// `StepStats::index_walks`), the entries it read there (`entries_scanned`)
// and the tuple keys it then descended for (`rows_resolved`), with the
// instant of the latest such run (`last_used`). A walk is counted where the
// index is read, so an index step under another step counts once per row of
// the outer, a value list once per value (a key set, CU), and **a step that
// names an index and walks its relation instead** - the `indexes` key off,
// the index dropped under a compiled chain, a correlated probe that could
// not key - counts nothing. A run is folded when it completes: a stopped
// run (a `LIMIT`) folds the prefix it ran, an `ANALYZE`'s run is a use like
// any other, and each relation walk of a query-layer statement is a run of
// its own - so a layer statement refused after a walk finished has folded
// that walk, and a write's own read through the layer (`INSERT ... SELECT`'s
// source) is counted. **What is not a step is not counted**: a unique
// index's check descents, a foreign key's, an index's own maintenance by a
// write, the dispatcher's walk of an `UPDATE`'s or a `DELETE`'s `WHERE`,
// and the chain a write's `WHERE` subquery runs, whose counters are dropped
// where it runs (the order's §7, DF-S0 item 2). An index a build has
// flagged `building` is chosen by no read and stays at 0.
//
// ---- Where the rows live, and the latch protocol ----------------------------
//
// **The instance owns the store** (DA-R6's reason): a restart, a restore and
// a second instance in one process each start empty with no reset call. The
// instance hands one object to every core's dispatcher; a dispatcher handed
// none uses one of its own. `OidParts` (`stats/oid_parts.hpp`): one latched
// part per core, the core a statement ran on its only writer, once per
// index step of a recorded run that walked its index.
//
// **A count never fails, delays or reorders the statement it describes**
// (DF-R2). The one allocation is a part's first sight of an index; a new
// index past `kIndexesPerCore`, or one that cannot be allocated, is dropped
// and counted (`sys.meta`'s `index_stats_dropped`). A dropped index's counts
// stay in the parts - its oid is never reissued - and the view, which lists
// the catalog's indexes, never shows them; `REINDEX` builds under a new oid,
// so a rebuilt index starts at 0.

namespace kds::stats {

struct IndexUse {
    std::uint64_t probes = 0;
    std::uint64_t ranges = 0;
    std::uint64_t entries_scanned = 0;
    std::uint64_t rows_resolved = 0;
    std::int64_t last_used_us = 0;  // UTC epoch microseconds (TIMESTAMP); 0: never

    IndexUse& operator+=(const IndexUse& o) noexcept {
        probes += o.probes;
        ranges += o.ranges;
        entries_scanned += o.entries_scanned;
        rows_resolved += o.rows_resolved;
        if (o.last_used_us > last_used_us) last_used_us = o.last_used_us;
        return *this;
    }
};

class IndexStats {
public:
    static constexpr std::size_t kIndexesPerCore = 4096;  // BO-Q7 (a)'s shape bound

    // One index step of one recorded run, on `core`.
    void Record(std::uint32_t core, catalog::Oid index_oid, const IndexUse& use) noexcept {
        parts_.With(core, index_oid, [&](IndexUse& counts) { counts += use; });
    }

    // Every index's counts, the cores' parts summed.
    std::unordered_map<catalog::Oid, IndexUse> Snapshot() { return parts_.Snapshot(); }

    // New indexes no part could take, over every core.
    std::uint64_t Dropped() noexcept { return parts_.Dropped(); }

private:
    OidParts<IndexUse, kIndexesPerCore> parts_;
};

}  // namespace kds::stats
