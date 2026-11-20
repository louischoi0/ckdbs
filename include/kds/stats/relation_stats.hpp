#pragma once

#include <cstdint>
#include <unordered_map>

#include "kds/catalog/oid.hpp"
#include "kds/sched/wait_state.hpp"
#include "kds/stats/oid_parts.hpp"

// **`sys.relation_stats`' activity columns: the rows each relation's writes
// reported** (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md`
// §3.2, BO-S6) **and the lock waits on it** (DF-S3). Memory-resident
// (BO-Q5 (b)): emptied by a restart and by a restore, never a catalog page.
// No switch (BO-Q8).
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
// (PU6). Spills and page counts are recorded nowhere (BO-Q13); a tree's
// leaf divisions are `stats/leaf_stats.hpp`'s and a relation's delete-marked
// rows the planner's steward's (`sys-views.md` §6.2c).
//
// **A lock wait is counted where its `sys.lock_waits` row is recorded**
// (`LockWaitLog::Record`), from that row's relation, time and outcome, so
// the four lock columns keep what that ring overwrites - and a row the ring
// lost to a reader's copy, since a count here takes no latch - and hold
// nothing that was never handed to it. A wait is not a write: a `DROP TABLE` that waited on a
// reader's borrow counts here and in no activity column.
//
// ---- Where the rows live, and the protocol ---------------------------------
//
// `OidParts` (`stats/oid_parts.hpp`): one part per core and **no latch**
// (`docs/rules/rules.md` §3a; DL-S2), the core a statement ran on its only
// writer - once per write statement at its end, once per lock wait at its
// end - and a reader summing the parts by oid. A relation's counts in a
// part are relaxed single-writer words: a record takes nothing and waits
// for nothing, and a row a view reads may differ by a statement in flight.
// No cross-core write is added to a statement's path.
//
// **A count never fails, delays or reorders what it describes.** A new
// relation past `kRelationsPerCore` is dropped and counted (a `sys.meta`
// field, SV3); one that cannot be allocated is dropped the same way. A
// dropped relation's counters stay in the parts - its oid is never reissued,
// and only the owning core writes a part - and the view, which lists the
// catalog's relations, never shows them. **So the bound counts every
// relation a core has written or waited on since the instance started,
// dropped ones included**: past 4096 on one core, that core's counts of a
// relation new to it are dropped and counted, and the view under-counts the
// relation - `relation_stats_dropped` above 0 is what says so.

namespace kds::stats {

enum class RelationWrite : std::uint8_t { kInsert, kUpdate, kDelete, kPurge };

struct RelationActivity {
    sched::RelaxedU64 inserts;
    sched::RelaxedU64 updates;
    sched::RelaxedU64 deletes;
    sched::RelaxedU64 purged;
    // The lock waits registered on a unit of the relation, cumulative
    // (DF-S3): every row `sys.lock_waits` was written, counted where it is
    // written, so the columns hold what that ring has overwritten too.
    sched::RelaxedU64 lock_waits;
    sched::RelaxedU64 lock_wait_us;
    sched::RelaxedU64 lock_deadlocks;   // of `lock_waits`, ended `deadlock`
    sched::RelaxedU64 lock_fault_nets;  // of `lock_waits`, ended `fault_net`

    // A reader's merge of two cores' parts.
    RelationActivity& operator+=(const RelationActivity& o) noexcept {
        inserts.add(o.inserts);
        updates.add(o.updates);
        deletes.add(o.deletes);
        purged.add(o.purged);
        lock_waits.add(o.lock_waits);
        lock_wait_us.add(o.lock_wait_us);
        lock_deadlocks.add(o.lock_deadlocks);
        lock_fault_nets.add(o.lock_fault_nets);
        return *this;
    }
};

class RelationStats {
public:
    static constexpr std::size_t kRelationsPerCore = 4096;  // BO-Q7 (a)'s shape bound

    // One write statement's rows, at its end. A count of 0 records nothing.
    static void Record(std::uint32_t core, catalog::Oid oid, RelationWrite kind,
                       std::uint64_t rows) noexcept;

    // One lock wait that ended, on a unit of relation `oid`
    // (`LockWaitLog::Record`'s one call). No relation, no count.
    static void RecordLockWait(std::uint32_t core, catalog::Oid oid, std::uint64_t waited_us,
                               bool deadlock, bool fault_net) noexcept;

    // Every relation's counts, the cores' parts summed.
    static std::unordered_map<catalog::Oid, RelationActivity> Snapshot();

    // New relations no part could take, over every core.
    static std::uint64_t Dropped() noexcept;

    // Empties every part; a test's alone.
    static void ResetForTest() noexcept;

private:
    using Parts = OidParts<RelationActivity, kRelationsPerCore>;
    static Parts& parts() noexcept;
};

}  // namespace kds::stats
