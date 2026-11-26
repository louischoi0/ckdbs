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

}  // namespace
}  // namespace kds::server
