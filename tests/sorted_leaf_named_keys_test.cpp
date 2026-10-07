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

// **A named key lands where it sorts** (BD,
// `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`), through SQL,
// on one core.
//
// BD-R5's claim, the one this file pins: a named key's `INSERT` is refused
// because of its pk for exactly two reasons - a duplicate (`AlreadyExists`)
// or a key outside `[1, 2^40 - 1]` (`OutOfRange`) - and each refusal names
// the pk token's byte. Everything else about the key is placed: below the
// relation's mark, in any order, and a key a rollback freed (W8, W12).
//
// The two-session cells (a named key meeting an undecided writer) are in
// `lock_family_test.cpp`, beside the lock family's other waits; the storage
// and crash cells are in `btree_test.cpp`, `recovery_undo_test.cpp` and
// `sorted_leaf_crash_test.cpp`.

namespace kds::server {
namespace {

// The ids a `SELECT *` reply carries, in reply order: each row is
// `id,qty`, separated by an escaped newline.
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

bool Placed(const DispatchOutcome& out) { return out.response.rfind("INSERTED", 0) == 0; }

// A transaction manager behind the dispatcher, which `BEGIN`/`ROLLBACK` and a
// multi-row `VALUES` need. No lock table and no log: the waits are
// `lock_family_test.cpp`'s, the crashes `sorted_leaf_crash_test.cpp`'s.
class SortedLeafSqlTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        d_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                   /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kGroup,
                   exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                   /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
        ASSERT_EQ(d_->Dispatch("CREATE TABLE t (id int64, qty int64) BTREE").response.substr(0, 7),
                  "CREATED");
    }

    DispatchOutcome Run(const std::string& sql) { return d_->Dispatch(sql); }

    // The relation in key order as the walk yields it, and whether the
    // compiler kept the walk's order for `ORDER BY id` - the elision reads
    // a leaf's slot order as its key order, which is what BD-R1 keeps.
    std::vector<std::uint64_t> Walk() { return EmittedIds(Run("SELECT * FROM t").response); }
    std::vector<std::uint64_t> Ordered() {
        return EmittedIds(Run("SELECT * FROM t ORDER BY id").response);
    }
    bool OrderByIdIsElided() {
        return Run("ANALYZE SELECT * FROM t ORDER BY id").response.find("sort ") ==
               std::string::npos;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> d_;
};

// ---- Red at BD-S0: a key below the mark is placed (W8) --------------------

TEST_F(SortedLeafSqlTest, AKeyBelowAnEarlierOneIsPlacedAndOrderByStaysElided) {
    // W8 verbatim: "pk=100을 먼저 쓰면 pk=99 삽입이 안되는 현상이 있으면 안돼".
    ASSERT_TRUE(Placed(Run("INSERT INTO t VALUES (100, 1)")));
    const DispatchOutcome below = Run("INSERT INTO t VALUES (99, 2)");
    ASSERT_TRUE(Placed(below)) << below.response;
    EXPECT_NE(below.response.find(" id=99 "), std::string::npos) << below.response;

    // Placed where it sorts, so the bare walk is already in key order and
    // the compiler still discards the sort.
    EXPECT_EQ(Walk(), (std::vector<std::uint64_t>{99, 100}));
    EXPECT_EQ(Ordered(), (std::vector<std::uint64_t>{99, 100}));
    EXPECT_TRUE(OrderByIdIsElided());

    // An omitted key still issues above every key placed (BD-R7).
    EXPECT_NE(Run("INSERT INTO t VALUES (3)").response.find("id=101"), std::string::npos);
}

TEST_F(SortedLeafSqlTest, ADescendingMultiRowValuesIsPlacedInKeyOrder) {
    // Enough rows to split the leaf more than once, so the incoming key
    // lands mid-leaf, at a divide, and on a leaf with a right sibling.
    std::string sql = "INSERT INTO t VALUES ";
    const int kRows = 450;
    for (int id = kRows; id >= 1; --id) {
        sql += "(" + std::to_string(id) + ", " + std::to_string(id) + ")";
        if (id > 1) sql += ", ";
    }
    const DispatchOutcome out = Run(sql);
    ASSERT_TRUE(Placed(out)) << out.response.substr(0, 300);

    std::vector<std::uint64_t> want;
    for (int id = 1; id <= kRows; ++id) want.push_back(static_cast<std::uint64_t>(id));
    EXPECT_EQ(Walk(), want) << "a leaf is not in key order";
    EXPECT_EQ(Ordered(), want);
    EXPECT_TRUE(OrderByIdIsElided());

    // Every row is still where a descent finds it.
    for (int id : {1, 2, 199, 200, 201, 449, 450}) {
        const std::string want_row = std::to_string(id) + "," + std::to_string(id);
        EXPECT_NE(Run("SELECT * FROM t WHERE id = " + std::to_string(id)).response.find(want_row),
                  std::string::npos)
            << "id " << id;
    }
}

TEST_F(SortedLeafSqlTest, ARolledBackKeyIsNamedAgainAndPlaced) {
    // W12: "롤백된 아이디는 당연히 재사용되어야해". The rolled-back tuple was
    // never visible to another snapshot (BD §1.7), so the key is free.
    ASSERT_TRUE(Placed(Run("INSERT INTO t VALUES (1, 1)")));
    Session s;
    ASSERT_EQ(d_->Dispatch("BEGIN", &s).response.rfind("BEGIN", 0), 0u);
    ASSERT_TRUE(Placed(d_->Dispatch("INSERT INTO t VALUES (5, 50)", &s)));
    ASSERT_EQ(d_->Dispatch("ROLLBACK", &s).response.rfind("ROLLBACK", 0), 0u);

    const DispatchOutcome again = Run("INSERT INTO t VALUES (5, 55)");
    ASSERT_TRUE(Placed(again)) << again.response;
    EXPECT_NE(Run("SELECT * FROM t WHERE id = 5").response.find("5,55"), std::string::npos);
    EXPECT_EQ(Walk(), (std::vector<std::uint64_t>{1, 5}));
}

// ---- Guard, green at BD-S0 and kept green: a deleted key is bound once ----

TEST_F(SortedLeafSqlTest, ADeletedKeyNamedAgainIsAlreadyExists) {
    // BD-R4: an id is consumed when its tuple commits, and the delete-marked
    // row is the tombstone that keeps it so.
    ASSERT_TRUE(Placed(Run("INSERT INTO t VALUES (5, 1)")));
    ASSERT_TRUE(Placed(Run("INSERT INTO t VALUES (9, 1)")));
    ASSERT_EQ(Run("DELETE FROM t WHERE id = 5").response.rfind("DELETED", 0), 0u);

    const DispatchOutcome again = Run("INSERT INTO t VALUES (5, 2)");
    EXPECT_EQ(again.status.code(), StatusCode::kAlreadyExists) << again.response;
    EXPECT_EQ(Walk(), (std::vector<std::uint64_t>{9}));
}

// ---- The census cell: every pk-caused refusal is one of two codes ---------

TEST_F(SortedLeafSqlTest, EveryRefusalANamedKeyGetsForItsPkIsDuplicateOrExhausted) {
    // BD §1.4's table, every row a statement can reach on one core without a
    // concurrent writer: a key is placed, or refused for its pk with exactly
    // the code BD-R5 gives its reason - never `InvalidArgument`, never the
    // other of the two - and the pk token's byte, which starts at 22 in
    // `INSERT INTO t VALUES (k, 1)`.
    //
    // 251 rows, so the first leaf has a right sibling: a key landing there
    // is §1.4's S3 row, refused today and placed under BD.
    std::string load = "INSERT INTO t VALUES ";
    for (int id = 1000; id <= 1250; ++id) {
        load += "(" + std::to_string(id) + ", 1)" + (id < 1250 ? ", " : "");
    }
    ASSERT_TRUE(Placed(Run(load)));
    ASSERT_TRUE(Placed(Run("INSERT INTO t VALUES (1300, 1)")));
    ASSERT_EQ(Run("DELETE FROM t WHERE id = 1300").response.rfind("DELETED", 0), 0u);

    struct Case {
        const char* key;
        std::optional<StatusCode> refused;  // nullopt: placed
    };
    const Case cases[] = {
        {"1000", StatusCode::kAlreadyExists},                // present, on a leaf with a right sibling
        {"1300", StatusCode::kAlreadyExists},                // deleted: bound once (BD-R4)
        {"0", StatusCode::kOutOfRange},                      // R3
        {"-7", StatusCode::kOutOfRange},                     // R2
        {"1099511627776", StatusCode::kOutOfRange},          // R4: 2^40
        {"9223372036854775808", StatusCode::kOutOfRange},    // 2^63, the lexer's overflow (R2)
        {"18446744073709551621", StatusCode::kOutOfRange},   // 2^64 + 5, placed as 5 today (§1.4)
        {"1", std::nullopt},                                 // below the mark, absent (L2, S5)
        {"1005", std::nullopt},                              // ...in a leaf with a right sibling (S3)
        {"1099511627775", std::nullopt},                     // the top of the id space
    };
    for (const Case& c : cases) {
        const DispatchOutcome out = Run(std::string("INSERT INTO t VALUES (") + c.key + ", 1)");
        if (!c.refused.has_value()) {
            EXPECT_TRUE(Placed(out)) << c.key << " -> " << out.response;
            continue;
        }
        EXPECT_EQ(out.status.code(), *c.refused) << c.key << " -> " << out.response;
        EXPECT_NE(out.response.find("(byte 22)"), std::string::npos)
            << c.key << " -> " << out.response;
    }

    // A key named twice in one statement is a duplicate at its second
    // occurrence (BD-R5), with that row's number and its token's byte.
    const DispatchOutcome twice = Run("INSERT INTO t VALUES (7, 1), (7, 2)");
    EXPECT_EQ(twice.status.code(), StatusCode::kAlreadyExists) << twice.response;
    EXPECT_NE(twice.response.find("(byte 30)"), std::string::npos) << twice.response;
    EXPECT_NE(twice.response.find("(row 2)"), std::string::npos) << twice.response;

    // Nothing a refusal answered landed: in particular not 5, the wrapped
    // 2^64 + 5.
    const std::vector<std::uint64_t> walked = Walk();
    EXPECT_EQ(std::count(walked.begin(), walked.end(), std::uint64_t{5}), 0);
    EXPECT_EQ(std::count(walked.begin(), walked.end(), std::uint64_t{7}), 0)
        << "a refused statement left its first row";
}

}  // namespace
}  // namespace kds::server
