#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/budget.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/sched/clock.hpp"
#include "kds/stats/decay.hpp"
#include "kds/storage/page_store.hpp"

// The physical-optimizer planner (docs/spec/physical-optimizer.md R2/R9/R10,
// workplan PX05) - the shadow half, and in v1 the only half.
//
// Pure planning: reads `sys.access_stats` and the catalog, weighs shapes
// with the R1 decay score, and produces `RelayoutPlan`s. **Every plan is
// blocked** - each of the three v1 kinds names the §6 gate that stops a
// mover from existing - so the output is a report, never an action. That is
// R11, not a limitation to apologize for: the report is what turns "should
// a gate be opened" from taste into a number.
//
// ---- Two entry points, one deliberate asymmetry --------------------------
//
// `PlanAllRelations` takes **no PageStore**: the bare form cannot fetch a
// relation page *by signature*, which is R10's "the all-relations form
// never walks" made structural rather than promised. `PlanRelation` takes
// one, because the per-relation form is allowed to pay for what statistics
// cannot know: delete-mark density and live fill, measured by a read-only,
// stoppable, budget-charged walk. The caller asked; the walk is the price.
//
// ---- What the weights mean (R2's honesty note) ---------------------------
//
// An access-stats row is `{use_count, last_seen}`, not a decay pair, so the
// decayed weight treats the whole count as if it happened at `last_seen` -
// an approximation that overweights recently-active shapes, which is the
// ranking direction a relayout report wants anyway. If the planner ever
// needs a true per-shape decay state, that is a *collection* change and
// belongs in access_stats.hpp (R2), not here.
//
// ---- What the survey counts ----------------------------------------------
//
// The survey is a census, not a read: it counts delete-marks without MVCC,
// because whether a marked tuple is reclaimable is exactly the question the
// reader-horizon gate exists for. `delete_marked` is therefore an upper
// bound on what a compact could drop, and the report says `blocked_on`
// beside the number it would buy. `chain_pages` counts pages holding at
// least one visitable slot - a page whose every slot was retired would be
// invisible to the walk, which nothing can produce today (user tuples are
// delete-marked, never retired).
//
// Concurrency: core-local, read-only, no state of its own (rules.md #3).

namespace kds::stats {

// The three v1 plan kinds, each permanently attached to the gate that
// blocks it (spec §5's table). Order is report order.
enum class RelayoutPlanKind : std::uint8_t {
    kCompact = 0,  // rewrite the chain dropping reclaimable delete-marks
    kCluster = 1,  // co-locate a hot set on fewer pages
    kDefrag = 2,   // rewrite a chain onto contiguous page ids
};

// Spec §6's gates, by number.
enum class RelayoutGate : std::uint8_t {
    kReaderHorizon = 1,   // no oldest-active horizon: readers unregistered
    kOrderedBetween = 2,  // clustering breaks the property kRange pruning reads
    kPageReuse = 3,       // a freed page reallocated across relations breaks trails
};

const char* RelayoutPlanKindName(RelayoutPlanKind kind) noexcept;
const char* RelayoutGateName(RelayoutGate gate) noexcept;

// One access shape with its decayed weight, Q24.8 per stats/decay.hpp.
struct ShapeWeight {
    exec::AccessKind kind{};
    std::uint64_t column_mask = 0;
    std::uint64_t use_count = 0;
    std::uint32_t decayed_weight = 0;
};

// What the per-relation walk measured.
struct RelationSurvey {
    std::uint64_t chain_pages = 0;      // pages holding >= 1 visitable slot
    std::uint64_t live_tuples = 0;
    std::uint64_t delete_marked = 0;    // upper bound on reclaimable (see above)
    std::uint64_t tuples_per_page = 0;  // from the schema constant, TuplesPerPage()
};

// One candidate plan. R9's pair is `predicted_*` / `measured_*`: the
// measured field exists now and is never populated in v1, so the promotion
// comparison - measured against predicted, once a mover exists - needs no
// format change, only numbers.
struct RelayoutPlan {
    RelayoutPlanKind kind{};
    RelayoutGate blocked_on{};

    // True when a survey fed the prediction. Distinguishes "0 pages saved,
    // measured" from "0 because the bare form had nothing to measure with".
    bool survey_backed = false;

    std::uint64_t predicted_pages_saved = 0;
    // R9: pages saved per execution x the decayed weight of the shapes that
    // walk the chain - weighted page-touches avoided, in **whole units**,
    // the weight's Q24.8 having been divided out by PredictedBenefit (one
    // page saved under one undecayed execution reads as 1, not as 256).
    // 0 for `cluster` (its input, a hot set, is Waystone's and no
    // collector supplies it yet - R2) and for `defrag` (it saves no pages;
    // its benefit is sequential I/O, which R9's metric does not express -
    // stated rather than faked with a number).
    std::uint64_t predicted_benefit = 0;
    std::uint64_t measured_pages_saved = 0;
};

// Everything the report says about one relation. `plans` is empty in two
// cases that must not be confused with "nothing worth doing": a btree
// relation (R5 scopes the mover to heap chains - the tree's physical form
// is its own concern) and a **catalog relation** (`system_relation` below),
// which the legal-move table puts outside the mover's jurisdiction
// entirely - §4's "must not touch any catalog page" is not a gate that
// opens, so listing gated plans for one would be a lie in the hopeful
// direction. The bare form skips catalog relations altogether; the named
// form answers with the reason, because the caller asked directly.
struct RelationReport {
    catalog::Oid rel_oid = 0;
    std::string name;
    catalog::ClusteredType clustered_type{};
    bool system_relation = false;  // oid below catalog::kUserOidStart
    std::vector<ShapeWeight> shapes;
    std::optional<RelationSurvey> survey;
    std::vector<RelayoutPlan> plans;
};

// ---- Pure math, unit-testable without an engine --------------------------

// Tuples a fresh heap page holds at `row_size`: the usable band between the
// heap header and the next_page_id reservation, divided by what one tuple
// costs (slot + tuple header + payload). Derived from heap_page.hpp's
// constants so it moves with the layout instead of drifting from it.
std::uint64_t TuplesPerPage(std::uint32_t row_size) noexcept;

// ceil(live / per_page), floored at 1 - a relation's chain never has fewer
// pages than its root. `per_page == 0` (a row wider than a page, which
// CREATE TABLE refuses) answers 0 defensively.
std::uint64_t PagesAfterCompact(std::uint64_t live_tuples, std::uint64_t per_page) noexcept;

// The R1 decay applied to an access-stats row: the whole count decayed
// from `last_seen` (the approximation the header states). Null clock, or a
// row never stamped (`last_seen == 0`), degrades to the raw count.
std::uint32_t ShapeDecayedWeight(std::uint64_t use_count, std::uint64_t last_seen,
                                 const sched::Clock* clock,
                                 sched::MonoTimeNs half_life_ns) noexcept;

// Whether a kind's executions walk the chain - the shapes a smaller chain
// makes cheaper. Descents and index probes are priced by depth, not chain
// length, and are deliberately excluded.
bool IsChainWalkKind(exec::AccessKind kind) noexcept;

// Sum of the walk-kind shapes' decayed weights, saturating.
std::uint64_t WalkWeightOf(std::span<const ShapeWeight> shapes) noexcept;

// R9: pages saved x decayed walk weight, with the weight's Q24.8 divided
// out (>> 8), so one page saved under one undecayed execution is 1.
std::uint64_t PredictedBenefit(std::uint64_t pages_saved, std::uint64_t walk_weight) noexcept;

// ---- The planner ---------------------------------------------------------

// The bare form: statistics and catalog only. No PageStore parameter, so
// it cannot walk a relation - by construction, not by discipline.
StatusOr<std::vector<RelationReport>> PlanAllRelations(catalog::Catalog& catalog,
                                                       const sched::Clock* clock,
                                                       sched::MonoTimeNs half_life_ns);

// The surveyed form: adds the read-only chain walk for a heap relation
// (budget-charged per slot examined; a spent budget fails the call with
// the budget's own error). A btree relation gets shapes and no survey.
StatusOr<RelationReport> PlanRelation(catalog::Catalog& catalog, storage::PageStore& store,
                                      catalog::Oid rel_oid, exec::Budget& budget,
                                      const sched::Clock* clock,
                                      sched::MonoTimeNs half_life_ns);

}  // namespace kds::stats
