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
    void SplitAt(std::uint64_t lo, std::uint32_t owner_core = 0) {
        const catalog::Oid oid = TableOid();
        auto head = boot_->catalog.CreateRangeEntryPage(oid, lo);
        ASSERT_TRUE(head.ok()) << head.status().message();
        ASSERT_TRUE(boot_->catalog.OpenRangeRows(oid, lo, owner_core, head.value()).ok());
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

// ---- R4/IS2: the range's owner is who may write it ----------------------
//
// A **named** pk is the one route that reaches a range without this core's
// lease naming it (the omitted-pk route is routed by id before anything is
// written, R4/IS3), so it is where the placement check is reachable and
// where it is pinned. The refusal is by name, at the id, and not the
// store's `MayWrite` naming a page number after the fact.
TEST_F(RangeChainTest, AnInsertWhoseIdFallsInAnotherCoresRangeIsRefusedByName) {
    Run("INSERT INTO t VALUES (1)");
    Run("INSERT INTO t VALUES (2)");
    SplitAt(kBoundary, /*owner_core=*/2);

    const std::string refused =
        Run("INSERT INTO t VALUES (" + std::to_string(kBoundary) + ", 7)");
    EXPECT_EQ(refused.rfind("ERR TXN_CONFLICT retryable=1 ", 0), 0u) << refused;
    EXPECT_NE(refused.find("owned by core 2"), std::string::npos) << refused;

    auto ranges = RangesOfTable();
    ASSERT_EQ(ranges.size(), 2u);
    // Nothing was written into the range core 2 owns, and the lower range
    // still holds exactly the rows it held before the refusal.
    EXPECT_TRUE(IdsInChain(ranges[1].entry_page).empty())
        << "a row was placed in a chain this core does not own";
    EXPECT_EQ(IdsInChain(ranges[0].entry_page).size(), 2u);

    // **And the refusal cost the mark**, which is worth pinning rather than
    // discovering: `AdmitExplicitRowId` moved `next_id` past the named key
    // before the placement check ran, so the next *omitted* key is above
    // the boundary too. K3 calls a burnt id free, so this is a burn and not
    // a leak.
    //
    // What that next statement now meets is the **routing** refusal, not
    // the placement one, and the difference is the whole of R4/IS3: this
    // core reads the id it is about to issue, finds core 2 owns its range,
    // and declines to run the statement here at all. On a real instance it
    // would ship there; this fixture has one core and no ship client, so
    // the honest answer is the cross-core refusal. What must never appear
    // again is "the insert was routed to the wrong core", which is the
    // placement backstop firing because the routing above it did not.
    const std::string after = Run("INSERT INTO t VALUES (3)");
    EXPECT_EQ(after.rfind("ERR TXN_CONFLICT retryable=1 ", 0), 0u) << after;
    EXPECT_EQ(after.find("routed to the wrong core"), std::string::npos)
        << "the placement backstop answered a statement the router should have: " << after;
}

// ---- R4/IS4: a predicate-shaped write goes to its range's owner ---------
//
// Arming spreading costs something and this is where it is pinned rather
// than discovered: on a relation whose ranges have different owners, a
// write that names a primary key still runs (it touches one range), and a
// write that names none is **refused by name** until multi-range
// transactions exist. Both answers are given before a page is written.
TEST_F(RangeChainTest, APkNamedWriteRunsOnItsRangesOwnerAndAnUnnamedOneIsRefused) {
    Run("INSERT INTO t VALUES (1)");
    Run("INSERT INTO t VALUES (2)");
    SplitAt(kBoundary, /*owner_core=*/2);

    // A pk in **this** core's range: it runs, and the walk never reaches
    // the range core 2 owns.
    const std::string updated = Run("UPDATE t SET v = 9 WHERE id = 1");
    EXPECT_EQ(updated, "UPDATED 1") << updated;
    const std::string deleted = Run("DELETE FROM t WHERE id = 2");
    EXPECT_EQ(deleted, "DELETED 1") << deleted;

    // A pk in core 2's range: refused as the cross-core write it is,
    // retryably, and never answered "0 rows" - which is what a walk that
    // silently skipped the foreign range would have said.
    const std::string foreign =
        Run("UPDATE t SET v = 9 WHERE id = " + std::to_string(kBoundary));
    EXPECT_EQ(foreign.rfind("ERR TXN_CONFLICT retryable=1 ", 0), 0u) << foreign;

    // No pk at all: the statement could touch every range, so it spans two
    // owners and is refused naming R6. **Not** retryable - retrying changes
    // nothing until multi-range writes exist.
    const std::string spanning = Run("UPDATE t SET v = 9 WHERE v = 0");
    EXPECT_EQ(spanning.rfind("ERR ", 0), 0u) << spanning;
    EXPECT_NE(spanning.find("several owners"), std::string::npos) << spanning;
    EXPECT_EQ(spanning.find("retryable=1"), std::string::npos)
        << "a refusal that no retry can clear carried the retry bit: " << spanning;

    const std::string spanning_delete = Run("DELETE FROM t WHERE v = 0");
    EXPECT_NE(spanning_delete.find("several owners"), std::string::npos) << spanning_delete;
}

// The same relation with every range on **this** core keeps every write it
// had: a split is not by itself a restriction, two owners are.
TEST_F(RangeChainTest, ASplitRelationWithOneOwnerKeepsItsUnnamedWrites) {
    Run("INSERT INTO t VALUES (1)");
    SplitAt(kBoundary);
    Run("INSERT INTO t VALUES (" + std::to_string(kBoundary) + ", 7)");

    const std::string updated = Run("UPDATE t SET v = 9 WHERE v = 7");
    EXPECT_EQ(updated, "UPDATED 1") << updated;
    const std::string every = Run("UPDATE t SET v = 5");
    EXPECT_EQ(every, "UPDATED 2") << every;
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

// ---- RD8 / §8 test 9: range equivalence -----------------------------
//
// **The split as the only variable.** Two relations, identical rows,
// identical statements; one relation cut at a boundary the matching rows
// straddle. Every reply must be byte-identical - not "the same rows", the
// same bytes, which is what the spec asks for and what catches an
// ordering difference a set comparison would forgive.
//
// The straddle is required rather than incidental (§8 test 9 says so): a
// boundary no predicate crosses is a boundary nothing checks, and every
// defect this milestone found - the insert head, the range-blind walk,
// the resumed prefix, the fan-in's grouping - produced a right answer for
// data that stayed on one side.
class RangeEquivalenceTest : public RangeChainTest {
protected:
    void SetUp() override {
        RangeChainTest::SetUp();
        Run("CREATE TABLE u (id int64, v int64)");
        // `t` is cut at the boundary; `u` is not. Rows are placed by name
        // in **ascending** id order into both, so the two differ in one
        // thing only.
        auto oid = boot_->catalog.FindTableOidByName("t");
        ASSERT_TRUE(oid.ok());
        auto head = boot_->catalog.CreateRangeEntryPage(oid.value(), kBoundary);
        ASSERT_TRUE(head.ok()) << head.status().message();
        ASSERT_TRUE(
            boot_->catalog.OpenRangeRows(oid.value(), kBoundary, 0, head.value()).ok());

        for (std::uint64_t id : Ids()) {
            const std::string v = std::to_string(id * 2);
            Run("INSERT INTO t VALUES (" + std::to_string(id) + ", " + v + ")");
            Run("INSERT INTO u VALUES (" + std::to_string(id) + ", " + v + ")");
        }

        // **Without this the whole suite is vacuous.** Every test below
        // compares `t` against `u`; if the cut had not happened, or if
        // the rows had all landed on one side of it, they would compare a
        // relation against a relation and pass however broken ranges
        // were. So: two ranges, and rows in both chains.
        const std::vector<catalog::SysRangeRow> rows = RangesOfTable();
        ASSERT_EQ(rows.size(), 2u) << "the fixture did not split t";
        ASSERT_FALSE(IdsInChain(rows[0].entry_page).empty()) << "the lower range is empty";
        ASSERT_FALSE(IdsInChain(rows[1].entry_page).empty()) << "the upper range is empty";
    }

    // Ids on **both sides** of the boundary, and adjacent to it: the
    // boundary belongs to the range above, so `kBoundary - 1` and
    // `kBoundary` are the pair that catches an off-by-one in either the
    // resolver or the walk.
    static std::vector<std::uint64_t> Ids() {
        return {1, 2, kBoundary - 1, kBoundary, kBoundary + 1, kBoundary + 2};
    }

    // A reply's rows, sorted, so two answers can be compared as **sets**
    // where their sequences are allowed to differ (H5). The header line is
    // kept and sorts with the rest - it is identical in both replies, so it
    // cannot mask a difference, and dropping it would need this helper to
    // know which line it was.
    static std::vector<std::string> SortedRowsOf(const std::string& reply) {
        std::vector<std::string> rows;
        std::size_t at = 0;
        while (at <= reply.size()) {
            const std::size_t nl = reply.find("\\n", at);
            if (nl == std::string::npos) {
                rows.push_back(reply.substr(at));
                break;
            }
            rows.push_back(reply.substr(at, nl - at));
            at = nl + 2;  // the wire's two-character "\\n"
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    }

    // The reply for `sql` against `t` (split) and against `u` (whole),
    // with the relation name the only textual difference.
    void ExpectSame(const std::string& sql) {
        std::string split_sql = sql;
        std::string whole_sql = sql;
        const std::size_t at = whole_sql.find(" t");
        ASSERT_NE(at, std::string::npos) << sql;
        whole_sql.replace(at, 2, " u");
        const std::string split = Run(split_sql);
        const std::string whole = Run(whole_sql);
        EXPECT_EQ(split, whole) << "the split changed the answer for: " << sql;
        EXPECT_EQ(split.rfind("ERR", 0), std::string::npos)
            << "both sides refused, which proves nothing: " << split;
    }
};

TEST_F(RangeEquivalenceTest, AWholeScanIsByteIdentical) { ExpectSame("SELECT * FROM t"); }

TEST_F(RangeEquivalenceTest, APkEqualityIsByteIdenticalOnBothSidesOfTheBoundary) {
    for (std::uint64_t id : Ids()) {
        ExpectSame("SELECT * FROM t WHERE id = " + std::to_string(id));
    }
}

TEST_F(RangeEquivalenceTest, APkRangeStraddlingTheBoundaryIsByteIdentical) {
    // The case §8 test 9 names: matching rows on both sides of the cut.
    ExpectSame("SELECT * FROM t WHERE id BETWEEN " + std::to_string(kBoundary - 1) + " AND " +
               std::to_string(kBoundary + 1));
    ExpectSame("SELECT * FROM t WHERE id BETWEEN 1 AND " + std::to_string(kBoundary + 2));
}

TEST_F(RangeEquivalenceTest, ANonPkPredicateIsByteIdentical) {
    // Names no range, so the default is every range - the fan-out §2a
    // says the gating discipline makes unavoidable.
    ExpectSame("SELECT * FROM t WHERE v = " + std::to_string(kBoundary * 2));
    ExpectSame("SELECT * FROM t WHERE v > 2");
}

TEST_F(RangeEquivalenceTest, AnAggregateOverASplitRelationIsByteIdentical) {
    ExpectSame("SELECT COUNT(*) FROM t");
    ExpectSame("SELECT SUM(v) FROM t");
    ExpectSame("SELECT MIN(id) FROM t");
    ExpectSame("SELECT MAX(id) FROM t");
}

TEST_F(RangeEquivalenceTest, AnOrderedReadIsByteIdenticalAcrossTheBoundary) {
    // `ORDER BY` sorts, so it is the one shape whose answer cannot depend
    // on walk order at all - which makes it the control: if this differed,
    // the split would have changed the *rows*, not their order.
    ExpectSame("SELECT * FROM t ORDER BY id DESC");
    ExpectSame("SELECT * FROM t ORDER BY v ASC");
}

TEST_F(RangeEquivalenceTest, ADeleteAndAnUpdateAcrossTheBoundaryAgree) {
    // The write half of equivalence, and the one that exercises
    // `VisitRelation`'s per-range walk rather than the step VM's.
    ExpectSame("UPDATE t SET v = 999 WHERE id = " + std::to_string(kBoundary));
    ExpectSame("SELECT * FROM t WHERE v = 999");
    ExpectSame("DELETE FROM t WHERE id = " + std::to_string(kBoundary + 1));
    ExpectSame("SELECT * FROM t");
}

// ---- H5: the shapes RB5's review did not get to enumerate ---------------
//
// RB5's first open question was whether every shippable shape has an
// equivalence test. Answered by enumeration rather than by assertion: the
// suite above covers the whole scan, a pk equality, a pk range, a non-pk
// filter, the four aggregates, `ORDER BY`, and the UPDATE/DELETE pair.
// **Four shapes it did not cover**, each of which reaches the per-range
// walk by a different route, are below.
//
// `LIMIT` is the one worth naming: it *stops* the walk, so on a split
// relation it stops it inside a range - and a cut that ended the walk at
// the end of the first chain instead of continuing into the next would
// return the right number of rows and the wrong ones.
TEST_F(RangeEquivalenceTest, PaginationAcrossTheBoundaryIsByteIdentical) {
    ExpectSame("SELECT * FROM t LIMIT 2");
    // A limit that lands exactly on the lower range's last row, which is
    // where a walk that stopped at the chain's end would still look right.
    ExpectSame("SELECT * FROM t LIMIT 3");
    ExpectSame("SELECT * FROM t LIMIT 3 OFFSET 2");
    // Past the boundary: the rows can only come from the upper chain.
    ExpectSame("SELECT * FROM t LIMIT 2 OFFSET 4");
    ExpectSame("SELECT * FROM t ORDER BY id ASC LIMIT 3");
}

TEST_F(RangeEquivalenceTest, AProjectionListAcrossTheBoundaryIsByteIdentical) {
    // Not `SELECT *`: the projection path renders from the step's column
    // list rather than the relation's schema, and it is the shape the
    // remote fan-in refuses - so on a split relation it is served by the
    // local per-range walk and by nothing else.
    ExpectSame("SELECT v FROM t");
    ExpectSame("SELECT id, v FROM t WHERE v > 2");
}

TEST_F(RangeEquivalenceTest, AGroupedAggregateAcrossTheBoundaryIsByteIdentical) {
    // `GROUP BY` accumulates across the whole walk, so a missed range is a
    // missing group rather than a missing row - a difference that a
    // COUNT(*) alone would show as a smaller number and this shows as an
    // absent key.
    ExpectSame("SELECT v, COUNT(*) FROM t GROUP BY v");
    ExpectSame("SELECT v, SUM(id) FROM t GROUP BY v");
}

TEST_F(RangeEquivalenceTest, AJoinDrivenByASplitRelationIsByteIdentical) {
    // Written out rather than through `ExpectSame`, which swaps the
    // relation name and would turn a join with `u` into a self-join. A
    // third relation `w` is the other side, so the two statements differ in
    // exactly the cut relation - and `SELECT *` is ambiguous across two
    // relations, so the columns are named.
    Run("CREATE TABLE w (id int64, v int64)");
    for (std::uint64_t id : Ids()) {
        Run("INSERT INTO w VALUES (" + std::to_string(id) + ", " + std::to_string(id * 3) + ")");
    }

    // Compared **below the header line**, which carries the relation name
    // and therefore differs by construction (`t.id,...` against
    // `u.id,...`). The rows are the claim.
    const auto rows_of = [](const std::string& reply) {
        const std::size_t nl = reply.find("\\n");
        return nl == std::string::npos ? std::string() : reply.substr(nl + 2);
    };

    // The split relation as the **driving** side, which walks it once per
    // statement.
    const std::string split_drives = Run("SELECT t.id, t.v, w.v FROM t JOIN w ON t.id = w.id");
    const std::string whole_drives = Run("SELECT u.id, u.v, w.v FROM u JOIN w ON u.id = w.id");
    ASSERT_EQ(split_drives.rfind("ERR", 0), std::string::npos) << split_drives;
    ASSERT_FALSE(rows_of(split_drives).empty()) << "the join matched nothing: " << split_drives;
    EXPECT_EQ(rows_of(split_drives), rows_of(whole_drives))
        << "the split changed a join it drives";

    // And as the **inner**, which the walked shape may re-walk per outer
    // row - the resumed-prefix path RB3's review found re-walking every
    // later range.
    const std::string split_inner = Run("SELECT w.id, w.v, t.v FROM w JOIN t ON w.id = t.id");
    const std::string whole_inner = Run("SELECT w.id, w.v, u.v FROM w JOIN u ON w.id = u.id");
    ASSERT_EQ(split_inner.rfind("ERR", 0), std::string::npos) << split_inner;
    EXPECT_EQ(rows_of(split_inner), rows_of(whole_inner))
        << "the split changed a join it is the inner of";
}

TEST_F(RangeEquivalenceTest, TheEquivalenceRestsOnInsertionOrderMatchingRangeOrder) {
    // **Stated because it bounds the claim above.** A heap relation's rows
    // come back in *chain* order, which is insertion order; a split
    // relation's come back per range, concatenated in `lo` order. The two
    // coincide only while rows were inserted ascending - which the fixture
    // does, and which is what an engine-issued pk produces.
    //
    // Insert one row out of order into both and the *sets* still agree
    // while the byte-identity does not. That is a property of the heap's
    // unordered rows (invariant 4), not of ranges, and a reader who takes
    // "byte-identical" as unconditional would be surprised by it later.
    //
    // ---- H5: the premise this test rests on cannot occur here -----------
    //
    // RB5's review asked whether this asserts the interesting half of its
    // own claim. It did not - and the reason is sharper than "the
    // assertion was missing" (found 2026-08-29, H5/CH5).
    //
    // The setup was `INSERT INTO t VALUES (3, 6)` after ids up to
    // `kBoundary + 2` had been placed, described as "one row out of order".
    // **It was refused, on both relations**, and the test then compared two
    // unchanged relations and found them equal. A heap relation rejects a
    // caller-named key below its high-water mark - `OutOfRange`,
    // `heap-and-tuple.md` §4.1 - so an out-of-order insert is not something
    // this fixture can perform. The test was vacuous twice over: the
    // divergence never happened, and the only comparison made would have
    // agreed even if it had.
    //
    // **On one core the two orders cannot diverge at all.** The mark forces
    // ids ascending, a chain grows only at its tail, and ranges concatenate
    // in `lo` order - so chain order *is* id order *is* range order, and
    // there is nothing to catch.
    //
    // Where the divergence R4 introduces actually lives is **two cores**:
    // blocks are carved ascending, so a core holding an earlier block holds
    // lower ids, and if it inserts *later* the row order in time departs
    // from the row order in the id space. That needs a ring and two
    // runtimes - `CoreRuntimeTest.TwoPeersEachInsertIntoTheirOwnRangesTail`
    // is the fixture that has them, and it asserts ids ascend **per range**
    // rather than per relation for exactly this reason.
    const std::string refused = Run("INSERT INTO t VALUES (3, 6)");
    EXPECT_EQ(refused.rfind("ERR ", 0), 0u)
        << "a below-mark key was admitted on a heap relation, and this test's premise "
           "would then need rewriting rather than retiring: " << refused;

    // What *is* true and worth keeping: the split and whole relations agree
    // both ordered and unordered, over the rows the fixture could place.
    const std::string split_ordered = Run("SELECT * FROM t ORDER BY id ASC");
    const std::string whole_ordered = Run("SELECT * FROM u ORDER BY id ASC");
    EXPECT_EQ(split_ordered, whole_ordered) << "ordered, the two must still agree exactly";

    const std::string split_rows = Run("SELECT * FROM t");
    const std::string whole_rows = Run("SELECT * FROM u");
    ASSERT_EQ(split_rows.rfind("ERR ", 0), std::string::npos) << split_rows;
    EXPECT_EQ(SortedRowsOf(split_rows), SortedRowsOf(whole_rows));
    EXPECT_EQ(SortedRowsOf(split_rows).size(), Ids().size() + 1)
        << "every row the fixture placed, plus the header";
}

}  // namespace
}  // namespace kds::server
