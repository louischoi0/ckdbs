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

// End-to-end cover for the key-mode amendment (docs/heap-and-tuple.md §4.1,
// docs/workplan-key-mode.md PK07). The unit-level pieces are tested where
// they live - the catalog gate in catalog_test.cpp, the leaf division in
// btree_test.cpp, the grammar in parser_test.cpp - and this file is the one
// place all of them run together, through SQL, the way a caller meets them.
//
// The claim under test is narrow and worth stating plainly: **a caller may
// name a relation's primary keys, those keys need not ascend, and nothing
// else about the engine changes.** Everything here is a consequence of that
// sentence or a refusal that protects it.

namespace kds::server {
namespace {

class KeyModeSqlTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
    }

    CommandDispatcher Dispatcher() {
        return CommandDispatcher(boot_->superblock, boot_->catalog, store_);
    }

    // The relation every test here uses: two columns, caller-keyed, and
    // btree-clustered because §4.1 requires it of an explicit relation.
    void CreateExplicit(CommandDispatcher& d, const char* name = "t") {
        auto out = d.Dispatch(std::string("CREATE TABLE ") + name +
                              " (id int64, qty int64) BTREE EXPLICIT");
        ASSERT_EQ(out.response.substr(0, 7), "CREATED") << out.response;
    }

    void CreateAssigned(CommandDispatcher& d, const char* name = "a") {
        auto out = d.Dispatch(std::string("CREATE TABLE ") + name +
                              " (id int64, qty int64) BTREE ASSIGNED");
        ASSERT_EQ(out.response.substr(0, 7), "CREATED") << out.response;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- The config value ----------------------------------------------------

TEST(KeyModeConfigTest, TheTwoWordsParseInAnyCase) {
    for (const char* text : {"assigned", "ASSIGNED", "Assigned"}) {
        auto parsed = catalog::ParseKeyMode(text);
        ASSERT_TRUE(parsed.ok()) << text << ": " << parsed.status().message();
        EXPECT_EQ(parsed.value(), catalog::KeyMode::kAssigned) << text;
    }
    for (const char* text : {"explicit", "EXPLICIT", "Explicit"}) {
        auto parsed = catalog::ParseKeyMode(text);
        ASSERT_TRUE(parsed.ok()) << text << ": " << parsed.status().message();
        EXPECT_EQ(parsed.value(), catalog::KeyMode::kExplicit) << text;
    }
}

TEST(KeyModeConfigTest, TheNameAndTheParserAgree) {
    // The round trip, so a rename cannot leave a config file that no longer
    // parses the word DESCRIBE prints.
    for (catalog::KeyMode mode : {catalog::KeyMode::kAssigned, catalog::KeyMode::kExplicit}) {
        auto parsed = catalog::ParseKeyMode(catalog::KeyModeName(mode));
        ASSERT_TRUE(parsed.ok()) << catalog::KeyModeName(mode);
        EXPECT_EQ(parsed.value(), mode);
    }
}

TEST(KeyModeConfigTest, AnUnknownWordIsRefusedNamingWhatWasGiven) {
    // Not a silent fall back to the default: a misspelled mode would leave
    // an instance quietly creating the wrong kind of relation, and the first
    // sign of it would be an INSERT arity refusal nobody can explain.
    auto parsed = catalog::ParseKeyMode("explict");
    EXPECT_FALSE(parsed.ok());
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(parsed.status().message().find("explict"), std::string::npos)
        << parsed.status().message();
}

// ---- `default_key_mode`: what silence means ------------------------------
//
// The setting decides what a CREATE TABLE naming no key-mode word does, and
// nothing else. A written word always wins, or the statement would not mean
// what it says.

class ExplicitDefaultTest : public KeyModeSqlTest {
protected:
    // The same dispatcher, on an instance configured `default_key_mode =
    // explicit`.
    CommandDispatcher Dispatcher() {
        return CommandDispatcher(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                                 /*clock=*/nullptr, /*wal=*/nullptr,
                                 wal::DurabilityClass::kGroup, exec::Budget(),
                                 /*recorder=*/nullptr, /*replay_enabled=*/false,
                                 /*access_statistics=*/true, /*cabins=*/nullptr, /*txn=*/nullptr,
                                 txn::IsolationLevel::kReadCommitted, /*core_id=*/0,
                                 /*indexes=*/true, parser::kDefaultMaxInsertRows,
                                 catalog::KeyMode::kExplicit);
    }
};

TEST_F(ExplicitDefaultTest, ABareCreateTableBecomesExplicitAndBtree) {
    auto d = Dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, qty int64)").response.substr(0, 7), "CREATED");

    // Both halves: the mode came from configuration, and the storage
    // followed it. Defaulting the storage is not decoration - HEAP is the
    // shipped storage default, so without it every unqualified statement on
    // an explicit-default instance would be refused.
    auto described = d.Dispatch("DESCRIBE t");
    EXPECT_NE(described.response.find("clustered_type=BTREE key_mode=EXPLICIT"),
              std::string::npos)
        << described.response;

    // And it behaves as one: the caller names the key, descending.
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (900, 1)").response.substr(0, 8), "INSERTED");
    EXPECT_EQ(d.Dispatch("INSERT INTO t VALUES (100, 2)").response.substr(0, 8), "INSERTED");
}

TEST_F(ExplicitDefaultTest, AWrittenWordBeatsTheSetting) {
    auto d = Dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE a (id int64, qty int64) ASSIGNED").response.substr(0, 7),
              "CREATED");

    auto described = d.Dispatch("DESCRIBE a");
    EXPECT_NE(described.response.find("key_mode=ASSIGNED"), std::string::npos)
        << described.response;
    // Storage follows the *written* mode's default, not the setting's.
    EXPECT_NE(described.response.find("clustered_type=HEAP"), std::string::npos)
        << described.response;

    // The engine issues its keys, so VALUES omits the pk.
    EXPECT_NE(d.Dispatch("INSERT INTO a VALUES (7)").response.find("id=1"), std::string::npos);
}

TEST_F(ExplicitDefaultTest, AWrittenStorageWordStillContradictsAndIsRefused) {
    auto d = Dispatcher();
    // The writer asked for a heap and the setting asks for explicit keys.
    // Resolution never pairs them itself, so this can only come from the
    // statement - and it is still refused rather than silently re-pointed.
    auto refused = d.Dispatch("CREATE TABLE h (id int64, qty int64) HEAP");
    EXPECT_EQ(refused.response.substr(0, 3), "ERR") << refused.response;
    EXPECT_NE(refused.response.find("must be BTREE"), std::string::npos) << refused.response;
}

TEST_F(KeyModeSqlTest, TheShippedDefaultIsAssignedAndHeap) {
    // The other side of the setting: with no configuration, silence means
    // exactly what it did before the amendment.
    auto d = Dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, qty int64)").response.substr(0, 7), "CREATED");

    auto described = d.Dispatch("DESCRIBE t");
    EXPECT_NE(described.response.find("clustered_type=HEAP key_mode=ASSIGNED"), std::string::npos)
        << described.response;
}

// ---- The claim itself ----------------------------------------------------

TEST_F(KeyModeSqlTest, ACallerNamesTheKeyAndItIsTheRowsIdentity) {
    auto d = Dispatcher();
    CreateExplicit(d);

    auto inserted = d.Dispatch("INSERT INTO t VALUES (500, 11)");
    EXPECT_NE(inserted.response.find("id=500"), std::string::npos)
        << "the reply must report the id the caller named: " << inserted.response;

    auto selected = d.Dispatch("SELECT * FROM t WHERE id = 500");
    EXPECT_NE(selected.response.find("11"), std::string::npos) << selected.response;
}

TEST_F(KeyModeSqlTest, ADescendingKeyIsAccepted) {
    auto d = Dispatcher();
    CreateExplicit(d);

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

TEST_F(KeyModeSqlTest, AFullyDescendingLoadStaysWholeAndFindable) {
    auto d = Dispatcher();
    CreateExplicit(d);

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

TEST_F(KeyModeSqlTest, ARangeScanIsCorrectAfterDescendingInserts) {
    auto d = Dispatcher();
    CreateExplicit(d);

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

TEST_F(KeyModeSqlTest, OrderByEmitsKeyOrderOnAnExplicitRelation) {
    auto d = Dispatcher();
    CreateExplicit(d);

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

TEST_F(KeyModeSqlTest, OrderByWithLimitTakesTheLowestKeysNotTheFirstSlots) {
    auto d = Dispatcher();
    CreateExplicit(d);

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

TEST_F(KeyModeSqlTest, OrderByStillWorksOnAnAssignedRelation) {
    auto d = Dispatcher();
    CreateAssigned(d);

    // The path that was always sound: an engine-issued id is appended above
    // every id on the page, so slot order *is* key order and the walk is
    // left untouched. This is the regression guard for that - the per-page
    // sort must not have become the only correct path.
    for (int k = 1; k <= 50; ++k) {
        ASSERT_EQ(d.Dispatch("INSERT INTO a VALUES (" + std::to_string(k) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }

    auto ordered = d.Dispatch("SELECT * FROM a ORDER BY id");
    ASSERT_NE(ordered.response.substr(0, 3), "ERR") << ordered.response;
    std::vector<std::uint64_t> ids = EmittedIds(ordered.response);
    ASSERT_EQ(ids.size(), 50u);
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
}

TEST_F(KeyModeSqlTest, InterleavedAscendingAndDescendingKeysAllLand) {
    auto d = Dispatcher();
    CreateExplicit(d);

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
class KeyModeBulkTest : public KeyModeSqlTest {
protected:
    void SetUp() override {
        KeyModeSqlTest::SetUp();
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

// ---- Rollback across a division (BI4, docs/txn.md §6) --------------------
//
// The trail records a row as `(page_id, slot)`, because for most of this
// engine's life a row's address was stable for life. A leaf division breaks
// that assumption *mid-transaction*: it moves half a leaf elsewhere and
// renumbers the slots of the half that stays. Compensating an entry recorded
// before the division then reaches whatever now occupies that slot - so the
// failure is not a rollback that misses rows, it is a rollback that writes
// over rows it never touched.
//
// Only a kExplicit relation can trigger a division mid-statement, which is
// why these live here rather than in the transaction suite.

TEST_F(KeyModeBulkTest, AFailedStatementThatDividedALeafRollsBackWhole) {
    CommandDispatcher& d = *d_;
    CreateExplicit(d);

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

TEST_F(KeyModeBulkTest, AnAbortedUpdateDoesNotSurviveADivisionInItsOwnTransaction) {
    CommandDispatcher& d = *d_;
    CreateExplicit(d);

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

TEST_F(KeyModeBulkTest, ABulkStatementMayNameKeysInAnyOrder) {
    CommandDispatcher& d = *d_;
    CreateExplicit(d);

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

TEST_F(KeyModeSqlTest, ADuplicateKeyIsRefused) {
    auto d = Dispatcher();
    CreateExplicit(d);

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

TEST_F(KeyModeSqlTest, ADuplicateOfADescendingKeyIsAlsoRefused) {
    auto d = Dispatcher();
    CreateExplicit(d);

    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (900, 1)").response.substr(0, 8), "INSERTED");
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (100, 2)").response.substr(0, 8), "INSERTED");

    // The one a high-water-mark check would wave through: 100 is far below
    // the mark, so only a real lookup can know it is taken.
    auto dup = d.Dispatch("INSERT INTO t VALUES (100, 3)");
    EXPECT_EQ(dup.response.substr(0, 3), "ERR") << dup.response;
    EXPECT_NE(dup.response.find("duplicate primary key"), std::string::npos) << dup.response;
}

TEST_F(KeyModeSqlTest, AKeyOutsideTheIdSpaceIsRefused) {
    auto d = Dispatcher();
    CreateExplicit(d);

    // 0 is reserved for "unset" (§4).
    auto zero = d.Dispatch("INSERT INTO t VALUES (0, 1)");
    EXPECT_EQ(zero.response.substr(0, 3), "ERR") << zero.response;

    // Past the 40-bit Keystone field: 2^40 = 1099511627776.
    auto too_big = d.Dispatch("INSERT INTO t VALUES (1099511627776, 1)");
    EXPECT_EQ(too_big.response.substr(0, 3), "ERR") << too_big.response;
    EXPECT_NE(too_big.response.find("40-bit"), std::string::npos) << too_big.response;
}

TEST_F(KeyModeSqlTest, ANonIntegerKeyIsRefusedWithItsByte) {
    auto d = Dispatcher();
    CreateExplicit(d);

    auto out = d.Dispatch("INSERT INTO t VALUES ('nope', 1)");
    EXPECT_EQ(out.response.substr(0, 3), "ERR") << out.response;
    EXPECT_NE(out.response.find("byte "), std::string::npos)
        << "a refusal has to carry the offending token's byte: " << out.response;
}

TEST_F(KeyModeSqlTest, OmittingTheKeyOnAnExplicitRelationIsRefused) {
    auto d = Dispatcher();
    CreateExplicit(d);

    auto out = d.Dispatch("INSERT INTO t VALUES (7)");
    EXPECT_EQ(out.response.substr(0, 3), "ERR") << out.response;
    EXPECT_NE(out.response.find("EXPLICIT"), std::string::npos)
        << "the message has to name the rule, or it reads as an off-by-one: " << out.response;
}

TEST_F(KeyModeSqlTest, SupplyingTheKeyOnAnAssignedRelationIsStillRefused) {
    auto d = Dispatcher();
    CreateAssigned(d);

    // The amendment relaxed *who may* name a key, per relation. It did not
    // make every relation accept one.
    auto out = d.Dispatch("INSERT INTO a VALUES (7, 1)");
    EXPECT_EQ(out.response.substr(0, 3), "ERR") << out.response;
    EXPECT_NE(out.response.find("engine-assigned"), std::string::npos) << out.response;
}

TEST_F(KeyModeSqlTest, AnAssignedRelationStillIssuesItsOwnKeys) {
    auto d = Dispatcher();
    CreateAssigned(d);

    auto first = d.Dispatch("INSERT INTO a VALUES (11)");
    EXPECT_NE(first.response.find("id=1"), std::string::npos) << first.response;
    auto second = d.Dispatch("INSERT INTO a VALUES (22)");
    EXPECT_NE(second.response.find("id=2"), std::string::npos) << second.response;
}

TEST_F(KeyModeSqlTest, AnExplicitHeapRelationIsRefusedAtCreate) {
    auto d = Dispatcher();

    // A heap chain grows only at its tail and cannot place a key that sorts
    // behind it, so this pairing is refused - and it is refused because the
    // *writer* asked for a heap, not because of a default.
    auto named = d.Dispatch("CREATE TABLE h (id int64, qty int64) HEAP EXPLICIT");
    EXPECT_EQ(named.response.substr(0, 3), "ERR") << named.response;
    EXPECT_NE(named.response.find("must be BTREE"), std::string::npos) << named.response;

    // Naming only the mode is a different statement: storage follows it to
    // btree, since that is the one storage the mode can use. Resolution
    // never pairs an explicit mode with a heap it chose itself, which is why
    // the refusal above can only ever come from a written word.
    auto defaulted = d.Dispatch("CREATE TABLE h2 (id int64, qty int64) EXPLICIT");
    EXPECT_EQ(defaulted.response.substr(0, 7), "CREATED") << defaulted.response;
    EXPECT_NE(d.Dispatch("DESCRIBE h2").response.find("clustered_type=BTREE key_mode=EXPLICIT"),
              std::string::npos);
}

TEST_F(KeyModeSqlTest, TheKeyIsStillNotUpdatable) {
    auto d = Dispatcher();
    CreateExplicit(d);
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (5, 1)").response.substr(0, 8), "INSERTED");

    // Naming a key at insert and changing one afterwards are unrelated
    // permissions (K2), and only the first was granted. The identity of a
    // row that other structures already point at is not a field of it.
    auto out = d.Dispatch("UPDATE t SET id = 6 WHERE id = 5");
    EXPECT_EQ(out.response.substr(0, 3), "ERR") << out.response;
}

// ---- The mode is a property of the relation ------------------------------

TEST_F(KeyModeSqlTest, TwoModesCoexistInOneDatabase) {
    auto d = Dispatcher();
    CreateExplicit(d, "caller_keyed");
    CreateAssigned(d, "engine_keyed");

    ASSERT_EQ(d.Dispatch("INSERT INTO caller_keyed VALUES (9000, 1)").response.substr(0, 8),
              "INSERTED");
    auto engine = d.Dispatch("INSERT INTO engine_keyed VALUES (1)");
    EXPECT_NE(engine.response.find("id=1"), std::string::npos)
        << "an explicit relation's high-water mark must not touch another relation's: "
        << engine.response;
}

TEST_F(KeyModeSqlTest, TheModeSurvivesAcrossDispatchers) {
    {
        auto d = Dispatcher();
        CreateExplicit(d);
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (77, 1)").response.substr(0, 8), "INSERTED");
    }
    // A second dispatcher over the same catalog reads the mode off the page
    // rather than remembering it.
    auto d2 = Dispatcher();
    auto out = d2.Dispatch("INSERT INTO t VALUES (33, 2)");
    EXPECT_EQ(out.response.substr(0, 8), "INSERTED") << out.response;
}

}  // namespace
}  // namespace kds::server
