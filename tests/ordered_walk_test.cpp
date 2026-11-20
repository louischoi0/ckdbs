#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// An `ORDER BY` served by the order a structure already holds (CV,
// `instructions/v3.2.0/workorder-cv-ordered-index-walk.md`).
//
// **CV-R1: the reply under `ORDER BY` is the sort's reply.** Every case
// here compares a statement the compiler elides against the same statement
// with its LIMIT and OFFSET taken off - which keeps the sort (CV-Q2 (a)) -
// cut to the same window. A literal expectation proves the engine agrees
// with whoever typed it; the window of the sorted reply proves it agrees
// with the definition. `ANALYZE` says which statements were elided, so a
// case cannot pass by sorting both.

namespace kds::server {
namespace {

class OrderedWalkTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_);
    }

    std::string Run(const std::string& sql) {
        auto out = dispatcher_->Dispatch(sql);
        EXPECT_EQ(out.response.rfind("ERR", 0), std::string::npos) << sql << ": " << out.response;
        return out.response;
    }

    static std::vector<std::string> RowsOf(const std::string& reply) {
        std::vector<std::string> rows;
        for (std::size_t at = reply.find("\\n"); at != std::string::npos;) {
            const std::size_t start = at + 2;
            const std::size_t next = reply.find("\\n", start);
            rows.push_back(reply.substr(start, next == std::string::npos ? std::string::npos
                                                                         : next - start));
            at = next;
        }
        return rows;
    }

    std::vector<std::string> Rows(const std::string& sql) { return RowsOf(Run(sql)); }

    // The sorted reply of `base` (which carries its ORDER BY and no
    // pagination), cut to `[offset, offset + limit)`.
    std::vector<std::string> Window(const std::string& base, std::size_t limit,
                                    std::size_t offset) {
        std::vector<std::string> all = Rows(base);
        if (offset >= all.size()) return {};
        const std::size_t end = std::min(all.size(), offset + limit);
        return std::vector<std::string>(all.begin() + static_cast<std::ptrdiff_t>(offset),
                                        all.begin() + static_cast<std::ptrdiff_t>(end));
    }

    // `base` with `LIMIT limit OFFSET offset`, checked elided and checked
    // against the window of `base` itself.
    void ExpectElidedWindow(const std::string& base, std::size_t limit, std::size_t offset,
                            const std::string& order) {
        const std::string paged = base + " LIMIT " + std::to_string(limit) +
                                  (offset != 0 ? " OFFSET " + std::to_string(offset) : "");
        const std::string plan = Run("ANALYZE " + paged);
        EXPECT_NE(plan.find("order=" + order), std::string::npos) << paged << ": " << plan;
        EXPECT_EQ(plan.find("sorted="), std::string::npos) << paged << " still sorted: " << plan;
        EXPECT_EQ(Rows(paged), Window(base, limit, offset)) << paged;
    }

    // ids 1..n with a ~2 KB cell, so a leaf holds about four rows and `n`
    // rows are about `n / 4` leaves.
    void LoadWide(const std::string& table, int n, int step = 1) {
        Run("CREATE TABLE " + table + " (id int64, k int64, pad varchar(2000))");
        for (int i = 1; i <= n; ++i) {
            Run("INSERT INTO " + table + " VALUES (" + std::to_string(i * step) + ", " +
                std::to_string(i % 7) + ", 'p')");
        }
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- O2: the reverse tuple key walk (CV-S1) --------------------------------

TEST_F(OrderedWalkTest, ADescendingTupleKeyUnderALimitIsTheSortsWindow) {
    LoadWide("t", 300);
    const std::string base = "SELECT id, k FROM t ORDER BY id DESC";
    for (const auto& [limit, offset] : std::vector<std::pair<std::size_t, std::size_t>>{
             {1, 0}, {20, 0}, {20, 1}, {7, 33}, {300, 0}, {500, 0}, {20, 290}, {20, 300},
             {20, 1000}, {0, 0}}) {
        ExpectElidedWindow(base, limit, offset, "tuple_key_desc");
    }
}

TEST_F(OrderedWalkTest, TheLimitStopsTheWalkAtTheRowsItLetsOut) {
    LoadWide("t", 300);
    const std::string plan = Run("ANALYZE SELECT id FROM t ORDER BY id DESC LIMIT 5");
    // Five rows from the top two leaves, where the sort examined all 300.
    const auto at = plan.find("examined=");
    ASSERT_NE(at, std::string::npos) << plan;
    EXPECT_LE(std::stoull(plan.substr(at + 9)), 8u) << plan;
    EXPECT_EQ(RowsOf(Run("SELECT id FROM t ORDER BY id DESC LIMIT 5")),
              (std::vector<std::string>{"300", "299", "298", "297", "296"}));
}

TEST_F(OrderedWalkTest, ATupleKeyRangeIsWalkedFromItsHighEnd) {
    LoadWide("t", 300);
    ExpectElidedWindow("SELECT id FROM t WHERE id BETWEEN 50 AND 120 ORDER BY id DESC", 10, 0,
                       "tuple_key_desc");
    ExpectElidedWindow("SELECT id FROM t WHERE id BETWEEN 50 AND 120 ORDER BY id DESC", 100, 60,
                       "tuple_key_desc");
    // A bound past either end of the relation.
    ExpectElidedWindow("SELECT id FROM t WHERE id BETWEEN 290 AND 9000 ORDER BY id DESC", 20, 0,
                       "tuple_key_desc");
    ExpectElidedWindow("SELECT id FROM t WHERE id BETWEEN 0 AND 3 ORDER BY id DESC", 20, 0,
                       "tuple_key_desc");
}

TEST_F(OrderedWalkTest, AFilterIsAppliedOnTheWayDown) {
    LoadWide("t", 300);
    ExpectElidedWindow("SELECT id, k FROM t WHERE k = 3 ORDER BY id DESC", 9, 2, "tuple_key_desc");
    // A filter no row passes walks to the head and returns nothing.
    ExpectElidedWindow("SELECT id FROM t WHERE k = 99 ORDER BY id DESC", 9, 0, "tuple_key_desc");
}

TEST_F(OrderedWalkTest, SparseNamedKeysAndDeletedRowsAreReadThroughTheirLeaves) {
    LoadWide("t", 200, /*step=*/37);
    Run("DELETE FROM t WHERE k = 2");
    Run("DELETE FROM t WHERE id BETWEEN 1000 AND 3000");
    Run("UPDATE t SET k = 50 WHERE k = 4");
    const std::string base = "SELECT id, k FROM t ORDER BY id DESC";
    for (const auto& [limit, offset] :
         std::vector<std::pair<std::size_t, std::size_t>>{{10, 0}, {10, 50}, {400, 0}}) {
        ExpectElidedWindow(base, limit, offset, "tuple_key_desc");
    }
}

TEST_F(OrderedWalkTest, AnEmptyAndAOneLeafRelation) {
    Run("CREATE TABLE e (id int64, v int64)");
    ExpectElidedWindow("SELECT id FROM e ORDER BY id DESC", 5, 0, "tuple_key_desc");
    for (const char* row : {"(4, 1)", "(9, 2)", "(2, 3)"}) {
        Run(std::string("INSERT INTO e VALUES ") + row);
    }
    ExpectElidedWindow("SELECT id, v FROM e ORDER BY id DESC", 5, 0, "tuple_key_desc");
    ExpectElidedWindow("SELECT id, v FROM e ORDER BY id DESC", 1, 1, "tuple_key_desc");
}

TEST_F(OrderedWalkTest, AJoinKeepsTheDriversOrder) {
    LoadWide("t", 120);
    Run("CREATE TABLE u (id int64, t_id int64, w int64)");
    for (int i = 1; i <= 120; ++i) {
        Run("INSERT INTO u VALUES (" + std::to_string(i) + ", " + std::to_string(121 - i) + ", " +
            std::to_string(i % 3) + ")");
    }
    // `u` is reached by a probe of its key per `t` row; the order is `t`'s.
    ExpectElidedWindow("SELECT t.id, u.w FROM t JOIN u ON u.id = t.id ORDER BY t.id DESC", 15, 4,
                       "tuple_key_desc");
}

TEST_F(OrderedWalkTest, AJoinThePlannerReordersKeepsTheSortItNeeds) {
    LoadWide("t", 120);
    Run("CREATE TABLE u (id int64, t_id int64, w int64)");
    for (int i = 1; i <= 120; ++i) {
        Run("INSERT INTO u VALUES (" + std::to_string(i) + ", " + std::to_string(121 - i) + ", " +
            std::to_string(i % 3) + ")");
    }
    // `u` is the cheap driver here; whichever relation the planner puts
    // first, the reply is the sort's window, and an order that does not
    // start at `t` carries the sort.
    const std::string base =
        "SELECT t.id, u.w FROM t JOIN u ON u.t_id = t.id WHERE u.id BETWEEN 1 AND 9 "
        "ORDER BY t.id DESC";
    const std::string paged = base + " LIMIT 4";
    const std::string plan = Run("ANALYZE " + paged);
    if (plan.find("reordered=1") != std::string::npos) {
        EXPECT_NE(plan.find("sorted="), std::string::npos) << plan;
        EXPECT_EQ(plan.find("order=tuple_key_desc"), std::string::npos) << plan;
    }
    EXPECT_EQ(Rows(paged), Window(base, 4, 0));
}

TEST_F(OrderedWalkTest, AKeySetKeepsItsSort) {
    LoadWide("t", 40);
    const std::string sql = "SELECT id FROM t WHERE id IN (3, 9, 5) ORDER BY id DESC LIMIT 2";
    const std::string plan = Run("ANALYZE " + sql);
    EXPECT_EQ(plan.find("order=tuple_key"), std::string::npos) << plan;
    EXPECT_NE(plan.find("sorted="), std::string::npos) << plan;
    EXPECT_EQ(Rows(sql), (std::vector<std::string>{"9", "5"}));
}

TEST_F(OrderedWalkTest, WhatTheReverseWalkDoesNotTake) {
    LoadWide("t", 40);
    // No LIMIT: the sort stays (CV-Q2 (a)).
    std::string plan = Run("ANALYZE SELECT id FROM t ORDER BY id DESC");
    EXPECT_EQ(plan.find("order=tuple_key"), std::string::npos) << plan;
    EXPECT_NE(plan.find("sorted="), std::string::npos) << plan;
    // Another key beside the tuple key, or a key on another column.
    plan = Run("ANALYZE SELECT id FROM t ORDER BY id DESC, k LIMIT 3");
    EXPECT_EQ(plan.find("order=tuple_key"), std::string::npos) << plan;
    plan = Run("ANALYZE SELECT id FROM t ORDER BY k DESC LIMIT 3");
    EXPECT_EQ(plan.find("order=tuple_key"), std::string::npos) << plan;
    // A lookup emits one row and keeps what it had.
    plan = Run("ANALYZE SELECT id FROM t WHERE id = 3 ORDER BY id DESC LIMIT 3");
    EXPECT_EQ(plan.find("order=tuple_key"), std::string::npos) << plan;
    // The ascending form was already elided, and stays unannotated.
    plan = Run("ANALYZE SELECT id FROM t ORDER BY id LIMIT 3");
    EXPECT_EQ(plan.find("order=tuple_key"), std::string::npos) << plan;
    EXPECT_EQ(plan.find("sorted="), std::string::npos) << plan;
}

}  // namespace
}  // namespace kds::server
