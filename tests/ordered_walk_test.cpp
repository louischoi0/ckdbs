#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/exec/step_vm.hpp"
#include "kds/parser/parser.hpp"
#include "kds/sched/coro.hpp"
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

    // `base` paged, checked elided, against the window of `reference`: the
    // same statement with its ties broken by descending tuple key, which is
    // CV-Q4 (a)'s tie rule for an elided `DESC` - so the sort that answers
    // `reference` defines the order the reverse walk owes.
    void ExpectElidedWindowAgainst(const std::string& base, const std::string& reference,
                                   std::size_t limit, std::size_t offset,
                                   const std::string& order) {
        const std::string paged = base + " LIMIT " + std::to_string(limit) +
                                  (offset != 0 ? " OFFSET " + std::to_string(offset) : "");
        const std::string plan = Run("ANALYZE " + paged);
        EXPECT_NE(plan.find("order=" + order), std::string::npos) << paged << ": " << plan;
        EXPECT_EQ(plan.find("sorted="), std::string::npos) << paged << " still sorted: " << plan;
        EXPECT_EQ(Rows(paged), Window(reference, limit, offset)) << paged;
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

// ---- O1: the forward ordered index walk (CV-S2) ----------------------------

class OrderedIndexWalkTest : public OrderedWalkTest {
protected:
    // `ev (id, user_id, created_at, note)`: three users of 30, 300 and 600
    // rows, interleaved through the tuple key order, `created_at` a
    // permutation that is not the tuple key's, with ties every 7 rows.
    void LoadEvents() {
        Run("CREATE TABLE ev (id int64, user_id int64, created_at int64, kind int64, "
            "note varchar)");
        for (int i = 1; i <= 930; ++i) {
            const int user = i % 31 == 0 ? 1 : (i % 3 == 0 ? 2 : 3);
            Run("INSERT INTO ev VALUES (" + std::to_string(i) + ", " + std::to_string(user) + ", " +
                std::to_string((i * 389) % 1000 / 7) + ", " + std::to_string(i % 4) + ", 'n')");
        }
    }
};

TEST_F(OrderedIndexWalkTest, ALatestRowsReadIsTheSortsWindow) {
    LoadEvents();
    Run("CREATE INDEX ev_user ON ev (user_id)");
    Run("CREATE INDEX ev_user_at ON ev (user_id, created_at)");
    for (const char* user : {"1", "2", "3", "4"}) {
        const std::string base =
            std::string("SELECT id, created_at FROM ev WHERE user_id = ") + user +
            " ORDER BY created_at";
        for (const auto& [limit, offset] : std::vector<std::pair<std::size_t, std::size_t>>{
                 {1, 0}, {20, 0}, {20, 7}, {5, 290}, {700, 0}, {0, 0}}) {
            ExpectElidedWindow(base, limit, offset, "index_asc");
        }
    }
    ExpectElidedWindow("SELECT id FROM ev WHERE user_id = 3 ORDER BY created_at, id", 25, 3,
                       "index_asc");
}

TEST_F(OrderedIndexWalkTest, ARangeAFilterAndAPermutedPrefix) {
    LoadEvents();
    Run("CREATE INDEX ev_kind_user_at ON ev (kind, user_id, created_at)");
    ExpectElidedWindow(
        "SELECT id FROM ev WHERE user_id = 3 AND kind = 2 ORDER BY created_at", 30, 2,
        "index_asc");
    ExpectElidedWindow("SELECT id FROM ev WHERE kind = 1 AND user_id = 2 AND created_at BETWEEN "
                       "20 AND 90 ORDER BY created_at",
                       12, 0, "index_asc");
    // A conjunct beyond the prefix is a residual on each row the walk reaches.
    ExpectElidedWindow(
        "SELECT id FROM ev WHERE kind = 1 AND user_id = 3 AND id > 400 ORDER BY created_at", 9,
        0, "index_asc");
}

TEST_F(OrderedIndexWalkTest, AWholeIndexIsWalkedOnlyForAStepWithNoConjunct) {
    LoadEvents();
    Run("CREATE INDEX ev_at ON ev (created_at)");
    ExpectElidedWindow("SELECT id, created_at FROM ev ORDER BY created_at", 40, 100, "index_asc");
    // A filter would have the walk resolve entry after entry to find its rows.
    const std::string plan =
        Run("ANALYZE SELECT id FROM ev WHERE kind = 1 ORDER BY created_at LIMIT 5");
    EXPECT_EQ(plan.find("order=index"), std::string::npos) << plan;
    // The LIMIT bounds what is resolved.
    const std::string bounded = Run("ANALYZE SELECT id FROM ev ORDER BY created_at LIMIT 5");
    const auto at = bounded.find("examined=");
    ASSERT_NE(at, std::string::npos) << bounded;
    EXPECT_LE(std::stoull(bounded.substr(at + 9)), 5u) << bounded;
}

TEST_F(OrderedIndexWalkTest, UpdatesAndDeletesLeaveTheOrderTheSorts) {
    LoadEvents();
    Run("CREATE INDEX ev_user_at ON ev (user_id, created_at)");
    Run("UPDATE ev SET created_at = 5000 WHERE id BETWEEN 1 AND 60");
    Run("UPDATE ev SET created_at = 1 WHERE id BETWEEN 300 AND 330");
    Run("UPDATE ev SET user_id = 2 WHERE id BETWEEN 400 AND 450");
    Run("DELETE FROM ev WHERE id BETWEEN 500 AND 560");
    for (const char* user : {"2", "3"}) {
        ExpectElidedWindow(std::string("SELECT id, created_at FROM ev WHERE user_id = ") + user +
                               " ORDER BY created_at",
                           50, 10, "index_asc");
    }
}

// Every key type an ordered walk serves, each a different encoding the key
// check re-derives from the decoded row (`[quiet-wrong]` if one disagreed:
// every row would fail it and the reply would be empty, or misplaced).
TEST_F(OrderedIndexWalkTest, EveryOrderPreservingKeyTypeIsWalkedInItsOrder) {
    Run("CREATE TABLE ty (id int64, d date, ts timestamp, m decimal(12, 2), "
        "w decimal(30, 2), i int32, u uint64)");
    const std::vector<std::string> rows = {
        "'2026-03-01', '2026-03-01 10:00:00', -3.75, '1701411834604692317316.87', -5, 5",
        "'1999-12-31', '2026-03-01 09:59:59', 10.05, '-5.00', 7, 18446744073709551615",
        "'2026-03-01', '1970-01-01 00:00:00', 2.50, '1.00', -2147483648, 9223372036854775808",
        "'2001-01-01', '2030-06-30 23:59:59', 100.00, '1701411834604692317316.86', 2147483647, 0",
        "'1999-12-31', '2026-03-01 10:00:00', 2.50, '-5.00', 7, 5",
    };
    for (int rep = 0; rep < 6; ++rep) {
        for (std::size_t r = 0; r < rows.size(); ++r) {
            Run("INSERT INTO ty VALUES (" + std::to_string(rep * 10 + r + 1) + ", " + rows[r] +
                ")");
        }
    }
    for (const char* col : {"d", "ts", "m", "w", "i", "u"}) {
        Run(std::string("CREATE INDEX ty_") + col + " ON ty (" + col + ")");
        ExpectElidedWindow(std::string("SELECT id, ") + col + " FROM ty ORDER BY " + col, 11, 4,
                           "index_asc");
    }
}

TEST_F(OrderedIndexWalkTest, AnEqualityOnALaterOrderKeyIsAConstantInTheOrder) {
    LoadEvents();
    Run("CREATE INDEX ev_user_at_kind ON ev (user_id, created_at, kind)");
    ExpectElidedWindow(
        "SELECT id FROM ev WHERE user_id = 3 AND kind = 1 ORDER BY created_at, kind", 15, 2,
        "index_asc");
}

// ---- O3: the reverse ordered index walk (CV-S3) ----------------------------

TEST_F(OrderedIndexWalkTest, ALatestRowsReadDescendingTiesByDescendingTupleKey) {
    LoadEvents();
    Run("CREATE INDEX ev_user_at ON ev (user_id, created_at)");
    for (const char* user : {"1", "2", "3", "4"}) {
        const std::string base = std::string("SELECT id, created_at FROM ev WHERE user_id = ") +
                                 user + " ORDER BY created_at DESC";
        for (const auto& [limit, offset] : std::vector<std::pair<std::size_t, std::size_t>>{
                 {1, 0}, {20, 0}, {20, 7}, {5, 290}, {700, 0}, {0, 0}}) {
            ExpectElidedWindowAgainst(base, base + ", id DESC", limit, offset, "index_desc");
        }
    }
    // With the tuple key written, the order is total and the sort's own.
    ExpectElidedWindow("SELECT id FROM ev WHERE user_id = 3 ORDER BY created_at DESC, id DESC",
                       25, 3, "index_desc");
}

TEST_F(OrderedIndexWalkTest, AReverseRangeAWholeIndexAndMovedRows) {
    LoadEvents();
    Run("CREATE INDEX ev_user_at ON ev (user_id, created_at)");
    Run("CREATE INDEX ev_at ON ev (created_at)");
    Run("UPDATE ev SET created_at = 5000 WHERE id BETWEEN 1 AND 60");
    Run("UPDATE ev SET created_at = 1 WHERE id BETWEEN 300 AND 330");
    Run("DELETE FROM ev WHERE id BETWEEN 500 AND 560");
    const std::string ranged =
        "SELECT id FROM ev WHERE user_id = 2 AND created_at BETWEEN 10 AND 100 "
        "ORDER BY created_at DESC";
    ExpectElidedWindowAgainst(ranged, ranged + ", id DESC", 30, 5, "index_desc");
    const std::string whole = "SELECT id, created_at FROM ev ORDER BY created_at DESC";
    ExpectElidedWindowAgainst(whole, whole + ", id DESC", 40, 100, "index_desc");
    ExpectElidedWindowAgainst(whole, whole + ", id DESC", 2000, 0, "index_desc");
    // A direction against the written tuple key keeps the sort.
    const std::string plan =
        Run("ANALYZE SELECT id FROM ev ORDER BY created_at DESC, id LIMIT 5");
    EXPECT_EQ(plan.find("order=index"), std::string::npos) << plan;
}

TEST_F(OrderedIndexWalkTest, AReverseWalkOverAnEmptyAOneLeafAndAnInvertedRange) {
    Run("CREATE TABLE s (id int64, v int64)");
    Run("CREATE INDEX s_v ON s (v)");
    const std::string all = "SELECT id, v FROM s ORDER BY v DESC";
    ExpectElidedWindowAgainst(all, all + ", id DESC", 5, 0, "index_desc");
    Run("INSERT INTO s VALUES (1, 40)");
    ExpectElidedWindowAgainst(all, all + ", id DESC", 5, 0, "index_desc");
    for (const char* row : {"(2, 10)", "(3, 40)", "(4, 25)"}) {
        Run(std::string("INSERT INTO s VALUES ") + row);
    }
    ExpectElidedWindowAgainst(all, all + ", id DESC", 3, 1, "index_desc");
    // Every entry above the high bound, and a range whose ends cross.
    const std::string above = "SELECT id FROM s WHERE v BETWEEN 1 AND 5 ORDER BY v DESC";
    ExpectElidedWindowAgainst(above, above + ", id DESC", 5, 0, "index_desc");
    const std::string inverted = "SELECT id FROM s WHERE v BETWEEN 100 AND 10 ORDER BY v DESC";
    ExpectElidedWindowAgainst(inverted, inverted + ", id DESC", 5, 0, "index_desc");
}

TEST_F(OrderedIndexWalkTest, AJoinKeepsTheOrderedDriversOrder) {
    LoadEvents();
    Run("CREATE INDEX ev_user_at ON ev (user_id, created_at)");
    Run("CREATE TABLE k (id int64, label int64)");
    for (int i = 0; i < 4; ++i) {
        Run("INSERT INTO k VALUES (" + std::to_string(i + 1) + ", " + std::to_string(i * 10) + ")");
    }
    ExpectElidedWindow(
        "SELECT ev.id, k.label FROM ev JOIN k ON k.id = ev.kind WHERE ev.user_id = 2 "
        "ORDER BY ev.created_at",
        17, 3, "index_asc");
}

// ---- The ordered runs at every slice size (audit H17) ----------------------
//
// The dispatcher slices at 16 or 64; `SlicePolicy` states no minimum. At one
// page a slice, a forward pass that ended on a due slice re-sought to the
// leaf it had just read - the re-read alone made the slice due again - and
// the walk yielded forever on its first leaf. Each run here is driven to its
// end under a bounded number of resumes and checked against the order the
// load defines, beside the relation walk's one-page cell in
// exec_chain_test.cpp.
TEST_F(OrderedIndexWalkTest, AnOrderedWalkReachesItsEndAtOnePageASlice) {
    LoadEvents();
    Run("CREATE INDEX ev_at ON ev (created_at)");
    Run("CREATE INDEX ev_user_at ON ev (user_id, created_at)");
    const auto created_at = [](std::uint64_t id) { return (id * 389) % 1000 / 7; };

    struct Walked {
        std::vector<std::uint64_t> ids;
        std::uint64_t leaves = 0;
    };
    const auto walk_at = [&](const std::string& sql, std::uint32_t pages_per_slice,
                             exec::EmitOrder order) -> Walked {
        Walked out;
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << sql;
        if (!parsed.ok()) return out;
        auto chain = exec::Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
        EXPECT_TRUE(chain.ok()) << sql << ": " << chain.status().message();
        if (!chain.ok()) return out;
        EXPECT_EQ(chain.value().steps.front().emit_order, order) << sql;
        exec::ExecStats stats;
        exec::SlicePolicy policy;
        policy.pages_per_slice = pages_per_slice;
        const exec::StepChain& c = chain.value();
        sched::Coro walk = exec::ExecuteAsync(
            boot_->catalog, store_, c,
            [&](const exec::ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                out.ids.push_back(static_cast<std::uint64_t>(frame.Get(c.projection.front()).int_val));
                return storage::VisitControl::kContinue;
            },
            &stats, exec::Budget(), /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr,
            /*snapshot=*/nullptr, /*indexes=*/true, policy);
        for (int resumes = 0; !walk.done(); ++resumes) {
            if (resumes > 100000) {
                ADD_FAILURE() << sql << " at " << pages_per_slice << " page(s) a slice never ended";
                return out;
            }
            if (!walk.TryResumeDeepest()) {
                ADD_FAILURE() << sql << ": the walk parked beneath a slice boundary";
                return out;
            }
        }
        EXPECT_TRUE(walk.result().ok()) << sql << ": " << walk.result().message();
        out.leaves = stats.Total().pages_fetched;
        return out;
    };

    const struct {
        std::string sql;
        exec::EmitOrder order;
        bool (*admits)(int);
    } cases[] = {
        {"SELECT id FROM ev ORDER BY created_at LIMIT 5000", exec::EmitOrder::kIndexKeyForward,
         [](int) { return true; }},
        {"SELECT id FROM ev WHERE user_id = 3 ORDER BY created_at LIMIT 5000",
         exec::EmitOrder::kIndexKeyForward, [](int i) { return i % 31 != 0 && i % 3 != 0; }},
        {"SELECT id FROM ev ORDER BY created_at DESC LIMIT 5000",
         exec::EmitOrder::kIndexKeyBackward, [](int) { return true; }},
    };
    for (const auto& one : cases) {
        const bool descending = one.order == exec::EmitOrder::kIndexKeyBackward;
        // The load's own order: by created_at, ties by tuple key, both
        // reversed for a descending walk (CV-Q4 (a)).
        std::vector<std::uint64_t> want;
        for (int i = 1; i <= 930; ++i) {
            if (one.admits(i)) want.push_back(static_cast<std::uint64_t>(i));
        }
        std::stable_sort(want.begin(), want.end(), [&](std::uint64_t a, std::uint64_t b) {
            return created_at(a) < created_at(b);
        });
        if (descending) std::reverse(want.begin(), want.end());

        const Walked wide = walk_at(one.sql, 64, one.order);
        ASSERT_GE(wide.leaves, 2u) << one.sql << ": the run never left its first leaf";
        EXPECT_EQ(wide.ids, want) << one.sql << " at 64 pages a slice";
        for (const std::uint32_t pages : {1u, 2u}) {
            EXPECT_EQ(walk_at(one.sql, pages, one.order).ids, want)
                << one.sql << " at " << pages << " page(s) a slice";
        }
    }
}


// ---- EB-S1: a bound with an open or a strict end ----------------------------
//
// `instructions/v3.2.0/workorder-eb-key-forms-in-the-executor.md` S1,
// EB-Q2 (b) as adopted: an open or strict end ranks where a strict tuple key
// range ranks - it replaces a bare walk and nothing else - and an index
// takes one where its walk in key order seeds an elided `ORDER BY ...
// LIMIT`. Every reply is checked against a twin the engine answered before
// EB, so the arm changes the cost and never the rows (EB-R1).

class KeyBoundTest : public OrderedIndexWalkTest {
protected:
    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }
    static bool Has(const std::string& plan, const std::string& what) {
        return plan.find(what) != std::string::npos;
    }
    static std::uint64_t Examined(const std::string& plan) {
        const auto at = plan.find("examined=");
        return at == std::string::npos ? ~std::uint64_t{0} : std::stoull(plan.substr(at + 9));
    }
};

TEST_F(KeyBoundTest, AnOpenTupleKeyBoundIsARangeOfTheWalksOwnLeaves) {
    LoadWide("t", 300);
    // Forward: from the bound to the top, elided, the LIMIT stopping it.
    std::string plan = Plan("SELECT id, k FROM t WHERE id >= 120 ORDER BY id LIMIT 10");
    EXPECT_TRUE(Has(plan, "Range t range=[120, 281474976710655]")) << plan;
    EXPECT_FALSE(Has(plan, "sorted=")) << plan;
    EXPECT_LE(Examined(plan), 16u) << plan;
    EXPECT_EQ(Rows("SELECT id, k FROM t WHERE id >= 120 ORDER BY id LIMIT 10"),
              Window("SELECT id, k FROM t WHERE id >= 120 AND id <= 300 ORDER BY id", 10, 0));
    EXPECT_EQ(Rows("SELECT id FROM t WHERE id > 119 ORDER BY id LIMIT 10"),
              Rows("SELECT id FROM t WHERE id >= 120 ORDER BY id LIMIT 10"));
    // Backward: keyset pagination, seeded at the bound (known-gaps "A
    // one-sided bound does not seed the reverse walk").
    ExpectElidedWindow("SELECT id, k FROM t WHERE id < 200 ORDER BY id DESC", 20, 0,
                       "tuple_key_desc");
    ExpectElidedWindow("SELECT id FROM t WHERE id <= 37 ORDER BY id DESC", 5, 3,
                       "tuple_key_desc");
    plan = Plan("SELECT id FROM t WHERE id < 200 ORDER BY id DESC LIMIT 20");
    EXPECT_TRUE(Has(plan, "Range t range=[1, 199]")) << plan;
    EXPECT_LE(Examined(plan), 28u) << plan;
    // Without an order, the rows the walk with the residual gave.
    EXPECT_EQ(Rows("SELECT id FROM t WHERE id >= 290"),
              Rows("SELECT id FROM t WHERE id BETWEEN 290 AND 300"));
    // The bounds that hold for no key: an inverted range and a bound above
    // the key space each stay the walk, and answer nothing.
    EXPECT_TRUE(Rows("SELECT id FROM t WHERE id < 1").empty());
    EXPECT_TRUE(Rows("SELECT id FROM t WHERE id >= 281474976710656").empty());
    plan = Plan("SELECT id FROM t WHERE id >= 281474976710656");
    EXPECT_FALSE(Has(plan, "Range t")) << plan;
}

TEST_F(KeyBoundTest, AnOpenTupleKeyBoundDisplacesNoOtherArm) {
    LoadWide("t", 120);
    Run("CREATE INDEX t_k ON t (k)");
    // An index equality outranks it.
    std::string plan = Plan("SELECT id FROM t WHERE k = 3 AND id >= 10");
    EXPECT_TRUE(Has(plan, "IndexProbe t")) << plan;
    EXPECT_EQ(Rows("SELECT id FROM t WHERE k = 3 AND id >= 10"),
              Rows("SELECT id FROM t WHERE k = 3 AND id BETWEEN 10 AND 120"));
    // A window bounded at both ends on an index, strict or not, is taken
    // before a tuple key range bounded at one.
    plan = Plan("SELECT id FROM t WHERE id >= 10 AND k > 2 AND k < 4");
    EXPECT_TRUE(Has(plan, "IndexRange t")) << plan;
    EXPECT_EQ(Rows("SELECT id FROM t WHERE id >= 10 AND k > 2 AND k < 4"),
              Rows("SELECT id FROM t WHERE id BETWEEN 10 AND 120 AND k = 3"));
    // A strict two-sided tuple key range still ranks before the index window.
    plan = Plan("SELECT id FROM t WHERE id > 9 AND id < 30 AND k > 2 AND k < 4");
    EXPECT_TRUE(Has(plan, "Range t range=[10, 29]")) << plan;
}

TEST_F(KeyBoundTest, AStrictWindowOnAnIndexIsTheInclusiveWindowsRows) {
    LoadEvents();
    Run("CREATE INDEX ev_at ON ev (created_at)");
    const std::string strict = "SELECT id FROM ev WHERE created_at > 20 AND created_at < 31";
    std::string plan = Plan(strict);
    EXPECT_TRUE(Has(plan, "IndexRange ev")) << plan;
    EXPECT_EQ(Rows(strict),
              Rows("SELECT id FROM ev WHERE created_at >= 21 AND created_at <= 30"));
    // One end strict, the other inclusive; an inclusive end written beside a
    // strict one on the same side wins.
    EXPECT_EQ(Rows("SELECT id FROM ev WHERE created_at > 20 AND created_at <= 30"),
              Rows("SELECT id FROM ev WHERE created_at >= 21 AND created_at <= 30"));
    EXPECT_EQ(Rows("SELECT id FROM ev WHERE created_at > 20 AND created_at >= 20 AND "
                   "created_at < 31"),
              Rows("SELECT id FROM ev WHERE created_at >= 21 AND created_at <= 30"));
    // An empty window and an inverted one.
    EXPECT_TRUE(Rows("SELECT id FROM ev WHERE created_at > 30 AND created_at < 31").empty());
    EXPECT_TRUE(Rows("SELECT id FROM ev WHERE created_at > 40 AND created_at < 10").empty());
    // One end alone, with no order to stop it, is no range to the ladder.
    plan = Plan("SELECT id FROM ev WHERE created_at > 100");
    EXPECT_FALSE(Has(plan, "IndexRange")) << plan;
}

TEST_F(KeyBoundTest, AnOpenIndexBoundSeedsTheOrderedWalk) {
    LoadEvents();
    Run("CREATE INDEX ev_at ON ev (created_at)");
    ExpectElidedWindow("SELECT id, created_at FROM ev WHERE created_at >= 50 ORDER BY created_at",
                       20, 0, "index_asc");
    ExpectElidedWindow("SELECT id, created_at FROM ev WHERE created_at > 50 ORDER BY created_at",
                       15, 4, "index_asc");
    ExpectElidedWindowAgainst(
        "SELECT id, created_at FROM ev WHERE created_at < 90 ORDER BY created_at DESC",
        "SELECT id, created_at FROM ev WHERE created_at < 90 ORDER BY created_at DESC, id DESC",
        12, 0, "index_desc");
    // Seeded: the walk resolves the rows it returns, not the rows below x.
    std::string plan =
        Plan("SELECT id FROM ev WHERE created_at >= 130 ORDER BY created_at LIMIT 5");
    EXPECT_LE(Examined(plan), 5u) << plan;
    // A conjunct on another column keeps the sort: the walk would resolve
    // entry after entry at random to find its rows.
    plan = Plan("SELECT id FROM ev WHERE kind = 1 AND created_at >= 50 ORDER BY created_at "
                "LIMIT 5");
    EXPECT_FALSE(Has(plan, "order=index")) << plan;
    // After an equality prefix, the open end seeds the range on the next key.
    Run("CREATE INDEX ev_user_at ON ev (user_id, created_at)");
    ExpectElidedWindow(
        "SELECT id FROM ev WHERE user_id = 3 AND created_at > 70 ORDER BY created_at, id", 10, 0,
        "index_asc");
    // A NULL bound holds for no row.
    EXPECT_TRUE(Rows("SELECT id FROM ev WHERE created_at >= NULL ORDER BY created_at LIMIT 3")
                    .empty());
}

TEST_F(KeyBoundTest, AStrictEndOnAStringLongerThanItsStoredPrefix) {
    // Values sharing the first 32 bytes the index stores (IX6): the strict
    // window holds every entry of the prefix, and the residual decides.
    Run("CREATE TABLE s (id int64, name varchar(64))");
    Run("CREATE TABLE sp (id int64, name varchar(64))");
    const std::string stem(32, 'p');
    for (int i = 1; i <= 30; ++i) {
        const std::string name = "'" + stem + std::string(1, static_cast<char>('a' + i % 26)) +
                                 std::to_string(i) + "'";
        Run("INSERT INTO s VALUES (" + std::to_string(i) + ", " + name + ")");
        Run("INSERT INTO sp VALUES (" + std::to_string(i) + ", " + name + ")");
    }
    Run("CREATE INDEX s_name ON s (name)");
    const std::string where = " WHERE name > '" + stem + "c' AND name < '" + stem + "k3'";
    EXPECT_TRUE(Has(Plan("SELECT id FROM s" + where), "IndexRange s"))
        << Plan("SELECT id FROM s" + where);
    EXPECT_EQ(Rows("SELECT id FROM s" + where + " ORDER BY id"),
              Rows("SELECT id FROM sp" + where + " ORDER BY id"));
    EXPECT_FALSE(Rows("SELECT id FROM sp" + where).empty());
}

TEST_F(KeyBoundTest, AnOpenTupleKeyBoundInAJoinIsPricedAsABound) {
    // The planner prices an open range by its bound, not by a window as
    // wide as the key space (`join_order.cpp`), and the rows are the
    // two-sided twin's.
    LoadWide("t", 120);
    Run("CREATE TABLE u (id int64, t_id int64)");
    for (int i = 1; i <= 40; ++i) {
        Run("INSERT INTO u VALUES (" + std::to_string(i) + ", " + std::to_string(i * 3) + ")");
    }
    EXPECT_EQ(Rows("SELECT u.id, t.id FROM t JOIN u ON u.t_id = t.id WHERE t.id >= 60 "
                   "ORDER BY u.id"),
              Rows("SELECT u.id, t.id FROM t JOIN u ON u.t_id = t.id WHERE t.id >= 60 "
                   "AND t.id <= 120 ORDER BY u.id"));
}

TEST_F(KeyBoundTest, AHeapsKeystoneUnderAValueListKeepsItsSort) {
    Run("CREATE TABLE h (k int64, v int64) HEAP");
    for (int i = 1; i <= 40; ++i) {
        Run("INSERT INTO h (k, v) VALUES (" + std::to_string(i % 3) + ", " + std::to_string(i) +
            ")");
    }
    const std::string sql =
        "SELECT keyidentity FROM h WHERE keyidentity IN (30, 4, 17) ORDER BY keyidentity DESC "
        "LIMIT 2";
    EXPECT_FALSE(Has(Plan(sql), "order=index")) << Plan(sql);
    EXPECT_EQ(Rows(sql), (std::vector<std::string>{"30", "17"}));
}

TEST_F(KeyBoundTest, AHeapsKeystoneIsWalkedInKeyOrderFromItsBound) {
    Run("CREATE TABLE h (k int64, v int64) HEAP");
    for (int i = 1; i <= 300; ++i) {
        Run("INSERT INTO h (k, v) VALUES (" + std::to_string(i % 7) + ", " + std::to_string(i) +
            ")");
    }
    Run("DELETE FROM h WHERE keyidentity = 125");
    Run("UPDATE h SET v = 0 WHERE keyidentity = 126");
    const std::string page =
        "SELECT keyidentity, v FROM h WHERE keyidentity >= 120 ORDER BY keyidentity";
    ExpectElidedWindow(page, 10, 0, "index_asc");
    EXPECT_EQ(Rows(page + " LIMIT 3 OFFSET 4"),
              (std::vector<std::string>{"124,124", "126,0", "127,127"}));
    ExpectElidedWindow("SELECT keyidentity FROM h WHERE keyidentity < 50 ORDER BY keyidentity DESC",
                       5, 0, "index_desc");
    ExpectElidedWindow("SELECT keyidentity, v FROM h ORDER BY keyidentity", 4, 0, "index_asc");
    // A strict window is the Keystone index's range.
    const std::string strict =
        "SELECT keyidentity FROM h WHERE keyidentity > 10 AND keyidentity < 15";
    std::string plan = Plan(strict);
    EXPECT_TRUE(Has(plan, "IndexRange h")) << plan;
    EXPECT_EQ(Rows(strict), (std::vector<std::string>{"11", "12", "13", "14"}));
    // An open end with no order to stop it stays the walk.
    plan = Plan("SELECT keyidentity FROM h WHERE keyidentity > 290");
    EXPECT_FALSE(Has(plan, "IndexRange")) << plan;
}


// ---- EB-S2: a join by a whole key ------------------------------------------
//
// `instructions/v3.2.0/workorder-eb-key-forms-in-the-executor.md` S2: a
// correlated index probe descends by every prefix column an equality pins -
// a literal's or an outer row's - and a join on every column of an embedded
// primary key descends by the id the outer row's values encode to (EB-R3).
// Each reply is checked against the same statement over a twin with no
// index and no primary key, which walks.

class WholeKeyJoinTest : public OrderedIndexWalkTest {
protected:
    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }
    static bool Has(const std::string& plan, const std::string& what) {
        return plan.find(what) != std::string::npos;
    }
    // `c (id, a, b, v)` and its twin `cp`, `a = id % 4`, `b = id`, with an
    // index on (a, b) on `c` alone; `o` the outer side, its rows naming hits,
    // a row whose `b` matches under another `a`, and a miss.
    void LoadComposite() {
        Run("CREATE TABLE c (id int64, a int64, b int64, v int64)");
        Run("CREATE TABLE cp (id int64, a int64, b int64, v int64)");
        for (int i = 1; i <= 200; ++i) {
            const std::string row = "(" + std::to_string(i) + ", " + std::to_string(i % 4) +
                                    ", " + std::to_string(i) + ", " + std::to_string(i * 10) +
                                    ")";
            Run("INSERT INTO c VALUES " + row);
            Run("INSERT INTO cp VALUES " + row);
        }
        Run("CREATE INDEX c_ab ON c (a, b)");
        Run("CREATE TABLE o (id int64, a int64, b int64)");
        for (const char* row : {"(1, 1, 5)", "(2, 2, 6)", "(3, 3, 6)", "(4, 0, 200)",
                                "(5, 1, 999)", "(6, 1, 5)"}) {
            Run(std::string("INSERT INTO o VALUES ") + row);
        }
    }
    // `e (a int32, b int16, v)` with its primary key embedded, and its twin
    // `ep` with none; rows at each column's bounds, the one key 48 bits
    // cannot name left out (it is refused).
    void LoadEmbedded() {
        Run("CREATE TABLE e (a int32, b int16, v int64, PRIMARY KEY (a, b))");
        Run("CREATE TABLE ep (id int64, a int32, b int16, v int64)");
        const std::vector<std::pair<std::string, std::string>> keys = {
            {"0", "0"},           {"0", "1"},          {"1", "0"},
            {"5", "7"},           {"5", "8"},          {"-1", "-1"},
            {"-2147483648", "-32768"}, {"2147483647", "32766"}, {"2147483646", "32767"},
            {"7", "-32768"},      {"7", "32767"}};
        int id = 1;
        for (const auto& [a, b] : keys) {
            const std::string v = std::to_string(id * 100);
            Run("INSERT INTO e VALUES (" + a + ", " + b + ", " + v + ")");
            Run("INSERT INTO ep VALUES (" + std::to_string(id) + ", " + a + ", " + b + ", " + v +
                ")");
            ++id;
        }
        Run("CREATE TABLE q (id int64, ea int32 NULL, eb int16 NULL)");
        for (const char* row :
             {"(1, 0, 1)", "(2, 5, 8)", "(3, -2147483648, -32768)", "(4, 2147483647, 32766)",
              "(5, 2147483646, 32767)", "(6, 2147483647, 32767)", "(7, 7, -32768)",
              "(8, NULL, 0)", "(9, 0, NULL)", "(10, 5, 9)", "(11, -1, -1)", "(12, 5, 7)"}) {
            Run(std::string("INSERT INTO q VALUES ") + row);
        }
    }
};

TEST_F(WholeKeyJoinTest, AJoinOnEveryColumnOfACompositeIndexDescendsByTheWholeKey) {
    LoadComposite();
    const std::string sql =
        "SELECT o.id, c.id, c.v FROM o JOIN c ON c.a = o.a WHERE c.b = o.b ORDER BY o.id";
    const std::string plan = Plan(sql);
    EXPECT_TRUE(Has(plan, "IndexProbe c key=0:0.1,0:0.2")) << plan;
    // One entry a matching outer row, where the leading column alone ran 50.
    EXPECT_TRUE(Has(plan, "index_scanned=4 ")) << plan;
    EXPECT_EQ(Rows(sql), Rows("SELECT o.id, cp.id, cp.v FROM o JOIN cp ON cp.a = o.a "
                              "WHERE cp.b = o.b ORDER BY o.id"));
    // Written the other way round, and with the equalities swapped.
    EXPECT_EQ(Rows("SELECT o.id, c.id FROM o JOIN c ON o.b = c.b WHERE o.a = c.a ORDER BY o.id"),
              Rows("SELECT o.id, cp.id FROM o JOIN cp ON cp.a = o.a WHERE cp.b = o.b "
                   "ORDER BY o.id"));
}

TEST_F(WholeKeyJoinTest, ALiteralAndAJoinKeyTogetherAreTheProbesPrefix) {
    LoadComposite();
    const std::string sql = "SELECT o.id, c.id FROM o JOIN c ON c.b = o.b WHERE c.a = 1 "
                            "ORDER BY o.id";
    const std::string plan = Plan(sql);
    EXPECT_TRUE(Has(plan, "IndexProbe c key=0:0.2")) << plan;
    EXPECT_TRUE(Has(plan, "index_scanned=2 ")) << plan;
    EXPECT_EQ(Rows(sql), Rows("SELECT o.id, cp.id FROM o JOIN cp ON cp.b = o.b WHERE cp.a = 1 "
                              "ORDER BY o.id"));
    // The leading column alone by the outer row is still a probe, as before.
    const std::string lead = "SELECT o.id, c.id FROM o JOIN c ON c.a = o.a WHERE o.id = 1 "
                             "ORDER BY c.id";
    EXPECT_TRUE(Has(Plan(lead), "IndexProbe c key=0:0.1")) << Plan(lead);
    EXPECT_EQ(Rows(lead), Rows("SELECT o.id, cp.id FROM o JOIN cp ON cp.a = o.a WHERE o.id = 1 "
                               "ORDER BY cp.id"));
}

TEST_F(WholeKeyJoinTest, AJoinOnAnEmbeddedKeyDescendsByTheIdItsValuesEncodeTo) {
    LoadEmbedded();
    const std::string sql =
        "SELECT q.id, e.a, e.b, e.v FROM q JOIN e ON e.a = q.ea WHERE e.b = q.eb ORDER BY q.id";
    const std::string plan = Plan(sql);
    EXPECT_TRUE(Has(plan, "Probe e key=embedded(0:0.1,0:0.2)")) << plan;
    // Each column's bounds, a NULL on either side, a miss, and the one key
    // 48 bits cannot name (q.id 6): the twin's walk is the answer.
    EXPECT_EQ(Rows(sql), Rows("SELECT q.id, ep.a, ep.b, ep.v FROM q JOIN ep ON ep.a = q.ea "
                              "WHERE ep.b = q.eb ORDER BY q.id"));
    EXPECT_EQ(Rows(sql).size(), 8u);
    // A literal and an outer column together (EB-S0's J3), ahead of the
    // literal prefix's window.
    const std::string mixed =
        "SELECT q.id, e.v FROM q JOIN e ON e.b = q.eb WHERE e.a = 5 ORDER BY q.id";
    EXPECT_TRUE(Has(Plan(mixed), "key=embedded(5,0:0.2)")) << Plan(mixed);
    EXPECT_EQ(Rows(mixed), Rows("SELECT q.id, ep.v FROM q JOIN ep ON ep.b = q.eb WHERE ep.a = 5 "
                                "ORDER BY q.id"));
    // A literal that does not fit its column matches no row, as the walk's
    // residual says.
    EXPECT_TRUE(Rows("SELECT q.id FROM q JOIN e ON e.b = q.eb WHERE e.a = 9999999999").empty());
}

// EB-S1's review: an open end on another index's `ORDER BY` column does not
// move a step the ladder put on a narrower index range with no prefix.
TEST_F(KeyBoundTest, AnOpenEndDoesNotMoveAStepOffAnotherIndexsRange) {
    LoadEvents();
    Run("CREATE INDEX ev_at ON ev (created_at)");
    Run("CREATE INDEX ev_kind ON ev (kind)");
    const std::string sql =
        "SELECT id, kind FROM ev WHERE created_at BETWEEN 1 AND 2 AND kind > 0 ORDER BY kind, id "
        "LIMIT 10";
    const std::string plan = Plan(sql);
    EXPECT_TRUE(Has(plan, "IndexRange ev")) << plan;
    EXPECT_FALSE(Has(plan, "order=index")) << plan;
    EXPECT_EQ(Rows(sql), Window("SELECT id, kind FROM ev WHERE created_at BETWEEN 1 AND 2 AND "
                                "kind > 0 ORDER BY kind, id",
                                10, 0));
}

// A probe whose key is computed from an outer row records and replays no
// trail (EB-Q10 (a)): `kProbe` is a replayable kind, and the computed key is
// what takes the step off the line, as a key set does.
TEST(EmbeddedProbeTrailTest, AnEmbeddedKeyProbeIsNoTrailsStep) {
    exec::Step step;
    step.kind = exec::AccessKind::kProbe;
    ASSERT_TRUE(exec::IsTrailReplayableKind(step.kind));
    step.from_row.emplace().embedded.push_back(exec::Operand{});
    EXPECT_FALSE(exec::IsTrailReplayable(step));
}


// ---- EB-S3: an order an embedded key already holds --------------------------
//
// `instructions/v3.2.0/workorder-eb-key-forms-in-the-executor.md` S3: the
// rows of a relation whose primary key is embedded are in key order, so an
// ascending `ORDER BY` of the key's columns - whole or a prefix - and the
// whole key descending under a LIMIT are not sorted. Every reply is checked
// against a twin whose tuple keys were issued in key order, so its sort's
// ties - by ascending tuple key - are the key's order too.

class EmbeddedOrderTest : public OrderedWalkTest {
protected:
    void Load() {
        Run("CREATE TABLE e (a int32, b int16, v int64, PRIMARY KEY (a, b))");
        Run("CREATE TABLE ep (id int64, a int32, b int16, v int64)");
        int id = 1;
        for (int a = -3; a <= 8; ++a) {
            for (int b : {-2, 0, 5}) {
                const std::string row = std::to_string(a) + ", " + std::to_string(b) + ", " +
                                        std::to_string((a * 7 + b) % 5);
                Run("INSERT INTO e VALUES (" + row + ")");
                Run("INSERT INTO ep VALUES (" + std::to_string(id++) + ", " + row + ")");
            }
        }
        Run("DELETE FROM e WHERE a = 2 AND b = 0");
        Run("DELETE FROM ep WHERE a = 2 AND b = 0");
    }
    std::string Plan(const std::string& sql) { return Run("ANALYZE " + sql); }
    void ExpectUnsorted(const std::string& e_sql, const std::string& twin_sql) {
        const std::string plan = Plan(e_sql);
        EXPECT_EQ(plan.find("sorted="), std::string::npos) << e_sql << ": " << plan;
        EXPECT_EQ(Rows(e_sql), Rows(twin_sql)) << e_sql;
    }
    void ExpectSorted(const std::string& e_sql, const std::string& twin_sql) {
        const std::string plan = Plan(e_sql);
        EXPECT_NE(plan.find("sorted="), std::string::npos) << e_sql << ": " << plan;
        EXPECT_EQ(Rows(e_sql), Rows(twin_sql)) << e_sql;
    }
};

TEST_F(EmbeddedOrderTest, TheWholeKeyAscendingIsTheWalksOrder) {
    Load();
    ExpectUnsorted("SELECT a, b, v FROM e ORDER BY a, b LIMIT 5 OFFSET 2",
                   "SELECT a, b, v FROM ep ORDER BY a, b LIMIT 5 OFFSET 2");
    ExpectUnsorted("SELECT a, b FROM e ORDER BY a, b",
                   "SELECT a, b FROM ep ORDER BY a, b");
    ExpectUnsorted("SELECT a, b FROM e WHERE a >= 5 ORDER BY a, b LIMIT 4",
                   "SELECT a, b FROM ep WHERE a >= 5 ORDER BY a, b LIMIT 4");
    ExpectUnsorted("SELECT a, b FROM e WHERE a = 6 ORDER BY a, b",
                   "SELECT a, b FROM ep WHERE a = 6 ORDER BY a, b");
}

TEST_F(EmbeddedOrderTest, AnAscendingPrefixIsElidedWithItsTiesInKeyOrder) {
    Load();
    ExpectUnsorted("SELECT a, b, v FROM e ORDER BY a LIMIT 7",
                   "SELECT a, b, v FROM ep ORDER BY a LIMIT 7");
    ExpectUnsorted("SELECT a, b FROM e WHERE v = 1 ORDER BY a",
                   "SELECT a, b FROM ep WHERE v = 1 ORDER BY a");
}

TEST_F(EmbeddedOrderTest, TheWholeKeyDescendingUnderALimitIsTheReverseWalk) {
    Load();
    const std::string sql = "SELECT a, b, v FROM e ORDER BY a DESC, b DESC LIMIT 5 OFFSET 1";
    EXPECT_NE(Plan(sql).find("order=tuple_key_desc"), std::string::npos) << Plan(sql);
    ExpectUnsorted(sql, "SELECT a, b, v FROM ep ORDER BY a DESC, b DESC LIMIT 5 OFFSET 1");
    ExpectUnsorted("SELECT a, b FROM e WHERE a < 0 ORDER BY a DESC, b DESC LIMIT 4",
                   "SELECT a, b FROM ep WHERE a < 0 ORDER BY a DESC, b DESC LIMIT 4");
}

TEST_F(EmbeddedOrderTest, WhatKeepsItsSort) {
    Load();
    // A descending prefix: its ties would leave descending (EB-Q8 (a)).
    ExpectSorted("SELECT a, b FROM e ORDER BY a DESC LIMIT 5",
                 "SELECT a, b FROM ep ORDER BY a DESC LIMIT 5");
    // Mixed directions, the key out of order, a column past the key, no LIMIT
    // on a descending order.
    ExpectSorted("SELECT a, b FROM e ORDER BY a, b DESC LIMIT 5",
                 "SELECT a, b FROM ep ORDER BY a, b DESC LIMIT 5");
    ExpectSorted("SELECT a, b FROM e ORDER BY b, a LIMIT 5",
                 "SELECT a, b FROM ep ORDER BY b, a LIMIT 5");
    ExpectSorted("SELECT a, b, v FROM e ORDER BY a, b, v LIMIT 5",
                 "SELECT a, b, v FROM ep ORDER BY a, b, v LIMIT 5");
    ExpectSorted("SELECT a, b FROM e ORDER BY a DESC, b DESC",
                 "SELECT a, b FROM ep ORDER BY a DESC, b DESC");
}


// EB-S3's review: the sign-flip and packing paths of the other embeddable
// types, a join the embedded relation drives, and a user index on the key's
// columns, which takes no bare walk's order away.
TEST_F(EmbeddedOrderTest, EveryEmbeddableTypeIsInItsKeysOrder) {
    Run("CREATE TABLE d (s int8, f bool, day date, v int64, PRIMARY KEY (s, f, day))");
    Run("CREATE TABLE dp (id int64, s int8, f bool, day date, v int64)");
    int id = 1;
    for (const char* s : {"-128", "-1", "0", "127"}) {
        for (const char* f : {"0", "1"}) {
            for (const char* day : {"'1970-01-01'", "'2026-11-27'"}) {
                const std::string row = std::string(s) + ", " + f + ", " + day + ", " +
                                        std::to_string(id);
                Run("INSERT INTO d VALUES (" + row + ")");
                Run("INSERT INTO dp VALUES (" + std::to_string(id++) + ", " + row + ")");
            }
        }
    }
    ExpectUnsorted("SELECT s, f, day FROM d ORDER BY s, f, day",
                   "SELECT s, f, day FROM dp ORDER BY s, f, day");
    ExpectUnsorted("SELECT s, f, v FROM d ORDER BY s, f LIMIT 9",
                   "SELECT s, f, v FROM dp ORDER BY s, f LIMIT 9");
    ExpectUnsorted("SELECT s, f, day FROM d ORDER BY s DESC, f DESC, day DESC LIMIT 6",
                   "SELECT s, f, day FROM dp ORDER BY s DESC, f DESC, day DESC LIMIT 6");
}

TEST_F(EmbeddedOrderTest, AJoinTheEmbeddedRelationDrivesKeepsItsOrder) {
    Load();
    Run("CREATE TABLE w (id int64, a int32)");
    for (int i = 1; i <= 6; ++i) {
        Run("INSERT INTO w VALUES (" + std::to_string(i) + ", " + std::to_string(i - 2) + ")");
    }
    const std::string sql =
        "SELECT e.a, e.b, w.id FROM e JOIN w ON w.a = e.a ORDER BY e.a, e.b LIMIT 8";
    EXPECT_EQ(Rows(sql), Rows("SELECT ep.a, ep.b, w.id FROM ep JOIN w ON w.a = ep.a "
                              "ORDER BY ep.a, ep.b LIMIT 8"));
}

TEST_F(EmbeddedOrderTest, AUserIndexOnTheKeyTakesNoBareWalksOrder) {
    Load();
    Run("CREATE INDEX e_a ON e (a)");
    const std::string sql = "SELECT a, b FROM e ORDER BY a LIMIT 5";
    const std::string plan = Plan(sql);
    EXPECT_EQ(plan.find("order=index"), std::string::npos) << plan;
    ExpectUnsorted(sql, "SELECT a, b FROM ep ORDER BY a LIMIT 5");
}

}  // namespace
}  // namespace kds::server
