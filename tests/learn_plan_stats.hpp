#pragma once

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/catalog.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/stats/plan_stats.hpp"
#include "kds/storage/page_store.hpp"

// The join planner's statistics learned from a query set (CC-S3), for the
// contract suites' learned-statistics cells: each statement run once, then
// two steward steps - the first seeds the priors the runs wanted and folds
// their counts, the second publishes what is left. A cell that learned
// nothing, or whose plans read nothing, would repeat the cold path and
// prove nothing of the planner that ships, so both are asserted.

namespace kds::testing_plan {

inline void LearnPlanStats(server::CommandDispatcher& dispatcher, stats::PlanStats& plan_stats,
                           catalog::Catalog& catalog, storage::PageStore& store,
                           const std::vector<std::string>& sqls) {
    dispatcher.set_plan_stats(&plan_stats);
    for (const std::string& sql : sqls) dispatcher.Dispatch(sql);
    for (std::uint64_t second : {1u, 2u}) {
        EXPECT_TRUE(plan_stats.StewardStep(catalog, store, second * 1'000'000'000).ok());
    }
    ASSERT_GT(plan_stats.generation(), 0u) << "nothing was learned";
    std::size_t informed = 0;
    for (const std::string& sql : sqls) {
        const std::string plan = dispatcher.Dispatch("ANALYZE " + sql).response;
        if (plan.find("planned order=") != std::string::npos &&
            plan.find(" gen=0 ") == std::string::npos) {
            ++informed;
        }
    }
    EXPECT_GT(informed, 0u) << "no plan read the learned statistics";
}

// `join_order = fixed` over a query set (CX-S0), for the contract suites'
// captured-plan cells: the statistics learned as above, then each statement
// run once more under `fixed`, which captures every searched shape - its
// relations' priors are published by then. A cell whose statements then ran
// no captured order would prove nothing of the default configuration, so
// that is asserted: a shape is held, and a later compile says it ran one.
inline void CapturePlans(server::CommandDispatcher& dispatcher, stats::PlanStats& plan_stats,
                         catalog::Catalog& catalog, storage::PageStore& store,
                         const std::vector<std::string>& sqls) {
    LearnPlanStats(dispatcher, plan_stats, catalog, store, sqls);
    dispatcher.set_join_order(exec::JoinOrder::kFixed);
    for (const std::string& sql : sqls) dispatcher.Dispatch(sql);
    ASSERT_GT(dispatcher.captured_plans().shapes(), 0u) << "no shape was captured";
    std::size_t captured = 0;
    for (const std::string& sql : sqls) {
        const std::string plan = dispatcher.Dispatch("ANALYZE " + sql).response;
        if (plan.find("planned order=captured") != std::string::npos) ++captured;
    }
    EXPECT_GT(captured, 0u) << "no statement ran a captured order";
}

}  // namespace kds::testing_plan
