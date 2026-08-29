#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// RD6 — per-range chains, through the dispatcher (work order
// `instructions/v2.5.0/range-directory.md` RB3; `crosscore.md` CC8).
//
// **The defect this row closes produces a wrong answer with nothing
// logged** (`TableAccess::HeapChainFor` states it), which is why it is
// tested end to end here and not only at that function: nothing refuses
// anywhere along the route, so only a test that inserts across a boundary
// and then reads back can see it. That is not a hypothetical standard -
// the insert sites were already fixed when this file first ran, and
// `AScanOverASplitRelationReturnsEveryRangesRows` is what found that the
// step VM's walk was still range-blind.
//
// The split here is made through the catalog directly rather than through
// the row-id lease: this fixture is one core with no ring, and what RD6
// owns is what happens *after* a boundary exists, whoever wrote it.

namespace kds::server {
namespace {

class RangeChainTest : public ::testing::Test {
protected:
    static constexpr std::uint64_t kBoundary = 4096;

    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        // The manager is what admits a multi-row VALUES at all (BI4: a
        // manager-less configuration cannot unwind a partially placed
        // statement and is refused upfront), and the straddle refusal
        // below is a property of that path.
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        Run("CREATE TABLE t (id int64, v int64)");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    catalog::Oid TableOid() {
        auto oid = boot_->catalog.FindTableOidByName("t");
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        return oid.ok() ? oid.value() : 0;
    }

    // The cut: CC10's two halves with nothing between them, because on one
    // core there is no page to hand off and no peer to tell.
    void SplitAt(std::uint64_t lo) {
        const catalog::Oid oid = TableOid();
        auto head = boot_->catalog.CreateRangeEntryPage(oid, lo);
        ASSERT_TRUE(head.ok()) << head.status().message();
        ASSERT_TRUE(boot_->catalog.OpenRangeRows(oid, lo, /*owner_core=*/0, head.value()).ok());
    }

    std::vector<catalog::SysRangeRow> RangesOfTable() {
        auto ranges = boot_->catalog.RangesOf(TableOid());
        EXPECT_TRUE(ranges.ok()) << ranges.status().message();
        return ranges.ok() ? ranges.value() : std::vector<catalog::SysRangeRow>{};
    }

    // Every id the chain rooted at `head` holds, live rows only.
    std::vector<std::uint64_t> IdsInChain(PageId head) {
        std::vector<std::uint64_t> ids;
        EXPECT_TRUE(heap::ChainVisit(store_, head, storage::PageAccess::kRead,
                                     [&](PageId, heap::PageView& page,
                                         std::uint16_t slot) -> StatusOr<storage::VisitControl> {
                                         auto tuple = page.ReadTuple(slot);
                                         if (!tuple.ok() || tuple.value().deleted) {
                                             return storage::VisitControl::kContinue;
                                         }
                                         auto id = KeystoneIdOfPayload(tuple.value().payload);
                                         EXPECT_TRUE(id.ok());
                                         if (id.ok()) ids.push_back(id.value());
                                         return storage::VisitControl::kContinue;
                                     })
                        .ok());
        return ids;
    }

    storage::InMemoryPageStore store_{1000};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(RangeChainTest, APostSplitInsertLandsInTheRangeItsIdNames) {
    Run("INSERT INTO t VALUES (1)");
    Run("INSERT INTO t VALUES (2)");
    SplitAt(kBoundary);

    auto ranges = boot_->catalog.RangesOf(TableOid());
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u);
    const PageId lower = ranges.value()[0].entry_page;
    const PageId upper = ranges.value()[1].entry_page;

    // A named pk at or above the high-water mark, which is what a heap
    // relation admits (heap-and-tuple.md §4.1) and the only way to reach
    // the upper range without burning four thousand ids.
    const std::string reply = Run("INSERT INTO t VALUES (" + std::to_string(kBoundary) + ", 7)");
    EXPECT_NE(reply.find("INSERTED"), std::string::npos) << reply;

    // **The row is in the upper chain and not in the lower one.** Before
    // RD6 both of these failed the other way round, silently.
    const std::vector<std::uint64_t> upper_ids = IdsInChain(upper);
    ASSERT_EQ(upper_ids.size(), 1u) << "the row did not land in the range its id names";
    EXPECT_EQ(upper_ids[0], kBoundary);

    const std::vector<std::uint64_t> lower_ids = IdsInChain(lower);
    EXPECT_EQ(lower_ids.size(), 2u) << "a row belonging above the boundary landed below it";
}

TEST_F(RangeChainTest, AScanOverASplitRelationReturnsEveryRangesRows) {
    Run("INSERT INTO t VALUES (1)");
    Run("INSERT INTO t VALUES (2)");
    SplitAt(kBoundary);
    Run("INSERT INTO t VALUES (" + std::to_string(kBoundary) + ", 7)");
    Run("INSERT INTO t VALUES (" + std::to_string(kBoundary + 1) + ", 8)");

    // §8 test 9's substance at one core: the split is the only variable,
    // and a walk that stopped at `desc_page_id`'s chain would return two
    // rows and report success.
    //
    // Asserted on the **row count** and on the two ids only the upper
    // chain can produce. Searching for "1" and "2" as substrings would
    // have passed on almost any reply - the answer is `id,v` pairs and a
    // `1` appears in `4096` - so a count is what makes "every range's
    // rows" a claim rather than a coincidence.
    const std::string all = Run("SELECT * FROM t");
    // The wire form separates rows with a literal two-character `\n`, not
    // a newline (the text protocol keeps one reply on one line), so the
    // separator count is the row count.
    std::size_t rows = 0;
    for (std::size_t at = all.find("\\n"); at != std::string::npos; at = all.find("\\n", at + 1)) {
        ++rows;
    }
    EXPECT_EQ(rows, 4u) << "expected a header and four rows, got: " << all;
    EXPECT_NE(all.find("4096"), std::string::npos) << all;
    EXPECT_NE(all.find("4097"), std::string::npos) << all;
}

TEST_F(RangeChainTest, AResumedPrefixWalkCoversEachRangeOnceRatherThanTwice) {
    Run("CREATE TABLE u (id int64, v int64)");
    Run("INSERT INTO t VALUES (1), (2), (3)");
    SplitAt(kBoundary);
    Run("INSERT INTO t VALUES (4096, 500)");
    Run("INSERT INTO t VALUES (4097, 501)");
    Run("INSERT INTO t VALUES (4098, 502)");

    // JB6's resume, over a boundary. The first outer row probes a value
    // that only the *upper* range holds, so the sub-chain's walk is cut
    // there and the mark it leaves names a range that is not the first;
    // the second probes a value no range holds, so it misses the prefix
    // and resumes from that mark.
    Run("INSERT INTO u VALUES (500)");
    Run("INSERT INTO u VALUES (999)");

    const std::string plan =
        Run("ANALYZE SELECT u.id FROM u WHERE EXISTS (SELECT t.id FROM t WHERE t.v = u.v)");
    // t holds six rows, and **every inner row is bucketed at most once per
    // statement** - join-inner-build.md §6's economics, and the property a
    // resume that restarted at the first range breaks: it would walk the
    // upper range a second time and bucket its rows again (build_rows=8).
    EXPECT_NE(plan.find("build_rows=6"), std::string::npos) << plan;
}

TEST_F(RangeChainTest, AResumedPrefixWalkCrossesTheBoundaryItStoppedBefore) {
    // The sibling of the test above, and the case it cannot reach: there
    // the mark lands in the **last** range, so a resume that never steps
    // to a next one is indistinguishable from a correct one. Here the mark
    // lands in the *first*, and the row the second outer probe needs is in
    // the second - so a resume that does not cross the boundary reports a
    // row that exists as absent, and sets `complete` while saying it.
    Run("CREATE TABLE u (id int64, v int64)");
    Run("INSERT INTO t VALUES (100), (101), (102)");
    SplitAt(kBoundary);
    Run("INSERT INTO t VALUES (4096, 500)");
    Run("INSERT INTO t VALUES (4097, 501)");

    // First outer row matches the *first* row of the lower range, so the
    // sub-chain's walk is cut one row in and the mark it leaves is inside
    // range 0 with both ranges still uncovered.
    Run("INSERT INTO u VALUES (100)");
    // Second outer row matches nothing the prefix holds and only the upper
    // range can answer it.
    Run("INSERT INTO u VALUES (501)");

    const std::string out =
        Run("SELECT u.id FROM u WHERE EXISTS (SELECT t.id FROM t WHERE t.v = u.v)");
    std::size_t rows = 0;
    for (std::size_t at = out.find("\\n"); at != std::string::npos; at = out.find("\\n", at + 1)) {
        ++rows;
    }
    EXPECT_EQ(rows, 2u) << "a resume that stopped at the boundary answers one row: " << out;
}

TEST_F(RangeChainTest, APkLookupAcrossTheBoundaryFindsItsRow) {
    Run("INSERT INTO t VALUES (1)");
    SplitAt(kBoundary);
    Run("INSERT INTO t VALUES (" + std::to_string(kBoundary) + ", 7)");

    // The reader's half of the same defect: the pk names the upper range,
    // and before RD6 the row was in the lower one, so this answered zero
    // rows with no error.
    const std::string hit = Run("SELECT * FROM t WHERE id = 4096");
    EXPECT_NE(hit.find("4096"), std::string::npos) << hit;

    const std::string below = Run("SELECT * FROM t WHERE id = 1");
    EXPECT_NE(below.find("1"), std::string::npos) << below;
}

TEST_F(RangeChainTest, EachRangeGrowsItsOwnChainRatherThanTheRelationsOne) {
    SplitAt(kBoundary);
    auto ranges = boot_->catalog.RangesOf(TableOid());
    ASSERT_TRUE(ranges.ok());
    const PageId lower = ranges.value()[0].entry_page;
    const PageId upper = ranges.value()[1].entry_page;

    // Enough rows in each to make both chains grow, which is where a
    // shared tail hint would show: a hint from the other chain is the
    // logic error `heap_chain.hpp` says that layer cannot detect.
    for (int i = 0; i < 200; ++i) Run("INSERT INTO t VALUES (" + std::to_string(i + 1) + ", 1)");
    for (int i = 0; i < 200; ++i) {
        Run("INSERT INTO t VALUES (" + std::to_string(kBoundary + i) + ", 2)");
    }

    const std::vector<std::uint64_t> lower_ids = IdsInChain(lower);
    const std::vector<std::uint64_t> upper_ids = IdsInChain(upper);
    EXPECT_EQ(lower_ids.size(), 200u);
    EXPECT_EQ(upper_ids.size(), 200u);
    for (std::uint64_t id : lower_ids) EXPECT_LT(id, kBoundary) << "id " << id << " is above the boundary";
    for (std::uint64_t id : upper_ids) EXPECT_GE(id, kBoundary) << "id " << id << " is below the boundary";
}

TEST_F(RangeChainTest, AMultiRowInsertStayingInOneRangeIsAdmitted) {
    SplitAt(kBoundary);
    // The batch path takes one chain for the whole contiguous run, so a
    // run inside one range is the ordinary case and must be unaffected by
    // the straddle refusal below it.
    const std::string reply = Run("INSERT INTO t VALUES (1), (2), (3)");
    EXPECT_EQ(reply.find("ERR"), std::string::npos) << reply;
    EXPECT_EQ(IdsInChain(RangesOfTable()[0].entry_page).size(), 3u);
}

TEST_F(RangeChainTest, AMultiRowInsertCrossingTheBoundaryIsRefusedRatherThanStraddling) {
    SplitAt(kBoundary);
    // The high-water mark moved to two below the boundary, so a three-row
    // run must cross it. A straddle would put one statement's rows in two
    // chains, which is the cross-range DML `crosscore.md:311-314` answers
    // with a retryable refusal - the alternative is a silent half-write.
    auto carve = boot_->catalog.AllocateRowIdRange(TableOid(), kBoundary - 3);
    ASSERT_TRUE(carve.ok()) << carve.status().message();

    const std::string reply = Run("INSERT INTO t VALUES (1), (2), (3)");
    EXPECT_NE(reply.find("ERR"), std::string::npos) << reply;
    EXPECT_NE(reply.find("range boundary"), std::string::npos) << reply;

    // And nothing landed: a refused statement is not a partial one.
    EXPECT_TRUE(IdsInChain(RangesOfTable()[0].entry_page).empty()) << "a refused batch wrote rows";
    EXPECT_TRUE(IdsInChain(RangesOfTable()[1].entry_page).empty());
}

}  // namespace
}  // namespace kds::server
