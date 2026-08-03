#include "kds/stats/access_stats.hpp"

namespace kds::stats {

namespace {

void RecordSteps(catalog::Catalog& catalog, const std::vector<exec::Step>& steps,
                 std::uint64_t now, AccessStatsCounters* counters);

void RecordSubChains(catalog::Catalog& catalog, const std::vector<exec::SubChain>& subs,
                     std::uint64_t now, AccessStatsCounters* counters) {
    for (const exec::SubChain& sub : subs) RecordSteps(catalog, sub.steps, now, counters);
}

void RecordSteps(catalog::Catalog& catalog, const std::vector<exec::Step>& steps,
                 std::uint64_t now, AccessStatsCounters* counters) {
    for (const exec::Step& step : steps) {
        // **No branch on `step.kind`.** Every kind is recorded by this one
        // line, which is what makes the statistic comparable across kinds -
        // "how often is this relation looked up versus filter-scanned" is
        // only answerable if both were counted the same way.
        const Status s = catalog.RecordAccess(exec::StoredAccessKind(step.kind), step.rel_oid,
                                              ColumnMaskOf(step), now);
        if (counters != nullptr) {
            if (s.ok()) {
                ++counters->steps_recorded;
            } else {
                // Dropped, never propagated. The statement already
                // succeeded; failing it now to report a counting problem
                // would be a worse trade than losing the count. The shape
                // cap arrives here too, which is deliberate - it is a
                // degraded statistic, not a degraded database.
                ++counters->write_failures;
            }
        }

        // A sub-chain's steps are real accesses against real relations and
        // are counted as such. They carry globally-numbered step ids but
        // that is irrelevant here: the shape is (kind, relation, columns),
        // and where in the statement it sat is not part of it.
        RecordSubChains(catalog, step.sub_chains, now, counters);
    }
}

}  // namespace

std::uint64_t ColumnMaskOf(const exec::Step& step) noexcept {
    std::uint64_t mask = 0;
    for (std::uint16_t col : step.access_columns) {
        // Past 63 there is no bit to set. The shape then records without
        // that column, merging with any other access differing only in it -
        // coarser, not wrong (catalog/rows.hpp states the same thing at the
        // field).
        if (col < 64) mask |= (std::uint64_t{1} << col);
    }
    return mask;
}

void RecordChainAccess(catalog::Catalog& catalog, const exec::StepChain& chain,
                       std::uint64_t now, AccessStatsCounters* counters) {
    // Hoisted sub-chains ran once, before the outer chain opened, so they
    // are one execution each exactly like a top-level step.
    RecordSubChains(catalog, chain.hoisted, now, counters);
    RecordSteps(catalog, chain.steps, now, counters);
}

}  // namespace kds::stats
