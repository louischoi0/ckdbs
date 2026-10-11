#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/session.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

// The write-path reservation protocol (docs/spec/assertion.md §§4.2, 6.2;
// workplan AST07), tested through the statement surface - which is where the
// acceptance criteria live: a race at bound-1 admits exactly one, an abort
// restores the aggregates exactly, a group-move charges only its
// destination, a decreasing UPDATE is check-free, and DELETE frees what it
// deletes. What is deliberately absent: the S-3 concurrent-history checks,
// gated with the isolation checker that does not exist, exactly as the
// workplan gates them.

namespace kds::server {
namespace {

class AssertionEnforceTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/6000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));

        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        mgr_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);

        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr,
                            /*clock=*/nullptr, /*wal=*/nullptr, wal::DurabilityClass::kRelaxed,
                            exec::Budget(), /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/true, /*cabins=*/nullptr, &*mgr_);

        ASSERT_EQ(Run("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                      .substr(0, 7),
                  "CREATED");
    }

    std::string Run(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Run(Session& s, const std::string& sql) {
        return dispatcher_->Dispatch(sql, &s).response;
    }

    std::size_t RowCount(const std::string& sql) {
        const std::string reply = Run(sql);
        std::size_t rows = 0;
        for (std::size_t at = reply.find("\\n"); at != std::string::npos;
             at = reply.find("\\n", at + 2)) {
            ++rows;
        }
        return rows;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> mgr_;
    std::optional<CommandDispatcher> dispatcher_;
};

TEST_F(AssertionEnforceTest, AnInsertPastTheBoundIsRefusedAndUnwound) {
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 2")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");

    const std::string out = Run("INSERT INTO trades VALUES (7, 1)");
    EXPECT_EQ(out.substr(0, 23), "ERR ASSERTION_VIOLATION") << out;
    EXPECT_NE(out.find("retryable=0"), std::string::npos) << out;
    EXPECT_NE(out.find("assertion \"cap\" group (account=7): COUNT(*) would exceed bound 2"),
              std::string::npos)
        << out;

    // Autocommit is statement-atomic: nothing written, and the refusal's
    // own reservation was never applied (admission is pure), so a different
    // group is untouched.
    EXPECT_EQ(RowCount("SELECT * FROM trades"), 2u);
    EXPECT_EQ(Run("INSERT INTO trades VALUES (8, 1)").substr(0, 8), "INSERTED");
}

TEST_F(AssertionEnforceTest, AReservationCountsAgainstARivalUntilItsFateIsKnown) {
    // §4.3's deliberate stricter-than-snapshot semantics, and the
    // acceptance's race: at bound-1, the reservation admits exactly one.
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .substr(0, 7),
              "CREATED");

    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO trades VALUES (7, 60)").substr(0, 8), "INSERTED");

    // The rival sees 60 reserved: 50 more would be 110.
    const std::string refused = Run("INSERT INTO trades VALUES (7, 50)");
    EXPECT_EQ(refused.substr(0, 23), "ERR ASSERTION_VIOLATION") << refused;

    // The reservation aborts; the aggregate is restored exactly, so the
    // same 50 is now admissible - the "bounded false rejection" §6.2
    // accepts, ending the moment the fate is known.
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 50)").substr(0, 8), "INSERTED");
    // And the ceiling is where it should be: 50 in, 50 more fits...
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 50)").substr(0, 8), "INSERTED");
    // ...and the bound holds.
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 23), "ERR ASSERTION_VIOLATION");
}

TEST_F(AssertionEnforceTest, AViolationInsideAnExplicitTransactionPoisonsIt) {
    // The AS9 resolution (2026-08-09): uniform with every other write
    // failure. The refusal poisons; ROLLBACK unwinds the earlier insert's
    // reservation with its row, and the group is empty again.
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 1")
                  .substr(0, 7),
              "CREATED");

    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run(s, "INSERT INTO trades VALUES (7, 1)").substr(0, 23),
              "ERR ASSERTION_VIOLATION");
    EXPECT_NE(Run(s, "SELECT * FROM trades").find("transaction is aborted"), std::string::npos);
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    // Restored exactly: one row fits again.
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
}

TEST_F(AssertionEnforceTest, ACommittedReservationHoldsTheGroundItTook) {
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 1")
                  .substr(0, 7),
              "CREATED");
    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run(s, "COMMIT").substr(0, 6), "COMMIT");
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 23), "ERR ASSERTION_VIOLATION");
}

TEST_F(AssertionEnforceTest, AGroupMoveChargesOnlyTheDestination) {
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 2")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");  // id 1
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");  // id 2
    ASSERT_EQ(Run("INSERT INTO trades VALUES (8, 1)").substr(0, 8), "INSERTED");  // id 3

    // Into the full group: refused, naming the destination.
    const std::string refused = Run("UPDATE trades SET account = 7 WHERE id = 3");
    EXPECT_EQ(refused.substr(0, 23), "ERR ASSERTION_VIOLATION") << refused;
    EXPECT_NE(refused.find("account=7"), std::string::npos) << refused;

    // Out of the full group: departure is check-free, and it frees a seat -
    // the row that could not move in now can.
    EXPECT_EQ(Run("UPDATE trades SET account = 8 WHERE id = 1").substr(0, 7), "UPDATED");
    EXPECT_EQ(Run("UPDATE trades SET account = 7 WHERE id = 3").substr(0, 7), "UPDATED");
}

TEST_F(AssertionEnforceTest, ADecreasingSumUpdateIsCheckFreeAndStillMaintained) {
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 90)").substr(0, 8), "INSERTED");  // id 1

    // Down: no check can refuse it (§4.2 row 3), and the headroom it frees
    // is real - the maintenance half.
    ASSERT_EQ(Run("UPDATE trades SET qty = 10 WHERE id = 1").substr(0, 7), "UPDATED");
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 90)").substr(0, 8), "INSERTED");  // 10+90=100

    // Up: checked, and the group is exactly full.
    EXPECT_EQ(Run("UPDATE trades SET qty = 11 WHERE id = 1").substr(0, 23),
              "ERR ASSERTION_VIOLATION");
}

TEST_F(AssertionEnforceTest, AnAggregateInvariantUpdateReservesNothing) {
    // §4.2 row 2: group unchanged, SUM column unchanged - for a COUNT
    // assertion, any qty change. If it appended, the count would drift and
    // this second insert would be refused.
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 2")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("UPDATE trades SET qty = 99 WHERE id = 1").substr(0, 7), "UPDATED");
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 23), "ERR ASSERTION_VIOLATION");
}

TEST_F(AssertionEnforceTest, DeleteFreesTheGroundItsRowHeld) {
    // Check-free (AS11) but not maintenance-free: §5's coverage contract is
    // over live rows, so the departure must land or the group overstates
    // forever and this test's second insert would be refused.
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 2")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");  // id 1
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");  // id 2
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 23), "ERR ASSERTION_VIOLATION");

    ASSERT_EQ(Run("DELETE FROM trades WHERE id = 1"), "DELETED 1");
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
}

TEST_F(AssertionEnforceTest, ARolledBackDeleteTakesItsGroundBack) {
    // The departure is a reservation like any other: aborting the DELETE
    // restores the group, so the seat it would have freed is not free.
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 1")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");  // id 1

    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "DELETE FROM trades WHERE id = 1"), "DELETED 1");
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 23), "ERR ASSERTION_VIOLATION");
}

TEST_F(AssertionEnforceTest, TwoAssertionsOnOneRelationBothHold) {
    ASSERT_EQ(Run("CREATE ASSERTION rows_cap ON trades GROUP BY (account) CHECK COUNT(*) <= 3")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("CREATE ASSERTION qty_cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 60)").substr(0, 8), "INSERTED");
    // The SUM refuses first...
    EXPECT_NE(Run("INSERT INTO trades VALUES (7, 60)").find("qty_cap"), std::string::npos);
    // ...and the COUNT refuses on its own axis.
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 10)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 10)").substr(0, 8), "INSERTED");
    EXPECT_NE(Run("INSERT INTO trades VALUES (7, 1)").find("rows_cap"), std::string::npos);
}

TEST_F(AssertionEnforceTest, DropStopsEnforcementAtOnce) {
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 1")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 23), "ERR ASSERTION_VIOLATION");
    ASSERT_EQ(Run("DROP ASSERTION cap").substr(0, 7), "DROPPED");
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
}

TEST_F(AssertionEnforceTest, EndToEndLifecycleLeavesNoResidue) {
    // AST10's scenario: create -> load -> CREATE ASSERTION -> mixed
    // workload with violations -> DROP ASSERTION -> no residue.
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 10)").substr(0, 8), "INSERTED");
    }
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 60")
                  .substr(0, 7),
              "CREATED");

    // Mixed workload: admitted and refused inserts, a group-move, an
    // update down and up, a delete, a poisoned transaction rolled back.
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 20)").substr(0, 8), "INSERTED");  // 60/60
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 23), "ERR ASSERTION_VIOLATION");
    ASSERT_EQ(Run("UPDATE trades SET account = 8 WHERE id = 5").substr(0, 7), "UPDATED");
    ASSERT_EQ(Run("UPDATE trades SET qty = 5 WHERE id = 1").substr(0, 7), "UPDATED");  // 35/60
    ASSERT_EQ(Run("DELETE FROM trades WHERE id = 2"), "DELETED 1");                    // 25/60
    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO trades VALUES (7, 35)").substr(0, 8), "INSERTED");  // 60/60
    ASSERT_EQ(Run(s, "INSERT INTO trades VALUES (7, 1)").substr(0, 23),
              "ERR ASSERTION_VIOLATION");
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");  // back to 25/60
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 35)").substr(0, 8), "INSERTED");

    // Drop: enforcement ends at once, the catalog row is gone, and the
    // name is free - no residue a client can observe.
    ASSERT_EQ(Run("DROP ASSERTION cap").substr(0, 7), "DROPPED");
    EXPECT_NE(Run("SHOW ASSERTIONS").find("assertions=0"), std::string::npos);
    EXPECT_EQ(Run("INSERT INTO trades VALUES (7, 999)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 5000")
                  .substr(0, 7),
              "CREATED");
}

TEST_F(AssertionEnforceTest, CountersAreMonotonicAndPrintedOnlyWhileHeld) {
    // §9's production counters (AST09), under AST07's own scenario: two
    // admitted inserts, one refusal, one aborted transaction.
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 2")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 23), "ERR ASSERTION_VIOLATION");
    Session s;
    ASSERT_EQ(Run(s, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(s, "INSERT INTO trades VALUES (8, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run(s, "ROLLBACK").substr(0, 8), "ROLLBACK");

    // 4 admission checks (3 autocommit inserts + the txn's), 1 violation,
    // 3 reservations applied (the refusal reserved nothing), 1 aborted.
    const std::string shown = Run("SHOW ASSERTIONS");
    EXPECT_NE(shown.find("checks=4"), std::string::npos) << shown;
    EXPECT_NE(shown.find("violations=1"), std::string::npos) << shown;
    EXPECT_NE(shown.find("reserved=3"), std::string::npos) << shown;
    EXPECT_NE(shown.find("aborted=1"), std::string::npos) << shown;

    // A dispatcher whose registry does not hold the assertion prints no
    // counters at all - zeros would claim a count that never ran.
    CommandDispatcher second(boot_->superblock, boot_->catalog, store_, nullptr, nullptr,
                             nullptr, wal::DurabilityClass::kRelaxed, exec::Budget(), nullptr,
                             false, true, nullptr, &*mgr_);
    const std::string cold = second.Dispatch("SHOW ASSERTIONS").response;
    EXPECT_EQ(cold.find("checks="), std::string::npos) << cold;
}

TEST_F(AssertionEnforceTest, AFreshDispatcherOverTheSameStoreReportsAndDoesNotEnforce) {
    // The recovery gap, reported rather than hidden: the catalog row and
    // the entry pages survive, the registry does not - a second dispatcher
    // (a restart, as near as one exists in-process) says enforcing=0 and
    // lets the write through, because a check it cannot run is not one it
    // may claim.
    ASSERT_EQ(Run("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK COUNT(*) <= 1")
                  .substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO trades VALUES (7, 1)").substr(0, 23), "ERR ASSERTION_VIOLATION");

    CommandDispatcher second(boot_->superblock, boot_->catalog, store_, nullptr, nullptr,
                             nullptr, wal::DurabilityClass::kRelaxed, exec::Budget(), nullptr,
                             false, true, nullptr, &*mgr_);
    const std::string shown = second.Dispatch("SHOW ASSERTIONS").response;
    EXPECT_NE(shown.find("enforcing=0"), std::string::npos) << shown;
    EXPECT_EQ(second.Dispatch("INSERT INTO trades VALUES (7, 1)").response.substr(0, 8),
              "INSERTED");
}

TEST_F(AssertionEnforceTest, ACreateOnARelationTheSessionCannotSeeIsRefusedAndBuildsNothing) {
    // A relation another session's open transaction created is not the
    // creating session's to read. `CREATE ASSERTION` resolves its target
    // under settled state to take the relation `X` that fences the writers,
    // and used to take no `X` when the session's lookup failed - then build
    // over the relation resolved unfiltered, which no writer was fenced
    // from: a row its creator wrote between the scan and the adoption was
    // in neither the directory nor any check.
    Session creator;
    ASSERT_EQ(Run(creator, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(creator, "CREATE TABLE ghost (id int64, g int64) BTREE").substr(0, 7),
              "CREATED");

    // The other session cannot see it ...
    ASSERT_EQ(Run("SELECT * FROM ghost").substr(0, 3), "ERR");
    // ... so it cannot put an assertion on it either.
    const std::string out =
        Run("CREATE ASSERTION cap ON ghost GROUP BY (g) CHECK COUNT(*) <= 1");
    EXPECT_EQ(out.substr(0, 3), "ERR") << out;
    EXPECT_NE(Run("SHOW ASSERTIONS").find("assertions=0"), std::string::npos)
        << "the refused create published an assertion: " << Run("SHOW ASSERTIONS");

    // The creator's own table is untouched by the refusal.
    EXPECT_EQ(Run(creator, "INSERT INTO ghost VALUES (1)").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run(creator, "INSERT INTO ghost VALUES (1)").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run(creator, "ROLLBACK").substr(0, 8), "ROLLBACK");
}

// The target resolves under settled state, never the session's view: a
// relation the session's own open transaction created is not there yet, and
// one dropped and re-created since a repeatable-read `BEGIN` is the new one -
// the transaction's view would name the old one, and the re-check after the
// lock would refuse every retry as "changed".
TEST_F(AssertionEnforceTest, ACreateResolvesItsTargetUnderSettledStateInsideATransaction) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, g int64) BTREE").substr(0, 7), "CREATED");
    Session mine(txn::IsolationLevel::kRepeatableRead);
    ASSERT_EQ(Run(mine, "BEGIN").substr(0, 5), "BEGIN");
    ASSERT_EQ(Run(mine, "CREATE TABLE mine (id int64, g int64) BTREE").substr(0, 7),
              "CREATED");
    const std::string own = Run(mine, "CREATE ASSERTION cap ON mine GROUP BY (g) CHECK COUNT(*) <= 1");
    EXPECT_NE(own.find("no relation named 'mine'"), std::string::npos) << own;

    ASSERT_EQ(Run("DROP TABLE t").substr(0, 7), "DROPPED");
    ASSERT_EQ(Run("CREATE TABLE t (id int64, g int64) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO t VALUES (1)").substr(0, 8), "INSERTED");
    const std::string out = Run(mine, "CREATE ASSERTION cap ON t GROUP BY (g) CHECK COUNT(*) <= 1");
    EXPECT_EQ(out.substr(0, 7), "CREATED") << out;
    ASSERT_EQ(Run(mine, "ROLLBACK").substr(0, 8), "ROLLBACK");
    EXPECT_EQ(Run("INSERT INTO t VALUES (1)").substr(0, 23), "ERR ASSERTION_VIOLATION");
}

// A group key built from a literal must be the key the stored row is counted
// under. An INSERT's values arrive as written (a `date` as a string, a
// `decimal` as text) while CREATE ASSERTION's backfill and DELETE/UPDATE key
// from decoded rows; before the fix the three disagreed, so the backfilled
// count never met a later write, and a DELETE was refused outright for
// departing from a group no insert had used.
TEST_F(AssertionEnforceTest, ADateGroupKeyedFromALiteralMeetsTheBackfilledCount) {
    ASSERT_EQ(Run("CREATE TABLE ev (id int64, d date) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON ev GROUP BY (d) CHECK COUNT(*) <= 2").substr(0, 7),
              "CREATED");
    EXPECT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01')").substr(0, 23),
              "ERR ASSERTION_VIOLATION");
    // Another day is another group.
    EXPECT_EQ(Run("INSERT INTO ev VALUES ('2026-01-02')").substr(0, 8), "INSERTED");
}

TEST_F(AssertionEnforceTest, ATimestampGroupKeyedFromALiteralMeetsTheBackfilledCount) {
    ASSERT_EQ(Run("CREATE TABLE ev (id int64, t timestamp) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01 10:00:00')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON ev GROUP BY (t) CHECK COUNT(*) <= 1").substr(0, 7),
              "CREATED");
    EXPECT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01 10:00:00')").substr(0, 23),
              "ERR ASSERTION_VIOLATION");
}

TEST_F(AssertionEnforceTest, ADecimalGroupKeyedFromALiteralMeetsTheBackfilledCount) {
    ASSERT_EQ(Run("CREATE TABLE px (id int64, p decimal(10,2)) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO px VALUES ('1.50')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO px VALUES ('1.50')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON px GROUP BY (p) CHECK COUNT(*) <= 2").substr(0, 7),
              "CREATED");
    EXPECT_EQ(Run("INSERT INTO px VALUES ('1.50')").substr(0, 23), "ERR ASSERTION_VIOLATION");
    EXPECT_EQ(Run("INSERT INTO px VALUES ('2.50')").substr(0, 8), "INSERTED");
}

TEST_F(AssertionEnforceTest, AWideDecimalGroupKeyedFromALiteralMeetsTheBackfilledCount) {
    ASSERT_EQ(Run("CREATE TABLE px (id int64, p decimal(30,2)) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO px VALUES ('12345678901234567890.50')").substr(0, 8),
              "INSERTED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON px GROUP BY (p) CHECK COUNT(*) <= 1").substr(0, 7),
              "CREATED");
    EXPECT_EQ(Run("INSERT INTO px VALUES ('12345678901234567890.50')").substr(0, 23),
              "ERR ASSERTION_VIOLATION");
    EXPECT_EQ(Run("INSERT INTO px VALUES ('1.50')").substr(0, 8), "INSERTED");
}

TEST_F(AssertionEnforceTest, ADeleteAndAnUpdateOnADateGroupAgreeWithTheInsertKey) {
    ASSERT_EQ(Run("CREATE TABLE ev (id int64, d date) BTREE").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("CREATE ASSERTION cap ON ev GROUP BY (d) CHECK COUNT(*) <= 2").substr(0, 7),
              "CREATED");
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01')").substr(0, 8), "INSERTED");  // id 1
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01')").substr(0, 8), "INSERTED");  // id 2
    ASSERT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01')").substr(0, 23),
              "ERR ASSERTION_VIOLATION");

    // The delete gives back under the key the inserts used: one slot frees,
    // and exactly one.
    ASSERT_EQ(Run("DELETE FROM ev WHERE id = 1"), "DELETED 1");
    EXPECT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01')").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01')").substr(0, 23),
              "ERR ASSERTION_VIOLATION");

    // An UPDATE that moves a row out of the full group frees a slot in it;
    // one that moves a row into it is charged there.
    ASSERT_EQ(Run("UPDATE ev SET d = '2026-01-02' WHERE id = 2").substr(0, 7), "UPDATED");
    EXPECT_EQ(Run("INSERT INTO ev VALUES ('2026-01-01')").substr(0, 8), "INSERTED");
    EXPECT_EQ(Run("UPDATE ev SET d = '2026-01-01' WHERE id = 2").substr(0, 23),
              "ERR ASSERTION_VIOLATION");
}

}  // namespace
}  // namespace kds::server
