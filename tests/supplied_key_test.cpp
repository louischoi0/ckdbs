#include "kds/server/command_dispatcher.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"

// End-to-end cover for caller-supplied primary keys (docs/spec/heap-and-tuple.md
// §4.1). The unit-level pieces are tested where they live - the admission
// gate in catalog_test.cpp, the leaf division in btree_test.cpp, the grammar
// in parser_test.cpp - and this file is the one place all of them run
// together, through SQL, the way a caller meets them.
//
// **The claim moved on 2026-08-25** and the file was renamed with it. It was
// *a caller may name a relation's primary keys, those keys need not ascend,
// and nothing else about the engine changes* - a per-relation mode, chosen at
// CREATE TABLE, btree-only. It is now:
//
//   **Every relation takes a caller-supplied primary key or issues one when
//   the INSERT omits it, per row. On a btree relation the key may sort
//   anywhere; on a heap one it must not fall below the relation's high-water
//   mark, because the chain's tail append is that ascent.**
//
// Everything here is a consequence of that sentence or a refusal protecting
// it. The tests that survived the rewrite unchanged are the ones about what
// a btree does with a descending key, which is the half that did not move.

namespace kds::server {
namespace {

class SuppliedKeySqlTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
    }

    CommandDispatcher Dispatcher() {
        return CommandDispatcher(boot_->superblock, boot_->catalog, store_);
    }

    // The relation most tests here use: two columns, btree-clustered, which
    // is the storage that takes a key sorting anywhere. Nothing about keys
    // is said at CREATE - there is nothing left to say.
    void CreateBtree(CommandDispatcher& d, const char* name = "t") {
        auto out =
            d.Dispatch(std::string("CREATE TABLE ") + name + " (id int64, qty int64) BTREE");
        ASSERT_EQ(out.response.substr(0, 7), "CREATED") << out.response;
    }

    void CreateHeap(CommandDispatcher& d, const char* name = "a") {
        auto out = d.Dispatch(std::string("CREATE TABLE ") + name + " (id int64, qty int64) HEAP");
        ASSERT_EQ(out.response.substr(0, 7), "CREATED") << out.response;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- The two arities, on one relation ------------------------------------
//
// The half the mode made impossible: a relation whose INSERTs may name a key
// or not, row by row.

TEST_F(SuppliedKeySqlTest, OneRelationTakesBothArities) {
    auto d = Dispatcher();
    CreateBtree(d);

    auto named = d.Dispatch("INSERT INTO t VALUES (500, 11)");
    EXPECT_NE(named.response.find("id=500"), std::string::npos) << named.response;

    // Omitted on the same relation, in the next statement. The issued id
    // comes from the mark the supplied one moved, so the two sources cannot
    // collide.
    auto issued = d.Dispatch("INSERT INTO t VALUES (22)");
    EXPECT_NE(issued.response.find("id=501"), std::string::npos) << issued.response;

    // And back again, above the new mark.
    auto again = d.Dispatch("INSERT INTO t VALUES (900, 33)");
    EXPECT_NE(again.response.find("id=900"), std::string::npos) << again.response;
    EXPECT_NE(d.Dispatch("INSERT INTO t VALUES (44)").response.find("id=901"), std::string::npos);
}

TEST_F(SuppliedKeySqlTest, AWrongArityNamesBothAcceptedCounts) {
    auto d = Dispatcher();
    CreateBtree(d);

    // Two legal counts, so a message naming one of them reads as an
    // off-by-one against whichever the writer did not mean.
    auto out = d.Dispatch("INSERT INTO t VALUES (1, 2, 3)");
    EXPECT_EQ(out.response.substr(0, 3), "ERR") << out.response;
    EXPECT_NE(out.response.find("2 value(s) including primary-key column 'id'"),
              std::string::npos)
        << out.response;
    EXPECT_NE(out.response.find("or 1 to have it issued"), std::string::npos) << out.response;
}

TEST_F(SuppliedKeySqlTest, TheShippedStorageDefaultIsStillHeap) {
    // The key mode used to be able to move this - an `explicit` default
    // pulled storage to btree so its own statements were not all refused.
    // With the mode gone, silence means the heap and nothing else.
    auto d = Dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, qty int64)").response.substr(0, 7), "CREATED");

    auto described = d.Dispatch("DESCRIBE t");
    EXPECT_NE(described.response.find("clustered_type=HEAP key_order=ascending"),
              std::string::npos)
        << described.response;

    // And it takes a named key like any other relation, ascending - which
    // is the whole of what a heap accepts.
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (100, 1)").response.substr(0, 8), "INSERTED");
    EXPECT_EQ(d.Dispatch("INSERT INTO t VALUES (900, 2)").response.substr(0, 8), "INSERTED");
}

// ---- The heap, which is the new half -------------------------------------
//
// `HEAP EXPLICIT` was refused at CREATE until 2026-08-25. It is now an
// ordinary relation, and what it cannot do is refused per id instead: the
// chain's tail append, its page-wise ordering and its tail-page-only
// duplicate check are all the ascent (§3.1b), so the mark is the rule.

TEST_F(SuppliedKeySqlTest, AHeapRelationTakesNamedKeysThatAscend) {
    auto d = Dispatcher();
    CreateHeap(d, "h");

    for (int id : {10, 25, 400, 900, 1000}) {
        auto out = d.Dispatch("INSERT INTO h VALUES (" + std::to_string(id) + ", " +
                              std::to_string(id) + ")");
        ASSERT_EQ(out.response.substr(0, 8), "INSERTED") << "id " << id << ": " << out.response;
    }

    for (int id : {10, 25, 400, 900, 1000}) {
        const std::string want = std::to_string(id) + "," + std::to_string(id);
        EXPECT_NE(d.Dispatch("SELECT * FROM h WHERE id = " + std::to_string(id)).response.find(want),
                  std::string::npos)
            << "lost id " << id;
    }

    // Enough rows to grow the chain past one page, still named by the
    // caller: the growth path sets each new page's min_key from the id that
    // opened it, which is only sound while the ids ascend.
    for (int id = 2000; id < 2400; ++id) {
        ASSERT_EQ(d.Dispatch("INSERT INTO h VALUES (" + std::to_string(id) + ", 1)")
                      .response.substr(0, 8),
                  "INSERTED")
            << "id " << id;
    }
    EXPECT_NE(d.Dispatch("SELECT * FROM h WHERE id = 2399").response.find("2399,1"),
              std::string::npos);
}

TEST_F(SuppliedKeySqlTest, AHeapRelationRefusesAKeyBelowItsMark) {
    auto d = Dispatcher();
    CreateHeap(d, "h");

    ASSERT_EQ(d.Dispatch("INSERT INTO h VALUES (600, 1)").response.substr(0, 8), "INSERTED");

    // The refusal that keeps §3.1b true. Refused at admission, before the
    // chain is touched at all - a tail page whose min_key is 600 would have
    // no legal place for 550, and the tail-page-only duplicate check would
    // stop meaning anything the moment a later page opened below it.
    auto backwards = d.Dispatch("INSERT INTO h VALUES (550, 2)");
    EXPECT_EQ(backwards.response.substr(0, 3), "ERR") << backwards.response;
    EXPECT_NE(backwards.response.find("must ascend"), std::string::npos) << backwards.response;
    EXPECT_NE(backwards.response.find("use BTREE"), std::string::npos)
        << "the refusal has to say what does take the key: " << backwards.response;

    // The relation is unharmed and the mark did not move: the next omitted
    // key is still 601.
    EXPECT_NE(d.Dispatch("INSERT INTO h VALUES (3)").response.find("id=601"), std::string::npos);
}

TEST_F(SuppliedKeySqlTest, AHeapRelationMixesNamedAndIssuedKeys) {
    auto d = Dispatcher();
    CreateHeap(d, "h");

    EXPECT_NE(d.Dispatch("INSERT INTO h VALUES (1)").response.find("id=1"), std::string::npos);
    ASSERT_EQ(d.Dispatch("INSERT INTO h VALUES (100, 2)").response.substr(0, 8), "INSERTED");
    // The issued id resumes above the named one, which is what keeps the
    // chain ascending across a mixed load.
    EXPECT_NE(d.Dispatch("INSERT INTO h VALUES (3)").response.find("id=101"), std::string::npos);
}

// ---- The claim itself ----------------------------------------------------

TEST_F(SuppliedKeySqlTest, ACallerNamesTheKeyAndItIsTheRowsIdentity) {
    auto d = Dispatcher();
    CreateBtree(d);

    auto inserted = d.Dispatch("INSERT INTO t VALUES (500, 11)");
    EXPECT_NE(inserted.response.find("id=500"), std::string::npos)
        << "the reply must report the id the caller named: " << inserted.response;

    auto selected = d.Dispatch("SELECT * FROM t WHERE id = 500");
    EXPECT_NE(selected.response.find("11"), std::string::npos) << selected.response;
}

TEST_F(SuppliedKeySqlTest, ADescendingKeyIsAccepted) {
    auto d = Dispatcher();
    CreateBtree(d);

    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (500, 1)").response.substr(0, 8), "INSERTED");

    // The whole point of the amendment. Under the ascending-only rule this
    // was an OutOfRange naming a sequence that had gone backwards.
    auto backwards = d.Dispatch("INSERT INTO t VALUES (100, 2)");
    EXPECT_EQ(backwards.response.substr(0, 8), "INSERTED") << backwards.response;

    EXPECT_NE(d.Dispatch("SELECT * FROM t WHERE id = 100").response.find("2"),
              std::string::npos);
    EXPECT_NE(d.Dispatch("SELECT * FROM t WHERE id = 500").response.find("1"),
              std::string::npos);
}

TEST_F(SuppliedKeySqlTest, AFullyDescendingLoadStaysWholeAndFindable) {
    auto d = Dispatcher();
    CreateBtree(d);

    // Enough rows to fill leaves and force repeated divisions, arriving in
    // the worst order there is. Each one lands in a leaf that already holds
    // keys above it, which is the case a monotonic sequence never produces.
    const int kRows = 300;
    for (int id = kRows; id >= 1; --id) {
        auto out = d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                              std::to_string(id * 2) + ")");
        ASSERT_EQ(out.response.substr(0, 8), "INSERTED") << "id " << id << ": " << out.response;
    }

    // Every row is still there, still paired with its own value. A division
    // that dropped or duplicated a tuple, or that left a separator pointing
    // at the wrong subtree, shows up here and nowhere earlier.
    for (int id = 1; id <= kRows; ++id) {
        const std::string want = std::to_string(id) + "," + std::to_string(id * 2);
        auto out = d.Dispatch("SELECT * FROM t WHERE id = " + std::to_string(id));
        EXPECT_NE(out.response.find(want), std::string::npos)
            << "id " << id << " came back wrong or missing: " << out.response;
    }

    // And the scan agrees with the probes - a descent and a leaf walk must
    // not disagree about what the relation holds.
    auto all = d.Dispatch("SELECT * FROM t");
    for (int id = 1; id <= kRows; ++id) {
        const std::string want = std::to_string(id) + "," + std::to_string(id * 2);
        EXPECT_NE(all.response.find(want), std::string::npos)
            << "the scan is missing id " << id << ", which the probe found";
    }
}

TEST_F(SuppliedKeySqlTest, ARangeScanIsCorrectAfterDescendingInserts) {
    auto d = Dispatcher();
    CreateBtree(d);

    // Range scans prune by page-wise `min_key` ordering (exec/step_vm.cpp):
    // the walk stops at the first page whose min_key passes the high bound,
    // which is only sound if pages stay in ascending key order. A division
    // preserves that - the new leaf's min_key is a key that was in the old
    // leaf, so it sits strictly between the old leaf's low bound and the
    // next page's - but the property is easy to break and silent when
    // broken: a scan simply returns fewer rows.
    const int kRows = 200;
    for (int id = kRows; id >= 1; --id) {
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                             std::to_string(id) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }

    auto ranged = d.Dispatch("SELECT * FROM t WHERE id > 50 AND id < 60");
    for (int id = 51; id <= 59; ++id) {
        const std::string want = std::to_string(id) + "," + std::to_string(id);
        EXPECT_NE(ranged.response.find(want), std::string::npos)
            << "the range scan pruned away id " << id << ": " << ranged.response;
    }
    // And it did not over-return: the bounds are exclusive.
    EXPECT_EQ(ranged.response.find("50,50"), std::string::npos) << ranged.response;
    EXPECT_EQ(ranged.response.find("60,60"), std::string::npos) << ranged.response;
}

// The ids a reply's rows carry, in the order they were emitted. Rows are
// "id,qty" lines separated by the dispatcher's escaped newline.
std::vector<std::uint64_t> EmittedIds(const std::string& response) {
    std::vector<std::uint64_t> ids;
    for (std::size_t i = 0; i < response.size();) {
        const std::size_t line_end = std::min(response.find("\\n", i), response.size());
        const std::size_t comma = response.find(',', i);
        if (comma != std::string::npos && comma < line_end) {
            const std::string digits = response.substr(i, comma - i);
            if (!digits.empty() &&
                std::all_of(digits.begin(), digits.end(),
                            [](unsigned char c) { return std::isdigit(c) != 0; })) {
                ids.push_back(std::stoull(digits));
            }
        }
        i = line_end + 2;
    }
    return ids;
}

TEST_F(SuppliedKeySqlTest, OrderByEmitsKeyOrderAfterADescendingLoad) {
    auto d = Dispatcher();
    CreateBtree(d);

    // Descending inserts put a page's slots deliberately out of key order:
    // each id is appended *below* everything already on the page, which is
    // the case an engine-issued sequence can never produce.
    const int kRows = 250;
    for (int id = kRows; id >= 1; --id) {
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                             std::to_string(id) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }

    auto ordered = d.Dispatch("SELECT * FROM t ORDER BY id");
    ASSERT_NE(ordered.response.substr(0, 3), "ERR") << ordered.response;

    std::vector<std::uint64_t> ids = EmittedIds(ordered.response);
    ASSERT_EQ(ids.size(), static_cast<std::size_t>(kRows)) << ordered.response;
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()))
        << "ORDER BY returned the right rows in the wrong order";
    EXPECT_EQ(ids.front(), 1u);
    EXPECT_EQ(ids.back(), static_cast<std::uint64_t>(kRows));
}

TEST_F(SuppliedKeySqlTest, OrderByWithLimitTakesTheLowestKeysNotTheFirstSlots) {
    auto d = Dispatcher();
    CreateBtree(d);

    // The case a per-page sort has to get right and a naive one would not:
    // LIMIT stops the walk part-way through a page, so the page's rows must
    // already be in key order when the quota fills - not merely sorted after
    // the fact.
    const int kRows = 250;
    for (int id = kRows; id >= 1; --id) {
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                             std::to_string(id) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }

    auto page1 = d.Dispatch("SELECT * FROM t ORDER BY id LIMIT 5");
    ASSERT_NE(page1.response.substr(0, 3), "ERR") << page1.response;
    EXPECT_EQ(EmittedIds(page1.response), (std::vector<std::uint64_t>{1, 2, 3, 4, 5}))
        << page1.response;

    // And OFFSET walks that same order rather than a slot order.
    auto page2 = d.Dispatch("SELECT * FROM t ORDER BY id LIMIT 5 OFFSET 5");
    ASSERT_NE(page2.response.substr(0, 3), "ERR") << page2.response;
    EXPECT_EQ(EmittedIds(page2.response), (std::vector<std::uint64_t>{6, 7, 8, 9, 10}))
        << page2.response;
}

TEST_F(SuppliedKeySqlTest, OrderByCostsNothingOnARelationThatNeverTookAnOutOfOrderKey) {
    auto d = Dispatcher();
    CreateHeap(d);

    // The path that was always sound, and the reason `key_order` is a fact
    // rather than a storage type: an id at or above the mark is appended
    // above every id on the page, so slot order *is* key order and the walk
    // is left untouched. The regression guard is that the per-page sort must
    // not have become the only correct path - which is what reading the
    // storage type instead of the flag would have produced on every btree
    // relation.
    for (int k = 1; k <= 50; ++k) {
        ASSERT_EQ(d.Dispatch("INSERT INTO a VALUES (" + std::to_string(k) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }
    EXPECT_NE(d.Dispatch("DESCRIBE a").response.find("key_order=ascending"), std::string::npos);

    auto ordered = d.Dispatch("SELECT * FROM a ORDER BY id");
    ASSERT_NE(ordered.response.substr(0, 3), "ERR") << ordered.response;
    std::vector<std::uint64_t> ids = EmittedIds(ordered.response);
    ASSERT_EQ(ids.size(), 50u);
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
}

TEST_F(SuppliedKeySqlTest, InterleavedAscendingAndDescendingKeysAllLand) {
    auto d = Dispatcher();
    CreateBtree(d);

    // Neither ordered nor reverse-ordered: the shape a real backfill or a
    // migration from another system produces.
    const std::vector<int> ids = {50, 900, 10, 400, 25, 1000, 5, 700, 300, 1};
    for (int id : ids) {
        auto out = d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                              std::to_string(id) + ")");
        ASSERT_EQ(out.response.substr(0, 8), "INSERTED") << "id " << id << ": " << out.response;
    }

    for (int id : ids) {
        const std::string want = std::to_string(id) + "," + std::to_string(id);
        EXPECT_NE(d.Dispatch("SELECT * FROM t WHERE id = " + std::to_string(id)).response.find(
                      want),
                  std::string::npos)
            << "lost id " << id;
    }
}

// A multi-row INSERT needs the transaction manager - BI4's rollback of the
// placed prefix replays its trail - so this one builds the configuration
// production always has, rather than the bare dispatcher above.
class SuppliedKeyBulkTest : public SuppliedKeySqlTest {
protected:
    void SetUp() override {
        SuppliedKeySqlTest::SetUp();
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        d_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                   /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                   exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                   /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
    }

    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> d_;
};

// ---- Rollback across a division (BI4, docs/spec/txn.md §6) --------------------
//
// The trail records a row as `(page_id, slot)`, because for most of this
// engine's life a row's address was stable for life. A leaf division breaks
// that assumption *mid-transaction*: it moves half a leaf elsewhere and
// renumbers the slots of the half that stays. Compensating an entry recorded
// before the division then reaches whatever now occupies that slot - so the
// failure is not a rollback that misses rows, it is a rollback that writes
// over rows it never touched.
//
// Only a key named below the relation's high-water mark can trigger a
// division mid-statement, which is why these live here rather than in the
// transaction suite.

TEST_F(SuppliedKeyBulkTest, AFailedStatementThatDividedALeafRollsBackWhole) {
    CommandDispatcher& d = *d_;
    CreateBtree(d);

    // A committed base, ascending and gapped so there is room to insert
    // between the keys later.
    std::string base = "INSERT INTO t VALUES ";
    for (int k = 1; k <= 400; ++k) {
        base += (k == 1 ? "" : ", ");
        base += "(" + std::to_string(k * 1000) + ", " + std::to_string(k) + ")";
    }
    ASSERT_EQ(d.Dispatch(base).response.substr(0, 8), "INSERTED");
    const std::string committed = d.Dispatch("SELECT COUNT(*) FROM t").response;

    // One statement that divides the first leaf several times over and then
    // fails on its last row: id 10 is already there. BI4 says the whole
    // statement unwinds.
    // Dense and low: every one of these routes into the *first* leaf, so it
    // fills, divides, refills and divides again inside one statement. One
    // division alone would not be enough - the base arrived in ascending
    // order, so its slots are already sorted and a first rebuild puts them
    // back where they were. It takes a division of a leaf that has since
    // been appended to out of order for slots to actually move.
    std::string doomed = "INSERT INTO t VALUES ";
    for (int k = 1; k <= 400; ++k) {
        doomed += "(" + std::to_string(k) + ", " + std::to_string(k) + "), ";
    }
    doomed += "(1000, 999)";
    auto failed = d.Dispatch(doomed);
    EXPECT_EQ(failed.response.substr(0, 3), "ERR") << failed.response;

    // Every row the statement placed before the duplicate must be gone -
    // including the ones a division relocated after their trail entry was
    // written.
    EXPECT_EQ(d.Dispatch("SELECT COUNT(*) FROM t").response, committed)
        << "a failed statement left rows behind after dividing a leaf";

    // **And the committed base has to be intact.** The count alone cannot
    // see the worse failure: compensating a stale `(page_id, slot)` retires
    // whichever row now occupies that slot, so the right *number* of rows
    // disappears while the wrong ones do. Checking identities is what
    // separates "rolled back" from "destroyed something else".
    auto all = d.Dispatch("SELECT * FROM t");
    for (int k = 1; k <= 400; ++k) {
        const std::string want = std::to_string(k * 1000) + "," + std::to_string(k);
        EXPECT_NE(all.response.find(want), std::string::npos)
            << "rollback destroyed committed row " << k * 1000;
    }
}

TEST_F(SuppliedKeyBulkTest, AnAbortedUpdateDoesNotSurviveADivisionInItsOwnTransaction) {
    CommandDispatcher& d = *d_;
    CreateBtree(d);

    std::string base = "INSERT INTO t VALUES ";
    for (int k = 1; k <= 300; ++k) {
        base += (k == 1 ? "" : ", ");
        base += "(" + std::to_string(k * 1000) + ", " + std::to_string(k) + ")";
    }
    ASSERT_EQ(d.Dispatch(base).response.substr(0, 8), "INSERTED");
    const std::string committed = d.Dispatch("SELECT COUNT(*) FROM t").response;

    Session session;
    ASSERT_EQ(d.Dispatch("BEGIN", &session).response.substr(0, 5), "BEGIN");

    // The write whose address the division will invalidate. Its trail entry
    // is recorded now, against a slot that is about to be renumbered.
    ASSERT_EQ(d.Dispatch("UPDATE t SET qty = 555 WHERE id = 1000", &session).response.substr(0, 3),
              "UPD");

    // Now divide the leaf that row sits on, repeatedly.
    for (int k = 1; k <= 400; ++k) {
        auto out = d.Dispatch(
            "INSERT INTO t VALUES (" + std::to_string(k) + ", " + std::to_string(k) + ")",
            &session);
        ASSERT_EQ(out.response.substr(0, 8), "INSERTED") << out.response;
    }

    auto rolled = d.Dispatch("ROLLBACK", &session);
    EXPECT_EQ(rolled.response.substr(0, 3), "ROL") << rolled.response;

    // Both halves of the claim: the inserts are gone, and the UPDATE did not
    // outlive its own transaction.
    EXPECT_EQ(d.Dispatch("SELECT COUNT(*) FROM t").response, committed)
        << "the aborted inserts survived";
    auto row = d.Dispatch("SELECT * FROM t WHERE id = 1000");
    EXPECT_NE(row.response.find("1000,1"), std::string::npos)
        << "an aborted UPDATE survived its own ROLLBACK: " << row.response;
    EXPECT_EQ(row.response.find("555"), std::string::npos) << row.response;

    // The committed base, whole. Compensating a stale slot writes over
    // whatever now sits there - the count would still balance while a row
    // this transaction never named was destroyed or overwritten.
    auto all = d.Dispatch("SELECT * FROM t");
    for (int k = 1; k <= 300; ++k) {
        const std::string want = std::to_string(k * 1000) + "," + std::to_string(k);
        EXPECT_NE(all.response.find(want), std::string::npos)
            << "rollback destroyed or corrupted committed row " << k * 1000;
    }
}

TEST_F(SuppliedKeyBulkTest, ABulkStatementMayNameKeysInAnyOrder) {
    CommandDispatcher& d = *d_;
    CreateBtree(d);

    // Each row gates individually and in statement order (BI2), so an
    // unordered set is not a special case - it is N single-row inserts that
    // happen to share a statement.
    auto out = d.Dispatch("INSERT INTO t VALUES (300, 3), (100, 1), (200, 2)");
    EXPECT_EQ(out.response.substr(0, 8), "INSERTED") << out.response;

    for (int id : {100, 200, 300}) {
        const std::string want = std::to_string(id) + "," + std::to_string(id / 100);
        EXPECT_NE(d.Dispatch("SELECT * FROM t WHERE id = " + std::to_string(id)).response.find(
                      want),
                  std::string::npos)
            << "lost id " << id;
    }
}

// ---- The refusals that protect it ----------------------------------------

TEST_F(SuppliedKeySqlTest, ADuplicateKeyIsRefused) {
    auto d = Dispatcher();
    CreateBtree(d);

    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (42, 1)").response.substr(0, 8), "INSERTED");

    // Uniqueness is not proved by the cursor any more - a descending id
    // makes the high-water mark say nothing about what is in use - so it is
    // proved by the descent, which lands on the one leaf that could hold the
    // key. This is the test that the proof actually runs.
    auto dup = d.Dispatch("INSERT INTO t VALUES (42, 2)");
    EXPECT_EQ(dup.response.substr(0, 3), "ERR") << dup.response;
    EXPECT_NE(dup.response.find("duplicate primary key"), std::string::npos) << dup.response;

    // And the loser changed nothing.
    EXPECT_NE(d.Dispatch("SELECT * FROM t WHERE id = 42").response.find("1"), std::string::npos);
}

TEST_F(SuppliedKeySqlTest, ADuplicateOfADescendingKeyIsAlsoRefused) {
    auto d = Dispatcher();
    CreateBtree(d);

    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (900, 1)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (100, 2)").response.substr(0, 8), "INSERTED");

    // The one a high-water-mark check would wave through: 100 is far below
    // the mark, so only a real lookup can know it is taken.
    auto dup = d.Dispatch("INSERT INTO t VALUES (100, 3)");
    EXPECT_EQ(dup.response.substr(0, 3), "ERR") << dup.response;
    EXPECT_NE(dup.response.find("duplicate primary key"), std::string::npos) << dup.response;
}

TEST_F(SuppliedKeySqlTest, AKeyOutsideTheIdSpaceIsRefused) {
    auto d = Dispatcher();
    CreateBtree(d);

    // 0 is reserved for "unset" (§4).
    auto zero = d.Dispatch("INSERT INTO t VALUES (0, 1)");
    EXPECT_EQ(zero.response.substr(0, 3), "ERR") << zero.response;

    // Past the 40-bit Keystone field: 2^40 = 1099511627776.
    auto too_big = d.Dispatch("INSERT INTO t VALUES (1099511627776, 1)");
    EXPECT_EQ(too_big.response.substr(0, 3), "ERR") << too_big.response;
    EXPECT_NE(too_big.response.find("40-bit"), std::string::npos) << too_big.response;
}

TEST_F(SuppliedKeySqlTest, ANonIntegerKeyIsRefusedWithItsByte) {
    auto d = Dispatcher();
    CreateBtree(d);

    auto out = d.Dispatch("INSERT INTO t VALUES ('nope', 1)");
    EXPECT_EQ(out.response.substr(0, 3), "ERR") << out.response;
    EXPECT_NE(out.response.find("byte "), std::string::npos)
        << "a refusal has to carry the offending token's byte: " << out.response;
}

TEST_F(SuppliedKeySqlTest, OmittingTheKeyIsAcceptedAndIssuesAboveTheMark) {
    // The inverse of what this file used to assert. Omitting was the
    // refusal that made the mode a mode; it is now the other arity.
    auto d = Dispatcher();
    CreateBtree(d);

    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (700, 1)").response.substr(0, 8), "INSERTED");
    auto out = d.Dispatch("INSERT INTO t VALUES (7)");
    EXPECT_NE(out.response.find("id=701"), std::string::npos)
        << "an issued id must clear every id the caller has named: " << out.response;
    EXPECT_NE(d.Dispatch("SELECT * FROM t WHERE id = 701").response.find("7"), std::string::npos);
}

TEST_F(SuppliedKeySqlTest, AHeapRelationStillIssuesItsOwnKeysWhenAsked) {
    auto d = Dispatcher();
    CreateHeap(d);

    auto first = d.Dispatch("INSERT INTO a VALUES (11)");
    EXPECT_NE(first.response.find("id=1"), std::string::npos) << first.response;
    auto second = d.Dispatch("INSERT INTO a VALUES (22)");
    EXPECT_NE(second.response.find("id=2"), std::string::npos) << second.response;
}

TEST_F(SuppliedKeySqlTest, HeapExplicitIsCreatedRatherThanRefused) {
    auto d = Dispatcher();

    // This pairing was `Unsupported` at CREATE until 2026-08-25, on both the
    // catalog path and the statement path. It creates an ordinary heap
    // relation now, and `EXPLICIT` is the vacuous word it has become.
    auto named = d.Dispatch("CREATE TABLE h (id int64, qty int64) HEAP EXPLICIT");
    EXPECT_EQ(named.response.substr(0, 7), "CREATED") << named.response;
    EXPECT_NE(d.Dispatch("DESCRIBE h").response.find("clustered_type=HEAP"), std::string::npos);
    EXPECT_EQ(d.Dispatch("INSERT INTO h VALUES (5, 1)").response.substr(0, 8), "INSERTED");

    // And bare EXPLICIT no longer drags storage to btree with it - the
    // resolution that did so existed only to keep the refusal above
    // reachable from a written word alone.
    auto defaulted = d.Dispatch("CREATE TABLE h2 (id int64, qty int64) EXPLICIT");
    EXPECT_EQ(defaulted.response.substr(0, 7), "CREATED") << defaulted.response;
    EXPECT_NE(d.Dispatch("DESCRIBE h2").response.find("clustered_type=HEAP"), std::string::npos);
}

TEST_F(SuppliedKeySqlTest, AssignedIsRefusedAtCreateWithItsByte) {
    auto d = Dispatcher();

    // The word that outlived nothing: it named a mode where supplying a pk
    // was refused, and on the relation this statement would create,
    // supplying one is admitted. Refused rather than ignored.
    auto out = d.Dispatch("CREATE TABLE a (id int64, qty int64) HEAP ASSIGNED");
    EXPECT_EQ(out.response.substr(0, 3), "ERR") << out.response;
    EXPECT_NE(out.response.find("no longer exists"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("byte "), std::string::npos) << out.response;
}

TEST_F(SuppliedKeySqlTest, TheKeyIsStillNotUpdatable) {
    auto d = Dispatcher();
    CreateBtree(d);
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (5, 1)").response.substr(0, 8), "INSERTED");

    // Naming a key at insert and changing one afterwards are unrelated
    // permissions (K2), and only the first was granted. The identity of a
    // row that other structures already point at is not a field of it.
    auto out = d.Dispatch("UPDATE t SET id = 6 WHERE id = 5");
    EXPECT_EQ(out.response.substr(0, 3), "ERR") << out.response;
}

// ---- The mark and the flag are per relation ------------------------------

TEST_F(SuppliedKeySqlTest, OneRelationsMarkDoesNotTouchAnothers) {
    auto d = Dispatcher();
    CreateBtree(d, "caller_keyed");
    CreateHeap(d, "engine_keyed");

    ASSERT_EQ(d.Dispatch("INSERT INTO caller_keyed VALUES (9000, 1)").response.substr(0, 8),
              "INSERTED");
    auto engine = d.Dispatch("INSERT INTO engine_keyed VALUES (1)");
    EXPECT_NE(engine.response.find("id=1"), std::string::npos)
        << "a named key must move its own relation's high-water mark and no other's: "
        << engine.response;
}

TEST_F(SuppliedKeySqlTest, TheKeyOrderSurvivesAcrossDispatchers) {
    {
        auto d = Dispatcher();
        CreateBtree(d);
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (77, 1)").response.substr(0, 8), "INSERTED");
        // Below 77's mark: the relation is unordered from here on.
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (33, 2)").response.substr(0, 8), "INSERTED");
    }
    // A second dispatcher over the same catalog reads the flag off the page
    // rather than remembering it - and so must still answer ORDER BY <pk>
    // with the per-page sort the first dispatcher's insert made necessary.
    auto d2 = Dispatcher();
    EXPECT_NE(d2.Dispatch("DESCRIBE t").response.find("key_order=unordered"), std::string::npos);
    EXPECT_EQ(EmittedIds(d2.Dispatch("SELECT * FROM t ORDER BY id").response),
              (std::vector<std::uint64_t>{33, 77}));
}

}  // namespace
}  // namespace kds::server
