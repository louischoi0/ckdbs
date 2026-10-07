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
// gate in catalog_test.cpp, the leaf division in btree_test.cpp (which SQL no
// longer reaches), the grammar in parser_test.cpp - and this file is the one
// place all of them run together, through SQL, the way a caller meets them.
//
// **The claim moved on 2026-08-25** and the file was renamed with it. It was
// *a caller may name a relation's primary keys, those keys need not ascend,
// and nothing else about the engine changes* - a per-relation mode, chosen at
// CREATE TABLE, btree-only. **It moved again on 2026-10-06** (BB-Q8 (b),
// BB-R3, `instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md`): a btree
// used to take a key sorting anywhere and flip the relation to `kUnordered`,
// and that capability is withdrawn. It is now:
//
//   **Every relation takes a caller-supplied primary key or issues one when
//   the INSERT omits it, per row. On every relation a named key must sort
//   above every key the relation has placed or issued - at or above its
//   high-water mark - because that ascent is what keeps every page's slot
//   order its key order. Below the mark it is refused: a btree answers
//   AlreadyExists when the key is present and OutOfRange when it is absent,
//   a heap OutOfRange for both (BB-R12).**
//
// Everything here is a consequence of that sentence or a refusal protecting
// it. The cells that pinned what a btree did with a descending key, or with
// keys in any order, are refusal cells now - or name their keys ascending,
// where their subject (a split, a range scan, ORDER BY, a rollback) is one
// an ascending key still reaches.

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

    // The relation most tests here use: two columns, btree-clustered, the
    // storage every new relation gets since SUS-1. Nothing about keys is
    // said at CREATE - there is nothing left to say.
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

TEST_F(SuppliedKeySqlTest, TheShippedStorageDefaultIsBtreeSinceSUS1) {
    // **The line SUS-1 moved.** The key mode used to be able to move this -
    // an `explicit` default pulled storage to btree so its own statements
    // were not all refused - and with the mode gone, silence meant the heap.
    // Heap relations are now suspended, so silence means a btree, and the
    // cell that used to pin the old answer pins the new one.
    auto d = Dispatcher();
    ASSERT_EQ(d.Dispatch("CREATE TABLE t (id int64, qty int64)").response.substr(0, 7), "CREATED");

    auto described = d.Dispatch("DESCRIBE t");
    EXPECT_NE(described.response.find("clustered_type=BTREE"), std::string::npos)
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

TEST_F(SuppliedKeySqlTest, AHeapRelationRefusesAKeyBelowItsMarkPresentOrAbsent) {
    // **The pointer to BTREE is withdrawn** (BB-Q8 (b), BB-R3). This refusal
    // used to end "use BTREE", because a btree took the key and flipped
    // itself to `kUnordered`. A btree now refuses the same key, so a reply
    // that sent the caller there would name a storage that answers the same
    // way. What the heap refuses, and its code, did not move (BB-R12).
    auto d = Dispatcher();
    CreateHeap(d, "h");

    ASSERT_EQ(d.Dispatch("INSERT INTO h VALUES (600, 1)").response.substr(0, 8), "INSERTED");

    // The refusal that keeps §3.1b true. Refused under the tail's hold,
    // before anything is placed (BB-R7) - a tail page whose min_key is 600
    // would have no legal place for 550, and the tail-page-only duplicate
    // check would stop meaning anything the moment a later page opened
    // below it.
    auto backwards = d.Dispatch("INSERT INTO h VALUES (550, 2)");
    EXPECT_EQ(backwards.response.substr(0, 3), "ERR") << backwards.response;
    EXPECT_NE(backwards.response.find("high-water mark"), std::string::npos)
        << backwards.response;
    EXPECT_EQ(backwards.response.find("use BTREE"), std::string::npos)
        << "a btree refuses the same key; the refusal may not send the caller there: "
        << backwards.response;

    // A present key gets the same answer. The heap's duplicate check reads
    // the tail alone and cannot prove presence below it, so it answers
    // OutOfRange for both, where a btree names a present key a duplicate.
    auto again = d.Dispatch("INSERT INTO h VALUES (600, 3)");
    EXPECT_EQ(again.response.substr(0, 3), "ERR") << again.response;
    EXPECT_NE(again.response.find("high-water mark"), std::string::npos) << again.response;
    EXPECT_EQ(again.response.find("duplicate primary key"), std::string::npos)
        << "a heap answers OutOfRange for a present key too (BB-R12): " << again.response;

    // The relation is unharmed and the mark did not move: 600 keeps its
    // value, 550 never landed, and the next omitted key is still 601.
    EXPECT_NE(d.Dispatch("SELECT * FROM h WHERE id = 600").response.find("600,1"),
              std::string::npos);
    EXPECT_EQ(d.Dispatch("SELECT * FROM h WHERE id = 550").response.find("550,"),
              std::string::npos);
    EXPECT_NE(d.Dispatch("INSERT INTO h VALUES (3)").response.find("id=601"), std::string::npos);
}

TEST_F(SuppliedKeySqlTest, AKeyBelowAHeapsTailIsRefusedAsBelowTheMark) {
    // The chain refuses a key below its tail's min_key itself, under the
    // tail's hold, before the mark is read (BB-R7) - and words it as the mark
    // it is below: the caller named a key, nothing went backwards (BB-R12).
    // A one-page chain's head opens at 0, so only a chain past its first
    // page reaches this: two `varchar(4000)` rows fill a page, so the third
    // issued id, 3, opens the tail.
    auto d = Dispatcher();
    auto created = d.Dispatch("CREATE TABLE h (id int64, s varchar(4000)) HEAP");
    ASSERT_EQ(created.response.substr(0, 7), "CREATED") << created.response;
    for (const char* v : {"'a'", "'b'", "'c'"}) {
        auto out = d.Dispatch(std::string("INSERT INTO h VALUES (") + v + ")");
        ASSERT_EQ(out.response.substr(0, 8), "INSERTED") << out.response;
    }

    auto below = d.Dispatch("INSERT INTO h VALUES (2, 'x')");
    EXPECT_EQ(below.response.substr(0, 3), "ERR") << below.response;
    EXPECT_NE(below.response.find("high-water mark"), std::string::npos) << below.response;
    EXPECT_EQ(below.response.find("gone backwards"), std::string::npos)
        << "a named key below the tail is the caller's error, not a sequence fault: "
        << below.response;
    EXPECT_NE(d.Dispatch("SELECT * FROM h WHERE id = 2").response.find("2,b"), std::string::npos);
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

// `ADescendingKeyIsAccepted` stood here: 500, then 100 below it, admitted.
// Withdrawn on BB-Q8 (b) and BB-R3, it became the refusal
// `ANamedKeyBelowTheMarkIsRefusedOnABtree` (BB-S2) already pins - one key
// placed, a lower absent one refused on the one leaf - so it was deleted
// rather than kept as a second copy. The mark that refusal must leave
// alone is asserted by `TheMarkThatRefusesAKeySurvivesAcrossDispatchers`.

TEST_F(SuppliedKeySqlTest, AnAscendingLoadStaysWholeAndFindableAcrossSplits) {
    auto d = Dispatcher();
    CreateBtree(d);

    // **Withdrawn in its old shape** on BB-Q8 (b) and BB-R3. This loaded its
    // keys descending, so each landed in a leaf already holding keys above
    // it and the leaves divided in the middle. A named key below the mark
    // is refused now, so SQL reaches no middle divide; the division keeps
    // its cover in btree_test.cpp. What an ascending key still reaches is
    // the tree growing under a load: enough named keys to split the
    // rightmost leaf again and again, each split writing a separator every
    // later descent has to follow.
    const int kRows = 600;
    for (int id = 1; id <= kRows; ++id) {
        auto out = d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                              std::to_string(id * 2) + ")");
        ASSERT_EQ(out.response.substr(0, 8), "INSERTED") << "id " << id << ": " << out.response;
    }

    // Every row is still there, still paired with its own value. A split
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

TEST_F(SuppliedKeySqlTest, ARangeScanIsCorrectAcrossSplitLeaves) {
    auto d = Dispatcher();
    CreateBtree(d);

    // Range scans prune by page-wise `min_key` ordering (exec/step_vm.cpp):
    // the walk stops at the first page whose min_key passes the high bound,
    // which is only sound if pages stay in ascending key order. The property
    // is easy to break and silent when broken: a scan simply returns fewer
    // rows.
    //
    // **Withdrawn in its old shape** on BB-Q8 (b) and BB-R3: the keys used
    // to arrive descending, so the pages came from middle divides, which SQL
    // no longer reaches. They arrive ascending now and the pages come from
    // append splits - each new leaf's min_key the key that opened it - and
    // the range is wide enough to cross from the first leaf into the next:
    // a row of these two columns takes 41 bytes of a leaf (20 of header, 16
    // of payload, 5 of slot), so the first leaf ends near id 198.
    const int kRows = 400;
    for (int id = 1; id <= kRows; ++id) {
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                             std::to_string(id) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }

    auto ranged = d.Dispatch("SELECT * FROM t WHERE id > 150 AND id < 260");
    for (int id = 151; id <= 259; ++id) {
        const std::string want = std::to_string(id) + "," + std::to_string(id);
        EXPECT_NE(ranged.response.find(want), std::string::npos)
            << "the range scan pruned away id " << id << ": " << ranged.response;
    }
    // And it did not over-return: the bounds are exclusive.
    EXPECT_EQ(ranged.response.find("150,150"), std::string::npos) << ranged.response;
    EXPECT_EQ(ranged.response.find("260,260"), std::string::npos) << ranged.response;
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

TEST_F(SuppliedKeySqlTest, AKeyInAGapBelowTheMarkIsRefusedAndOrderByStaysKeyOrder) {
    auto d = Dispatcher();
    CreateBtree(d);

    // **Withdrawn** on BB-Q8 (b) and BB-R3. Descending inserts used to put
    // a page's slots deliberately out of key order - each id appended
    // *below* everything already on the page - and this pinned the per-page
    // sort ORDER BY then needed. Such a key is refused now, so no page's
    // slots leave key order. What this pins is the refusal that keeps it so,
    // where the key falls in a gap of a leaf with a right sibling, and the
    // order ORDER BY emits over the relation it protected.
    //
    // The keys ascend with gaps (2, 4, ... 500) over more than one leaf; 3
    // sorts into the first one.
    const int kRows = 250;
    for (int k = 1; k <= kRows; ++k) {
        const int id = 2 * k;
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                             std::to_string(id) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }

    // Absent and below the mark: OutOfRange (BB-R12), answered by the held
    // leaf, whose right sibling already holds a higher key (BB-R3 step 6).
    auto gap = d.Dispatch("INSERT INTO t VALUES (3, 3)");
    EXPECT_EQ(gap.response.substr(0, 3), "ERR") << gap.response;
    EXPECT_NE(gap.response.find("high-water mark"), std::string::npos) << gap.response;
    EXPECT_NE(gap.response.find("right sibling"), std::string::npos)
        << "the key sorts into a leaf with a right sibling, which refuses it: " << gap.response;

    auto ordered = d.Dispatch("SELECT * FROM t ORDER BY id");
    ASSERT_NE(ordered.response.substr(0, 3), "ERR") << ordered.response;

    std::vector<std::uint64_t> ids = EmittedIds(ordered.response);
    ASSERT_EQ(ids.size(), static_cast<std::size_t>(kRows))
        << "the refused key landed after all: " << ordered.response;
    EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()))
        << "ORDER BY returned the right rows in the wrong order";
    EXPECT_EQ(ids.front(), 2u);
    EXPECT_EQ(ids.back(), static_cast<std::uint64_t>(2 * kRows));

    // And the mark did not move: the next omitted key is still 501.
    EXPECT_NE(d.Dispatch("INSERT INTO t VALUES (7)").response.find("id=501"), std::string::npos);
}

TEST_F(SuppliedKeySqlTest, OrderByWithLimitTakesTheLowestKeysAcrossALeafBoundary) {
    auto d = Dispatcher();
    CreateBtree(d);

    // **Withdrawn in its old shape** on BB-Q8 (b) and BB-R3. A descending
    // load used to put the lowest keys in a page's last slots, and this
    // pinned that LIMIT took them rather than the first slots - the case a
    // per-page sort had to get right. No key below the mark lands now, so a
    // page's first slots are its lowest keys. What an ascending load still
    // reaches is the subject's other half: LIMIT stops the walk part-way
    // through a page, so the rows must already be in key order when the
    // quota fills, and a window has to carry that order on into the next
    // leaf.
    const int kRows = 250;
    for (int id = 1; id <= kRows; ++id) {
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                             std::to_string(id) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }

    auto page1 = d.Dispatch("SELECT * FROM t ORDER BY id LIMIT 5");
    ASSERT_NE(page1.response.substr(0, 3), "ERR") << page1.response;
    EXPECT_EQ(EmittedIds(page1.response), (std::vector<std::uint64_t>{1, 2, 3, 4, 5}))
        << page1.response;

    // And OFFSET walks that same order across the first leaf's end: a row of
    // these two columns takes 41 bytes of a leaf, so the first leaf ends
    // near id 198, inside this window.
    auto page2 = d.Dispatch("SELECT * FROM t ORDER BY id LIMIT 40 OFFSET 170");
    ASSERT_NE(page2.response.substr(0, 3), "ERR") << page2.response;
    std::vector<std::uint64_t> window;
    for (std::uint64_t id = 171; id <= 210; ++id) window.push_back(id);
    EXPECT_EQ(EmittedIds(page2.response), window) << page2.response;
}

TEST_F(SuppliedKeySqlTest, OrderByThePkCostsNothing) {
    auto d = Dispatcher();
    CreateHeap(d);

    // The one path there is since BB: an id at or above the mark is appended
    // above every id on the page, so slot order *is* key order and the walk
    // is left untouched - the compiler discards the sort.
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

TEST_F(SuppliedKeySqlTest, AnUnorderedBackfillLandsOnlyTheKeysAboveTheRunningMark) {
    auto d = Dispatcher();
    CreateBtree(d);

    // Neither ordered nor reverse-ordered: the shape a real backfill or a
    // migration from another system produces. **It used to land whole;
    // that is withdrawn** (BB-Q8 (b), BB-R3). Each key is judged alone,
    // against the mark the keys before it left: one at or above it lands
    // and raises it, one below it - 400 included, though it falls in a gap
    // between two placed keys - is refused OutOfRange and moves nothing.
    const std::vector<int> ids = {50, 900, 10, 400, 25, 1000, 5, 700, 300, 1};
    const std::vector<int> landed = {50, 900, 1000};
    const auto lands = [&](int id) {
        return std::find(landed.begin(), landed.end(), id) != landed.end();
    };
    for (int id : ids) {
        auto out = d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                              std::to_string(id) + ")");
        if (lands(id)) {
            ASSERT_EQ(out.response.substr(0, 8), "INSERTED") << "id " << id << ": " << out.response;
        } else {
            EXPECT_EQ(out.response.substr(0, 3), "ERR") << "id " << id << ": " << out.response;
            EXPECT_NE(out.response.find("high-water mark"), std::string::npos)
                << "id " << id << ": " << out.response;
        }
    }

    for (int id : ids) {
        auto out = d.Dispatch("SELECT * FROM t WHERE id = " + std::to_string(id));
        if (lands(id)) {
            const std::string want = std::to_string(id) + "," + std::to_string(id);
            EXPECT_NE(out.response.find(want), std::string::npos) << "lost id " << id;
        } else {
            EXPECT_TRUE(EmittedIds(out.response).empty())
                << "refused id " << id << " landed: " << out.response;
        }
    }
    EXPECT_EQ(EmittedIds(d.Dispatch("SELECT * FROM t ORDER BY id").response),
              (std::vector<std::uint64_t>{50, 900, 1000}));

    // The refused keys moved nothing: the mark is the last landed key's.
    EXPECT_NE(d.Dispatch("INSERT INTO t VALUES (7)").response.find("id=1001"), std::string::npos);
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

// ---- Rollback across a split (BI4, docs/spec/txn.md §6) -----------------------
//
// The trail records a row as `(page_id, slot)`, so a structural change between
// a row's placement and its compensation is what a rollback has to survive.
// Until BB-S3 a key named below the relation's high-water mark could divide a
// leaf mid-statement - moving half of it and renumbering the half that stayed -
// and a rollback compensating a pre-division entry wrote over a row it never
// touched. SQL reaches no division since BB-R3 refuses that key; what it still
// reaches mid-statement is the append split, which moves nothing, and these
// cells pin rollback across it.

TEST_F(SuppliedKeyBulkTest, AFailedStatementThatSplitALeafRollsBackWhole) {
    CommandDispatcher& d = *d_;
    CreateBtree(d);

    // A committed base, ascending: 400 rows over three leaves (198, 198, 4).
    std::string base = "INSERT INTO t VALUES ";
    for (int k = 1; k <= 400; ++k) {
        base += (k == 1 ? "" : ", ");
        base += "(" + std::to_string(k * 1000) + ", " + std::to_string(k) + ")";
    }
    ASSERT_EQ(d.Dispatch(base).response.substr(0, 8), "INSERTED");
    const std::string committed = d.Dispatch("SELECT COUNT(*) FROM t").response;

    // One statement that append-splits the rightmost leaf twice over - 400
    // keys above the mark - and then fails on its last row: 1000 is already
    // there, a duplicate far below the mark (`AlreadyExists`, BB-R12). BI4
    // says the whole statement unwinds, across the leaves it opened.
    std::string doomed = "INSERT INTO t VALUES ";
    for (int k = 1; k <= 400; ++k) {
        doomed += "(" + std::to_string(400000 + k) + ", " + std::to_string(k) + "), ";
    }
    doomed += "(1000, 999)";
    auto failed = d.Dispatch(doomed);
    EXPECT_EQ(failed.response.substr(0, 3), "ERR") << failed.response;
    EXPECT_NE(failed.response.find("duplicate primary key"), std::string::npos)
        << failed.response;
    EXPECT_NE(failed.response.find("(row 401)"), std::string::npos) << failed.response;

    // Every row the statement placed before the duplicate must be gone,
    // including the ones on the leaves its splits created.
    EXPECT_EQ(d.Dispatch("SELECT COUNT(*) FROM t").response, committed)
        << "a failed statement left rows behind after splitting a leaf";

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

TEST_F(SuppliedKeyBulkTest, AnAbortedUpdateDoesNotSurviveAnAppendSplitInItsOwnTransaction) {
    // **Withdrawn in its old shape** on BB-Q8 (b) and BB-R3. This divided
    // the leaf an UPDATE had written, by naming keys below the mark inside
    // the same transaction, so the UPDATE's trail entry pointed at a slot
    // the division renumbered. A key below the mark is refused now, so SQL
    // reaches no division. The structural change it still reaches
    // mid-transaction is the append split, which moves nothing: this pins
    // that a rollback across append splits of the very leaf the UPDATE
    // wrote restores the UPDATE, retires every insert - from that leaf and
    // from the leaves the transaction itself opened - and leaves the
    // committed base whole.
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

    // The write on the rightmost leaf - the base's highest key - so its
    // trail entry is recorded against the leaf the inserts below will split.
    ASSERT_EQ(
        d.Dispatch("UPDATE t SET qty = 555 WHERE id = 300000", &session).response.substr(0, 3),
        "UPD");

    // Now split the leaf that row sits on, and the ones after it, by keys
    // above the mark.
    for (int k = 1; k <= 400; ++k) {
        auto out = d.Dispatch("INSERT INTO t VALUES (" + std::to_string(300000 + k) + ", " +
                                  std::to_string(k) + ")",
                              &session);
        ASSERT_EQ(out.response.substr(0, 8), "INSERTED") << out.response;
    }

    auto rolled = d.Dispatch("ROLLBACK", &session);
    EXPECT_EQ(rolled.response.substr(0, 3), "ROL") << rolled.response;

    // Both halves of the claim: the inserts are gone, and the UPDATE did not
    // outlive its own transaction.
    EXPECT_EQ(d.Dispatch("SELECT COUNT(*) FROM t").response, committed)
        << "the aborted inserts survived";
    auto row = d.Dispatch("SELECT * FROM t WHERE id = 300000");
    EXPECT_NE(row.response.find("300000,300"), std::string::npos)
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

TEST_F(SuppliedKeyBulkTest, ABulkStatementNamingKeysOutOfOrderIsRefusedWhole) {
    CommandDispatcher& d = *d_;
    CreateBtree(d);

    // Each row gates individually and in statement order (BI2), so an
    // unordered set is not a special case - it is N single-row inserts that
    // happen to share a statement. **Which is why it is refused now**
    // (BB-Q8 (b), BB-R3, withdrawing "a bulk statement may name keys in any
    // order"): row 1 raises the mark to 301, row 2 names 100 below it and is
    // refused OutOfRange with its ordinal, and the statement fails whole
    // (BI4).
    auto out = d.Dispatch("INSERT INTO t VALUES (300, 3), (100, 1), (200, 2)");
    EXPECT_EQ(out.response.substr(0, 3), "ERR") << out.response;
    EXPECT_NE(out.response.find("high-water mark"), std::string::npos) << out.response;
    EXPECT_NE(out.response.find("(row 2)"), std::string::npos) << out.response;

    // Nothing landed - not even row 1, which was placed before the refusal.
    for (int id : {100, 200, 300}) {
        EXPECT_TRUE(
            EmittedIds(d.Dispatch("SELECT * FROM t WHERE id = " + std::to_string(id)).response)
                .empty())
            << "id " << id << " outlived its refused statement";
    }

    // The refused row moved no mark. Row 1's admission did, and like every
    // id a failed statement's placed prefix took, it stays burned (BI9): the
    // next omitted key is 301.
    EXPECT_NE(d.Dispatch("INSERT INTO t VALUES (7)").response.find("id=301"), std::string::npos);
    // And that issued row is the relation's only one: the probes above read
    // an absence, which an erroring reply would also give.
    EXPECT_EQ(EmittedIds(d.Dispatch("SELECT * FROM t ORDER BY id").response),
              (std::vector<std::uint64_t>{301}));
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

TEST_F(SuppliedKeySqlTest, ADuplicateFarBelowTheMarkIsStillNamedADuplicate) {
    auto d = Dispatcher();
    CreateBtree(d);

    // **Withdrawn in its old shape** on BB-Q8 (b) and BB-R3: the key used to
    // be placed descending - 900, then 100 below it - which is refused now.
    // The key is placed by an ascending load instead, long enough that it
    // sits in a leaf with a right sibling, far below the mark.
    const int kRows = 300;
    for (int id = 1; id <= kRows; ++id) {
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                             std::to_string(id) + ")")
                      .response.substr(0, 8),
                  "INSERTED");
    }

    // The one a high-water-mark check would answer as merely below the
    // mark: 100 is far below 301, on a leaf with a right sibling, and only
    // a real lookup can know it is taken. The held leaf is read for it
    // first (BB-R3 step 5, before step 6), so the answer is AlreadyExists
    // (BB-R12) and a client that detects duplicates by it keeps working.
    auto dup = d.Dispatch("INSERT INTO t VALUES (100, 3)");
    EXPECT_EQ(dup.response.substr(0, 3), "ERR") << dup.response;
    EXPECT_NE(dup.response.find("duplicate primary key"), std::string::npos) << dup.response;
    EXPECT_EQ(dup.response.find("high-water mark"), std::string::npos)
        << "a present key is AlreadyExists, not OutOfRange (BB-R12): " << dup.response;

    // And the loser changed nothing: 100 keeps its value, the mark stays.
    EXPECT_NE(d.Dispatch("SELECT * FROM t WHERE id = 100").response.find("100,100"),
              std::string::npos);
    EXPECT_NE(d.Dispatch("INSERT INTO t VALUES (7)").response.find("id=301"), std::string::npos);
}

// ---- BB-S2: red on one core at BB's start ----------------------------------
//
// `instructions/v3.0.0/workorder-bb-issue-under-the-leaf.md`, on BB-Q8's mark:
// no relation is ever out of key order, so a named key below the mark is
// refused on a btree as on a heap, and nothing a refused key does may leave
// the relation unordered.

TEST_F(SuppliedKeySqlTest, ANamedKeyBelowTheMarkIsRefusedOnABtree) {
    auto d = Dispatcher();
    CreateBtree(d);
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (77, 1)").response.substr(0, 8), "INSERTED");

    // Absent and below the mark: OutOfRange (BB-R12).
    auto below = d.Dispatch("INSERT INTO t VALUES (33, 2)");
    EXPECT_EQ(below.response.substr(0, 3), "ERR") << below.response;
    EXPECT_NE(below.response.find("high-water mark"), std::string::npos) << below.response;
    // The code itself, which the outcome carries since BB-S3 (a KWP client
    // reads it rather than the line).
    EXPECT_EQ(below.status.code(), StatusCode::kOutOfRange) << below.status.message();
    EXPECT_EQ(EmittedIds(d.Dispatch("SELECT * FROM t").response),
              (std::vector<std::uint64_t>{77}));
}

TEST_F(SuppliedKeySqlTest, APresentKeyBelowTheMarkIsRefusedAsADuplicate) {
    // BB §1.11: the key-order flip ran before the descent looked for the
    // key, so an ordinary duplicate - refused at the descent - left the
    // relation unordered for good. The flip is gone with `kUnordered`
    // (BB-S3b); what stays is BB-R12's order - presence is asked before the
    // mark, so a present key is a duplicate, not a key below the mark.
    auto d = Dispatcher();
    CreateBtree(d);
    ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (1)").response.substr(0, 8), "INSERTED");
    auto dup = d.Dispatch("INSERT INTO t VALUES (1, 9)");
    EXPECT_EQ(dup.response.substr(0, 3), "ERR") << dup.response;
    EXPECT_NE(dup.response.find("duplicate primary key"), std::string::npos)
        << "a present key is refused AlreadyExists (BB-R12): " << dup.response;
    EXPECT_EQ(dup.status.code(), StatusCode::kAlreadyExists) << dup.status.message();
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
    // **Since SUS-1 that default is BTREE**, so what this half now pins is
    // the unchanged half of the claim: `EXPLICIT` still moves nothing, and
    // the relation lands on whatever the default is rather than on a form
    // the word dragged it to.
    auto defaulted = d.Dispatch("CREATE TABLE h2 (id int64, qty int64) EXPLICIT");
    EXPECT_EQ(defaulted.response.substr(0, 7), "CREATED") << defaulted.response;
    EXPECT_NE(d.Dispatch("DESCRIBE h2").response.find("clustered_type=BTREE"), std::string::npos);
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

// ---- The mark is per relation, and lives on its page ----------------------

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

TEST_F(SuppliedKeySqlTest, TheMarkThatRefusesAKeySurvivesAcrossDispatchers) {
    // **Withdrawn in its old shape** on BB-Q8 (b) and BB-R3: this pinned the
    // `kUnordered` flag a key below the mark set, read off the page by a
    // second dispatcher. That key is refused now and sets nothing, so there
    // is no flag to carry; what the second dispatcher must still read off
    // the page is the mark that refused it.
    {
        auto d = Dispatcher();
        CreateBtree(d);
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (77, 1)").response.substr(0, 8), "INSERTED");
        // Below 77's mark: refused, and nothing changes.
        ASSERT_EQ(d.Dispatch("INSERT INTO t VALUES (33, 2)").response.substr(0, 3), "ERR");
    }
    // A second dispatcher over the same catalog reads the mark off
    // `sys.tables` rather than remembering it - so it refuses the same key,
    // holds the one row, and issues above 77 rather than above 33.
    auto d2 = Dispatcher();
    auto again = d2.Dispatch("INSERT INTO t VALUES (33, 2)");
    EXPECT_EQ(again.response.substr(0, 3), "ERR") << again.response;
    EXPECT_NE(again.response.find("high-water mark"), std::string::npos) << again.response;
    EXPECT_EQ(EmittedIds(d2.Dispatch("SELECT * FROM t ORDER BY id").response),
              (std::vector<std::uint64_t>{77}));
    EXPECT_NE(d2.Dispatch("INSERT INTO t VALUES (3)").response.find("id=78"), std::string::npos);
}

}  // namespace
}  // namespace kds::server
