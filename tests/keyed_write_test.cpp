// DU: a write that finds its rows by a key
// (`instructions/v3.2.0/workorder-du-keyed-write.md`).
//
// Three families, one per claim the order makes of an `UPDATE` or a
// `DELETE` whose WHERE bounds its tuple key or names an index:
//
//   - **the leaves it holds** (`*HoldTest`): a write has no `ANALYZE` of its
//     leaves, so the pages it pins exclusive tell a bounded walk and a
//     descent from a walk of the whole relation (`leaf_watching_store.hpp`);
//   - **the rows it writes** (DU-R1): every statement leaves the relation
//     its walk leaves, the walk being the same WHERE written so that no arm
//     reads it (`Walked`);
//   - **the units it takes** (DU-R3): the window declared and the row units
//     asked are the walk's, read off the transaction's own ledger, and a
//     concurrent writer inside and outside them waits or does not as it did.

#include "leaf_watching_store.hpp"
#include "lock_family_fixture.hpp"
#include "two_core_rig.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"

namespace kds::server {
namespace {

// The same predicate, no longer a shape any arm reads: in parentheses, with
// one disjunct that adds no row. A group holding `OR` is one boolean
// conjunct - never a key, a bound or an index's (`parser-v2.md` I10) - so
// the statement walks its whole relation and answers as it did before DU.
std::string Walked(const std::string& predicate, const std::string& no_row) {
    return "((" + predicate + ") OR " + no_row + ")";
}

std::uint64_t OidOf(const std::string& insert_reply) {
    const auto at = insert_reply.find("oid=");
    EXPECT_NE(at, std::string::npos) << insert_reply;
    return at == std::string::npos ? 0 : std::strtoull(insert_reply.c_str() + at + 4, nullptr, 10);
}

// ---- The leaves a bounded write holds (DU-S1) -------------------------------

class BoundedWriteHoldTest : public ::testing::Test {
protected:
    static constexpr int kRows = 1200;

    void SetUp() override { Open(/*indexes=*/true); }

    void Open(bool indexes) {
        dispatcher_.reset();
        boot_.reset();
        inner_.emplace(kFirstUserPageId);
        store_.emplace(*inner_);
        auto boot = bootstrap::BootstrapDatabase(*store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, *store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, /*txns=*/nullptr,
                            txn::IsolationLevel::kReadCommitted, /*core_id=*/0, indexes);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    // `name`: ids 1..kRows in key order, v = 0, over several pages. Returns
    // its oid.
    std::uint64_t Load(const std::string& name, const std::string& create,
                       const std::string& row_tail = ", 0)") {
        EXPECT_EQ(Run(create).rfind("CREATED", 0), 0u) << create;
        std::string inserted;
        for (int id = 1; id <= kRows; ++id) {
            inserted = Run("INSERT INTO " + name + " VALUES (" + std::to_string(id) + row_tail);
            EXPECT_EQ(inserted.rfind("INSERTED", 0), 0u) << inserted;
        }
        return OidOf(inserted);
    }

    // The pages of `oid` that `sql` took exclusive.
    std::size_t Held(std::uint64_t oid, const std::string& sql, const std::string& want) {
        store_->Watch(oid);
        EXPECT_EQ(Run(sql), want) << sql;
        return store_->held();
    }

    std::optional<storage::InMemoryPageStore> inner_;
    std::optional<LeafWatchingStore> store_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// A bound on the tuple key - two-sided or one-sided, strict or not - is the
// leaves the walk visits: it seeks to the low key and ends at the first key
// past the high one, so at most the leaf after the window's last is taken.
TEST_F(BoundedWriteHoldTest, ABtreeWriteOverATupleKeyBoundHoldsTheLeavesOfItsWindow) {
    const std::uint64_t w = Load("w", "CREATE TABLE w (id int64, v int64) BTREE");
    const std::size_t every_leaf = Held(w, "UPDATE w SET v = 0 WHERE v < 0", "UPDATED 0");
    ASSERT_GE(every_leaf, 6u) << "the relation is too small to tell a bound from a walk";

    EXPECT_LE(Held(w, "UPDATE w SET v = v + 1 WHERE id BETWEEN 30 AND 31", "UPDATED 2"), 2u);
    EXPECT_LE(Held(w, "UPDATE w SET v = v + 1 WHERE id > 599 AND id < 602", "UPDATED 2"), 2u);
    // One-sided: the head of the relation, and its tail - where there is no
    // leaf past the window to take.
    EXPECT_LE(Held(w, "UPDATE w SET v = v + 1 WHERE id < 3", "UPDATED 2"), 2u);
    EXPECT_EQ(Held(w, "UPDATE w SET v = v + 1 WHERE id >= 1199", "UPDATED 2"), 1u);
    // A window over several leaves: more than one, fewer than the relation's.
    const std::size_t middle =
        Held(w, "UPDATE w SET v = v + 1 WHERE id BETWEEN 300 AND 900", "UPDATED 601");
    EXPECT_GT(middle, 1u);
    EXPECT_LT(middle, every_leaf);
    // Beside any other conjunct, wherever it is written.
    EXPECT_LE(Held(w, "UPDATE w SET v = v + 1 WHERE v = 1 AND id <= 31 AND id >= 30", "UPDATED 2"),
              2u);
    // A window of one key that is no equality, and a window past every row.
    EXPECT_LE(Held(w, "UPDATE w SET v = v + 1 WHERE id BETWEEN 30 AND 30", "UPDATED 1"), 2u);
    EXPECT_LE(Held(w, "UPDATE w SET v = v + 1 WHERE id > 5000", "UPDATED 0"), 1u);
    // An empty window names no row, and no leaf is fetched for it.
    EXPECT_EQ(Held(w, "UPDATE w SET v = v + 1 WHERE id > 40 AND id < 35", "UPDATED 0"), 0u);
    EXPECT_LE(Held(w, "DELETE FROM w WHERE id BETWEEN 600 AND 601", "DELETED 2"), 2u);

    EXPECT_EQ(Run("SELECT id, v FROM w WHERE v > 1"), "id,v\\n30,3\\n31,2");
    EXPECT_EQ(Run("SELECT COUNT(*) FROM w WHERE v = 1"), "count(*)\\n603");
    EXPECT_EQ(Run("SELECT COUNT(*) FROM w"), "count(*)\\n" + std::to_string(kRows - 2));

    // What still walks every leaf: the same bound where no arm reads it, and
    // a bound beside a subquery (DU-Q5 (a),
    // docs/pending/keyed-write-beside-a-subquery.md).
    EXPECT_EQ(Held(w, "UPDATE w SET v = 9 WHERE " + Walked("id >= 40 AND id <= 41", "id = 40"),
                   "UPDATED 2"),
              every_leaf);
    EXPECT_EQ(Held(w,
                   "UPDATE w SET v = 9 WHERE id BETWEEN 50 AND 51 AND "
                   "EXISTS (SELECT id FROM w WHERE id = 1)",
                   "UPDATED 2"),
              every_leaf);
}

// An embedded primary key's prefix or range is a tuple key window the WHERE
// does not spell (DR-S3e): the walk is bounded by it as a read's range is.
TEST_F(BoundedWriteHoldTest, AnEmbeddedKeysPrefixBoundsTheWalk) {
    ASSERT_EQ(Run("CREATE TABLE e (a int16, b int16, v int64, PRIMARY KEY (a, b)) BTREE")
                  .rfind("CREATED", 0),
              0u);
    std::string inserted;
    for (int a = 1; a <= 40; ++a) {
        for (int b = 1; b <= 30; ++b) {
            inserted = Run("INSERT INTO e VALUES (" + std::to_string(a) + ", " +
                           std::to_string(b) + ", 0)");
            ASSERT_EQ(inserted.rfind("INSERTED", 0), 0u) << inserted;
        }
    }
    const std::uint64_t e = OidOf(inserted);
    const std::size_t every_leaf = Held(e, "UPDATE e SET v = 0 WHERE v < 0", "UPDATED 0");
    ASSERT_GE(every_leaf, 6u);

    EXPECT_LE(Held(e, "UPDATE e SET v = v + 1 WHERE a = 7", "UPDATED 30"), 3u);
    EXPECT_LE(Held(e, "UPDATE e SET v = v + 1 WHERE a = 7 AND b BETWEEN 3 AND 5", "UPDATED 3"),
              2u);
    EXPECT_LE(Held(e, "DELETE FROM e WHERE a = 9 AND b BETWEEN 3 AND 5", "DELETED 3"), 2u);
    // A key column that is not the leading one names no window.
    EXPECT_EQ(Held(e, "UPDATE e SET v = v + 1 WHERE b = 30", "UPDATED 40"), every_leaf);
    EXPECT_EQ(Run("SELECT a, b, v FROM e WHERE v > 1"),
              "a,b,v\\n7,3,2\\n7,4,2\\n7,5,2\\n7,30,2");
    EXPECT_EQ(Run("SELECT COUNT(*) FROM e"), "count(*)\\n1197");
}

// A heap's pages are in no key order, so its window is a range of its
// Keystone index: the rows are collected through it and written by the
// point arm, and only the pages they are on are taken.
TEST_F(BoundedWriteHoldTest, AHeapWriteOverATupleKeyBoundHoldsThePagesItsRowsAreOn) {
    const std::uint64_t hp = Load("hp", "CREATE TABLE hp (id int64 PRIMARY KEY, v int64) HEAP");
    const std::size_t every_page = Held(hp, "UPDATE hp SET v = 0 WHERE v < 0", "UPDATED 0");
    ASSERT_GE(every_page, 6u);

    EXPECT_LE(Held(hp, "UPDATE hp SET v = v + 1 WHERE id BETWEEN 30 AND 31", "UPDATED 2"), 2u);
    // Shapes a read of a heap walks for (a strict bound, a one-sided one):
    // the write's window is its lock window, which folds both.
    EXPECT_LE(Held(hp, "UPDATE hp SET v = v + 1 WHERE id > 599 AND id < 602", "UPDATED 2"), 2u);
    EXPECT_LE(Held(hp, "UPDATE hp SET v = v + 1 WHERE id >= 1199", "UPDATED 2"), 2u);
    EXPECT_EQ(Held(hp, "UPDATE hp SET v = v + 1 WHERE id > 5000", "UPDATED 0"), 0u);
    EXPECT_EQ(Held(hp, "UPDATE hp SET v = v + 1 WHERE id > 40 AND id < 35", "UPDATED 0"), 0u);
    EXPECT_LE(Held(hp, "DELETE FROM hp WHERE id BETWEEN 600 AND 601", "DELETED 2"), 2u);
    // A deleted row's Keystone entry stays until a PURGE: the collection
    // names it, the point arm finds it delete-marked and writes nothing.
    EXPECT_LE(Held(hp, "UPDATE hp SET v = v + 1 WHERE id BETWEEN 599 AND 602", "UPDATED 2"), 2u);
    EXPECT_EQ(Held(hp, "UPDATE hp SET v = 9 WHERE " + Walked("id >= 40 AND id <= 41", "id = 40"),
                   "UPDATED 2"),
              every_page);
    EXPECT_EQ(Run("SELECT id, v FROM hp WHERE v > 0 ORDER BY id"),
              "id,v\\n30,1\\n31,1\\n40,9\\n41,9\\n599,1\\n602,1\\n1199,1\\n1200,1");

    // A hidden Keystone, bounded by `keyidentity`.
    const std::uint64_t hh = Load("hh", "CREATE TABLE hh (n int64, v int64) HEAP");
    const std::size_t hh_every = Held(hh, "UPDATE hh SET v = 0 WHERE v < 0", "UPDATED 0");
    ASSERT_GE(hh_every, 6u);
    EXPECT_LE(Held(hh, "UPDATE hh SET v = v + 1 WHERE keyidentity BETWEEN 30 AND 31", "UPDATED 2"),
              2u);
    EXPECT_EQ(Run("SELECT n, v FROM hh WHERE v > 0 ORDER BY n"), "n,v\\n30,1\\n31,1");
}

// `indexes = off` takes the Keystone range away, and the heap's walk
// answers: the same rows, every page.
TEST_F(BoundedWriteHoldTest, WithIndexesOffAHeapsBoundIsItsWalk) {
    Open(/*indexes=*/false);
    const std::uint64_t hp = Load("hp", "CREATE TABLE hp (id int64 PRIMARY KEY, v int64) HEAP");
    const std::size_t every_page = Held(hp, "UPDATE hp SET v = 0 WHERE v < 0", "UPDATED 0");
    ASSERT_GE(every_page, 6u);
    EXPECT_EQ(Held(hp, "UPDATE hp SET v = v + 1 WHERE id BETWEEN 30 AND 31", "UPDATED 2"),
              every_page);
    EXPECT_EQ(Held(hp, "DELETE FROM hp WHERE id BETWEEN 600 AND 601", "DELETED 2"), every_page);
    EXPECT_EQ(Run("SELECT id, v FROM hp WHERE v > 0 ORDER BY id"), "id,v\\n30,1\\n31,1");
    // A btree's bound reads no index, and the switch does not reach it.
    const std::uint64_t w = Load("w", "CREATE TABLE w (id int64, v int64) BTREE");
    EXPECT_LE(Held(w, "UPDATE w SET v = v + 1 WHERE id BETWEEN 30 AND 31", "UPDATED 2"), 2u);
}

// ---- DU-R1: the rows written are the walk's ---------------------------------

// Two instances of one data set. Each statement runs keyed in one and walked
// in the other, and the relations are compared after every statement - the
// reply first, since a count is an answer too.
class KeyedWriteTwin {
public:
    explicit KeyedWriteTwin(bool indexes = true) {
        for (Side* side : {&keyed_, &walked_}) {
            auto boot = bootstrap::BootstrapDatabase(side->store, 1000);
            EXPECT_TRUE(boot.ok()) << boot.status().message();
            side->boot.emplace(std::move(boot.value()));
            side->dispatcher.emplace(side->boot->superblock, side->boot->catalog, side->store,
                                     /*log=*/nullptr, /*clock=*/nullptr, /*wal=*/nullptr,
                                     wal::DurabilityClass::kRelaxed, exec::Budget(),
                                     /*recorder=*/nullptr, /*replay=*/false,
                                     /*access_statistics=*/false, /*cabins=*/nullptr,
                                     /*txns=*/nullptr, txn::IsolationLevel::kReadCommitted,
                                     /*core_id=*/0, indexes);
        }
    }

    // On both sides.
    void Both(const std::string& sql) {
        for (Side* side : {&keyed_, &walked_}) {
            const std::string out = side->dispatcher->Dispatch(sql).response;
            EXPECT_NE(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        }
    }

    // `head` + `predicate` keyed, `head` + its walked form walked; then every
    // relation of `relations` compared. Every relation here has a column `v`
    // that is never negative, so `v < 0` is the disjunct that adds no row.
    void Same(const std::string& head, const std::string& predicate,
              const std::vector<std::string>& relations) {
        const std::string keyed = keyed_.dispatcher->Dispatch(head + predicate).response;
        const std::string walked =
            walked_.dispatcher->Dispatch(head + Walked(predicate, "v < 0")).response;
        EXPECT_NE(keyed.rfind("ERR", 0), 0u) << head << predicate << " -> " << keyed;
        EXPECT_EQ(keyed, walked) << head << predicate;
        for (const std::string& relation : relations) {
            const std::string all = "SELECT * FROM " + relation;
            EXPECT_EQ(keyed_.dispatcher->Dispatch(all).response,
                      walked_.dispatcher->Dispatch(all).response)
                << "after " << head << predicate;
        }
    }

private:
    struct Side {
        storage::InMemoryPageStore store{kFirstUserPageId};
        std::optional<bootstrap::BootstrapResult> boot;
        std::optional<CommandDispatcher> dispatcher;
    };
    Side keyed_;
    Side walked_;
};

TEST(KeyedWriteSameRowsTest, EveryBoundedWriteLeavesWhatItsWalkLeaves) {
    KeyedWriteTwin twin;
    twin.Both("CREATE TABLE w (id int64, v int64, s varchar(16) NULL) BTREE");
    twin.Both("CREATE TABLE hp (id int64 PRIMARY KEY, v int64, s varchar(16) NULL) HEAP");
    twin.Both("CREATE TABLE e (a int16, b int16, v int64, PRIMARY KEY (a, b)) BTREE");
    for (int id = 1; id <= 400; ++id) {
        const std::string row = "(" + std::to_string(id) + ", " + std::to_string(id % 7) + ", " +
                                (id % 5 == 0 ? "NULL" : "'s" + std::to_string(id % 3) + "'") + ")";
        twin.Both("INSERT INTO w VALUES " + row);
        twin.Both("INSERT INTO hp VALUES " + row);
        twin.Both("INSERT INTO e VALUES (" + std::to_string(id % 20) + ", " +
                  std::to_string(id) + ", 0)");
    }
    // Deleted rows inside and at the edge of every window below.
    twin.Both("DELETE FROM w WHERE id = 120");
    twin.Both("DELETE FROM hp WHERE id = 120");
    twin.Both("DELETE FROM w WHERE id = 100");
    twin.Both("DELETE FROM hp WHERE id = 100");

    const std::vector<std::string> all = {"w", "hp", "e"};
    for (const char* relation : {"w", "hp"}) {
        const std::string update = std::string("UPDATE ") + relation + " SET v = v + 100 WHERE ";
        const std::string remove = std::string("DELETE FROM ") + relation + " WHERE ";
        twin.Same(update, "id BETWEEN 100 AND 140", all);
        twin.Same(update, "id > 100 AND id < 140 AND s = 's1'", all);
        twin.Same(update, "id >= 390", all);
        twin.Same(update, "id < 5", all);
        twin.Same(update, "id <= 150 AND id >= 150", all);
        twin.Same(update, "id BETWEEN 100 AND 140 AND s IS NULL", all);
        twin.Same(update, "id > 120 AND id < 120", all);
        twin.Same(update, "id > 100 AND v = 103 AND id <= 300", all);
        twin.Same(update, "id BETWEEN 10 AND 20 AND id BETWEEN 15 AND 30", all);
        twin.Same(update, "id > 4000", all);
        // A SET that reads and moves the columns its WHERE reads.
        twin.Same(std::string("UPDATE ") + relation + " SET v = v + 1, s = 's1' WHERE ",
                  "id BETWEEN 200 AND 260 AND s = 's0'", all);
        twin.Same(remove, "id BETWEEN 300 AND 310", all);
        twin.Same(remove, "id > 395", all);
        twin.Same(remove, "id BETWEEN 300 AND 320 AND v > 3", all);
        // Over a window whose rows are all deleted.
        twin.Same(update, "id BETWEEN 300 AND 310", all);
    }
    // **Retired slots inside a window** (a `PURGE`'s; a slot with no key),
    // and keys placed back into the gap: the bounded walk's stop rests on
    // the keyed slots of a leaf ascending, which a retired slot does not
    // break and a key placed among them must not.
    twin.Both("DELETE FROM w WHERE id BETWEEN 40 AND 90");
    twin.Both("PURGE FROM w WHERE id BETWEEN 40 AND 90");
    for (const int id : {70, 45, 88, 41, 60}) {
        twin.Both("INSERT INTO w VALUES (" + std::to_string(id) + ", 5, 's1')");
    }
    twin.Same("UPDATE w SET v = v + 100 WHERE ", "id BETWEEN 35 AND 95", all);
    twin.Same("UPDATE w SET v = v + 100 WHERE ", "id > 44 AND id < 71", all);
    twin.Same("UPDATE w SET v = v + 100 WHERE ", "id >= 88 AND id <= 91", all);
    twin.Same("DELETE FROM w WHERE ", "id BETWEEN 42 AND 69", all);
    twin.Same("UPDATE w SET v = v + 100 WHERE ", "id BETWEEN 35 AND 95", all);

    twin.Same("UPDATE e SET v = v + 1 WHERE ", "a = 7", all);
    twin.Same("UPDATE e SET v = v + 1 WHERE ", "a = 7 AND b BETWEEN 100 AND 300", all);
    twin.Same("UPDATE e SET v = v + 1 WHERE ", "a = 7 AND b > 100", all);
    twin.Same("UPDATE e SET v = v + 1 WHERE ", "a = 19 AND v = 1", all);
    twin.Same("DELETE FROM e WHERE ", "a = 3 AND b BETWEEN 1 AND 200", all);
    twin.Same("DELETE FROM e WHERE ", "a = 4", all);
    twin.Same("UPDATE e SET v = v + 1 WHERE ", "a = 4", all);
}

// ---- DU-R3: the units are the walk's ----------------------------------------

class KeyedWriteLockTest : public LockFamilyTest {
protected:
    void SetUp() override {
        LockFamilyTest::SetUp();
        ASSERT_EQ(Local("CREATE TABLE b (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
        ASSERT_EQ(Local("CREATE TABLE h (id int64 PRIMARY KEY, v int64) HEAP").rfind("CREATED", 0),
                  0u);
        ASSERT_EQ(Local("CREATE TABLE e (a int16, b int16, v int64, PRIMARY KEY (a, b)) BTREE")
                      .rfind("CREATED", 0),
                  0u);
        std::string b;
        std::string h;
        std::string e;
        for (int id = 1; id <= 10; ++id) {
            if (id == 5) continue;  // a key inside every window that no row holds
            const std::string row = " VALUES (" + std::to_string(id) + ", 0)";
            b = Local("INSERT INTO b" + row);
            h = Local("INSERT INTO h" + row);
            e = Local("INSERT INTO e VALUES (1, " + std::to_string(id) + ", 0)");
            ASSERT_EQ(e.rfind("INSERTED", 0), 0u) << e;
        }
        ASSERT_EQ(Local("INSERT INTO e VALUES (2, 1, 0)").rfind("INSERTED", 0), 0u);
        b_ = static_cast<catalog::Oid>(OidOf(b));
        h_ = static_cast<catalog::Oid>(OidOf(h));
        e_ = static_cast<catalog::Oid>(OidOf(e));
    }

    std::string In(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    catalog::Oid b_ = 0;
    catalog::Oid h_ = 0;
    catalog::Oid e_ = 0;
};

// The window a bounded write declares is the one its walk declared, in the
// walk's mode, and it takes no row unit under it - on a btree, whose walk is
// bounded, and on a heap, whose rows come through the Keystone index.
TEST_F(KeyedWriteLockTest, ABoundedWriteDeclaresTheWalksWindowAndNoRowUnit) {
    struct Case {
        const char* relation;
        catalog::Oid oid;
    };
    for (const Case& c : {Case{"b", b_}, Case{"h", h_}}) {
        const std::string name = c.relation;
        Session s;
        ASSERT_EQ(In(s, "BEGIN").rfind("BEGIN", 0), 0u);
        ASSERT_EQ(In(s, "UPDATE " + name + " SET v = v + 1 WHERE id BETWEEN 3 AND 7"), "UPDATED 4");
        const txn::LockHoldings& held = s.transaction()->borrows();
        EXPECT_TRUE(held.HoldsAt(txn::LockKey::Range(c.oid, 3, 8), txn::LockMode::kUpdate)) << name;
        // The relation's intention and the window: nothing per row.
        EXPECT_EQ(held.size(), 2u) << name;
        for (std::uint64_t id = 3; id <= 7; ++id) {
            EXPECT_FALSE(held.Holds(txn::LockKey::Tuple(c.oid, id))) << name << " id " << id;
        }
        ASSERT_EQ(In(s, "DELETE FROM " + name + " WHERE id > 8"), "DELETED 2");
        EXPECT_TRUE(held.HoldsAt(txn::LockKey::Range(c.oid, 9, kIdSpaceEnd),
                                 txn::LockMode::kExclusive))
            << name;
        EXPECT_EQ(held.size(), 3u) << name;
        ASSERT_EQ(In(s, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
        EXPECT_EQ(locks_->EntryCount(), 0u) << name;
    }
}

// An embedded key's prefix bounds the walk and declares nothing, as before
// DU: the rows written take their row units.
TEST_F(KeyedWriteLockTest, AnEmbeddedKeysBoundDeclaresNothingAndTakesRowUnits) {
    Session s;
    ASSERT_EQ(In(s, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(In(s, "UPDATE e SET v = v + 1 WHERE a = 1 AND b BETWEEN 2 AND 4"), "UPDATED 3");
    const txn::LockHoldings& held = s.transaction()->borrows();
    // The relation's intention and three rows.
    EXPECT_EQ(held.size(), 4u);
    ASSERT_EQ(In(s, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
}

// A concurrent insert of a key inside the window waits for the bounded
// write's decide, and one outside it does not - the fence the walk held.
TEST_F(KeyedWriteLockTest, AnInsertInsideABoundedWritesWindowWaitsAndOneOutsideDoesNot) {
    for (const char* relation : {"b", "h"}) {
        const std::string name = relation;
        Session writer;
        ASSERT_EQ(In(writer, "BEGIN").rfind("BEGIN", 0), 0u);
        ASSERT_EQ(In(writer, "UPDATE " + name + " SET v = v + 1 WHERE id BETWEEN 3 AND 7"),
                  "UPDATED 4");

        Session outside;
        const DispatchOutcome placed =
            RunAsync("INSERT INTO " + name + " VALUES (20, 0)", outside);
        EXPECT_EQ(placed.response.rfind("INSERTED", 0), 0u) << placed.response;

        Session inside;
        Started held = Start("INSERT INTO " + name + " VALUES (5, 0)", inside);
        Pump();
        ASSERT_FALSE(*held.done) << name << ": an insert inside the window did not wait: "
                                 << held.out->response;
        ASSERT_EQ(In(writer, "COMMIT").rfind("COMMIT", 0), 0u);
        Pump();
        ASSERT_TRUE(*held.done) << name;
        EXPECT_EQ(held.out->response.rfind("INSERTED", 0), 0u) << held.out->response;
        // The row placed after the decide was never the write's.
        EXPECT_EQ(Local("SELECT id, v FROM " + name + " WHERE id BETWEEN 3 AND 7 ORDER BY id"),
                  "id,v\\n3,1\\n4,1\\n5,0\\n6,1\\n7,1");
        EXPECT_EQ(locks_->EntryCount(), 0u) << name;
    }
}

// A bounded walk that declares nothing parks on a held row and resumes at
// it: the rows below are not written again, the rows past the bound are not
// read into the statement, and the count is the statement's.
TEST_F(KeyedWriteLockTest, ABoundedWalkParkedOnAHeldRowResumesInsideItsBound) {
    Session holder;
    ASSERT_EQ(In(holder, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(In(holder, "UPDATE e SET v = 50 WHERE a = 1 AND b = 6"), "UPDATED 1");

    Session writer;
    Started walk = Start("UPDATE e SET v = v + 100 WHERE a = 1", writer);
    Pump();
    ASSERT_FALSE(*walk.done) << "the walk passed a held row: " << walk.out->response;
    ASSERT_EQ(In(holder, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*walk.done);
    EXPECT_EQ(walk.out->response, "UPDATED 9") << walk.out->response;
    // Every row of the prefix once, and the row of another prefix not at all.
    EXPECT_EQ(Local("SELECT COUNT(*) FROM e WHERE a = 1 AND v = 100"), "count(*)\\n9");
    EXPECT_EQ(Local("SELECT a, b, v FROM e WHERE a = 2"), "a,b,v\\n2,1,0");
    EXPECT_EQ(locks_->EntryCount(), 0u);
}

// **The heap arm's own resume has no cell here, and is not reachable by a
// held row**: a window of more than one key is declared before the first
// row, so a held row is waited for at the declaration with nothing written,
// and a window of one key writes one row. What a key loop does when it parks
// with rows written - collect again, start at the key it stopped on - is
// `WriteKeys`' one copy for every arm, and DU-S2's index arm, which declares
// nothing, is where a held row reaches it.

// DU-Q2 (a): a first collection past `max_locks_per_txn` is not written
// from. The walk answers, and the index is counted as used by neither.
class KeyedWriteCapTest : public KeyedWriteLockTest {
protected:
    std::size_t LockCap() const override { return 6; }

    std::string KeystoneUse() {
        return Local("SELECT ranges, rows_resolved FROM sys.indexes WHERE rel_name = 'h'");
    }
};

TEST_F(KeyedWriteCapTest, AHeapWindowPastTheCollectionsBoundIsAnsweredByTheWalk) {
    const std::string unused = KeystoneUse();
    // Nine rows in the window, a cap of six: the walk writes them, under the
    // window the statement declared, which takes no row unit.
    EXPECT_EQ(Local("UPDATE h SET v = v + 1 WHERE id > 0"), "UPDATED 9");
    EXPECT_EQ(KeystoneUse(), unused) << "a collection the walk answered for was counted";
    // Four rows: inside the cap, and through the Keystone index.
    EXPECT_EQ(Local("UPDATE h SET v = v + 1 WHERE id BETWEEN 1 AND 4"), "UPDATED 4");
    EXPECT_NE(KeystoneUse(), unused);
    EXPECT_EQ(Local("SELECT id, v FROM h ORDER BY id"),
              "id,v\\n1,2\\n2,2\\n3,2\\n4,2\\n6,1\\n7,1\\n8,1\\n9,1\\n10,1");
}

// ---- The leaves an index-driven write holds (DU-S2) --------------------------

// `x`: ids 1..kRows, `owner = id % 300` - four rows a value, each on a leaf
// of its own - `grp = id % 3`, `tag` a string of `id % 50`, `v` 0.
class IndexWriteHoldTest : public BoundedWriteHoldTest {
protected:
    void LoadIndexed(const std::string& storage) {
        ASSERT_EQ(Run("CREATE TABLE x (id int64" +
                      std::string(storage == "HEAP" ? " PRIMARY KEY" : "") +
                      ", owner int64, grp int64, tag varchar(24), v int64) " + storage)
                      .rfind("CREATED", 0),
                  0u);
        ASSERT_EQ(Run("CREATE INDEX x_owner ON x (owner)").rfind("CREATED", 0), 0u);
        ASSERT_EQ(Run("CREATE INDEX x_grp_owner ON x (grp, owner)").rfind("CREATED", 0), 0u);
        ASSERT_EQ(Run("CREATE INDEX x_tag ON x (tag)").rfind("CREATED", 0), 0u);
        std::string inserted;
        for (int id = 1; id <= kRows; ++id) {
            inserted = Run("INSERT INTO x VALUES (" + std::to_string(id) + ", " +
                           std::to_string(id % 300) + ", " + std::to_string(id % 3) +
                           ", 'tag-with-a-long-shared-prefix-" + std::to_string(id % 50) +
                           "', 0)");
            ASSERT_EQ(inserted.rfind("INSERTED", 0), 0u) << inserted;
        }
        x_ = OidOf(inserted);
        every_page_ = Held(x_, "UPDATE x SET v = 0 WHERE v < 0", "UPDATED 0");
        ASSERT_GE(every_page_, 10u) << "the relation is too small to tell an index from a walk";
    }

    std::uint64_t x_ = 0;
    std::size_t every_page_ = 0;
};

// An equality on an indexed column, a list on it, and an equality prefix of
// a composite index: the rows are found through the index and only the
// leaves they are on are taken - at most one a row, where the walk takes
// every leaf whatever it writes.
TEST_F(IndexWriteHoldTest, ABtreeWriteByAnIndexedColumnHoldsOnlyItsRowsLeaves) {
    LoadIndexed("BTREE");
    // owner = 7: ids 7, 307, 607, 907, all of grp 1.
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 7", "UPDATED 4"), 4u);
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE owner IN (7, 8, 450)", "UPDATED 8"), 8u);
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE grp = 1 AND owner = 7", "UPDATED 4"), 4u);
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 7 AND v = 3", "UPDATED 4"), 4u);
    EXPECT_LE(Held(x_, "DELETE FROM x WHERE owner = 9 AND grp = 0", "DELETED 4"), 4u);
    // A miss is an index read and no leaf of the relation: no such value,
    // and a pair of values no row holds together.
    EXPECT_EQ(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 4242", "UPDATED 0"), 0u);
    EXPECT_EQ(Held(x_, "UPDATE x SET v = v + 1 WHERE grp = 0 AND owner = 7", "UPDATED 0"), 0u);
    // A one-sided bound on the tuple key beside the index: the index finds
    // the rows and the bound drops the keys outside it.
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 8 AND id > 600", "UPDATED 2"), 2u);
    // A string key is stored truncated (IX6), so a run may name rows of
    // other values and the WHERE on the row decides.
    EXPECT_EQ(Run("UPDATE x SET v = v + 1 WHERE tag = 'tag-with-a-long-shared-prefix-11'"),
              "UPDATED 24");
    EXPECT_EQ(Run("SELECT id, v FROM x WHERE owner = 7"), "id,v\\n7,4\\n307,4\\n607,4\\n907,4");
    EXPECT_EQ(Run("SELECT id, v FROM x WHERE owner = 8"), "id,v\\n8,1\\n308,1\\n608,2\\n908,2");

    // What still walks: the same equality where no arm reads it, a column
    // with no index, and a subquery beside the equality (DU-Q5 (a)).
    EXPECT_EQ(Held(x_, "UPDATE x SET v = 9 WHERE " + Walked("owner = 13", "v < 0"), "UPDATED 4"),
              every_page_);
    EXPECT_EQ(Held(x_, "UPDATE x SET v = v WHERE v = 9", "UPDATED 4"), every_page_);
    EXPECT_EQ(Held(x_,
                   "UPDATE x SET v = 8 WHERE owner = 14 AND EXISTS (SELECT id FROM x WHERE id = 1)",
                   "UPDATED 4"),
              every_page_);
}

// With both ends of a tuple key range written, the range is the arm, as it
// is a read's: the walk is bounded and the index is not read.
TEST_F(IndexWriteHoldTest, ATwoSidedTupleKeyRangeOutranksTheIndex) {
    LoadIndexed("BTREE");
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 7 AND id BETWEEN 300 AND 310",
                   "UPDATED 1"),
              2u);
    EXPECT_EQ(Run("SELECT id, v FROM x WHERE v > 0"), "id,v\\n307,1");
    EXPECT_EQ(Run("SELECT name, probes, ranges FROM sys.indexes WHERE name = 'x_owner'"),
              "name,probes,ranges\\nx_owner,0,0");
}

// The rank is the read ladder's, where it differs too: a strict end is no
// range to a read, and a heap has none at all, so there the index is the arm
// and the bound drops the keys outside it.
TEST_F(IndexWriteHoldTest, AStrictRangeAndAHeapsRangeRankBelowTheIndex) {
    const std::string used = "SELECT probes FROM sys.indexes WHERE name = 'x_owner'";
    LoadIndexed("BTREE");
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 7 AND id > 299 AND id < 311",
                   "UPDATED 1"),
              1u);
    EXPECT_EQ(Run(used), "probes\\n1");
    Open(/*indexes=*/true);
    LoadIndexed("HEAP");
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 7 AND id BETWEEN 300 AND 310",
                   "UPDATED 1"),
              1u);
    EXPECT_EQ(Run(used), "probes\\n1");
    EXPECT_EQ(Run("SELECT id, v FROM x WHERE v > 0"), "id,v\\n307,1");
}

// **On a heap a keyed write meets its rows in key order, and its walk in
// page order** (DU-Q7 (a), an accepted difference). A check made per row
// against the rows as they stand - here a unique key shifted by one - can
// answer one order and refuse the other. A btree's walk is in key order, so
// there the two agree.
TEST_F(IndexWriteHoldTest, OnAHeapAnOrderDependentWriteIsJudgedInKeyOrder) {
    ASSERT_EQ(Run("CREATE TABLE s (id int64 PRIMARY KEY, owner int64, u int64) HEAP")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Run("CREATE UNIQUE INDEX s_u ON s (u)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Run("CREATE INDEX s_owner ON s (owner)").rfind("CREATED", 0), 0u);
    // Placed in the order 10, then 5: the page's order is not the keys'.
    ASSERT_EQ(Run("INSERT INTO s VALUES (10, 1, 2)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Run("INSERT INTO s VALUES (5, 1, 1)").rfind("INSERTED", 0), 0u);
    // Key order: id 5 takes u = 2 while id 10 still holds it.
    const std::string keyed = Run("UPDATE s SET u = u + 1 WHERE owner = 1");
    EXPECT_EQ(keyed.rfind("ERR", 0), 0u) << keyed;
    EXPECT_NE(keyed.find("s_u"), std::string::npos) << keyed;
    EXPECT_EQ(Run("SELECT id, u FROM s ORDER BY id"), "id,u\\n5,1\\n10,2");
    // Page order: id 10 moves to 3 first, and id 5 then takes the 2 it left.
    EXPECT_EQ(Run("UPDATE s SET u = u + 1 WHERE " + Walked("owner = 1", "u < 0")), "UPDATED 2");
    EXPECT_EQ(Run("SELECT id, u FROM s ORDER BY id"), "id,u\\n5,2\\n10,3");
}

TEST_F(IndexWriteHoldTest, AHeapWriteByAnIndexedColumnHoldsOnlyItsRowsPages) {
    LoadIndexed("HEAP");
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 7", "UPDATED 4"), 4u);
    EXPECT_LE(Held(x_, "DELETE FROM x WHERE owner IN (8, 9) AND grp = 0", "DELETED 4"), 8u);
    EXPECT_LE(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 8 AND id > 600", "UPDATED 2"), 2u);
    EXPECT_EQ(Run("SELECT COUNT(*) FROM x WHERE v = 1"), "count(*)\\n6");
    EXPECT_EQ(Held(x_, "UPDATE x SET v = 9 WHERE " + Walked("owner = 13", "v < 0"), "UPDATED 4"),
              every_page_);
}

TEST_F(IndexWriteHoldTest, WithIndexesOffAnIndexedWriteWalks) {
    Open(/*indexes=*/false);
    LoadIndexed("BTREE");
    EXPECT_EQ(Held(x_, "UPDATE x SET v = v + 1 WHERE owner = 7", "UPDATED 4"), every_page_);
    EXPECT_EQ(Held(x_, "DELETE FROM x WHERE owner IN (8, 9) AND grp = 0", "DELETED 4"),
              every_page_);
}

// ---- DU-R1 for an index-driven write ----------------------------------------

TEST(KeyedWriteSameRowsTest, EveryIndexedWriteLeavesWhatItsWalkLeaves) {
    KeyedWriteTwin twin;
    for (const char* create :
         {"CREATE TABLE x (id int64, owner int64, grp int64, tag varchar(24), code int64, "
          "v int64) BTREE",
          "CREATE TABLE hx (id int64 PRIMARY KEY, owner int64, grp int64, tag varchar(24), "
          "code int64, v int64) HEAP"}) {
        twin.Both(create);
    }
    for (const char* relation : {"x", "hx"}) {
        const std::string r = relation;
        twin.Both("CREATE INDEX " + r + "_owner ON " + r + " (owner)");
        twin.Both("CREATE INDEX " + r + "_grp_owner ON " + r + " (grp, owner) COVERING (v)");
        twin.Both("CREATE INDEX " + r + "_tag ON " + r + " (tag)");
        twin.Both("CREATE UNIQUE INDEX " + r + "_uniq ON " + r + " (grp, code)");
        for (int id = 1; id <= 300; ++id) {
            twin.Both("INSERT INTO " + r + " VALUES (" + std::to_string(id) + ", " +
                      std::to_string(id % 10) + ", " + std::to_string(id % 3) +
                      ", 'a-tag-longer-than-its-indexed-prefix-" + std::to_string(id % 4) +
                      "', " + std::to_string(5000 + id) + ", 0)");
        }
        // The entries an earlier version left: a key that moved, one that
        // came back, a deleted row's.
        twin.Both("UPDATE " + r + " SET owner = 3 WHERE id = 14");
        twin.Both("UPDATE " + r + " SET owner = 5 WHERE id = 23");
        twin.Both("UPDATE " + r + " SET owner = 3 WHERE id = 23");
        twin.Both("DELETE FROM " + r + " WHERE id = 33");
    }
    const std::vector<std::string> all = {"x", "hx"};
    for (const char* relation : {"x", "hx"}) {
        const std::string update = std::string("UPDATE ") + relation + " SET v = v + 100 WHERE ";
        const std::string remove = std::string("DELETE FROM ") + relation + " WHERE ";
        twin.Same(update, "owner = 3", all);
        twin.Same(update, "owner = 4", all);   // the key id 14 left
        twin.Same(update, "owner = 5", all);   // the key id 23 visited
        twin.Same(update, "owner = 77", all);  // a miss
        twin.Same(update, "owner IN (3, 4, 77, 3)", all);
        twin.Same(update, "owner IN (1, 2) AND grp = 1", all);
        twin.Same(update, "grp = 2 AND owner = 6", all);
        twin.Same(update, "grp = 2", all);
        twin.Same(update, "grp = 1 AND owner IN (1, 2, 3)", all);
        twin.Same(update, "owner = 3 AND v > 150", all);
        twin.Same(update, "owner = 3 AND id > 100", all);
        twin.Same(update, "owner = 3 AND id < 100 AND grp = 0", all);
        twin.Same(update, "grp = 1 AND code = 5004", all);  // the unique key whole
        twin.Same(update, "grp = 1 AND code > 5200", all);
        twin.Same(update, "tag = 'a-tag-longer-than-its-indexed-prefix-2'", all);
        twin.Same(update, "tag = 'a-tag-longer-than-its-indexed-prefix-2' AND owner = 2", all);
        // A statement that moves the key it is found by: off it, onto
        // another value's run, and onto its own.
        twin.Same(std::string("UPDATE ") + relation + " SET owner = owner + 1 WHERE ", "owner = 8",
                  all);
        twin.Same(std::string("UPDATE ") + relation + " SET owner = 9, v = v + 1 WHERE ",
                  "owner IN (9, 1)", all);
        twin.Same(std::string("UPDATE ") + relation + " SET owner = 2 WHERE ", "owner = 2", all);
        twin.Same(std::string("UPDATE ") + relation + " SET grp = 2, v = v + 1 WHERE ",
                  "grp = 0 AND owner = 0", all);
        twin.Same(remove, "owner = 9", all);
        twin.Same(remove, "owner IN (0, 4) AND grp = 1", all);
        twin.Same(remove, "grp = 2 AND owner = 6", all);
        // Over rows a statement above deleted or moved.
        twin.Same(update, "owner = 9", all);
        twin.Same(update, "owner IN (8, 9, 1)", all);
        twin.Same(remove, "tag = 'a-tag-longer-than-its-indexed-prefix-1' AND v > 150", all);
    }
}

// ---- DU-R3 for an index-driven write ----------------------------------------

class IndexWriteLockTest : public LockFamilyTest {
protected:
    void SetUp() override {
        LockFamilyTest::SetUp();
        for (const char* create : {"CREATE TABLE b (id int64, owner int64, v int64) BTREE",
                                   "CREATE TABLE h (id int64 PRIMARY KEY, owner int64, v int64) HEAP"}) {
            ASSERT_EQ(Local(create).rfind("CREATED", 0), 0u) << create;
        }
        ASSERT_EQ(Local("CREATE INDEX b_owner ON b (owner)").rfind("CREATED", 0), 0u);
        ASSERT_EQ(Local("CREATE INDEX h_owner ON h (owner)").rfind("CREATED", 0), 0u);
        std::string b;
        std::string h;
        for (int id = 1; id <= 12; ++id) {
            const std::string row =
                " VALUES (" + std::to_string(id) + ", " + std::to_string(id % 3) + ", 0)";
            b = Local("INSERT INTO b" + row);
            h = Local("INSERT INTO h" + row);
            ASSERT_EQ(h.rfind("INSERTED", 0), 0u) << h;
        }
        b_ = static_cast<catalog::Oid>(OidOf(b));
        h_ = static_cast<catalog::Oid>(OidOf(h));
    }

    std::string In(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    catalog::Oid b_ = 0;
    catalog::Oid h_ = 0;
};

// An index-driven write declares nothing and takes the row unit of each row
// it writes, in the walk's mode - and of no row its index named and its
// WHERE declined.
TEST_F(IndexWriteLockTest, AnIndexedWriteTakesTheRowUnitsOfTheRowsItWrites) {
    struct Case {
        const char* relation;
        catalog::Oid oid;
    };
    for (const Case& c : {Case{"b", b_}, Case{"h", h_}}) {
        const std::string name = c.relation;
        // id 4 leaves owner 1: its entry under 1 stays (IX2).
        ASSERT_EQ(Local("UPDATE " + name + " SET owner = 7 WHERE id = 4"), "UPDATED 1");
        Session s;
        ASSERT_EQ(In(s, "BEGIN").rfind("BEGIN", 0), 0u);
        // owner = 1: ids 1, 7, 10, and 4 by the entry it left.
        ASSERT_EQ(In(s, "UPDATE " + name + " SET v = v + 1 WHERE owner = 1 AND v < 5"),
                  "UPDATED 3");
        const txn::LockHoldings& held = s.transaction()->borrows();
        for (const std::uint64_t id : {1, 7, 10}) {
            EXPECT_TRUE(held.HoldsAt(txn::LockKey::Tuple(c.oid, id), txn::LockMode::kUpdate))
                << name << " id " << id;
        }
        // Named by the index and declined by the row: no unit.
        EXPECT_FALSE(held.Holds(txn::LockKey::Tuple(c.oid, 4))) << name;
        // The relation's intention and three rows: no window.
        EXPECT_EQ(held.size(), 4u) << name;
        ASSERT_EQ(In(s, "DELETE FROM " + name + " WHERE owner = 2 AND v = 0"), "DELETED 4");
        for (const std::uint64_t id : {2, 5, 8, 11}) {
            EXPECT_TRUE(held.HoldsAt(txn::LockKey::Tuple(c.oid, id), txn::LockMode::kExclusive))
                << name << " id " << id;
        }
        EXPECT_EQ(held.size(), 8u) << name;
        ASSERT_EQ(In(s, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
        EXPECT_EQ(locks_->EntryCount(), 0u) << name;
    }
}

// A locking read is the write's row walk (DC-R3), so it finds its rows
// through the index too: each claimed once, though two entries name it, and
// returned in key order - on a relation and through a one-relation view.
TEST_F(IndexWriteLockTest, ALockingReadByAnIndexedColumnClaimsEachRowOnceInKeyOrder) {
    struct Case {
        const char* relation;
        catalog::Oid oid;
    };
    for (const Case& c : {Case{"b", b_}, Case{"h", h_}}) {
        const std::string name = c.relation;
        // id 4 leaves owner 1 and comes back: two entries under 1, one under 7.
        ASSERT_EQ(Local("UPDATE " + name + " SET owner = 7 WHERE id = 4"), "UPDATED 1");
        ASSERT_EQ(Local("UPDATE " + name + " SET owner = 1 WHERE id = 4"), "UPDATED 1");
        ASSERT_EQ(Local("CREATE VIEW v" + name + " AS SELECT id, owner, v FROM " + name)
                      .rfind("CREATED", 0),
                  0u);
        Session s;
        ASSERT_EQ(In(s, "BEGIN").rfind("BEGIN", 0), 0u);
        EXPECT_EQ(In(s, "SELECT id, owner FROM " + name + " WHERE owner IN (7, 1) FOR UPDATE"),
                  "id,owner\\n1,1\\n4,1\\n7,1\\n10,1")
            << name;
        const txn::LockHoldings& held = s.transaction()->borrows();
        for (const std::uint64_t id : {1, 4, 7, 10}) {
            EXPECT_TRUE(held.HoldsAt(txn::LockKey::Tuple(c.oid, id), txn::LockMode::kUpdate))
                << name << " id " << id;
        }
        EXPECT_EQ(held.size(), 5u) << name;
        EXPECT_EQ(In(s, "SELECT id FROM v" + name + " WHERE owner = 2 FOR UPDATE"),
                  "id\\n2\\n5\\n8\\n11")
            << name;
        EXPECT_EQ(held.size(), 9u) << name;
        ASSERT_EQ(In(s, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
        EXPECT_EQ(locks_->EntryCount(), 0u) << name;
    }
}

// An index-driven write parks on a row another transaction holds, with the
// rows below it written, and resumes at that row: every row once, the count
// the statement's - on a btree and on a heap, whose walk could not take a
// resume by key.
TEST_F(IndexWriteLockTest, AnIndexedWriteParkedOnAHeldRowResumesAtIt) {
    for (const char* relation : {"b", "h"}) {
        const std::string name = relation;
        Session holder;
        ASSERT_EQ(In(holder, "BEGIN").rfind("BEGIN", 0), 0u);
        ASSERT_EQ(In(holder, "UPDATE " + name + " SET v = 50 WHERE id = 7"), "UPDATED 1");

        // owner = 1: ids 1, 4, 7, 10. The statement moves the key it is
        // found by, so a row written twice would show in `owner` and in `v`.
        Session writer;
        Started write =
            Start("UPDATE " + name + " SET owner = owner + 3, v = v + 100 WHERE owner = 1", writer);
        Pump();
        ASSERT_FALSE(*write.done) << name << ": the write passed a held row: "
                                  << write.out->response;
        ASSERT_EQ(In(holder, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
        Pump();
        ASSERT_TRUE(*write.done) << name;
        EXPECT_EQ(write.out->response, "UPDATED 4") << name;
        EXPECT_EQ(Local("SELECT id, owner, v FROM " + name + " WHERE v > 0 ORDER BY id"),
                  "id,owner,v\\n1,4,100\\n4,4,100\\n7,4,100\\n10,4,100")
            << name;
        EXPECT_EQ(locks_->EntryCount(), 0u) << name;
    }
}

// The holder commits a version the statement's view cannot see: the row the
// index named is refused first-updater-wins, as the walk refuses it, and the
// rows written before it are taken back with the statement.
TEST_F(IndexWriteLockTest, AnIndexedWriteMeetingANewerCommittedVersionIsRefusedAsItsWalkIs) {
    Session holder;
    ASSERT_EQ(In(holder, "BEGIN").rfind("BEGIN", 0), 0u);
    ASSERT_EQ(In(holder, "UPDATE b SET v = 50 WHERE id = 7"), "UPDATED 1");
    Session writer;
    Started write = Start("UPDATE b SET v = v + 100 WHERE owner = 1", writer);
    Pump();
    ASSERT_FALSE(*write.done);
    ASSERT_EQ(In(holder, "COMMIT").rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*write.done);
    EXPECT_EQ(write.out->response.rfind("ERR", 0), 0u) << write.out->response;
    EXPECT_EQ(Local("SELECT id, v FROM b WHERE v > 0"), "id,v\\n7,50");
    EXPECT_EQ(locks_->EntryCount(), 0u);
}

// DU-Q2 (a): a first collection past `max_locks_per_txn` is not written
// from, and the walk answers - here a statement whose index run names more
// rows than the cap while its WHERE passes fewer, which a refusal on the
// collection's size would have refused.
class IndexWriteCapTest : public IndexWriteLockTest {
protected:
    std::size_t LockCap() const override { return 6; }
};

TEST_F(IndexWriteCapTest, ACollectionPastTheCapIsAnsweredByTheWalk) {
    for (const char* relation : {"b", "h"}) {
        const std::string name = relation;
        const std::string use =
            "SELECT probes, rows_resolved FROM sys.indexes WHERE name = '" + name + "_owner'";
        // owner = 0: ids 3, 6, 9, 12. The other eight rows visit owner 0 and
        // leave it - under a window, which takes no row unit - so its run
        // names twelve rows where four hold it.
        ASSERT_EQ(Local("UPDATE " + name + " SET owner = 0 WHERE id BETWEEN 1 AND 12 AND owner > 0"),
                  "UPDATED 8");
        ASSERT_EQ(Local("UPDATE " + name +
                        " SET owner = 5 WHERE id BETWEEN 1 AND 12 AND id IN (1, 2, 4, 5, 7, 8, 10, 11)"),
                  "UPDATED 8");
        const std::string unused = Local(use);
        EXPECT_EQ(Local("UPDATE " + name + " SET v = v + 1 WHERE owner = 0"), "UPDATED 4") << name;
        EXPECT_EQ(Local(use), unused) << name << ": a collection the walk answered for was counted";
        EXPECT_EQ(Local("SELECT id, v FROM " + name + " WHERE v > 0 ORDER BY id"),
                  "id,v\\n3,1\\n6,1\\n9,1\\n12,1")
            << name;
        // The cap is on the entries read, before a bound beside the index
        // drops any: owner = 5 names eight rows, past a cap of six, though
        // the bound leaves two. The window answers - a btree's walk, a
        // heap's Keystone range - and this index is still used by nothing.
        EXPECT_EQ(Local("UPDATE " + name + " SET v = v + 1 WHERE owner = 5 AND id > 9"),
                  "UPDATED 2")
            << name;
        EXPECT_EQ(Local(use), unused) << name << ": eight entries are past a cap of six";
    }
}

// A statement whose collection was past the cap is in its walk, and parked
// there it resumes there (`WalkCursor::walked`): the rows written before the
// park are not offered again, and the collection it was refused for its size
// is not read whole on the way back in.
TEST_F(IndexWriteCapTest, AWalkTakenForTheCapResumesAsAWalk) {
    for (const char* relation : {"b", "h"}) {
        const std::string name = relation;
        ASSERT_EQ(Local("UPDATE " + name + " SET owner = 0 WHERE id BETWEEN 1 AND 12 AND owner > 0"),
                  "UPDATED 8");
        ASSERT_EQ(Local("UPDATE " + name +
                        " SET owner = 5 WHERE id BETWEEN 1 AND 12 AND id IN (1, 2, 4, 5, 7, 8, 10, 11)"),
                  "UPDATED 8");
        Session holder;
        ASSERT_EQ(In(holder, "BEGIN").rfind("BEGIN", 0), 0u);
        ASSERT_EQ(In(holder, "UPDATE " + name + " SET v = 50 WHERE id = 9"), "UPDATED 1");

        Session writer;
        Started write = Start("UPDATE " + name + " SET v = v + 100 WHERE owner = 0", writer);
        Pump();
        ASSERT_FALSE(*write.done) << name << ": " << write.out->response;
        ASSERT_EQ(In(holder, "ROLLBACK").rfind("ROLLBACK", 0), 0u);
        Pump();
        ASSERT_TRUE(*write.done) << name;
        EXPECT_EQ(write.out->response, "UPDATED 4") << name;
        EXPECT_EQ(Local("SELECT id, v FROM " + name + " WHERE v > 0 ORDER BY id"),
                  "id,v\\n3,100\\n6,100\\n9,100\\n12,100")
            << name;
        EXPECT_EQ(Local("SELECT probes, rows_resolved FROM sys.indexes WHERE name = '" + name +
                        "_owner'"),
                  "probes,rows_resolved\\n0,0")
            << name;
        EXPECT_EQ(locks_->EntryCount(), 0u) << name;
    }
}

// ---- Two cores ----------------------------------------------------------------

// The index is the instance's and so is the lock table: a write that finds
// its rows through the index on one core is another core's to see only once
// it decides, and the rows it wrote are held against a write from the other
// core that finds them through the same index.
class IndexWriteTwoCoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto opened = TwoCoreRig::Open();
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        rig_ = std::move(opened.value());
        Session setup;
        ASSERT_EQ(Run(0, setup, "CREATE TABLE t (id int64, owner int64, v int64) BTREE")
                      .rfind("CREATED", 0),
                  0u);
        ASSERT_EQ(Run(0, setup, "CREATE INDEX t_owner ON t (owner)").rfind("CREATED", 0), 0u);
        ASSERT_EQ(Run(1, setup,
                      "INSERT INTO t VALUES (1, 1, 0), (2, 2, 0), (3, 1, 0), (4, 2, 0), (5, 1, 0), "
                      "(6, 2, 0)")
                      .rfind("INSERTED", 0),
                  0u);
    }

    std::string Run(std::uint32_t core, Session& s, const std::string& sql) {
        return rig_->core(core).dispatcher().Dispatch(sql, &s).response;
    }

    std::unique_ptr<TwoCoreRig> rig_;
};

TEST_F(IndexWriteTwoCoreTest, AnIndexedWriteIsItsTransactionsUntilItDecidesOnEitherCore) {
    Session writer;
    Session other;
    ASSERT_EQ(Run(0, writer, "BEGIN").rfind("BEGIN", 0), 0u);
    EXPECT_EQ(Run(0, writer, "UPDATE t SET owner = 2, v = v + 1 WHERE owner = 1"), "UPDATED 3");
    // Its own rows, through the key they moved to and the key they left.
    EXPECT_EQ(Run(0, writer, "UPDATE t SET v = v + 10 WHERE owner = 2"), "UPDATED 6");
    EXPECT_EQ(Run(0, writer, "UPDATE t SET v = v + 100 WHERE owner = 1"), "UPDATED 0");
    // The other core sees none of it, and its write of the rows the first
    // one holds - found through the index's entries for the key they left -
    // is refused on a path that cannot park.
    EXPECT_EQ(Run(1, other, "SELECT id, owner, v FROM t WHERE owner = 1"),
              "id,owner,v\\n1,1,0\\n3,1,0\\n5,1,0");
    const std::string held = Run(1, other, "UPDATE t SET v = v + 1000 WHERE owner = 1");
    EXPECT_EQ(held.rfind("ERR", 0), 0u) << held;
    ASSERT_EQ(Run(0, writer, "ROLLBACK").rfind("ROLLBACK", 0), 0u);

    // Rolled back: the entries the first write appended under owner 2 name
    // rows that do not hold it, and a write from the other core by either
    // key writes what its walk would.
    EXPECT_EQ(Run(1, other, "UPDATE t SET v = v + 1 WHERE owner = 2"), "UPDATED 3");
    EXPECT_EQ(Run(1, other, "DELETE FROM t WHERE owner = 1 AND id > 1"), "DELETED 2");
    EXPECT_EQ(Run(0, writer, "SELECT id, owner, v FROM t"),
              "id,owner,v\\n1,1,0\\n2,2,1\\n4,2,1\\n6,2,1");
}

// The index picks the rows and the statement's row function does the rest:
// a foreign key in both directions, an assertion, a Transition and a unique
// key are asked of each row the index arm writes, as of a walked one.
TEST_F(IndexWriteTwoCoreTest, AnIndexedWriteRunsEveryCheckTheWalkRuns) {
    Session s;
    const auto refused = [&](const std::string& sql, const std::string& needle) {
        const std::string reply = Run(0, s, sql);
        EXPECT_EQ(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
        EXPECT_NE(reply.find(needle), std::string::npos) << sql << " -> " << reply;
    };
    const auto ok = [&](const std::string& sql) {
        const std::string reply = Run(0, s, sql);
        ASSERT_NE(reply.rfind("ERR", 0), 0u) << sql << " -> " << reply;
    };

    // A foreign key: the parent found by its indexed column, the child by its.
    ok("CREATE TABLE p (id int64, grp int64) BTREE");
    ok("CREATE INDEX p_grp ON p (grp)");
    ok("INSERT INTO p VALUES (1, 7), (2, 7), (3, 8)");
    ok("CREATE TABLE c (id int64, pid int64 REFERENCES p, tag int64) BTREE");
    ok("CREATE INDEX c_tag ON c (tag)");
    ok("INSERT INTO c VALUES (1, 1, 5), (2, 3, 5), (3, 3, 6)");
    refused("DELETE FROM p WHERE grp = 7", "c");  // id 1 is referenced
    EXPECT_EQ(Run(0, s, "SELECT id FROM p"), "id\\n1\\n2\\n3");
    refused("UPDATE c SET pid = 9 WHERE tag = 5", "9");  // no such parent
    EXPECT_EQ(Run(0, s, "UPDATE c SET pid = 3 WHERE tag = 5"), "UPDATED 2");
    EXPECT_EQ(Run(0, s, "DELETE FROM p WHERE grp = 7"), "DELETED 2");

    // A unique key the statement's rows would share.
    ok("CREATE TABLE q (id int64, grp int64, k int64) BTREE");
    ok("CREATE INDEX q_grp ON q (grp)");
    ok("CREATE UNIQUE INDEX q_k ON q (k)");
    ok("INSERT INTO q VALUES (1, 7, 10), (2, 7, 20), (3, 8, 30)");
    refused("UPDATE q SET k = 99 WHERE grp = 7", "q_k");
    EXPECT_EQ(Run(0, s, "SELECT id, k FROM q"), "id,k\\n1,10\\n2,20\\n3,30");
    EXPECT_EQ(Run(0, s, "UPDATE q SET k = k + 5 WHERE grp = 7"), "UPDATED 2");

    // An assertion over the rows the index names.
    ok("CREATE TABLE a (id int64, acct int64, qty int64) BTREE");
    ok("CREATE INDEX a_acct ON a (acct)");
    ok("INSERT INTO a VALUES (1, 7, 10), (2, 7, 10), (3, 7, 10), (4, 8, 10)");
    ok("CREATE ASSERTION cap ON a GROUP BY (acct) CHECK SUM(qty) <= 100");
    refused("UPDATE a SET qty = 50 WHERE acct = 7", "cap");
    EXPECT_EQ(Run(0, s, "SELECT id, qty FROM a"), "id,qty\\n1,10\\n2,10\\n3,10\\n4,10");
    EXPECT_EQ(Run(0, s, "UPDATE a SET qty = 30 WHERE acct = 7"), "UPDATED 3");
    EXPECT_EQ(Run(0, s, "DELETE FROM a WHERE acct = 7 AND id > 1"), "DELETED 2");

    // A Transition: one of the rows is in a state no edge leaves.
    ok("CREATE TABLE f (id int64, grp int64, st varchar(16)) BTREE");
    ok("CREATE INDEX f_grp ON f (grp)");
    ok("INSERT INTO f VALUES (1, 7, 'new'), (2, 7, 'new'), (3, 7, 'new'), (4, 8, 'new')");
    ok("CREATE TRANSITION flow ON f(st) ON INSERT ('new') ON UPDATE ('new' => 'done', "
       "'new' => 'held')");
    EXPECT_EQ(Run(0, s, "UPDATE f SET st = 'done' WHERE grp = 7 AND id = 2"), "UPDATED 1");
    refused("UPDATE f SET st = 'held' WHERE grp = 7", "flow");
    EXPECT_EQ(Run(0, s, "SELECT id, st FROM f"), "id,st\\n1,new\\n2,done\\n3,new\\n4,new");
    EXPECT_EQ(Run(0, s, "UPDATE f SET st = 'held' WHERE grp = 8"), "UPDATED 1");
}

}  // namespace
}  // namespace kds::server
