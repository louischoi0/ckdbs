#include "kds/stats/relayout_planner.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"

namespace kds::stats {

namespace {

// The three candidate plans for a heap relation, in report order. Every
// kind is emitted whether or not its benefit is computable: the report's
// job is to show the whole candidate set with each one's gate, and a kind
// that vanished when its number was unknown would read as "nothing to do".
std::vector<RelayoutPlan> BuildPlans(const std::vector<ShapeWeight>& shapes,
                                     const std::optional<RelationSurvey>& survey) {
    std::vector<RelayoutPlan> plans;
    const std::uint64_t walk_weight = WalkWeightOf(shapes);

    RelayoutPlan compact;
    compact.kind = RelayoutPlanKind::kCompact;
    compact.blocked_on = RelayoutGate::kReaderHorizon;
    if (survey.has_value()) {
        compact.survey_backed = true;
        const std::uint64_t after =
            PagesAfterCompact(survey->live_tuples, survey->tuples_per_page);
        if (survey->chain_pages > after) {
            compact.predicted_pages_saved = survey->chain_pages - after;
        }
        compact.predicted_benefit =
            PredictedBenefit(compact.predicted_pages_saved, walk_weight);
    }
    plans.push_back(compact);

    RelayoutPlan cluster;
    cluster.kind = RelayoutPlanKind::kCluster;
    cluster.blocked_on = RelayoutGate::kOrderedBetween;
    plans.push_back(cluster);

    RelayoutPlan defrag;
    defrag.kind = RelayoutPlanKind::kDefrag;
    defrag.blocked_on = RelayoutGate::kPageReuse;
    defrag.survey_backed = survey.has_value();
    plans.push_back(defrag);

    return plans;
}

// The shapes recorded for one relation, decayed. `rows` is the whole
// `sys.access_stats` relation; filtering here keeps the catalog read to one
// scan however many relations the report covers.
std::vector<ShapeWeight> ShapesFor(catalog::Oid rel_oid,
                                   const std::vector<catalog::SysAccessStatRow>& rows,
                                   const sched::Clock* clock,
                                   sched::MonoTimeNs half_life_ns) {
    std::vector<ShapeWeight> shapes;
    for (const catalog::SysAccessStatRow& row : rows) {
        if (row.rel_id != rel_oid) continue;
        // A stored kind this build does not know is skipped, not guessed:
        // the same stance AccessKindOfStored's other reader takes.
        auto kind = exec::AccessKindOfStored(row.kind);
        if (!kind.has_value()) continue;
        ShapeWeight shape;
        shape.kind = *kind;
        shape.column_mask = row.column_mask;
        shape.use_count = row.use_count;
        shape.decayed_weight =
            ShapeDecayedWeight(row.use_count, row.last_seen, clock, half_life_ns);
        shapes.push_back(shape);
    }
    return shapes;
}

StatusOr<RelationReport> ReportFor(catalog::Catalog& catalog, const catalog::SysObjectRow& object,
                                   const std::vector<catalog::SysAccessStatRow>& stats,
                                   const sched::Clock* clock, sched::MonoTimeNs half_life_ns) {
    auto access = catalog.InitTableAccess(object.oid);
    if (!access.ok()) return access.status();

    RelationReport report;
    report.rel_oid = object.oid;
    report.name = std::string(catalog::NameView(object.name));
    report.clustered_type = access.value()->clustered_type;
    report.system_relation = object.oid < catalog::kUserOidStart;
    report.shapes = ShapesFor(object.oid, stats, clock, half_life_ns);
    if (report.clustered_type == catalog::ClusteredType::kHeap && !report.system_relation) {
        report.plans = BuildPlans(report.shapes, /*survey=*/std::nullopt);
    }
    return report;
}

}  // namespace

const char* RelayoutPlanKindName(RelayoutPlanKind kind) noexcept {
    switch (kind) {
        case RelayoutPlanKind::kCompact: return "compact";
        case RelayoutPlanKind::kCluster: return "cluster";
        case RelayoutPlanKind::kDefrag: return "defrag";
    }
    return "?";
}

const char* RelayoutGateName(RelayoutGate gate) noexcept {
    switch (gate) {
        case RelayoutGate::kReaderHorizon: return "reader-horizon";
        case RelayoutGate::kOrderedBetween: return "ordered-between";
        case RelayoutGate::kPageReuse: return "page-reuse";
    }
    return "?";
}

std::uint64_t TuplesPerPage(std::uint32_t row_size) noexcept {
    constexpr std::uint64_t kUsable =
        heap::kNextPageIdOffset - (heap::kHeapHeaderOffset + heap::kHeaderSize);
    const std::uint64_t per_tuple =
        heap::kSlotOnDiskSize + heap::kTupleHeaderOnDiskSize + row_size;
    return kUsable / per_tuple;
}

std::uint64_t PagesAfterCompact(std::uint64_t live_tuples, std::uint64_t per_page) noexcept {
    if (per_page == 0) return 0;
    if (live_tuples == 0) return 1;
    return (live_tuples + per_page - 1) / per_page;
}

std::uint32_t ShapeDecayedWeight(std::uint64_t use_count, std::uint64_t last_seen,
                                 const sched::Clock* clock,
                                 sched::MonoTimeNs half_life_ns) noexcept {
    // Saturate into the score's Q24.8 before decaying: a count too large to
    // scale caps at the ceiling, the same non-wrap stance the score itself
    // takes.
    constexpr std::uint64_t kMaxCount =
        std::numeric_limits<std::uint32_t>::max() / kDecayScoreScale;
    const std::uint32_t scaled =
        use_count >= kMaxCount ? std::numeric_limits<std::uint32_t>::max()
                               : static_cast<std::uint32_t>(use_count * kDecayScoreScale);
    if (clock == nullptr || last_seen == 0) return scaled;
    return DecayedScaledAt(DecayState{scaled, last_seen}, clock->Now(), half_life_ns);
}

bool IsChainWalkKind(exec::AccessKind kind) noexcept {
    switch (kind) {
        case exec::AccessKind::kScan:
        case exec::AccessKind::kFilterScan:
        case exec::AccessKind::kRange:
            return true;
        // Descents and probes are priced by depth or by entry count, not by
        // chain length; a Cabin probe's miss path walks, but its hit path -
        // the one worth weighting - does not.
        case exec::AccessKind::kLookup:
        case exec::AccessKind::kProbe:
        case exec::AccessKind::kCabinProbe:
        case exec::AccessKind::kIndexProbe:
        case exec::AccessKind::kIndexRange:
            return false;
    }
    return false;
}

std::uint64_t WalkWeightOf(std::span<const ShapeWeight> shapes) noexcept {
    std::uint64_t sum = 0;
    for (const ShapeWeight& shape : shapes) {
        if (!IsChainWalkKind(shape.kind)) continue;
        const std::uint64_t next = sum + shape.decayed_weight;
        sum = next < sum ? std::numeric_limits<std::uint64_t>::max() : next;
    }
    return sum;
}

std::uint64_t PredictedBenefit(std::uint64_t pages_saved, std::uint64_t walk_weight) noexcept {
    // Overflow-safe: pages_saved is bounded by a chain's page count (2^31)
    // and the shifted weight by 2^56, so the product fits with the check
    // here for the pathological caller.
    if (pages_saved != 0 &&
        walk_weight > std::numeric_limits<std::uint64_t>::max() / pages_saved) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (pages_saved * walk_weight) >> 8;
}

StatusOr<std::vector<RelationReport>> PlanAllRelations(catalog::Catalog& catalog,
                                                       const sched::Clock* clock,
                                                       sched::MonoTimeNs half_life_ns) {
    auto tables = catalog.ListTables();
    if (!tables.ok()) return tables.status();
    auto stats = catalog.ListAccessStats();
    if (!stats.ok()) return stats.status();

    std::vector<RelationReport> reports;
    reports.reserve(tables.value().size());
    for (const catalog::SysObjectRow& object : tables.value()) {
        // Catalog relations (`sys.pattern_defs`, `sys.assertions` - the two
        // stored in user tuple format, which is why ListTables carries
        // them) are outside the mover's jurisdiction, not gated: the bare
        // report skips them. Ask by name to see one - the named form
        // answers with the reason instead of plans.
        if (object.oid < catalog::kUserOidStart) continue;
        auto report = ReportFor(catalog, object, stats.value(), clock, half_life_ns);
        // An object the catalog cannot resolve into a table (no columns -
        // the listing carries more than user relations) is not a relation
        // this planner can say anything about: skipped, not fatal. The
        // per-relation form does the opposite - a *named* relation that
        // fails to resolve is the caller's error and is reported as one.
        if (!report.ok()) continue;
        reports.push_back(std::move(report.value()));
    }
    return reports;
}

StatusOr<RelationReport> PlanRelation(catalog::Catalog& catalog, storage::PageStore& store,
                                      catalog::Oid rel_oid, exec::Budget& budget,
                                      const sched::Clock* clock,
                                      sched::MonoTimeNs half_life_ns) {
    auto tables = catalog.ListTables();
    if (!tables.ok()) return tables.status();
    const auto object =
        std::find_if(tables.value().begin(), tables.value().end(),
                     [&](const catalog::SysObjectRow& row) { return row.oid == rel_oid; });
    if (object == tables.value().end()) {
        return Status::NotFound("no relation with oid " + std::to_string(rel_oid));
    }
    auto stats = catalog.ListAccessStats();
    if (!stats.ok()) return stats.status();

    auto report = ReportFor(catalog, *object, stats.value(), clock, half_life_ns);
    if (!report.ok()) return report.status();

    if (report.value().clustered_type != catalog::ClusteredType::kHeap ||
        report.value().system_relation) {
        // Shapes only: a btree relation has no chain to survey (R5), and a
        // catalog relation is outside the mover's jurisdiction - surveying
        // one would price a walk whose plans can never exist.
        return report;
    }

    auto access = catalog.InitTableAccess(rel_oid);
    if (!access.ok()) return access.status();

    RelationSurvey survey;
    survey.tuples_per_page = TuplesPerPage(access.value()->layout.row_size);
    PageId last_page = kInvalidPageId;
    // The survey is a bulk sequential read by construction, so it is a
    // ring consumer (spec-eviction §5, EVT06): a census of a relation
    // larger than memory must not flood the pool it is surveying for.
    auto ring = store.OpenScanRing();
    Status walked = heap::ChainVisit(
        store, access.value()->desc_page_id, storage::PageAccess::kRead,
        [&](PageId page_id, heap::PageView& page,
            std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            // Priced like any other relation read (V19): a spent budget
            // stops the walk with the budget's own error, and the caller
            // gets no half-survey to mistake for a small relation.
            if (Status charged = budget.ChargeRow(); !charged.ok()) return charged;
            if (page_id != last_page) {
                ++survey.chain_pages;
                last_page = page_id;
            }
            auto tuple = page.ReadTuple(slot);
            if (tuple.ok()) {
                if (tuple.value().deleted) {
                    ++survey.delete_marked;
                } else {
                    ++survey.live_tuples;
                }
            }
            return storage::VisitControl::kContinue;
        },
        ring.get());
    if (!walked.ok()) return walked;

    report.value().survey = survey;
    report.value().plans = BuildPlans(report.value().shapes, report.value().survey);
    return report;
}

}  // namespace kds::stats
