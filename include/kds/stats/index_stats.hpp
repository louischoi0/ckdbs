#pragma once

#include <cstdint>
#include <unordered_map>

#include "kds/catalog/oid.hpp"
#include "kds/sched/wait_state.hpp"
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
// instant of the latest such run (`last_used`, its statement's head). A walk is counted where the
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
// ---- Where the rows live, and the protocol ----------------------------------
//
// **The instance owns the store** (DA-R6's reason): a restart, a restore and
// a second instance in one process each start empty with no reset call. The
// instance hands one object to every core's dispatcher; a dispatcher handed
// none uses one of its own. `OidParts` (`stats/oid_parts.hpp`): one latched
// part per core, whose latch guards the part's *map* - a core's first sight
// of an index (`CountsOf`, the one allocation) and a reader's walk of it.
//
// **The counts themselves are not under the latch.** An index's counts in a
// part are relaxed single-writer words (`sched::RelaxedU64`), written by
// the part's core alone and never moved or erased - a map node's address is
// stable and nothing removes one - so the core keeps the pointer `CountsOf`
// gave it and adds through it with no latch, no hash and no allocation
// (`CommandDispatcher`'s small cache). A reader takes the part's latch for
// the map and reads the words as they stand: a row's columns may differ by a
// run in flight. **One writer a part is the caller's premise**, as it is
// `RefusalStats`': two threads recording as one core lose counts and nothing
// else.
//
// **A count never fails, delays or reorders the statement it describes**
// (DF-R2). A new index past `kIndexesPerCore`, or one that cannot be
// allocated, is dropped and counted (`sys.meta`'s `index_stats_dropped`). A
// dropped index's counts stay in the parts - its oid is never reissued - and
// the view, which lists the catalog's indexes, never shows them; `REINDEX`
// builds under a new oid, so a rebuilt index starts at 0.

namespace kds::stats {

struct IndexUse {
    sched::RelaxedU64 probes;
    sched::RelaxedU64 ranges;
    sched::RelaxedU64 entries_scanned;
    sched::RelaxedU64 rows_resolved;
    // When the index last served a run: the head of that run's statement on
    // the instance's monotonic clock - no clock is read for it - which the
    // view turns into a wall instant. 0: never, or an instance with no clock.
    sched::RelaxedU64 last_used_ns;

    // One recorded run's walks of the index, by the part's own core.
    void Add(bool range, std::uint64_t walks, std::uint64_t scanned, std::uint64_t resolved,
             std::uint64_t now_ns) noexcept {
        (range ? ranges : probes).add(walks);
        entries_scanned.add(scanned);
        rows_resolved.add(resolved);
        if (now_ns != 0) last_used_ns.set(now_ns);
    }

    // A reader's merge of two cores' parts.
    IndexUse& operator+=(const IndexUse& o) noexcept {
        probes.add(o.probes);
        ranges.add(o.ranges);
        entries_scanned.add(o.entries_scanned);
        rows_resolved.add(o.rows_resolved);
        last_used_ns.raise_to(o.last_used_ns);
        return *this;
    }
};

class IndexStats {
public:
    static constexpr std::size_t kIndexesPerCore = 4096;  // BO-Q7 (a)'s shape bound

    // `core`'s counts of `index_oid`, made at its first sight; null where the
    // part is full or the allocation failed, which is counted. The pointer
    // holds for the store's life, and only `core` writes through it.
    IndexUse* CountsOf(std::uint32_t core, catalog::Oid index_oid) noexcept {
        IndexUse* counts = nullptr;
        parts_.With(core, index_oid, [&](IndexUse& found) { counts = &found; });
        return counts;
    }

    // Every index's counts, the cores' parts summed.
    std::unordered_map<catalog::Oid, IndexUse> Snapshot() { return parts_.Snapshot(); }

    // New indexes no part could take, over every core.
    std::uint64_t Dropped() noexcept { return parts_.Dropped(); }

private:
    OidParts<IndexUse, kIndexesPerCore> parts_;
};

}  // namespace kds::stats
