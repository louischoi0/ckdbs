#include "kds/server/shipped_statement_executor.hpp"

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

// SS3: what the owner does with a statement that arrived from another core.
//
// The seam is driven **directly** here - no ring, no waiter - because what
// this row adds is the execution, and SS1 already pins the transport. What
// is worth pinning is in the order it would hurt:
//
//   1. the answer a shipped statement produces is the answer the same
//      statement produces locally, refusals included and byte for byte:
//      a client must not be able to tell where its statement ran;
//   2. a duplicate is answered from the record and **not run again** - the
//      engine issues primary keys, so a re-execution is a second row;
//   3. where the record cannot answer, the reply is `UnknownOutcome` and
//      never a guess;
//   4. the rank the arrival core authenticated is the rank the statement
//      runs under - a shipped write from a readonly connection is refused
//      on the owner too.

namespace kds::server {
namespace {

class ShippedStatementExecutorTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        // With a real transaction manager, because an owner has one and
        // what a shipped statement is allowed to do to it is part of the
        // contract: a session that dies inside a transaction pins
        // `ReadHorizon()` for the life of the process.
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, /*wal=*/nullptr);
        txns_.emplace(*ids_, *undo_, store_, /*wal=*/nullptr);
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr, &clock_,
                            /*wal=*/nullptr, wal::DurabilityClass::kRelaxed, exec::Budget(),
                            /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*txns_);
        scheduler_.emplace(clock_, io_);
        executor_.emplace(/*core_id=*/1, *dispatcher_, *scheduler_, clock_);

        ASSERT_EQ(Local("CREATE TABLE t (id int64, v int64)").rfind("CREATED", 0), 0u);
    }

    // The same statement run the ordinary way, for the comparisons: a
    // shipped answer is only right if it is *this* answer.
    std::string Local(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }

    // What one arrival core's request looks like at the seam.
    struct Answer {
        bool answered = false;
        Status status;
        std::string text;
        // RR0 / D3: what this participant reported it is reading at.
        std::uint64_t watermark = 0;
    };

    Answer Ship(const std::string& sql, std::uint64_t session_id, std::uint64_t sequence,
                Role role = Role::kReadWrite, std::uint32_t requester = 0,
                bool retry = false, bool in_txn = false, bool join = false,
                std::optional<txn::IsolationLevel> isolation = std::nullopt) {
        StatementShipServer::ShippedStatement statement;
        statement.requester = requester;
        statement.session_id = session_id;
        statement.sequence = sequence;
        statement.target_oid = 0;
        statement.role = role;
        statement.retry = retry;
        statement.in_txn = in_txn;
        statement.join = join;
        statement.isolation = isolation;
        statement.text = sql;

        auto answer = std::make_shared<Answer>();
        executor_->Seam()(std::move(statement),
                          [answer](const Status& status, std::string_view text,
                                   std::uint64_t watermark) {
                              answer->answered = true;
                              answer->status = status;
                              answer->text.assign(text);
                              answer->watermark = watermark;
                          });
        // The seam may answer immediately (a duplicate, a refusal that
        // costs nothing) or many turns later (an execution that parks).
        for (int i = 0; i < 64 && !answer->answered; ++i) scheduler_->RunOnce();
        return *answer;
    }

    std::string Rows() { return Local("SELECT * FROM t"); }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    sched::ManualClock clock_;
    sched::NullIoBackend io_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txns_;
    std::optional<CommandDispatcher> dispatcher_;
    std::optional<sched::Scheduler> scheduler_;
    std::optional<ShippedStatementExecutor> executor_;
};

TEST_F(ShippedStatementExecutorTest, AShippedStatementRunsHereAndAnswersWhatLocalExecutionWould) {
    const Answer out = Ship("INSERT INTO t VALUES (7)", /*session_id=*/99, /*sequence=*/1);
    ASSERT_TRUE(out.answered);
    EXPECT_TRUE(out.status.ok()) << out.status.message();
    EXPECT_EQ(out.text.rfind("INSERTED", 0), 0u) << out.text;
    EXPECT_EQ(executor_->executed(), 1u);
    EXPECT_EQ(executor_->running(), 0u);

    // The row is here, on this core, in this core's relation - which is D3:
    // the statement was not simulated, it ran.
    EXPECT_NE(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(ShippedStatementExecutorTest, ARefusalCrossesAsItsCodeAndRendersBackIdentically) {
    // The whole round trip a client sees: the owner refuses, the code
    // crosses, and the arrival core's `ErrorReply` reproduces the owner's
    // line. If this drifts, a client's retry loop reads a bit the owner
    // did not mean.
    const std::string local = Local("INSERT INTO nosuch VALUES (1)");
    ASSERT_EQ(local.rfind("ERR ", 0), 0u) << local;

    const Answer out = Ship("INSERT INTO nosuch VALUES (1)", 99, 1);
    ASSERT_TRUE(out.answered);
    EXPECT_FALSE(out.status.ok());
    EXPECT_TRUE(out.text.empty()) << "a refusal's message belongs to the status: " << out.text;
    EXPECT_EQ(ErrorReply(out.status), local);
}

TEST_F(ShippedStatementExecutorTest, ADuplicateIsAnsweredFromTheRecordAndNotRunAgain) {
    const Answer first = Ship("INSERT INTO t VALUES (7)", /*session_id=*/99, /*sequence=*/1);
    ASSERT_TRUE(first.status.ok()) << first.status.message();

    const Answer again = Ship("INSERT INTO t VALUES (7)", /*session_id=*/99, /*sequence=*/1);
    ASSERT_TRUE(again.answered);
    EXPECT_TRUE(again.status.ok()) << again.status.message();
    // The recorded answer, not a new one: the same id, the same page, the
    // same slot - which is what makes it recognisable as *not* a second
    // execution.
    EXPECT_EQ(again.text, first.text);
    EXPECT_EQ(executor_->executed(), 1u);
    EXPECT_EQ(executor_->deduped(), 1u);

    // And the relation holds one row, which is the fact this exists for:
    // the pk was engine-issued, so a re-execution would have inserted a
    // second row rather than failed.
    const std::string rows = Rows();
    EXPECT_EQ(rows.find(",7"), rows.rfind(",7")) << rows;
}

TEST_F(ShippedStatementExecutorTest, ADuplicateRefusalIsAlsoAnsweredFromTheRecord) {
    // Recorded on both arms, not just the committed one. A refusal costs
    // nothing to re-run, but answering from the record is what keeps
    // "every duplicate is answered from the record" a rule rather than a
    // tendency - and a refusal that became a success on the retry would be
    // the worst kind of surprise.
    const Answer first = Ship("INSERT INTO nosuch VALUES (1)", 99, 1);
    ASSERT_FALSE(first.status.ok());
    const Answer again = Ship("INSERT INTO nosuch VALUES (1)", 99, 1);
    EXPECT_EQ(again.status.code(), first.status.code());
    EXPECT_EQ(again.status.message(), first.status.message());
    EXPECT_EQ(executor_->deduped(), 1u);
    EXPECT_EQ(executor_->executed(), 1u);
}

TEST_F(ShippedStatementExecutorTest, ASupersededSequenceIsUnknownOutcomeAndNeverAGuess) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", /*session_id=*/99, /*sequence=*/2).status.ok());

    // Sequence 1 arrives after 2 was answered: this core no longer holds
    // what it said about 1, and it may have committed. Neither re-running
    // it (a second row) nor refusing it retryably (a second row, later) is
    // available.
    const Answer late = Ship("INSERT INTO t VALUES (7)", /*session_id=*/99, /*sequence=*/1);
    ASSERT_TRUE(late.answered);
    EXPECT_EQ(late.status.code(), StatusCode::kUnknownOutcome);
    EXPECT_FALSE(IsRetryable(late.status.code()));
    EXPECT_EQ(executor_->unanswerable(), 1u);
    EXPECT_EQ(executor_->executed(), 1u) << "the superseded statement must not have run";
    EXPECT_EQ(ErrorReply(late.status).rfind("ERR UNKNOWN_OUTCOME retryable=0 ", 0), 0u);
}

TEST_F(ShippedStatementExecutorTest, TwoCoresMayMintTheSameSessionIdAndAreNotDuplicates) {
    // The reason the record is keyed on (requester, session): a session id
    // is minted per core, so core 2 and core 3 both hold session 99, and a
    // record keyed on 99 alone would answer core 3's statement with core
    // 2's result.
    const Answer from2 = Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite,
                              /*requester=*/2);
    const Answer from3 = Ship("INSERT INTO t VALUES (8)", 99, 1, Role::kReadWrite,
                              /*requester=*/3);
    ASSERT_TRUE(from2.status.ok()) << from2.status.message();
    ASSERT_TRUE(from3.status.ok()) << from3.status.message();
    EXPECT_EQ(executor_->executed(), 2u);
    EXPECT_EQ(executor_->deduped(), 0u);
    const std::string rows = Rows();
    EXPECT_NE(rows.find(",7"), std::string::npos) << rows;
    EXPECT_NE(rows.find(",8"), std::string::npos) << rows;
}

TEST_F(ShippedStatementExecutorTest, ARecordPastItsRetentionIsGoneAndTheStatementRunsAgain) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1).status.ok());
    EXPECT_EQ(executor_->executed(), 1u);

    // Two deadlines on: nothing can still be parked on the original, so
    // there is nothing left for the record to answer. Pinned because it is
    // the boundary the bound is *derived* from - shortening the retention
    // below the deadline would silently open the double-execute window.
    clock_.Advance(kShippedDedupRetentionNs);
    const Answer again = Ship("INSERT INTO t VALUES (7)", 99, 1);
    EXPECT_TRUE(again.status.ok()) << again.status.message();
    EXPECT_EQ(executor_->executed(), 2u);
    EXPECT_EQ(executor_->deduped(), 0u);
    EXPECT_EQ(executor_->early_evictions(), 0u);
}

TEST_F(ShippedStatementExecutorTest, TheArrivalCoresRankIsTheRankTheStatementRunsUnder) {
    // A `Session` holds kAdmin by default (the auth-off contract), so an
    // owner that minted its own would run every shipped statement as
    // admin. The rank crosses instead, and the owner asks the same
    // question of the same answer.
    const Answer refused = Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadOnly);
    ASSERT_TRUE(refused.answered);
    EXPECT_FALSE(refused.status.ok());
    EXPECT_NE(refused.status.message().find("readonly"), std::string::npos)
        << refused.status.message();
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << "the refused write ran anyway";

    // And the rank that covers it is admitted, on the same statement.
    const Answer admitted = Ship("INSERT INTO t VALUES (7)", 99, 2, Role::kReadWrite);
    EXPECT_TRUE(admitted.status.ok()) << admitted.status.message();
}

TEST_F(ShippedStatementExecutorTest, AReadShipsAndAnswersWithItsRows) {
    // D1's read half at the seam: the same mechanism, no second path.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1).status.ok());
    const Answer out = Ship("SELECT * FROM t", 99, 2, Role::kReadOnly);
    ASSERT_TRUE(out.status.ok()) << out.status.message();
    EXPECT_EQ(out.text, Local("SELECT * FROM t"));
}

TEST_F(ShippedStatementExecutorTest, ShowMetaOmitsTheShippingBlockWhereNothingIsWired) {
    // "Absent rather than zeroed" is the rule the recovery and scheduler
    // blocks already follow, and it is the honest reading of a single-core
    // instance: shipping is not armed here, so there is nothing to report -
    // not a row of zeros that looks like an armed core doing nothing.
    // This dispatcher has neither half installed.
    const std::string meta = Local("SHOW META");
    EXPECT_EQ(meta.find("shipped_"), std::string::npos) << meta;
}

TEST_F(ShippedStatementExecutorTest, ARefusedShippedStatementAllocatesNothing) {
    // D5, generalised from G2's fix: the refusal paths a shipped statement
    // can reach take no page. Driven as the storm it is meant to survive -
    // a conforming client retrying a statement its rank forbids, one whose
    // relation does not exist, and one the parser rejects - with the
    // store's page count as the whole verdict.
    const std::size_t before = store_.page_count();
    for (std::uint64_t i = 0; i < 200; ++i) {
        EXPECT_FALSE(Ship("INSERT INTO t VALUES (7)", 1, i, Role::kReadOnly).status.ok());
        EXPECT_FALSE(Ship("INSERT INTO nosuch VALUES (1)", 2, i).status.ok());
        EXPECT_FALSE(Ship("INSERT INTO t VALUES (", 3, i).status.ok());
    }
    EXPECT_EQ(store_.page_count(), before) << "a refusal took a page";
    EXPECT_EQ(executor_->early_evictions(), 0u);
}

TEST_F(ShippedStatementExecutorTest, ADuplicateThatMeetsItsOriginalStillRunningIsNotRunAgain) {
    // **The half of D4's window the record alone does not cover.** An
    // arrival core's deadline fires *because* the owner is slow, so the
    // retry it provokes arrives while the original is still executing here
    // and before anything has been recorded. Answering it by running the
    // statement is the double insert the whole design exists to prevent.
    auto ship = [&](std::uint64_t sequence, const std::shared_ptr<Answer>& answer) {
        StatementShipServer::ShippedStatement statement;
        statement.requester = 0;
        statement.session_id = 99;
        statement.sequence = sequence;
        statement.role = Role::kReadWrite;
        statement.text = "INSERT INTO t VALUES (7)";
        executor_->Seam()(std::move(statement),
                          [answer](const Status& status, std::string_view text, std::uint64_t) {
                              answer->answered = true;
                              answer->status = status;
                              answer->text.assign(text);
                          });
    };

    auto original = std::make_shared<Answer>();
    auto retry = std::make_shared<Answer>();
    ship(/*sequence=*/1, original);
    ship(/*sequence=*/1, retry);  // no pump in between: the original is still running

    // The retry is answered at once, and answered with the one true thing
    // this core can say - not with a second execution and not with a guess.
    ASSERT_TRUE(retry->answered);
    EXPECT_EQ(retry->status.code(), StatusCode::kUnknownOutcome);
    EXPECT_FALSE(IsRetryable(retry->status.code()));

    for (int i = 0; i < 64 && !original->answered; ++i) scheduler_->RunOnce();
    EXPECT_TRUE(original->status.ok()) << original->status.message();
    EXPECT_EQ(executor_->executed(), 1u);

    // One row. Before this rule there were two.
    const std::string rows = Rows();
    EXPECT_EQ(rows.find(",7"), rows.rfind(",7")) << rows;
}

TEST_F(ShippedStatementExecutorTest, ARecordedSessionHoldsOneOrderEntryHoweverManyItShips) {
    // `kShippedDedupMaxRecords` is stated as the memory bound. It bounds
    // records, so the order list has to hold one node per *key* and move it
    // - one per statement would make the real bound the shipping rate times
    // the retention, which is a bound on nothing a cap can state.
    for (std::uint64_t sequence = 1; sequence <= 3000; ++sequence) {
        ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", /*session_id=*/99, sequence).status.ok());
    }
    EXPECT_EQ(executor_->records(), 1u);
    EXPECT_EQ(executor_->early_evictions(), 0u);
}

TEST_F(ShippedStatementExecutorTest, AShippedStatementMayNotLeaveATransactionOpen) {
    // D1 scopes shipping to autocommit, and the reason it has to be
    // enforced *here* and not only at the fork: this session is destroyed
    // with the statement, so a transaction it adopted would stay `active_`
    // forever - pinning `ReadHorizon()`, stalling the undo purge, and
    // answering `IsInFlight` true for the life of the process. A dropped
    // connection is rolled back (docs/spec/txn.md section 10-8); so is this.
    const Answer out = Ship("BEGIN", /*session_id=*/99, /*sequence=*/1);
    ASSERT_TRUE(out.answered);
    EXPECT_EQ(out.status.code(), StatusCode::kUnsupported) << out.status.message();
    EXPECT_EQ(txns_->ActiveCount(), 0u) << "a shipped BEGIN left a transaction running";
}

// R6-0 (Finding 1, `instructions/v2.4.0/2pc.md` §2): A1 of the post-SS5
// verification order. Force the bounded record past its bound, then send a
// statement whose entry is gone **marked as a retry**. The record is the
// only thing standing between a retry and a second row against an
// engine-issued pk, so what the owner does when it no longer holds one is
// the case the whole scheme rests on: it must not guess, and it must not
// execute.
TEST_F(ShippedStatementExecutorTest, ADuplicateWhoseRecordWasEvictedEarlyIsNotReExecutedOnRetry) {
    const std::uint64_t kVictim = 1;
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", kVictim, /*sequence=*/1).status.ok());
    // One record per distinct session, up to and past the cap: the victim's
    // is the oldest, so it is the one the memory bound drops first.
    for (std::uint64_t s = 2; s <= kShippedDedupMaxRecords + 1; ++s) {
        ASSERT_TRUE(Ship("INSERT INTO t VALUES (8)", s, /*sequence=*/1).status.ok());
    }
    ASSERT_GT(executor_->early_evictions(), 0u) << "the cap did not bite; the test proves nothing";

    const std::uint64_t before = executor_->executed();
    const Answer again = Ship("INSERT INTO t VALUES (7)", kVictim, /*sequence=*/1,
                              Role::kReadWrite, /*requester=*/0, /*retry=*/true);
    ASSERT_TRUE(again.answered);
    EXPECT_EQ(again.status.code(), StatusCode::kUnknownOutcome) << again.status.message();
    EXPECT_FALSE(IsRetryable(again.status.code()));
    EXPECT_EQ(executor_->executed(), before)
        << "the duplicate ran a second time because its record was gone";
}

TEST_F(ShippedStatementExecutorTest, ARetryThatMeetsAPresentRecordIsStillAnsweredFromIt) {
    // The bit only changes what an *absent* record means. Where the record
    // is still there - the ordinary case, since nothing evicts most of the
    // time - a retry is answered exactly like any other duplicate: from the
    // record, not refused for carrying the bit.
    const Answer first = Ship("INSERT INTO t VALUES (7)", /*session_id=*/99, /*sequence=*/1);
    ASSERT_TRUE(first.status.ok()) << first.status.message();

    const Answer retried = Ship("INSERT INTO t VALUES (7)", /*session_id=*/99, /*sequence=*/1,
                                Role::kReadWrite, /*requester=*/0, /*retry=*/true);
    ASSERT_TRUE(retried.answered);
    EXPECT_TRUE(retried.status.ok()) << retried.status.message();
    EXPECT_EQ(retried.text, first.text);
    EXPECT_EQ(executor_->executed(), 1u);
    EXPECT_EQ(executor_->deduped(), 1u);
}

// ---- R6-2: the participant's transaction context ---------------------------
//
// The row's whole content is that an *enrolled* statement runs under a
// transaction the owner holds across statements, while an autocommit one is
// untouched. What is pinned, in the order it would hurt:
//
//   1. two enrolled statements share one transaction, and it is still open
//      between them - the property everything else in R6 rests on;
//   2. the autocommit path is **byte-for-byte what SS3 built**, refusal
//      included, because R6-2 must not be a tax on the common case;
//   3. nothing leaks: an abandoned context is rolled back by the ceiling,
//      and a live one is not.

TEST_F(ShippedStatementExecutorTest, TwoEnrolledStatementsShareOneOpenTransaction) {
    const Answer first = Ship("INSERT INTO t VALUES (7)", /*session_id=*/99, /*sequence=*/1,
                              Role::kReadWrite, /*requester=*/0, /*retry=*/false,
                              /*in_txn=*/true);
    ASSERT_TRUE(first.status.ok()) << first.status.message();
    EXPECT_EQ(executor_->enrolled(), 1u);
    EXPECT_EQ(executor_->enrolments(), 1u);
    // Still open: the statement finished and the transaction did not.
    EXPECT_EQ(txns_->ActiveCount(), 1u) << "the enrolled transaction was closed under R6-2";

    const Answer second = Ship("INSERT INTO t VALUES (8)", 99, 2, Role::kReadWrite, 0, false,
                               /*in_txn=*/true);
    ASSERT_TRUE(second.status.ok()) << second.status.message();
    // **One** transaction for both, not two: a second `enrolments()` would
    // mean the second statement opened its own, which is the failure that
    // makes a cross-owner transaction non-atomic on this core.
    EXPECT_EQ(executor_->enrolments(), 1u);
    EXPECT_EQ(executor_->enrolled(), 1u);
    EXPECT_EQ(txns_->ActiveCount(), 1u);
    EXPECT_EQ(executor_->executed(), 2u);
}

TEST_F(ShippedStatementExecutorTest, AnEnrolledWriteIsInvisibleUntilItsTransactionEnds) {
    // Isolation, from outside the transaction: the row a held transaction
    // wrote must not be readable by an ordinary local statement while that
    // transaction is still open. This is the property that makes prepare
    // (R6-3) meaningful - if the write were already visible there would be
    // nothing for a decision to decide.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 0, false,
                     /*in_txn=*/true)
                    .status.ok());
    EXPECT_EQ(Rows().find(",7"), std::string::npos)
        << "an uncommitted cross-owner write was visible: " << Rows();

    // And it appears once the transaction ends - which R6-2 has no decide
    // leg for, so the ceiling's rollback is used here only to show the row
    // was genuinely uncommitted rather than never written.
    clock_.Advance(kShippedTxnIdleCeilingNs);
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << "a rolled-back write survived: " << Rows();
}

TEST_F(ShippedStatementExecutorTest, AnAutocommitStatementIsUntouchedByR62) {
    // The no-regression half, and the reason the wire carries a bit rather
    // than the owner inferring anything: with `in_txn` unset the path is
    // exactly SS3's - the statement commits on its own and leaves no
    // context behind.
    const Answer out = Ship("INSERT INTO t VALUES (7)", 99, 1);
    ASSERT_TRUE(out.status.ok()) << out.status.message();
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(executor_->enrolments(), 0u);
    EXPECT_EQ(txns_->ActiveCount(), 0u) << "an autocommit shipped statement left a transaction";
    EXPECT_NE(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(ShippedStatementExecutorTest, AnAutocommitBeginIsStillRefusedExactlyAsBefore) {
    // SS3's refusal, unmoved by R6-2. A `BEGIN` that arrives *without* the
    // enrolment bit is still the shape that would leave a transaction
    // `active_` on a session nothing can reach again.
    const Answer out = Ship("BEGIN", /*session_id=*/99, /*sequence=*/1);
    ASSERT_TRUE(out.answered);
    EXPECT_EQ(out.status.code(), StatusCode::kUnsupported) << out.status.message();
    EXPECT_EQ(txns_->ActiveCount(), 0u) << "a shipped BEGIN left a transaction running";
    EXPECT_EQ(executor_->enrolled(), 0u);
}

TEST_F(ShippedStatementExecutorTest, AnEnrolledStatementThatEndsItsTransactionIsRefused) {
    // The decision belongs to the coordinator (D4). A shipped `COMMIT` would
    // take it away, and leaving the context standing afterwards would let
    // the next statement run outside any transaction while the coordinator
    // still believed one was open.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 0, false,
                     /*in_txn=*/true)
                    .status.ok());
    ASSERT_EQ(executor_->enrolled(), 1u);

    const Answer committed = Ship("COMMIT", 99, 2, Role::kReadWrite, 0, false, /*in_txn=*/true);
    ASSERT_TRUE(committed.answered);
    EXPECT_EQ(committed.status.code(), StatusCode::kUnsupported) << committed.status.message();
    EXPECT_NE(committed.status.message().find("belongs to the coordinator"), std::string::npos)
        << committed.status.message();
    // The context is gone rather than left half-alive.
    EXPECT_EQ(executor_->enrolled(), 0u);
}

TEST_F(ShippedStatementExecutorTest, AnAbandonedTransactionIsRolledBackAtTheIdleCeiling) {
    // The backstop for a coordinator that never decides. Nothing on a
    // healthy path reaches it, which is why the counter is worth having.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 0, false,
                     /*in_txn=*/true)
                    .status.ok());
    ASSERT_EQ(txns_->ActiveCount(), 1u);

    // Not yet: one nanosecond under the ceiling is still a live transaction,
    // and tearing it down early reaches the client as an abort it did not
    // ask for.
    clock_.Advance(kShippedTxnIdleCeilingNs - 1);
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->enrolled(), 1u) << "a transaction was expired before the ceiling";
    EXPECT_EQ(executor_->enrolment_expiries(), 0u);

    clock_.Advance(1);
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(executor_->enrolment_expiries(), 1u);
    EXPECT_EQ(txns_->ActiveCount(), 0u) << "the ceiling did not unwind the transaction";
}

TEST_F(ShippedStatementExecutorTest, AStatementThatMayOnlyJoinIsRefusedOnceTheCeilingTookIt) {
    // **RR0, and the failure the bit exists to stop.** The ceiling rolled
    // the context back while the coordinator's transaction was still open;
    // the next statement of that same transaction arrives with `join` set,
    // finds nothing to join, and is refused **retryably** - the transaction
    // has nothing left on this core and can be run again from the top.
    //
    // Without the bit the statement below opened a second transaction, and
    // a later prepare/commit made *that* one durable while the first was
    // already gone: a transaction committed in part, with the coordinator
    // told it committed whole.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 0, false,
                     /*in_txn=*/true)
                    .status.ok());
    clock_.Advance(kShippedTxnIdleCeilingNs);
    executor_->ExpireEnrolled();
    ASSERT_EQ(executor_->enrolled(), 0u);
    ASSERT_EQ(executor_->enrolments(), 1u);

    const Answer refused = Ship("INSERT INTO t VALUES (8)", 99, 2, Role::kReadWrite, 0, false,
                                /*in_txn=*/true, /*join=*/true);
    EXPECT_FALSE(refused.status.ok()) << refused.text;
    EXPECT_EQ(refused.status.code(), StatusCode::kTxnConflict);
    EXPECT_TRUE(refused.status.retryable());
    EXPECT_EQ(executor_->join_refusals(), 1u);
    // Nothing was opened and nothing ran: `enrolments()` is still the one
    // context the ceiling took.
    EXPECT_EQ(executor_->enrolments(), 1u);
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(txns_->ActiveCount(), 0u);
}

TEST_F(ShippedStatementExecutorTest, AWatermarkIsReportedForRepeatableReadAndForNothingElse) {
    // **RR0 / D3's ratified `[OPEN]`, at the participant.** The watermark
    // is the `up_to_trx_id` this core's enrolled transaction pinned, and it
    // is reported for REPEATABLE READ only - an RC transaction re-mints its
    // view at every statement boundary, so a watermark for it would name a
    // view already gone.
    const Answer rr = Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 0, false,
                           /*in_txn=*/true, /*join=*/false,
                           txn::IsolationLevel::kRepeatableRead);
    ASSERT_TRUE(rr.status.ok()) << rr.status.message();
    EXPECT_NE(rr.watermark, 0u);
    // The same context, one statement later: pinned at BEGIN and not moved.
    const Answer again = Ship("INSERT INTO t VALUES (8)", 99, 2, Role::kReadWrite, 0, false,
                              true, /*join=*/true, txn::IsolationLevel::kRepeatableRead);
    ASSERT_TRUE(again.status.ok()) << again.status.message();
    EXPECT_EQ(again.watermark, rr.watermark);

    const Answer rc = Ship("INSERT INTO t VALUES (9)", 98, 1, Role::kReadWrite, 0, false,
                           /*in_txn=*/true, /*join=*/false,
                           txn::IsolationLevel::kReadCommitted);
    ASSERT_TRUE(rc.status.ok()) << rc.status.message();
    EXPECT_EQ(rc.watermark, 0u) << "READ COMMITTED reported a watermark it is ratified to skip";

    // And an autocommit statement, which is a whole transaction and states
    // no level at all.
    const Answer autocommit = Ship("INSERT INTO t VALUES (10)", 97, 1);
    ASSERT_TRUE(autocommit.status.ok()) << autocommit.status.message();
    EXPECT_EQ(autocommit.watermark, 0u);
}

TEST_F(ShippedStatementExecutorTest, AStatementKeepsItsTransactionAliveAcrossTheCeiling) {
    // Idleness, not age: a transaction still receiving statements is not the
    // thing the sweep looks for, however old it is.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 0, false, true)
                    .status.ok());
    for (std::uint64_t sequence = 2; sequence <= 4; ++sequence) {
        clock_.Advance(kShippedTxnIdleCeilingNs - 1);
        executor_->ExpireEnrolled();
        ASSERT_EQ(executor_->enrolled(), 1u) << "expired at sequence " << sequence;
        ASSERT_TRUE(Ship("INSERT INTO t VALUES (8)", 99, sequence, Role::kReadWrite, 0, false,
                         true)
                        .status.ok());
    }
    EXPECT_EQ(executor_->enrolment_expiries(), 0u);
    EXPECT_EQ(executor_->enrolments(), 1u);
}

TEST_F(ShippedStatementExecutorTest, TwoCoordinatorsHoldTwoSeparateTransactions) {
    // The dedup key's argument, one level up: a session id is minted per
    // core, so core 2's session 99 and core 3's session 99 are different
    // transactions and must not share one.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, /*requester=*/2, false,
                     /*in_txn=*/true)
                    .status.ok());
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (8)", 99, 1, Role::kReadWrite, /*requester=*/3, false,
                     /*in_txn=*/true)
                    .status.ok());
    EXPECT_EQ(executor_->enrolled(), 2u);
    EXPECT_EQ(executor_->enrolments(), 2u);
    EXPECT_EQ(txns_->ActiveCount(), 2u);
}

TEST_F(ShippedStatementExecutorTest, RollingBackAllEnrolledLeavesNoTransactionBehind) {
    // The teardown `~CoreRuntime` runs. A transaction that outlived its
    // executor would pin `ReadHorizon()` for the life of the process, which
    // is the hazard the autocommit refusal exists for - R6-2 must not
    // reintroduce it by another door.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 2, false, true)
                    .status.ok());
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (8)", 99, 1, Role::kReadWrite, 3, false, true)
                    .status.ok());
    ASSERT_EQ(txns_->ActiveCount(), 2u);

    executor_->RollbackAllEnrolled();
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(txns_->ActiveCount(), 0u);
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(ShippedStatementExecutorTest, TheCeilingSkipsAContextAStatementIsRunningOn) {
    // **The load-bearing line of R6-2**, and it survived the first round of
    // mutation testing untested: `ExpireEnrolled`'s `running_` guard. A
    // parked statement holds a raw `Session*` into the `Enrolled` the sweep
    // would destroy, so tearing one out from under a live statement is a
    // use-after-free - and it is also why `Finish` can trust that the
    // context it finds is the one its statement ran on.
    //
    // Driven without pumping, so the statement is submitted and its
    // coroutine has not run: `running_` is populated, which is exactly the
    // state the guard is for.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 0, false,
                     /*in_txn=*/true)
                    .status.ok());
    ASSERT_EQ(executor_->enrolled(), 1u);

    auto answer = std::make_shared<Answer>();
    StatementShipServer::ShippedStatement second;
    second.requester = 0;
    second.session_id = 99;
    second.sequence = 2;
    second.role = Role::kReadWrite;
    second.in_txn = true;
    second.text = "INSERT INTO t VALUES (8)";
    executor_->Seam()(std::move(second),
                      [answer](const Status& status, std::string_view text, std::uint64_t) {
                          answer->answered = true;
                          answer->status = status;
                          answer->text.assign(text);
                      });
    ASSERT_EQ(executor_->running(), 1u) << "the statement did not stay in flight";

    clock_.Advance(kShippedTxnIdleCeilingNs);
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->enrolled(), 1u)
        << "the sweep tore a context out from under a running statement";
    EXPECT_EQ(executor_->enrolment_expiries(), 0u);

    // And the statement it protected still completes on that context.
    for (int i = 0; i < 64 && !answer->answered; ++i) scheduler_->RunOnce();
    ASSERT_TRUE(answer->answered);
    EXPECT_TRUE(answer->status.ok()) << answer->status.message();
    EXPECT_EQ(executor_->enrolments(), 1u);
}

TEST_F(ShippedStatementExecutorTest, AParticipantRefusesPastItsEnrolmentLimitRetryably) {
    // The cap exists so a coordinator storm cannot take the whole core's
    // 64-slot live-transaction table and refuse *local* clients' `BEGIN`
    // with nothing naming the cause. Retryable, because another cross-owner
    // transaction ending is a thing that happens on its own.
    for (std::uint64_t coordinator = 1; coordinator <= kShippedMaxEnrolled; ++coordinator) {
        ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite,
                         static_cast<std::uint32_t>(coordinator), false, /*in_txn=*/true)
                        .status.ok())
            << "coordinator " << coordinator;
    }
    ASSERT_EQ(executor_->enrolled(), kShippedMaxEnrolled);

    const Answer refused =
        Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite,
             static_cast<std::uint32_t>(kShippedMaxEnrolled + 1), false, /*in_txn=*/true);
    ASSERT_TRUE(refused.answered);
    EXPECT_EQ(refused.status.code(), StatusCode::kTxnConflict) << refused.status.message();
    EXPECT_TRUE(IsRetryable(refused.status.code()));
    EXPECT_EQ(executor_->enrolment_refusals(), 1u);
    // Counted apart from the duplicate population, which means something
    // else entirely.
    EXPECT_EQ(executor_->unanswerable(), 0u);
    EXPECT_EQ(executor_->enrolled(), kShippedMaxEnrolled) << "the refusal opened one anyway";
}

TEST_F(ShippedStatementExecutorTest, AnEnrolledStatementStillDedupesOnItsSequence) {
    // R6-0's record is orthogonal to R6-2's context and must stay so: a
    // duplicate inside a transaction is answered from the record, not run a
    // second time into the same transaction.
    const Answer first = Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 0, false,
                              /*in_txn=*/true);
    ASSERT_TRUE(first.status.ok()) << first.status.message();

    const Answer again = Ship("INSERT INTO t VALUES (7)", 99, 1, Role::kReadWrite, 0, false,
                              /*in_txn=*/true);
    ASSERT_TRUE(again.status.ok()) << again.status.message();
    EXPECT_EQ(again.text, first.text);
    EXPECT_EQ(executor_->deduped(), 1u);
    EXPECT_EQ(executor_->executed(), 1u);
    EXPECT_EQ(executor_->enrolments(), 1u);
}

}  // namespace
}  // namespace kds::server
