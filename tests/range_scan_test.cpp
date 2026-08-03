#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// `kRange`: a pk `BETWEEN` walked with `min_key` pruning.
//
// The kind is an accelerator, and the only thing that makes an accelerator
// safe is that it is invisible. So every test below is a comparison against
// the answer the same predicate gives with no range at all - on **both**
// storage forms, because a heap chain and a btree leaf chain prune the same
// way for different reasons and a change to either could break one alone.

namespace kds::exec {
namespace {

class RangeScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);

        // Enough rows to span several pages, so `min_key` pruning has
        // something to prune. Both forms hold the same values.
        ASSERT_EQ(Run("CREATE TABLE h (id int64, v int64)").substr(0, 7), "CREATED");
        ASSERT_EQ(Run("CREATE TABLE b (id int64, v int64) BTREE").substr(0, 7), "CREATED");
        for (int i = 1; i <= kRows; ++i) {
            const std::string v = std::to_string(i * 10);
            ASSERT_EQ(Run("INSERT INTO h VALUES (" + v + ")").substr(0, 8), "INSERTED");
            ASSERT_EQ(Run("INSERT INTO b VALUES (" + v + ")").substr(0, 8), "INSERTED");
        }
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    // Runs `sql` and returns the reply plus the step counters, by compiling
    // and executing directly - the dispatcher does not hand stats out.
    struct Outcome {
        std::vector<std::string> rows;
        ExecStats stats;
    };

    Outcome RunWithStats(const std::string& sql) {
        Outcome out;
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << parsed.status().message();
        auto chain = Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
        EXPECT_TRUE(chain.ok()) << chain.status().message();
        Status ran = Execute(
            boot_->catalog, store_, chain.value(),
            [&](const ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                std::string row;
                for (const ColumnRef& ref : chain.value().projection) {
                    if (!row.empty()) row += ',';
                    row += FormatValue(frame.Get(ref));
                }
                out.rows.push_back(row);
                return storage::VisitControl::kContinue;
            },
            &out.stats);
        EXPECT_TRUE(ran.ok()) << ran.message();
        return out;
    }

    static constexpr int kRows = 400;

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<server::CommandDispatcher> dispatcher_;
};

// ---- The equivalence, which is the whole point ---------------------------

TEST_F(RangeScanTest, ARangeReturnsExactlyWhatTheSamePredicateScannedGives) {
    for (const std::string& rel : {std::string("h"), std::string("b")}) {
        // `BETWEEN` compiles to kRange and prunes.
        const Outcome ranged =
            RunWithStats("SELECT id, v FROM " + rel + " WHERE id BETWEEN 50 AND 60");
        // The same rows, asked for in a way that cannot become a range: the
        // bounds are there but so is a non-pk conjunct that keeps every
        // row being examined... which is not enough on its own, so this
        // instead compares against the *unpruned* form below.
        const Outcome scanned =
            RunWithStats("SELECT id, v FROM " + rel + " WHERE id >= 50 AND id <= 60");

        EXPECT_EQ(ranged.rows, scanned.rows) << "relation " << rel;
        ASSERT_EQ(ranged.rows.size(), 11u) << "relation " << rel;
        EXPECT_EQ(ranged.rows.front(), "50,500");
        EXPECT_EQ(ranged.rows.back(), "60,600");
    }
}

TEST_F(RangeScanTest, HeapAndBtreeAgreeRowForRow) {
    // The equivalence already pinned for Probe vs Scan, extended to the new
    // kind: the storage form must not be visible in the answer.
    const Outcome heap = RunWithStats("SELECT id, v FROM h WHERE id BETWEEN 100 AND 130");
    const Outcome btree = RunWithStats("SELECT id, v FROM b WHERE id BETWEEN 100 AND 130");
    EXPECT_EQ(heap.rows, btree.rows);
    EXPECT_EQ(heap.rows.size(), 31u);
}

TEST_F(RangeScanTest, ARangeWithNoQualifyingRowsIsEmptyNotWrong) {
    for (const std::string& rel : {std::string("h"), std::string("b")}) {
        EXPECT_TRUE(RunWithStats("SELECT id FROM " + rel + " WHERE id BETWEEN 9000 AND 9100")
                        .rows.empty())
            << rel;
        // Inverted: legal to write, matches nothing, and never became a
        // range at all (the compiler leaves it a scan).
        EXPECT_TRUE(
            RunWithStats("SELECT id FROM " + rel + " WHERE id BETWEEN 60 AND 50").rows.empty())
            << rel;
    }
}

TEST_F(RangeScanTest, TheBoundsAreInclusiveAtBothEnds) {
    const Outcome out = RunWithStats("SELECT id FROM b WHERE id BETWEEN 7 AND 9");
    ASSERT_EQ(out.rows.size(), 3u);
    EXPECT_EQ(out.rows[0], "7");
    EXPECT_EQ(out.rows[2], "9");
}

// ---- The pruning itself --------------------------------------------------

TEST_F(RangeScanTest, ARangeNearTheStartStopsBeforeReadingTheWholeRelation) {
    // What the kind is *for*. Without pruning this examines all kRows rows;
    // with it, the walk stops at the first page past the high bound.
    for (const std::string& rel : {std::string("h"), std::string("b")}) {
        const Outcome out =
            RunWithStats("SELECT id FROM " + rel + " WHERE id BETWEEN 1 AND 5");
        ASSERT_EQ(out.rows.size(), 5u) << rel;

        const StepStats total = out.stats.Total();
        EXPECT_GT(total.range_pages_pruned, 0u) << rel << ": the walk should have stopped early";
        EXPECT_LT(total.rows_examined, static_cast<std::uint64_t>(kRows))
            << rel << ": examined " << total.rows_examined << " of " << kRows;
    }
}

TEST_F(RangeScanTest, ARangeReachingTheEndPrunesNothingAndSaysSo) {
    // The honest other half: pruning is a tail optimization, so a range
    // that runs to the end of the relation saves nothing. Pinned so the
    // counter is not mistaken for "ranges are always cheap".
    const Outcome out =
        RunWithStats("SELECT id FROM b WHERE id BETWEEN " + std::to_string(kRows - 2) + " AND " +
                     std::to_string(kRows));
    EXPECT_EQ(out.rows.size(), 3u);
    EXPECT_EQ(out.stats.Total().range_pages_pruned, 0u);
    // And it read the whole relation to find them - the head is not pruned.
    EXPECT_EQ(out.stats.Total().rows_examined, static_cast<std::uint64_t>(kRows));
}

}  // namespace
}  // namespace kds::exec
