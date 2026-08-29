#include <algorithm>
#include <cctype>
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
        // The third relation, unsplit and named by neither substitution:
        // the join and subquery shapes below need a partner that is *not*
        // the twin, or they would compare `t JOIN u` against `u JOIN u`,
        // which is refused (a binding may not repeat).
        Run("CREATE TABLE j (id int64, v int64)");
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
        // `j` matches one row from **each** range, so a join or a subquery
        // over it has to reach both to answer - the straddle discipline
        // one relation over.
        Run("INSERT INTO j VALUES (1, 2)");
        Run("INSERT INTO j VALUES (2, " + std::to_string(kBoundary * 2) + ")");

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

    // `sql` with every standalone identifier `t` rewritten to `u`, which
    // is how the whole-relation twin of a statement is spelled.
    //
    // **Token-level, not the first `" t"`.** The original substitution
    // replaced one occurrence, and RB5's review had to check each call
    // site by hand to confirm it hit the right one. A qualified name
    // (`SELECT t.id FROM t`) hits the wrong one and a join names the
    // relation twice, so rewriting every identifier occurrence is what
    // makes the substitution correct by construction rather than by
    // inspection - and it is what lets the shapes below be joins and
    // subqueries at all.
    static std::string AgainstTheWholeRelation(const std::string& sql) {
        std::string out;
        out.reserve(sql.size());
        for (std::size_t i = 0; i < sql.size(); ++i) {
            const bool starts =
                i == 0 || !(std::isalnum(static_cast<unsigned char>(sql[i - 1])) ||
                            sql[i - 1] == '_' || sql[i - 1] == '.');
            const bool ends = i + 1 == sql.size() ||
                              !(std::isalnum(static_cast<unsigned char>(sql[i + 1])) ||
                                sql[i + 1] == '_');
            out.push_back(sql[i] == 't' && starts && ends ? 'u' : sql[i]);
        }
        return out;
    }

    // The reply for `sql` against `t` (split) and against `u` (whole),
    // with the relation name the only textual difference.
    //
    // A **qualified** projection (`SELECT t.id`) puts the relation's name
    // in the reply's header, so the substitution that makes the twin
    // statement also changes the twin's header - `u.id` against `t.id`.
    // That is the substitution showing through, not the split, so the
    // header is renamed back before the comparison and the rows are
    // compared untouched. Restricted to the header on purpose: a value
    // containing `u.` must never be rewritten.
    void ExpectSame(const std::string& sql) {
        const std::string whole_sql = AgainstTheWholeRelation(sql);
        ASSERT_NE(whole_sql, sql) << "the statement names no relation to substitute: " << sql;
        const std::string split = Run(sql);
        std::string whole = Run(whole_sql);
        const std::size_t body = whole.find("\\n");
        for (std::size_t at = whole.find("u."); at != std::string::npos && at < body;
             at = whole.find("u.", at + 1)) {
            whole[at] = 't';
        }
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

// ---- RB5's first review item: the shapes the suite did not cover ------
//
// The review asked *"does every shippable shape have an equivalence
// test"* and the answer was no. Eight shapes were covered - the whole
// scan, pk equality, a pk range, non-pk predicates, four aggregates,
// ordered reads and the write half - and the ones below were not. They
// are not filler: each is a route that reaches the per-range walk
// differently from the eight, and three of them (LIMIT, GROUP BY, the
// correlated subquery) are places where a range-blind implementation
// returns a **plausible short answer** rather than an error, which is the
// exact defect class every one of this milestone's reviews caught.

TEST_F(RangeEquivalenceTest, AProjectionIsByteIdentical) {
    // `SELECT *` and a projected list take different emit paths; the
    // eight shapes only ever asked for the star.
    ExpectSame("SELECT id FROM t");
    ExpectSame("SELECT v, id FROM t");
}

TEST_F(RangeEquivalenceTest, ALimitCrossingTheBoundaryIsByteIdentical) {
    // **The early-termination shape, and the one most able to lie.** A
    // walk that satisfies the limit inside the lower range never reaches
    // the boundary, so a range-blind continuation is invisible until the
    // limit *exceeds* the lower range's three rows. All three cases:
    // stopping short of the cut, stopping exactly on it, and crossing it.
    ExpectSame("SELECT * FROM t LIMIT 2");
    ExpectSame("SELECT * FROM t LIMIT 3");
    ExpectSame("SELECT * FROM t LIMIT 4");
    ExpectSame("SELECT * FROM t LIMIT 100");
    // And the offset, which is the other half of `LIMIT n OFFSET m` means
    // "rows [m, m+n) of the unlimited reply": an offset consumed per
    // range rather than per relation would skip three rows twice.
    ExpectSame("SELECT * FROM t LIMIT 2 OFFSET 2");
    ExpectSame("SELECT * FROM t LIMIT 3 OFFSET 3");
    ExpectSame("SELECT * FROM t LIMIT 0");
}

TEST_F(RangeEquivalenceTest, AnOrderedLimitIsByteIdentical) {
    // `ORDER BY` under `LIMIT` is the top-N heap path, not the sort path,
    // and it is fed by the walk this milestone changed.
    ExpectSame("SELECT * FROM t ORDER BY id DESC LIMIT 2");
    ExpectSame("SELECT * FROM t ORDER BY v ASC LIMIT 4");
}

TEST_F(RangeEquivalenceTest, AGroupedAggregateWhoseGroupStraddlesTheBoundaryIsByteIdentical) {
    // **A group with a member in each range**, which is the aggregate
    // shape a boundary can break: per-range partials must merge into one
    // group, not emit the group twice. The four aggregates already
    // covered are ungrouped, so nothing tested the merge.
    for (const char* rel : {"t", "u"}) {
        const std::string r = rel;
        Run("UPDATE " + r + " SET v = 100 WHERE id = 1");
        Run("UPDATE " + r + " SET v = 100 WHERE id = " + std::to_string(kBoundary));
    }
    ExpectSame("SELECT v, COUNT(*) FROM t GROUP BY v");
    ExpectSame("SELECT v, SUM(id) FROM t GROUP BY v");
    ExpectSame("SELECT v, MIN(id), MAX(id) FROM t GROUP BY v");
}

TEST_F(RangeEquivalenceTest, TheRemainingAggregateFormsAreByteIdentical) {
    // `COUNT(DISTINCT)` carries a **set** across the boundary rather than
    // a scalar, which is the accumulator shape neither `SUM` nor `COUNT`
    // exercises. (`AVG` is not here: it requires a DECIMAL column and
    // both columns are `int64`, so it refuses on both sides - a refusal
    // `ExpectSame` correctly calls proof of nothing.)
    ExpectSame("SELECT COUNT(DISTINCT v) FROM t");
    // An aggregate under a predicate that straddles: the filter runs per
    // range and the accumulator does not.
    ExpectSame("SELECT COUNT(*) FROM t WHERE id BETWEEN 2 AND " + std::to_string(kBoundary));
}

TEST_F(RangeEquivalenceTest, AJoinOverTheSplitRelationIsByteIdenticalOnEitherSide) {
    // The split relation as the **outer** of the join, driving the walk...
    ExpectSame("SELECT t.id, j.id FROM t JOIN j ON t.v = j.v");
    // ...and as the **inner**, where it is walked once per outer row and
    // a range-blind walk answers a subset without saying so.
    ExpectSame("SELECT j.id, t.id FROM j JOIN t ON j.v = t.v");
    // A pk-keyed join, which executes as a probe rather than a scan and
    // so resolves the range per outer row instead of walking every one.
    //
    // **`j` needs a key above the cut for this to say anything.** Its two
    // fixture rows are ids 1 and 2, so every probe resolves to the lower
    // range and a resolver that answered "range 0" unconditionally would
    // pass - the vacuity this file's other shapes are built to avoid.
    // This id is above `j`'s own high-water mark, so `j` admits it -
    // asserted, because a refusal here would return the shape to being
    // vacuous and both sides would still agree while it was.
    ASSERT_EQ(Run("INSERT INTO j VALUES (" + std::to_string(kBoundary) + ", 1)").rfind("ERR", 0),
              std::string::npos);
    ExpectSame("SELECT j.id, t.v FROM j JOIN t ON j.id = t.id");
}

TEST_F(RangeEquivalenceTest, ASubqueryOverTheSplitRelationIsByteIdentical) {
    // Correlated `EXISTS` with the split relation inside - the shape
    // `AResumedPrefixWalkCrossesTheBoundaryItStoppedBefore` found a live
    // defect in, here as an equivalence rather than a row count.
    ExpectSame("SELECT j.id FROM j WHERE EXISTS (SELECT t.id FROM t WHERE t.v = j.v)");
    ExpectSame("SELECT j.id FROM j WHERE NOT EXISTS (SELECT t.id FROM t WHERE t.v = j.v)");
    // Uncorrelated `IN` / `NOT IN`, which materialise the inner side once.
    ExpectSame("SELECT j.id FROM j WHERE j.v IN (SELECT t.v FROM t)");
    ExpectSame("SELECT j.id FROM j WHERE j.v NOT IN (SELECT t.v FROM t)");
    // And the split relation on the **outside**, with the unsplit one in
    // the predicate: the walk is the split relation's and the subquery is
    // evaluated per row of it.
    ExpectSame("SELECT t.id FROM t WHERE t.v IN (SELECT j.v FROM j)");
    // A scalar subquery, which is a third evaluation shape again. (An
    // *aggregate* inside a subquery is refused engine-wide, AG8, so the
    // scalar has to be a plain single-row select.)
    ExpectSame("SELECT t.id FROM t WHERE t.v = (SELECT j.v FROM j WHERE j.id = 2)");
}

TEST_F(RangeEquivalenceTest, TheRemainingComparisonOperatorsAreByteIdenticalAcrossTheBoundary) {
    // The eight shapes used `=`, `>`, and `BETWEEN`. The rest of
    // `PredicateKind`'s value comparisons, each with its match set
    // straddling the cut - an operator whose pk bound is derived wrong
    // narrows the resolved range set and drops the far side silently.
    const std::string b = std::to_string(kBoundary);
    ExpectSame("SELECT * FROM t WHERE id != " + b);
    ExpectSame("SELECT * FROM t WHERE id >= " + std::to_string(kBoundary - 1));
    ExpectSame("SELECT * FROM t WHERE id <= " + std::to_string(kBoundary + 1));
    ExpectSame("SELECT * FROM t WHERE id < " + std::to_string(kBoundary + 2));
    ExpectSame("SELECT * FROM t WHERE id > 1");
    // A predicate matching nothing must also agree: "no rows" from a walk
    // that never crossed the boundary reads the same as "no rows" from
    // one that did, and only the split/whole comparison separates them.
    ExpectSame("SELECT * FROM t WHERE id = " + std::to_string(kBoundary * 4));
    ExpectSame("SELECT * FROM t WHERE v = 999999");
}

TEST_F(RangeEquivalenceTest, AnUnpredicatedWriteAcrossTheBoundaryAgrees) {
    // The write half covered a pk-named `UPDATE` and `DELETE`. The
    // unpredicated forms are the ones that must touch **every** range,
    // and they are legal here because every range is one core's (§7b: a
    // split is not by itself a restriction, two owners are).
    ExpectSame("UPDATE t SET v = 5");
    ExpectSame("SELECT * FROM t");
    // Straddling, and **narrow on purpose**: it takes one row from each
    // side rather than everything above `kBoundary - 1`, so the
    // unpredicated `DELETE` below still has rows in *both* ranges to
    // remove. Deleting four here left it two rows in the lower range
    // alone, which is the one thing the unpredicated form exists to test.
    ExpectSame("DELETE FROM t WHERE id BETWEEN " + std::to_string(kBoundary - 1) + " AND " +
               std::to_string(kBoundary));
    ExpectSame("SELECT * FROM t");
    ExpectSame("DELETE FROM t");
    ExpectSame("SELECT COUNT(*) FROM t");
}

// **The bound on every claim above, asserted rather than described.**
//
// Byte-identity holds because a heap relation **refuses** a named key
// below its high-water mark (`catalog.cpp`'s `AdmitExplicitRowId`,
// heap-and-tuple.md §3.1b/§4.1): chain order and range order coincide only
// while rows arrive ascending, and on one core nothing else is admitted.
// So the out-of-order insert RB5's version described in a comment is not
// merely untested here, it is unconstructible.
//
// The falsifier lives where R4 makes it reachable - each core issues from
// its **own leased block**, which is never checked against the mark - and
// is `core_runtime_test.cpp`'s `TwoPeersEachInsertIntoTheirOwnRangesTail`,
// round 3.
TEST_F(RangeEquivalenceTest, AnOutOfOrderInsertIsRefusedSoOneCoreCannotBreakTheByteIdentity) {
    // Id 3 sorts into the *lower* range, whose chain already ends at
    // `kBoundary - 1`; in a whole relation it would land at the tail,
    // after `kBoundary + 2`. Both relations refuse it, by the same rule,
    // and the messages differ only in the oid they name.
    const std::string into_split = Run("INSERT INTO t VALUES (3, 6)");
    const std::string into_whole = Run("INSERT INTO u VALUES (3, 6)");
    for (const std::string& refusal : {into_split, into_whole}) {
        EXPECT_EQ(refusal.rfind("ERR primary key 3 is below relation ", 0), 0u) << refusal;
        EXPECT_NE(refusal.find("high-water mark 4099"), std::string::npos) << refusal;
        EXPECT_NE(refusal.find("ids must ascend"), std::string::npos) << refusal;
    }

    // So the two still agree, and they agree **because** the ascent holds
    // rather than because ranges preserve an arbitrary order.
    EXPECT_EQ(Run("SELECT * FROM t"), Run("SELECT * FROM u"));
    EXPECT_EQ(Run("SELECT * FROM t ORDER BY id ASC"), Run("SELECT * FROM u ORDER BY id ASC"));

    // And the refusal left nothing behind: the lower chain is still the
    // ascending run the equivalence above is a statement about.
    const std::vector<catalog::SysRangeRow> ranges = RangesOfTable();
    ASSERT_EQ(ranges.size(), 2u);
    EXPECT_EQ(IdsInChain(ranges[0].entry_page),
              (std::vector<std::uint64_t>{1, 2, kBoundary - 1}));
}

}  // namespace
}  // namespace kds::server
