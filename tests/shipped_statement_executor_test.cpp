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
    };

    Answer Ship(const std::string& sql, std::uint64_t session_id, std::uint64_t sequence,
                Role role = Role::kReadWrite, std::uint32_t requester = 0) {
        StatementShipServer::ShippedStatement statement;
        statement.requester = requester;
        statement.session_id = session_id;
        statement.sequence = sequence;
        statement.target_oid = 0;
        statement.role = role;
        statement.text = sql;

        auto answer = std::make_shared<Answer>();
        executor_->Seam()(std::move(statement),
                          [answer](const Status& status, std::string_view text) {
                              answer->answered = true;
                              answer->status = status;
                              answer->text.assign(text);
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
                          [answer](const Status& status, std::string_view text) {
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

// **DISABLED, and it fails rather than passes** (A1 of the post-SS5
// verification order). The order requires that a duplicate whose record the
// memory bound dropped be answered `UnknownOutcome`; this executor runs it
// again, which against an engine-issued pk is a second row. The behaviour is
// the one this header already states - "an early eviction is the one
// condition under which a duplicate could reach an empty record and be
// re-executed" - so what the order asks for is a change to it, and every
// available fix is a policy decision the operator owns (refuse the 4097th
// shipping session rather than evict; carry a retry bit on the request; keep
// a tombstone under a second bound). It is written now, and kept failing
// rather than deleted or weakened, so that whichever fix lands has its
// acceptance test already in the tree.
//
// Not reachable today: nothing re-sends a landed request (`SendRetryTask`
// retries only a send the ring refused, `sched/send_retry.hpp`), so no live
// path produces a duplicate at all. This is the retry paths a routing layer
// will bring, met early.
TEST_F(ShippedStatementExecutorTest, DISABLED_ADuplicateWhoseRecordWasEvictedEarlyIsNotReExecuted) {
    // A1 of the post-SS5 verification order: force the bounded record past
    // its bound, then retry a statement whose entry is gone. The record is
    // the only thing standing between a retry and a second row against an
    // engine-issued pk, so what the owner does when it no longer holds one
    // is the case the whole scheme rests on.
    const std::uint64_t kVictim = 1;
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", kVictim, /*sequence=*/1).status.ok());
    // One record per distinct session, up to and past the cap: the victim's
    // is the oldest, so it is the one the memory bound drops first.
    for (std::uint64_t s = 2; s <= kShippedDedupMaxRecords + 1; ++s) {
        ASSERT_TRUE(Ship("INSERT INTO t VALUES (8)", s, /*sequence=*/1).status.ok());
    }
    ASSERT_GT(executor_->early_evictions(), 0u) << "the cap did not bite; the test proves nothing";

    const std::uint64_t before = executor_->executed();
    const Answer again = Ship("INSERT INTO t VALUES (7)", kVictim, /*sequence=*/1);
    ASSERT_TRUE(again.answered);
    EXPECT_EQ(again.status.code(), StatusCode::kUnknownOutcome) << again.status.message();
    EXPECT_FALSE(IsRetryable(again.status.code()));
    EXPECT_EQ(executor_->executed(), before)
        << "the duplicate ran a second time because its record was gone";
}

}  // namespace
}  // namespace kds::server
