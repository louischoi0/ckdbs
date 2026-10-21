#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/transition_catalog.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "view_text.hpp"

// Transition - BV-S2 (`instructions/v3.2.0/workorder-bv-transition-
// constraints.md`): the surface, the create-time refusals and the three
// checks, one cell per write path (BV-R2). Every refusal of a write is
// `TRANSITION_VIOLATION retryable=0`; every refusal of a declaration names
// its byte.

namespace kds::server {
namespace {

constexpr const char* kOrderStatus =
    "CREATE TRANSITION order_status ON orders(status) "
    "ON INSERT ('NEW') "
    "ON UPDATE ('NEW' => 'CHECKED', 'CHECKED' => 'FINALIZED', "
    "'NEW' => 'CANCELLED', 'CHECKED' => 'CANCELLED') "
    "ON DELETE FROM ('NEW', 'CANCELLED')";

class TransitionTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/6000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        Reopen();
        ASSERT_EQ(Run("CREATE TABLE orders (id int64, status varchar(32), amount int64)")
                      .substr(0, 7),
                  "CREATED");
    }

    // A dispatcher over the same catalog and store - what a new statement
    // stream sees; the restart cell below builds a new catalog too.
    void Reopen() {
        dispatcher_.reset();
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    static bool Violates(const std::string& reply) {
        return reply.rfind("ERR TRANSITION_VIOLATION retryable=0 ", 0) == 0;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

// ---- The surface ----------------------------------------------------------

TEST_F(TransitionTest, ADeclarationIsCreatedListedAndDropped) {
    const std::string created = Run(kOrderStatus);
    ASSERT_EQ(created.rfind("CREATED TRANSITION name=order_status", 0), 0u) << created;
    EXPECT_NE(created.find("on=orders column=status rows=0"), std::string::npos) << created;

    const std::string shown = Run("SHOW TRANSITIONS");
    const auto view = testing_view::ReadViewText(shown);
    ASSERT_EQ(view.rows.size(), 1u) << shown;
    EXPECT_EQ(view.Find("name", "order_status", "rel_name"), "orders") << shown;
    EXPECT_EQ(view.Find("name", "order_status", "column_no"), "1") << shown;
    EXPECT_EQ(view.Find("name", "order_status", "column"), "status") << shown;
    EXPECT_EQ(view.Find("name", "order_status", "def").value_or("").rfind("CREATE TRANSITION", 0),
              0u)
        << shown;

    EXPECT_EQ(Run("DROP TRANSITION ORDER_STATUS").rfind("DROPPED TRANSITION", 0), 0u);
    EXPECT_EQ(testing_view::ReadViewText(Run("SHOW TRANSITIONS")).rows.size(), 0u);
    // Unchecked once dropped.
    EXPECT_EQ(Run("INSERT INTO orders VALUES ('FINALIZED', 1)").substr(0, 8), "INSERTED");
}

TEST_F(TransitionTest, TheWordsAreNotReserved) {
    // BV-R5: nothing is reserved, so `transition` stays a usable name. The
    // fingerprint not moving is the golden corpus' to prove
    // (`parser_golden_test.cpp`, `fingerprint_test.cpp`).
    EXPECT_EQ(Run("CREATE TABLE transition (id int64, insert_at int64)").substr(0, 7), "CREATED");
}

// ---- The three checks -----------------------------------------------------

TEST_F(TransitionTest, AnInsertMustStartInAnInitialState) {
    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
    EXPECT_EQ(Run("INSERT INTO orders VALUES ('NEW', 10)").substr(0, 8), "INSERTED");

    const std::string refused = Run("INSERT INTO orders VALUES ('CHECKED', 10)");
    ASSERT_TRUE(Violates(refused)) << refused;
    EXPECT_NE(refused.find("transition 'order_status' on column 'status': a row may not be "
                           "inserted in state 'CHECKED'"),
              std::string::npos)
        << refused;
    EXPECT_NE(refused.find("(byte 27)"), std::string::npos) << refused;

    // A named tuple key, and a multi-row statement: each row is checked.
    EXPECT_EQ(Run("INSERT INTO orders VALUES (100, 'NEW', 1)").substr(0, 8), "INSERTED");
    EXPECT_TRUE(Violates(Run("INSERT INTO orders VALUES ('NEW', 1), ('FINALIZED', 2)")));
    EXPECT_EQ(Run("SELECT * FROM orders WHERE amount = 2"), "id,status,amount");  // no row
}

TEST_F(TransitionTest, AnUpdateMovesAlongADeclaredEdgeOnly) {
    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO orders VALUES (1, 'NEW', 10)").substr(0, 8), "INSERTED");

    // NEW => FINALIZED skips CHECKED: the path property, refused.
    const std::string skipped = Run("UPDATE orders SET status = 'FINALIZED' WHERE id = 1");
    ASSERT_TRUE(Violates(skipped)) << skipped;
    EXPECT_NE(skipped.find("'NEW' => 'FINALIZED' is not a declared edge, row id 1"),
              std::string::npos)
        << skipped;

    EXPECT_EQ(Run("UPDATE orders SET status = 'CHECKED' WHERE id = 1").substr(0, 7), "UPDATED");
    // CN-4 T2: an unchanged value is no transition.
    EXPECT_EQ(Run("UPDATE orders SET status = 'CHECKED' WHERE id = 1").substr(0, 7), "UPDATED");
    // A SET that does not touch the state column is not checked.
    EXPECT_EQ(Run("UPDATE orders SET amount = 11 WHERE id = 1").substr(0, 7), "UPDATED");
    // An expression is checked by the value it computes (BJ).
    EXPECT_EQ(Run("UPDATE orders SET status = CASE WHEN amount > 5 THEN 'FINALIZED' ELSE "
                  "'CANCELLED' END WHERE id = 1")
                  .substr(0, 7),
              "UPDATED");
    // FINALIZED is terminal.
    EXPECT_TRUE(Violates(Run("UPDATE orders SET status = 'CANCELLED' WHERE id = 1")));
    EXPECT_NE(Run("SELECT status FROM orders WHERE id = 1").find("FINALIZED"),
              std::string::npos);
}

TEST_F(TransitionTest, ADeleteLeavesOnlyFromADeletableState) {
    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO orders VALUES (1, 'NEW', 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO orders VALUES (2, 'NEW', 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("UPDATE orders SET status = 'CHECKED' WHERE id = 2").substr(0, 7), "UPDATED");

    const std::string refused = Run("DELETE FROM orders WHERE id = 2");
    ASSERT_TRUE(Violates(refused)) << refused;
    EXPECT_NE(refused.find("a row may not be deleted from state 'CHECKED', row id 2"),
              std::string::npos)
        << refused;
    EXPECT_EQ(Run("DELETE FROM orders WHERE id = 1").substr(0, 7), "DELETED");
}

TEST_F(TransitionTest, WithoutOnDeleteEveryDeleteIsAllowed) {
    ASSERT_EQ(Run("CREATE TRANSITION t ON orders(status) ON INSERT ('NEW') "
                  "ON UPDATE ('NEW' => 'DONE')")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO orders VALUES (1, 'NEW', 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("UPDATE orders SET status = 'DONE' WHERE id = 1").substr(0, 7), "UPDATED");
    EXPECT_EQ(Run("DELETE FROM orders WHERE id = 1").substr(0, 7), "DELETED");
}

TEST_F(TransitionTest, AnIntegerStateColumnComparesByValue) {
    ASSERT_EQ(Run("CREATE TABLE jobs (id int64, phase int32)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE TRANSITION phases ON jobs(phase) ON INSERT (0) "
                  "ON UPDATE (0 => 1, 1 => 2)")
                  .substr(0, 7),
              "CREATED");
    EXPECT_TRUE(Violates(Run("INSERT INTO jobs VALUES (1)")));
    ASSERT_EQ(Run("INSERT INTO jobs VALUES (1, 0)").substr(0, 8), "INSERTED");
    EXPECT_TRUE(Violates(Run("UPDATE jobs SET phase = 2 WHERE id = 1")));
    EXPECT_EQ(Run("UPDATE jobs SET phase = phase + 1 WHERE id = 1").substr(0, 7), "UPDATED");
}

TEST_F(TransitionTest, ARefusalInsideATransactionFailsTheTransaction) {
    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO orders VALUES (1, 'NEW', 1)").substr(0, 8), "INSERTED");
    EXPECT_TRUE(Violates(Run(s, "UPDATE orders SET status = 'FINALIZED' WHERE id = 1")));
    // No statement-level rollback: the transaction is failed, as for every
    // other write failure.
    EXPECT_EQ(Run(s, "COMMIT").substr(0, 3), "ERR");
    EXPECT_EQ(Run("SELECT * FROM orders"), "id,status,amount");  // no row
}

// ---- Every write path (BV-R2) --------------------------------------------

TEST_F(TransitionTest, TheLoadPathsInsertIsCheckedLikeATextualOne) {
    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
    // The KWP load session's door: an InsertStmt handed to ExecuteInsert.
    auto parsed = parser::Parse("INSERT INTO orders VALUES ('CHECKED', 1)");
    ASSERT_TRUE(parsed.ok());
    Session s;
    const DispatchOutcome out =
        dispatcher_->ExecuteInsert(std::get<parser::InsertStmt>(parsed.value()), s);
    EXPECT_EQ(out.status.code(), StatusCode::kTransitionViolation) << out.response;
}

// ---- Create-time refusals ---------------------------------------------------

TEST_F(TransitionTest, EveryCreateTimeRefusalNamesItsByte) {
    ASSERT_EQ(Run("CREATE TABLE loose (id int64, status varchar(32) NULL, price decimal(8, 2))")
                  .substr(0, 7),
              "CREATED");
    const auto refused = [&](const std::string& sql, const std::string& expect) {
        const std::string out = Run(sql);
        EXPECT_EQ(out.rfind("ERR", 0), 0u) << sql << " -> " << out;
        EXPECT_NE(out.find(expect), std::string::npos) << sql << " -> " << out;
        EXPECT_NE(out.find("byte"), std::string::npos) << sql << " -> " << out;
    };
    refused("CREATE TRANSITION t ON nowhere(status) ON INSERT ('A') ON UPDATE ('A' => 'B')",
            "no relation named 'nowhere'");
    refused("CREATE TRANSITION t ON orders(nope) ON INSERT ('A') ON UPDATE ('A' => 'B')",
            "no column 'nope'");
    refused("CREATE TRANSITION t ON orders(id) ON INSERT (1) ON UPDATE (1 => 2)",
            "cannot govern the tuple key");
    refused("CREATE TRANSITION t ON loose(status) ON INSERT ('A') ON UPDATE ('A' => 'B')",
            "is nullable");
    refused("CREATE TRANSITION t ON loose(price) ON INSERT ('1') ON UPDATE ('1' => '2')",
            "cannot be a state column");
    refused("CREATE TRANSITION t ON orders(status) ON UPDATE ('A' => 'B')",
            "requires ON INSERT");
    refused("CREATE TRANSITION t ON orders(status) ON INSERT ('A')", "requires ON UPDATE");
    refused("CREATE TRANSITION t ON orders(status) ON INSERT ('A') ON UPDATE ('A' = > 'B')",
            "<state> => <state>");
    refused("CREATE TRANSITION t ON orders(status) ON INSERT ('A') ON UPDATE ('B' => 'C')",
            "'B' is reachable from no ON INSERT state");
    refused("CREATE TRANSITION t ON orders(status) ON INSERT (7) ON UPDATE (7 => 8)",
            "must be a string literal");
    refused("CREATE TRANSITION t ON orders(amount) ON INSERT ('x') ON UPDATE ('x' => 'y')",
            "must be an integer literal");
    // A state the column cannot hold (the S2 review's B3): a string for an
    // integer column, a literal wrapped past 64 bits, one past a narrow width.
    refused("CREATE TRANSITION t ON orders(amount) ON INSERT ('7') ON UPDATE ('7' => '8')",
            "must be an integer literal");
    refused("CREATE TRANSITION t ON orders(amount) ON INSERT (18446744073709551616) "
            "ON UPDATE (18446744073709551616 => 1)",
            "does not fit column 'amount'");
    ASSERT_EQ(Run("CREATE TABLE tiny (id int64, s int8)").substr(0, 7), "CREATED");
    refused("CREATE TRANSITION t ON tiny(s) ON INSERT (0) ON UPDATE (0 => 300)",
            "state 300 does not fit column 's'");

    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
    refused(std::string(kOrderStatus), "already carries transition");
    refused("CREATE TRANSITION order_status ON orders(amount) ON INSERT (0) ON UPDATE (0 => 1)",
            "already exists");
}

TEST_F(TransitionTest, ACreateOverARowOutsideTheStateSetIsRefused) {
    ASSERT_EQ(Run("INSERT INTO orders VALUES (1, 'LOST', 1)").substr(0, 8), "INSERTED");
    const std::string out = Run(kOrderStatus);
    ASSERT_TRUE(Violates(out)) << out;
    EXPECT_NE(out.find("an existing row is in state 'LOST'"), std::string::npos) << out;
    EXPECT_EQ(testing_view::ReadViewText(Run("SHOW TRANSITIONS")).rows.size(), 0u);

    // History is never verified: a row already in a late state is accepted.
    ASSERT_EQ(Run("UPDATE orders SET status = 'FINALIZED' WHERE id = 1").substr(0, 7),
              "UPDATED");
    const std::string created = Run(kOrderStatus);
    EXPECT_EQ(created.rfind("CREATED", 0), 0u) << created;
    EXPECT_NE(created.find("rows=1"), std::string::npos) << created;
}

// ---- Lifecycle --------------------------------------------------------------

TEST_F(TransitionTest, ARestartedCatalogEnforcesFromTheStoredDeclaration) {
    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO orders VALUES (1, 'NEW', 1)").substr(0, 8), "INSERTED");

    // A second catalog over the same pages: the rule is re-parsed from
    // `sys.transitions`, not carried in memory.
    catalog::Catalog reopened(store_);
    CommandDispatcher fresh(boot_->superblock, reopened, store_, nullptr, nullptr, nullptr,
                            wal::DurabilityClass::kRelaxed, exec::Budget(), nullptr, false, true,
                            nullptr, &*mgr_);
    const std::string out =
        fresh.Dispatch("UPDATE orders SET status = 'FINALIZED' WHERE id = 1").response;
    EXPECT_TRUE(Violates(out)) << out;
}

TEST_F(TransitionTest, ARenamedColumnKeepsItsTransition) {
    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
    ASSERT_EQ(Run("ALTER TABLE orders RENAME COLUMN status TO state").substr(0, 7), "RENAMED");
    EXPECT_TRUE(Violates(Run("INSERT INTO orders VALUES ('CHECKED', 1)")));
}

TEST_F(TransitionTest, DropTableTakesItsTransitionsAndAnExplicitDropIsRefused) {
    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");

    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    const std::string refused = Run(s, "DROP TABLE orders");
    EXPECT_NE(refused.find("DROP TRANSITION first"), std::string::npos) << refused;
    Run(s, "ROLLBACK");

    ASSERT_EQ(Run("DROP TABLE orders").substr(0, 7), "DROPPED");
    EXPECT_EQ(testing_view::ReadViewText(Run("SHOW TRANSITIONS")).rows.size(), 0u);
    // The name is free again.
    ASSERT_EQ(Run("CREATE TABLE orders (id int64, status varchar(32), amount int64)")
                  .substr(0, 7),
              "CREATED");
    EXPECT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
}

// ---- BV-Q4 (a): PURGE frees no key of a governed relation ---------------

TEST_F(TransitionTest, PurgeIsRefusedOnARelationCarryingATransition) {
    ASSERT_EQ(Run(kOrderStatus).substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO orders VALUES (5, 'NEW', 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("DELETE FROM orders WHERE id = 5").substr(0, 7), "DELETED");
    // Freed, key 5 could come back in 'NEW' and restart its history.
    const std::string refused = Run("PURGE FROM orders WHERE id = 5");
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    EXPECT_NE(refused.find("carries transition 'order_status'"), std::string::npos) << refused;
    EXPECT_NE(refused.find("byte"), std::string::npos) << refused;
    // The key stays bound: naming it again is the duplicate it is.
    EXPECT_NE(Run("INSERT INTO orders VALUES (5, 'NEW', 1)").find("duplicate"), std::string::npos);

    // Without the Transition the statement is PURGE's own again.
    ASSERT_EQ(Run("DROP TRANSITION order_status").rfind("DROPPED", 0), 0u);
    EXPECT_EQ(Run("PURGE FROM orders WHERE id = 5"), "PURGED 1");
}

// ---- BV-R4: the memo is the fence ------------------------------------------

// Two cores: two catalogs over one store sharing one schema word, one
// manager and one lock table. Core B creates a Transition in the window
// between core A's memo and A's first relation intention - the window
// AT-S5e recorded as uncovered (C2). A must not write under the memo that
// lacks the Transition: it is refused to run again, and the re-run meets
// the check. Deterministic: the seam runs B's statement on this thread.
class TransitionMemoFenceTest : public ::testing::Test {
protected:
    std::unique_ptr<txn::LockTable> locks_;

    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/6000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        boot_->catalog.SetInstanceWords(&words_);
        peer_catalog_.emplace(store_);
        peer_catalog_->SetInstanceWords(&words_);
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        auto table = txn::LockTable::Create(/*core_count=*/1, /*max_locks_per_txn=*/1024);
        ASSERT_TRUE(table.ok()) << table.status().message();
        locks_ = std::move(table.value());
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr, /*visibility=*/nullptr, /*core=*/0,
                     locks_.get());
        for (auto [d, c] : {std::pair{&a_, &boot_->catalog}, std::pair{&b_, &*peer_catalog_}}) {
            d->emplace(boot_->superblock, *c, store_, /*log=*/nullptr, /*clock=*/nullptr,
                       /*wal=*/nullptr, wal::DurabilityClass::kRelaxed, exec::Budget(),
                       /*recorder=*/nullptr, /*replay_enabled=*/false,
                       /*access_statistics=*/false, /*cabins=*/nullptr, &*mgr_);
            (*d)->set_locks(locks_.get());
        }
        ASSERT_EQ(a_->Dispatch("CREATE TABLE orders (id int64, status varchar(32))")
                      .response.substr(0, 7),
                  "CREATED");
        // A's memo holds `orders` without a Transition.
        ASSERT_EQ(a_->Dispatch("INSERT INTO orders VALUES ('NEW')").response.substr(0, 8),
                  "INSERTED");
    }

    catalog::InstanceWords words_;
    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<catalog::Catalog> peer_catalog_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> a_;
    std::optional<CommandDispatcher> b_;
};

TEST_F(TransitionMemoFenceTest, ATransitionCreatedBeforeAWritersFirstIntentionIsNotSkipped) {
    // After A's boundary has settled its memo and before A resolves the
    // relation (the epoch seam's kAfterPublish): B takes `X`, publishes the
    // Transition and moves the word, and A then binds from its stale memo.
    bool ran = false;
    std::string created;
    a_->SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, [&] {
        if (ran) return;
        ran = true;
        created = b_->Dispatch(kOrderStatus).response;
    });
    const std::string first = a_->Dispatch("INSERT INTO orders VALUES ('CHECKED')").response;
    a_->SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, nullptr);
    ASSERT_TRUE(ran);
    ASSERT_EQ(created.rfind("CREATED TRANSITION", 0), 0u) << created;

    // Refused to run again - never written under the stale memo.
    EXPECT_EQ(first.rfind("ERR TXN_CONFLICT retryable=1", 0), 0u) << first;
    const std::string retried = a_->Dispatch("INSERT INTO orders VALUES ('CHECKED')").response;
    EXPECT_EQ(retried.rfind("ERR TRANSITION_VIOLATION", 0), 0u) << retried;
    EXPECT_EQ(a_->Dispatch("SELECT * FROM orders WHERE status = 'CHECKED'").response,
              "id,status");  // no row
}

TEST_F(TransitionMemoFenceTest, ACreateWhoseMemoWentStaleStillSeesTheColumnsTransition) {
    // The S2 review's B2, the DDL as the victim: B publishes a Transition on
    // `status` after A's boundary settled its memo. A's CREATE of a second
    // one on the same column must still meet CN-4 T6, which it reads from
    // the memo it makes current under the `X`.
    bool ran = false;
    std::string first;
    a_->SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, [&] {
        if (ran) return;
        ran = true;
        first = b_->Dispatch(kOrderStatus).response;
    });
    const std::string second =
        a_->Dispatch("CREATE TRANSITION other ON orders(status) ON INSERT ('NEW') "
                     "ON UPDATE ('NEW' => 'DONE')")
            .response;
    a_->SetEpochSeamForTest(CommandDispatcher::EpochSeam::kAfterPublish, nullptr);
    ASSERT_TRUE(ran);
    ASSERT_EQ(first.rfind("CREATED TRANSITION", 0), 0u) << first;
    EXPECT_NE(second.find("already carries transition 'order_status'"), std::string::npos)
        << second;
}

}  // namespace
}  // namespace kds::server
