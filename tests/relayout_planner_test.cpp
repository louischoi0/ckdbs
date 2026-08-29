#include "kds/stats/relayout_planner.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The physical-optimizer planner (docs/spec/physical-optimizer.md §5,
// workplan PX05). Two claims carry the acceptance: the math is a pure
// function a test can pin without an engine, and the bare form fetches no
// relation page - proven here with a counting store, on top of being true
// by signature (PlanAllRelations takes no PageStore at all).

namespace kds::server {
namespace {

constexpr sched::MonoTimeNs kHalfLife = 600'000'000'000ULL;

// ---- Pure math -----------------------------------------------------------

TEST(RelayoutPlannerMathTest, TuplesPerPageMatchesTheLayoutConstants) {
    // usable = kNextPageIdOffset - (heap header end); per tuple =
    // slot + tuple header + payload. At row_size 75 that is 8140 / 100.
    constexpr std::uint64_t usable =
        heap::kNextPageIdOffset - (heap::kHeapHeaderOffset + heap::kHeaderSize);
    EXPECT_EQ(stats::TuplesPerPage(75),
              usable / (heap::kSlotOnDiskSize + heap::kTupleHeaderOnDiskSize + 75));
    EXPECT_GT(stats::TuplesPerPage(16), stats::TuplesPerPage(1000));
}

TEST(RelayoutPlannerMathTest, PagesAfterCompactCeilsAndFloorsAtOne) {
    EXPECT_EQ(stats::PagesAfterCompact(0, 100), 1u);    // a chain keeps its root
    EXPECT_EQ(stats::PagesAfterCompact(1, 100), 1u);
    EXPECT_EQ(stats::PagesAfterCompact(100, 100), 1u);
    EXPECT_EQ(stats::PagesAfterCompact(101, 100), 2u);  // ceil, not floor
    EXPECT_EQ(stats::PagesAfterCompact(5, 0), 0u);      // defensive: unreachable input
}

TEST(RelayoutPlannerMathTest, ShapeWeightDecaysFromLastSeen) {
    sched::ManualClock clock(kHalfLife);
    // 8 uses last seen one half-life ago weigh 4.
    EXPECT_EQ(stats::ShapeDecayedWeight(8, 0 + 1, &clock, kHalfLife),
              stats::ShapeDecayedWeight(8, 1, &clock, kHalfLife));
    clock.SetNow(2 * kHalfLife);
    EXPECT_EQ(stats::ShapeDecayedWeight(8, kHalfLife, &clock, kHalfLife),
              4 * stats::kDecayScoreScale);
    // No clock, or a row never stamped, degrades to the raw count.
    EXPECT_EQ(stats::ShapeDecayedWeight(8, kHalfLife, nullptr, kHalfLife),
              8 * stats::kDecayScoreScale);
    EXPECT_EQ(stats::ShapeDecayedWeight(8, 0, &clock, kHalfLife), 8 * stats::kDecayScoreScale);
    // A count too large to scale saturates rather than wrapping.
    EXPECT_EQ(stats::ShapeDecayedWeight(std::uint64_t{1} << 40, 0, nullptr, kHalfLife),
              std::numeric_limits<std::uint32_t>::max());
}

TEST(RelayoutPlannerMathTest, WalkWeightSumsOnlyChainWalkKinds) {
    EXPECT_TRUE(stats::IsChainWalkKind(exec::AccessKind::kScan));
    EXPECT_TRUE(stats::IsChainWalkKind(exec::AccessKind::kFilterScan));
    EXPECT_TRUE(stats::IsChainWalkKind(exec::AccessKind::kRange));
    EXPECT_FALSE(stats::IsChainWalkKind(exec::AccessKind::kLookup));
    EXPECT_FALSE(stats::IsChainWalkKind(exec::AccessKind::kIndexProbe));

    std::vector<stats::ShapeWeight> shapes(3);
    shapes[0].kind = exec::AccessKind::kScan;
    shapes[0].decayed_weight = 512;
    shapes[1].kind = exec::AccessKind::kLookup;  // priced by depth: excluded
    shapes[1].decayed_weight = 10000;
    shapes[2].kind = exec::AccessKind::kFilterScan;
    shapes[2].decayed_weight = 256;
    EXPECT_EQ(stats::WalkWeightOf(shapes), 768u);
}

TEST(RelayoutPlannerMathTest, PredictedBenefitIsPagesTimesWeight) {
    // 3 pages saved under a decayed walk weight of 4.0 (1024 in Q24.8):
    // 12 weighted page-touches avoided.
    EXPECT_EQ(stats::PredictedBenefit(3, 4 * stats::kDecayScoreScale), 12u);
    EXPECT_EQ(stats::PredictedBenefit(0, 1024), 0u);
    EXPECT_EQ(stats::PredictedBenefit(1, stats::kDecayScoreScale), 1u);
}

// ---- The planner over a live catalog -------------------------------------

// Delegates everything, remembers which page ids were fetched. What the
// acceptance calls "a page-fetch counter": the bare form must never touch
// a relation page, and with every relation page at or above
// kFirstUserPageId the assertion is one set intersection.
class CountingStore final : public storage::PageStore {
public:
    explicit CountingStore(storage::InMemoryPageStore& inner) : inner_(inner) {}

    StatusOr<std::span<std::byte, kPageSize>> CreateAtUnpinned(PageId page_id) override {
        return inner_.CreateAtUnpinned(page_id);
    }
    StatusOr<std::pair<PageId, std::span<std::byte, kPageSize>>> CreateNewUnpinned() override {
        return inner_.CreateNewUnpinned();
    }
    StatusOr<std::span<std::byte, kPageSize>> GetUnpinned(PageId page_id) override {
        fetched_.insert(page_id);
        return inner_.GetUnpinned(page_id);
    }
    StatusOr<std::span<std::byte, kPageSize>> GetForReadUnpinned(PageId page_id) override {
        fetched_.insert(page_id);
        return inner_.GetForReadUnpinned(page_id);
    }

    void ResetFetches() { fetched_.clear(); }
    bool FetchedAnyUserPage() const {
        for (PageId id : fetched_) {
            if (id >= kFirstUserPageId) return true;
        }
        return false;
    }
    bool Fetched(PageId id) const { return fetched_.count(id) != 0; }

private:
    storage::InMemoryPageStore& inner_;
    std::unordered_set<PageId> fetched_;
};

class Instance {
public:
    Instance() : counting_(inner_) {
        auto boot = bootstrap::BootstrapDatabase(counting_, 1000);
        EXPECT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, counting_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    catalog::Catalog& catalog() { return boot_->catalog; }
    CountingStore& store() { return counting_; }

private:
    storage::InMemoryPageStore inner_{kFirstUserPageId};
    CountingStore counting_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// One heap relation with enough churn that a compact would shrink it -
// 400 rows over several pages, 300 of them delete-marked - plus a btree
// relation, which must report shapes and no plans.
void Load(Instance& db) {
    ASSERT_EQ(db.Run("CREATE TABLE h (id int64, v int64)").substr(0, 7), "CREATED");
    ASSERT_EQ(db.Run("CREATE TABLE b (id int64, v int64) BTREE").substr(0, 7), "CREATED");
    for (int i = 1; i <= 400; ++i) {
        ASSERT_EQ(db.Run("INSERT INTO h VALUES (" + std::to_string(i) + ")").substr(0, 8),
                  "INSERTED");
    }
    for (int i = 1; i <= 4; ++i) {
        ASSERT_EQ(db.Run("INSERT INTO b VALUES (" + std::to_string(i) + ")").substr(0, 8),
                  "INSERTED");
    }
    for (int i = 1; i <= 300; ++i) {
        ASSERT_EQ(db.Run("DELETE FROM h WHERE id = " + std::to_string(i)).substr(0, 7),
                  "DELETED");
    }
    // Shapes: a scan and a lookup on h, a lookup on b.
    db.Run("SELECT * FROM h");
    db.Run("SELECT * FROM h");
    db.Run("SELECT * FROM h WHERE id = 350");
    db.Run("SELECT * FROM b WHERE id = 2");
}

catalog::Oid OidOf(Instance& db, const char* name) {
    auto oid = db.catalog().FindTableOidByName(name);
    EXPECT_TRUE(oid.ok()) << oid.status().message();
    return oid.value();
}

const stats::RelationReport* FindReport(const std::vector<stats::RelationReport>& reports,
                                        const std::string& name) {
    for (const stats::RelationReport& r : reports) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

TEST(RelayoutPlannerTest, TheBareFormFetchesNoRelationPage) {
    Instance db;
    Load(db);

    db.store().ResetFetches();
    auto reports = stats::PlanAllRelations(db.catalog(), /*clock=*/nullptr, kHalfLife);
    ASSERT_TRUE(reports.ok()) << reports.status().message();
    EXPECT_FALSE(db.store().FetchedAnyUserPage())
        << "the all-relations form walked a relation, which R10 forbids by construction";

    // And it still said everything the stats can say: both relations, the
    // heap one's three gated plans, the btree one's empty candidate set.
    const stats::RelationReport* h = FindReport(reports.value(), "h");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->clustered_type, catalog::ClusteredType::kHeap);
    EXPECT_FALSE(h->survey.has_value());
    ASSERT_EQ(h->plans.size(), 3u);
    EXPECT_EQ(h->plans[0].kind, stats::RelayoutPlanKind::kCompact);
    EXPECT_EQ(h->plans[0].blocked_on, stats::RelayoutGate::kReaderHorizon);
    EXPECT_FALSE(h->plans[0].survey_backed);
    EXPECT_EQ(h->plans[0].predicted_pages_saved, 0u);
    EXPECT_EQ(h->plans[1].kind, stats::RelayoutPlanKind::kCluster);
    EXPECT_EQ(h->plans[1].blocked_on, stats::RelayoutGate::kOrderedBetween);
    EXPECT_EQ(h->plans[2].kind, stats::RelayoutPlanKind::kDefrag);
    EXPECT_EQ(h->plans[2].blocked_on, stats::RelayoutGate::kPageReuse);
    EXPECT_EQ(h->plans[0].measured_pages_saved, 0u);  // R9's pair: present, unpopulated

    bool has_scan = false;
    for (const stats::ShapeWeight& shape : h->shapes) {
        if (shape.kind == exec::AccessKind::kScan) {
            has_scan = true;
            EXPECT_GE(shape.use_count, 2u);
            // No clock on this dispatcher, so last_seen is 0 and the weight
            // degrades to the raw count - the stated best-effort stance.
            EXPECT_EQ(shape.decayed_weight, shape.use_count * stats::kDecayScoreScale);
        }
    }
    EXPECT_TRUE(has_scan) << "the scans above were not recorded";

    const stats::RelationReport* b = FindReport(reports.value(), "b");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->clustered_type, catalog::ClusteredType::kBtree);
    EXPECT_TRUE(b->plans.empty()) << "a btree relation has no v1 mover candidates (R5)";
    EXPECT_FALSE(b->shapes.empty());
}

TEST(RelayoutPlannerTest, TheSurveyedFormMeasuresAndPredicts) {
    Instance db;
    Load(db);

    const catalog::Oid h_oid = OidOf(db, "h");
    auto access = db.catalog().InitTableAccess(h_oid);
    ASSERT_TRUE(access.ok());

    exec::Budget budget;
    db.store().ResetFetches();
    auto report =
        stats::PlanRelation(db.catalog(), db.store(), h_oid, budget, /*clock=*/nullptr,
                            kHalfLife, /*core_id=*/0);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_TRUE(db.store().Fetched(access.value()->desc_page_id))
        << "the surveyed form is supposed to walk the chain, and did not";

    ASSERT_TRUE(report.value().survey.has_value());
    const stats::RelationSurvey& survey = *report.value().survey;
    EXPECT_EQ(survey.live_tuples, 100u);
    EXPECT_EQ(survey.delete_marked, 300u);
    EXPECT_EQ(survey.tuples_per_page,
              stats::TuplesPerPage(access.value()->layout.row_size));
    EXPECT_GT(survey.chain_pages, 1u) << "400 rows were meant to span pages";
    EXPECT_GT(budget.touched(), 0u) << "the walk was not charged";

    // The golden compact plan: live rows fit fewer pages than the chain
    // holds, the saving is exact arithmetic over the survey, and the gate
    // is still named - a measured benefit does not open it.
    ASSERT_EQ(report.value().plans.size(), 3u);
    const stats::RelayoutPlan& compact = report.value().plans[0];
    EXPECT_TRUE(compact.survey_backed);
    const std::uint64_t after =
        stats::PagesAfterCompact(survey.live_tuples, survey.tuples_per_page);
    ASSERT_GT(survey.chain_pages, after);
    EXPECT_EQ(compact.predicted_pages_saved, survey.chain_pages - after);
    EXPECT_EQ(compact.blocked_on, stats::RelayoutGate::kReaderHorizon);
    EXPECT_EQ(compact.predicted_benefit,
              stats::PredictedBenefit(compact.predicted_pages_saved,
                                      stats::WalkWeightOf(report.value().shapes)));
    EXPECT_GT(compact.predicted_benefit, 0u);
    EXPECT_EQ(compact.measured_pages_saved, 0u);
}

// ---- H3: the survey covers every range, not the lower one ----------------
//
// RD6 gave a relation one chain per range, and this survey kept walking
// `desc_page_id` - which heads the lo = 0 range and nothing more. The
// counts it reported were then the *lower* range's, and every density the
// planner derives from them was computed over an undersized relation.
//
// **A read-back test, not a fixed call site**, which is RB3's own lesson:
// its call-site inventory was believed and was wrong, and what caught it
// was a test that asked for the rows back. So this asserts the totals
// across a boundary rather than asserting that a particular head was
// fetched.
TEST(RelayoutPlannerTest, TheSurveyCoversEveryRangeOfASplitRelation) {
    Instance db;
    Load(db);

    const catalog::Oid h_oid = OidOf(db, "h");
    exec::Budget whole_budget;
    auto whole = stats::PlanRelation(db.catalog(), db.store(), h_oid, whole_budget,
                                     /*clock=*/nullptr, kHalfLife, /*core_id=*/0);
    ASSERT_TRUE(whole.ok()) << whole.status().message();
    ASSERT_TRUE(whole.value().survey.has_value());
    const stats::RelationSurvey before = *whole.value().survey;
    ASSERT_GT(before.live_tuples, 0u);

    // Split above every id the relation holds, so the upper range is empty
    // and the totals **must not move**. A survey that walked only the lower
    // chain would also not move, so this is the control rather than the
    // assertion - it pins that opening a boundary changes no count.
    auto head = db.catalog().CreateRangeEntryPage(h_oid, /*lo=*/1'000'000);
    ASSERT_TRUE(head.ok()) << head.status().message();
    ASSERT_TRUE(db.catalog().OpenRangeRows(h_oid, 1'000'000, /*owner_core=*/0, head.value()).ok());

    exec::Budget split_budget;
    auto split = stats::PlanRelation(db.catalog(), db.store(), h_oid, split_budget,
                                     /*clock=*/nullptr, kHalfLife, /*core_id=*/0);
    ASSERT_TRUE(split.ok()) << split.status().message();
    ASSERT_TRUE(split.value().survey.has_value());
    EXPECT_EQ(split.value().survey->live_tuples, before.live_tuples);
    EXPECT_EQ(split.value().survey->delete_marked, before.delete_marked);
    EXPECT_EQ(split.value().survey->surveyed_ranges, 2u);
    EXPECT_EQ(split.value().survey->relation_ranges, 2u);

    // **The assertion proper**: rows placed above the boundary land in the
    // upper range's own chain, and the survey must count them. Before H3
    // this read `before.live_tuples` - the upper chain was never walked.
    for (int i = 0; i < 5; ++i) {
        const std::string reply =
            db.Run("INSERT INTO h VALUES (" + std::to_string(1'000'000 + i) + ", 7)");
        ASSERT_NE(reply.find("INSERTED"), std::string::npos) << reply;
    }

    exec::Budget after_budget;
    auto after = stats::PlanRelation(db.catalog(), db.store(), h_oid, after_budget,
                                     /*clock=*/nullptr, kHalfLife, /*core_id=*/0);
    ASSERT_TRUE(after.ok()) << after.status().message();
    ASSERT_TRUE(after.value().survey.has_value());
    EXPECT_EQ(after.value().survey->live_tuples, before.live_tuples + 5)
        << "the survey missed the upper range's rows";
    EXPECT_GT(after.value().survey->chain_pages, before.chain_pages)
        << "the upper range's chain page was not counted";
}

TEST(RelayoutPlannerTest, TheSurveyRespectsTheRowBudget) {
    Instance db;
    Load(db);

    exec::Budget budget(10);  // 400 slots to walk: refused, not truncated
    auto report = stats::PlanRelation(db.catalog(), db.store(), OidOf(db, "h"), budget,
                                      /*clock=*/nullptr, kHalfLife, /*core_id=*/0);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.status().code(), StatusCode::kResourceExhausted)
        << report.status().message();
}

TEST(RelayoutPlannerTest, ABtreeRelationSurveysNothingAndPlansNothing) {
    Instance db;
    Load(db);

    exec::Budget budget;
    auto report = stats::PlanRelation(db.catalog(), db.store(), OidOf(db, "b"), budget,
                                      /*clock=*/nullptr, kHalfLife, /*core_id=*/0);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_FALSE(report.value().survey.has_value());
    EXPECT_TRUE(report.value().plans.empty());
    EXPECT_EQ(budget.touched(), 0u) << "a btree relation must not be walked";

    auto missing = stats::PlanRelation(db.catalog(), db.store(), /*rel_oid=*/999999, budget,
                                       /*clock=*/nullptr, kHalfLife, /*core_id=*/0);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), StatusCode::kNotFound);
}

}  // namespace
}  // namespace kds::server
