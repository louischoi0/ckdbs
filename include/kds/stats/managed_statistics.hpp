#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/oid.hpp"

// **Managed statistics: a relation's statistics object** (CY-S1b, CY-S4;
// `docs/spec/plan-stats.md` §9; work order
// `instructions/v3.2.0/workorder-cy-managed-statistics.md`). What an
// operator's `GATHER`, `IMPORT`, `RESTORE`, `PUBLISH`, `LOCK` and `DELETE
// STATISTICS` act on: per relation, a published version the planner reads, a
// pending one it does not, the replaced versions (the last
// `kHistoryVersions`), a lock, and the counter that numbers versions.
//
// **Only an explicit act changes it** (CY-R3): nothing here gathers on its
// own, and the steward's sampled priors (PS1) are not in it - they sit beside
// it in `PlanStats` and price a relation nobody gathered (CY-Q16 (a)).
//
// Memory during a run and unlogged (W14): the shutdown tail writes it into
// `sys.statistics` (PS15) and the next mount reads it back (PS16), so a crash
// or a restore loses it (W15).
//
// ---- The latch protocol --------------------------------------------------
//
// One latch over the whole store. Every act is an operator's statement, the
// mount's load or the shutdown's export, so nothing here is on a statement's
// path but those: a compile reads the published bands only through the
// `PlanStatsSnapshot` its `PlanStats` built from them (`PublishedBands`).

namespace kds::stats {

enum class StatisticsOrigin : std::uint8_t {
    kGathered = 1,  // a `GATHER`'s walk
    kImported = 2,  // an `IMPORT`'s document
};

const char* StatisticsOriginName(StatisticsOrigin origin) noexcept;

// One version of a relation object: rows and leaves (CY-Q26 (a)).
struct RelationStatistic {
    std::uint64_t version = 0;  // only grows, per relation (CY-R6)
    StatisticsOrigin origin = StatisticsOrigin::kGathered;
    std::uint64_t rows = 0;
    std::uint64_t leaves = 0;
    std::int64_t made_us = 0;  // UTC epoch microseconds
};

// What a relation's slot holds, as the views and the shutdown read it.
struct StatisticsSlotView {
    catalog::Oid oid = 0;
    std::optional<RelationStatistic> published;
    std::int16_t band = 0;  // the published version's band (PS5), when one is
    std::optional<RelationStatistic> pending;
    std::vector<RelationStatistic> history;  // oldest first
    bool locked = false;
    std::uint64_t last_version = 0;
};

class ManagedStatistics {
public:
    // CY-Q12 (a): the last 8 replaced versions per object.
    static constexpr std::size_t kHistoryVersions = 8;

    // A `GATHER` or an `IMPORT`'s result, numbered here. Published at once
    // unless `pending` (CY-Q8 (a)); a publish of a locked slot is refused
    // without `force`, a pending store is not (CY-Q28 (a)). The previous
    // published version enters the history. A relation dropped since the
    // walk began is refused `NotFound`.
    StatusOr<RelationStatistic> Store(catalog::Oid oid, RelationStatistic stat, bool pending,
                                      bool force);
    // `PUBLISH STATISTICS`: the pending version becomes the published one.
    StatusOr<RelationStatistic> PublishPending(catalog::Oid oid, bool force);
    // `DISCARD PENDING STATISTICS`.
    Status DiscardPending(catalog::Oid oid);
    // `RESTORE STATISTICS ... TO VERSION n`: a version in the history - or
    // the published one - published again as a new version.
    StatusOr<RelationStatistic> Restore(catalog::Oid oid, std::uint64_t version, bool force);
    // `LOCK` / `UNLOCK STATISTICS`, whether or not an object exists.
    Status Lock(catalog::Oid oid);
    Status Unlock(catalog::Oid oid);
    // `DELETE STATISTICS`: the published, pending and history versions; the
    // lock and the version counter stay, so a later version is still newer.
    Status Delete(catalog::Oid oid, bool force);
    // A committed `DROP TABLE`: the slot, and every later store for the oid.
    // Answers whether a published version went with it, so a drop of a
    // relation nobody gathered moves no generation.
    bool Forget(catalog::Oid oid);
    // The same for a relation's drop, which waits for no statistics
    // statement: nullopt, and nothing forgotten, where the store's latch is
    // held (`PlanStats::ForgetRelation` queues it).
    std::optional<bool> TryForget(catalog::Oid oid);
    // The store's latch, for a test that holds it as a statement does.
    Latch* LatchForTest() const noexcept { return &latch_; }
    // The lock's refusal, for an act about to change the published object
    // (CY-Q28 (a)); OK on an unlocked slot or with `force`.
    Status CheckPublishable(catalog::Oid oid, bool force) const;

    // The planner's input: every published version's band, by oid. A gather
    // publishing over a gather keeps CC's hysteresis (PS5); an import, a
    // restore and a mount publish fresh (CY-Q19 (a)).
    std::vector<std::pair<catalog::Oid, std::int16_t>> PublishedBands() const;
    // The same for the steward, which runs on a reactor and waits for no
    // operator's statement: false, and `out` untouched, where the store's
    // latch is held.
    bool TryPublishedBands(std::vector<std::pair<catalog::Oid, std::int16_t>>* out) const;
    // Every slot, by oid, for the views and the shutdown's write.
    std::vector<StatisticsSlotView> Slots() const;
    // The mount's load of what a clean shutdown wrote: replaces the store.
    void Load(std::vector<StatisticsSlotView> slots);

private:
    // `Forget`'s body, `latch_` held.
    bool ForgetLocked(catalog::Oid oid);
    // `PublishedBands`' body, `latch_` held.
    std::vector<std::pair<catalog::Oid, std::int16_t>> PublishedBandsLocked() const;

    struct Slot {
        std::optional<RelationStatistic> published;
        std::int16_t band = 0;
        std::optional<RelationStatistic> pending;
        std::deque<RelationStatistic> history;
        bool locked = false;
        std::uint64_t last_version = 0;
    };

    Status CheckUnlocked(const Slot& slot, bool force) const;
    // Publishes `stat` over the slot's published version, which enters the
    // history. `fresh`: an import's, a restore's - no hysteresis carried.
    void Publish(Slot& slot, const RelationStatistic& stat, bool fresh);

    mutable Latch latch_{LatchKind::kStatsRing};
    std::map<catalog::Oid, Slot> slots_;
    std::set<catalog::Oid> forgotten_;
};

}  // namespace kds::stats
