#include "kds/server/txn_2pc_service.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/sched/clock.hpp"
#include "kds/sched/io_backend.hpp"
#include "kds/sched/scheduler.hpp"
#include "kds/sched/send_retry.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/server/shipped_statement_executor.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"
#include "kds/wal/stream.hpp"

// R6-3: prepare and decide (`instructions/v2.4.0/2pc.md` D4).
//
// Two fixtures, because the row has two halves that fail differently.
//
// **The participant** is driven through its seams, with a real WAL under it,
// because what R6-3 adds on that side is a *durability claim*: "prepared"
// means the record is on the platter and this core may no longer abort. The
// things worth pinning, in the order they would hurt:
//
//   1. a prepared context is not rolled back - not by the idle sweep, not by
//      the shutdown path. Either would append TXN_ABORT for a transaction
//      the coordinator may have committed, which is the one durable
//      disagreement two-phase commit exists to prevent;
//   2. the record names the coordinator's `(core, session, transaction)` and
//      carries the *participant's own* transaction id in its envelope - no
//      foreign id enters this stream (D2);
//   3. a participant that cannot prepare refuses, and the refusal is
//      retryable, because a refused prepare aborts the whole transaction and
//      nothing committed anywhere.
//
// **The coordinator** is driven over a real ring with three cores, because
// what it adds is a waiter over N participants: the phase settles when every
// one has answered or the deadline passes, and the two are not the same
// answer.

namespace kds::server {
namespace {

// ---- The participant --------------------------------------------------------

class Txn2pcParticipantTest : public ::testing::Test {
protected:
    static constexpr std::size_t kSegmentSize = 1 << 20;
    static constexpr std::uint32_t kCoordinator = 0;
    static constexpr std::uint64_t kSession = 99;
    static constexpr std::uint64_t kCoordinatorTxn = 4242;

    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(device.ok()) << device.status().message();
        log_device_ = std::move(device.value());
        wal::WalManagerConfig config;
        config.ring_capacity = wal::kMinRingCapacity;
        auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/1, config);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());

        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, wal_.get());
        txns_.emplace(*ids_, *undo_, store_, wal_.get());
        // `group`, because that is what a server runs and what makes the
        // decide leg's commit take the group committer rather than an
        // inline sync.
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr, &clock_,
                            wal_.get(), wal::DurabilityClass::kGroup, exec::Budget(),
                            /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*txns_);
        scheduler_.emplace(clock_, io_);
        executor_.emplace(/*core_id=*/1, *dispatcher_, *scheduler_, clock_, /*log=*/nullptr,
                          wal_.get());

        ASSERT_EQ(Local("CREATE TABLE t (id int64, v int64)").rfind("CREATED", 0), 0u);
    }

    std::string Local(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Rows() { return Local("SELECT * FROM t"); }

    struct Answer {
        bool answered = false;
        Status status;
        std::string text;
    };

    // The reactor's turn, plus the drain a reactor's post-task hook runs:
    // a prepare parks on `IsDurable`, and nothing makes a record durable
    // without one (`WalManager::DrainOnce`).
    void Pump(const std::shared_ptr<Answer>& answer, int turns = 64) {
        for (int i = 0; i < turns && !answer->answered; ++i) {
            (void)wal_->DrainOnce();
            scheduler_->RunOnce();
        }
    }

    Answer Ship(const std::string& sql, std::uint64_t sequence, bool in_txn = true) {
        StatementShipServer::ShippedStatement statement;
        statement.requester = kCoordinator;
        statement.session_id = kSession;
        statement.sequence = sequence;
        statement.role = Role::kReadWrite;
        statement.in_txn = in_txn;
        statement.text = sql;

        auto answer = std::make_shared<Answer>();
        executor_->Seam()(std::move(statement),
                          [answer](const Status& status, std::string_view text) {
                              answer->answered = true;
                              answer->status = status;
                              answer->text.assign(text);
                          });
        Pump(answer);
        return *answer;
    }

    Answer Prepare(std::uint64_t transaction_id = kCoordinatorTxn,
                   std::uint64_t session_id = kSession) {
        Txn2pcServer::PrepareAsk ask;
        ask.coordinator = kCoordinator;
        ask.session_id = session_id;
        ask.transaction_id = transaction_id;
        auto answer = std::make_shared<Answer>();
        executor_->PrepareSeam()(ask, [answer](const Status& status) {
            answer->answered = true;
            answer->status = status;
        });
        Pump(answer);
        return *answer;
    }

    Answer Decide(TxnDecision decision, std::uint64_t transaction_id = kCoordinatorTxn,
                  std::uint64_t session_id = kSession, bool retry = false) {
        Txn2pcServer::DecideAsk ask;
        ask.coordinator = kCoordinator;
        ask.session_id = session_id;
        ask.transaction_id = transaction_id;
        ask.decision = decision;
        ask.retry = retry;
        auto answer = std::make_shared<Answer>();
        executor_->DecideSeam()(ask, [answer](const Status& status) {
            answer->answered = true;
            answer->status = status;
        });
        Pump(answer);
        return *answer;
    }

    // Every record the *device* holds, read back through the device rather
    // than asked of the manager - `insert_wal_test.cpp`'s rule: what the
    // manager believes it appended is not evidence of what a crash leaves.
    std::vector<wal::DecodedRecord> DeviceRecords(
        std::vector<std::vector<std::byte>>& storage) {
        EXPECT_TRUE(wal_->Flush().ok());
        std::vector<wal::DecodedRecord> found;
        for (std::uint64_t seg = 0; seg < log_device_->segment_count(); ++seg) {
            storage.emplace_back(kSegmentSize - wal::kSegmentHeaderSize);
            std::vector<std::byte>& body = storage.back();
            EXPECT_TRUE(log_device_->ReadAt(seg, wal::kSegmentHeaderSize, body).ok());
            wal::RecordReader reader(body, seg * kSegmentSize + wal::kSegmentHeaderSize);
            while (std::optional<wal::DecodedRecord> record = reader.Next()) {
                if (record->type() == wal::RecordType::kPad) break;
                found.push_back(*record);
            }
        }
        return found;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    sched::ManualClock clock_;
    sched::NullIoBackend io_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txns_;
    std::optional<CommandDispatcher> dispatcher_;
    std::optional<sched::Scheduler> scheduler_;
    std::optional<ShippedStatementExecutor> executor_;
};

TEST_F(Txn2pcParticipantTest, PrepareLogsTheCoordinatorsIdentityUnderThisCoresOwnTransaction) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_EQ(executor_->enrolled(), 1u);

    const Answer prepared = Prepare();
    ASSERT_TRUE(prepared.answered);
    EXPECT_TRUE(prepared.status.ok()) << prepared.status.message();
    EXPECT_EQ(executor_->prepared(), 1u);
    EXPECT_EQ(executor_->in_doubt(), 1u);

    // The record, read back off the device. D2's pairing is the thing being
    // pinned: the coordinator's ids in the payload, and an envelope txn_id
    // that is **this** core's own - a foreign id in this stream is what
    // `CoreRuntime::Open`'s mount check refuses.
    std::vector<std::vector<std::byte>> storage;
    std::size_t prepares = 0;
    for (const wal::DecodedRecord& record : DeviceRecords(storage)) {
        if (record.type() != wal::RecordType::kTxnPrepare) continue;
        ++prepares;
        auto fields = wal::DecodeTxnPrepare(record.payload);
        ASSERT_TRUE(fields.ok()) << fields.status().message();
        EXPECT_EQ(fields.value().coordinator_core, kCoordinator);
        EXPECT_EQ(fields.value().coordinator_session_id, kSession);
        EXPECT_EQ(fields.value().coordinator_txn_id, kCoordinatorTxn);
        EXPECT_NE(record.header.txn_id, kCoordinatorTxn)
            << "the envelope must carry this core's own id, not the coordinator's";
        EXPECT_NE(record.header.txn_id, 0u);
        EXPECT_EQ(record.header.page_id, kInvalidPageId);
    }
    EXPECT_EQ(prepares, 1u);
}

TEST_F(Txn2pcParticipantTest, APreparedTransactionIsNotExpiredByTheIdleSweep) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    // Well past the ceiling that would end an un-prepared one.
    clock_.Advance(kShippedTxnIdleCeilingNs * 4);
    executor_->ExpireEnrolled();

    EXPECT_EQ(executor_->enrolled(), 1u) << "a prepared participant may not abort unilaterally";
    EXPECT_EQ(executor_->enrolment_expiries(), 0u);
    EXPECT_EQ(executor_->in_doubt(), 1u);
}

TEST_F(Txn2pcParticipantTest, APreparedTransactionIsLeftInDoubtAtShutdownRatherThanRolledBack) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    executor_->RollbackAllEnrolled();

    EXPECT_EQ(executor_->enrolled(), 1u);
    EXPECT_EQ(executor_->left_in_doubt_at_stop(), 1u);
    // And no TXN_ABORT was written for it, which is the durable half of the
    // same statement: an abort record here would contradict a coordinator
    // that committed.
    std::vector<std::vector<std::byte>> storage;
    for (const wal::DecodedRecord& record : DeviceRecords(storage)) {
        EXPECT_NE(record.type(), wal::RecordType::kTxnAbort);
    }
}

TEST_F(Txn2pcParticipantTest, AnUnpreparedTransactionIsStillTheSweepsAndTheShutdownPaths) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    clock_.Advance(kShippedTxnIdleCeilingNs * 2);
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(executor_->enrolment_expiries(), 1u);
    EXPECT_EQ(executor_->left_in_doubt_at_stop(), 0u);
}

TEST_F(Txn2pcParticipantTest, CommitAppliesTheTransactionAndReleasesTheContext) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());
    // Invisible until the decision: the transaction is open, so nothing
    // outside it sees the row.
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();

    const Answer acked = Decide(TxnDecision::kCommit);
    ASSERT_TRUE(acked.answered);
    EXPECT_TRUE(acked.status.ok()) << acked.status.message();
    EXPECT_EQ(executor_->decides_committed(), 1u);
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(executor_->in_doubt(), 0u);
    EXPECT_NE(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcParticipantTest, AbortUndoesItAndReleasesTheContext) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    const Answer acked = Decide(TxnDecision::kAbort);
    ASSERT_TRUE(acked.answered);
    EXPECT_TRUE(acked.status.ok()) << acked.status.message();
    EXPECT_EQ(executor_->decides_aborted(), 1u);
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(executor_->in_doubt(), 0u);
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcParticipantTest, APrepareForATransactionThisCoreDoesNotHoldIsRefusedRetryably) {
    const Answer refused = Prepare();
    ASSERT_TRUE(refused.answered);
    EXPECT_FALSE(refused.status.ok());
    // Retryable, and that is the point: a refused prepare aborts the whole
    // transaction, so nothing committed anywhere and a retry is safe.
    EXPECT_TRUE(refused.status.retryable()) << refused.status.message();
    EXPECT_EQ(executor_->prepare_refusals(), 1u);
    EXPECT_EQ(executor_->prepared(), 0u);
}

TEST_F(Txn2pcParticipantTest, AnAbortedTransactionRefusesPrepareInsteadOfSayingNothing) {
    // R6-2 left this named: a poisoned session is still "in a transaction",
    // so every later statement answered "current transaction is aborted"
    // and the coordinator learned nothing. Prepare is where it becomes
    // legible.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    const Answer failed = Ship("INSERT INTO nosuch VALUES (1)", 2);
    ASSERT_FALSE(failed.status.ok());

    const Answer refused = Prepare();
    ASSERT_TRUE(refused.answered);
    EXPECT_FALSE(refused.status.ok());
    EXPECT_TRUE(refused.status.retryable()) << refused.status.message();
    EXPECT_EQ(executor_->prepare_refusals(), 1u);
    // And the doomed transaction is gone rather than left standing.
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcParticipantTest, ACommitForATransactionThisCoreNeverPreparedIsRefused) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());

    const Answer refused = Decide(TxnDecision::kCommit);
    ASSERT_TRUE(refused.answered);
    EXPECT_FALSE(refused.status.ok());
    EXPECT_EQ(executor_->decide_refusals(), 1u);
    EXPECT_EQ(executor_->decides_committed(), 0u);
    // Not committed, and not thrown away either: the context stands, and
    // the sweep still owns it because it never prepared.
    EXPECT_EQ(executor_->enrolled(), 1u);
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcParticipantTest, AnAbortForAContextThatIsAlreadyGoneIsBenign) {
    // The reachable case: the sweep rolled it back, then the coordinator's
    // abort arrives. There is nothing left to do and nothing to complain
    // about - from the coordinator's side the abort *did* happen.
    const Answer acked = Decide(TxnDecision::kAbort);
    ASSERT_TRUE(acked.answered);
    EXPECT_TRUE(acked.status.ok()) << acked.status.message();
    EXPECT_EQ(executor_->decides_aborted(), 1u);
    EXPECT_EQ(executor_->decide_refusals(), 0u);
}

TEST_F(Txn2pcParticipantTest, AResentPrepareIsAnsweredAgainAndADifferentTransactionIsRefused) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    const Answer again = Prepare();
    EXPECT_TRUE(again.status.ok()) << again.status.message();
    // Answered from the promise already made, not made a second time: one
    // record, one count.
    EXPECT_EQ(executor_->prepared(), 1u);
    std::vector<std::vector<std::byte>> storage;
    std::size_t prepares = 0;
    for (const wal::DecodedRecord& record : DeviceRecords(storage)) {
        if (record.type() == wal::RecordType::kTxnPrepare) ++prepares;
    }
    EXPECT_EQ(prepares, 1u);

    const Answer other = Prepare(/*transaction_id=*/kCoordinatorTxn + 1);
    EXPECT_FALSE(other.status.ok());
    EXPECT_EQ(executor_->prepare_refusals(), 1u);
}

TEST_F(Txn2pcParticipantTest, ADecisionThatArrivesDuringPrepareIsHeldAndAppliedOnWake) {
    // **The reachable case is not a race at a ceiling**: another
    // participant refuses instantly, so the coordinator decides ABORT and
    // sends it while this core's prepare is still reaching the device.
    // Refusing that decide would leave a core that goes on to become
    // prepared with no decision ever coming - the sweep skips it, shutdown
    // skips it, and this row has no resolution ask.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());

    auto prepare_answer = std::make_shared<Answer>();
    Txn2pcServer::PrepareAsk ask;
    ask.coordinator = kCoordinator;
    ask.session_id = kSession;
    ask.transaction_id = kCoordinatorTxn;
    executor_->PrepareSeam()(ask, [prepare_answer](const Status& status) {
        prepare_answer->answered = true;
        prepare_answer->status = status;
    });
    // One reactor turn and **no drain**: the coroutine reaches its park and
    // the record is not durable, which is the state a decide can meet.
    scheduler_->RunOnce();
    ASSERT_FALSE(prepare_answer->answered);
    ASSERT_EQ(executor_->prepared(), 0u);

    auto ack = std::make_shared<Answer>();
    Txn2pcServer::DecideAsk decide;
    decide.coordinator = kCoordinator;
    decide.session_id = kSession;
    decide.transaction_id = kCoordinatorTxn;
    decide.decision = TxnDecision::kAbort;
    executor_->DecideSeam()(decide, [ack](const Status& status) {
        ack->answered = true;
        ack->status = status;
    });
    EXPECT_FALSE(ack->answered) << "the decision is held until the prepare wakes";
    EXPECT_EQ(executor_->decide_refusals(), 0u);

    for (int i = 0; i < 64 && !ack->answered; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    EXPECT_TRUE(prepare_answer->answered);
    EXPECT_TRUE(prepare_answer->status.ok()) << prepare_answer->status.message();
    ASSERT_TRUE(ack->answered) << "the held decision was never applied";
    EXPECT_TRUE(ack->status.ok()) << ack->status.message();
    EXPECT_EQ(executor_->decides_aborted(), 1u);
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_EQ(executor_->in_doubt(), 0u);
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcParticipantTest, AResentDecisionForAReleasedTransactionIsAcknowledged) {
    // R6-1 put the bit on the decide leg to separate exactly these two: a
    // benign resend after the ack was lost, and a decide for a transaction
    // this core never prepared. Reading it is what keeps `decide_refusals()`
    // - whose header calls it the anomaly that is not a lost message -
    // readable on its first live day.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());
    ASSERT_TRUE(Decide(TxnDecision::kCommit).status.ok());
    ASSERT_EQ(executor_->enrolled(), 0u);

    const Answer resent =
        Decide(TxnDecision::kCommit, kCoordinatorTxn, kSession, /*retry=*/true);
    EXPECT_TRUE(resent.status.ok()) << resent.status.message();
    EXPECT_EQ(executor_->decide_refusals(), 0u);
    // And an *unmarked* commit for the same absent context stays the
    // anomaly it was.
    const Answer unmarked = Decide(TxnDecision::kCommit);
    EXPECT_FALSE(unmarked.status.ok());
    EXPECT_EQ(executor_->decide_refusals(), 1u);
}

TEST_F(Txn2pcParticipantTest, APreparedTransactionTakesNoFurtherStatement) {
    // The promise prepare makes is about what is *already* durable. A
    // statement admitted after it would write rows the PREPARE record does
    // not cover, and the commit decided on that promise would make the
    // transaction durable in part.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    const Answer late = Ship("INSERT INTO t VALUES (8)", 2);
    ASSERT_TRUE(late.answered);
    EXPECT_FALSE(late.status.ok());
    EXPECT_TRUE(late.status.retryable()) << late.status.message();
    EXPECT_EQ(executor_->enrolment_refusals(), 1u);

    // And the transaction it could not join is untouched: still prepared,
    // still owed a decision, and it commits exactly what it prepared.
    EXPECT_EQ(executor_->in_doubt(), 1u);
    ASSERT_TRUE(Decide(TxnDecision::kCommit).status.ok());
    const std::string rows = Rows();
    EXPECT_NE(rows.find(",7"), std::string::npos) << rows;
    EXPECT_EQ(rows.find(",8"), std::string::npos) << rows;
}

TEST_F(Txn2pcParticipantTest, ADecideForAnotherTransactionOnThisSessionIsRefused) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    const Answer refused = Decide(TxnDecision::kCommit, /*transaction_id=*/kCoordinatorTxn + 1);
    EXPECT_FALSE(refused.status.ok());
    EXPECT_EQ(executor_->decide_refusals(), 1u);
    EXPECT_EQ(executor_->decides_committed(), 0u);
    EXPECT_EQ(executor_->in_doubt(), 1u) << "the prepared transaction is still owed a decision";
}

// ---- The coordinator, over a real ring ---------------------------------------

class Txn2pcCoordinatorTest : public ::testing::Test {
protected:
    static constexpr std::uint64_t kSession = 7;
    static constexpr std::uint64_t kTxn = 1234;

    void SetUp() override {
        auto transport = sched::RealRingTransport::Create(
            /*core_count=*/3, 16, sched::kCoreRingPayloadBytes);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        transport_.emplace(std::move(transport.value()));

        for (std::uint32_t core = 0; core < 3; ++core) {
            reactors_.emplace_back(std::make_unique<sched::Scheduler>(clock_, io_));
            ASSERT_TRUE(reactors_.back()->AttachTransport(&*transport_, core).ok());
        }
        client_.emplace(/*core_id=*/0, *reactors_[0], *transport_, clock_);
        ASSERT_TRUE(client_->RegisterReplyReceivers().ok());
    }

    // A participant that answers `answer` to everything, on `core`.
    void InstallParticipant(std::uint32_t core, Status prepare_answer,
                            Status decide_answer = Status::OK()) {
        auto server = std::make_unique<Txn2pcServer>(
            core, *reactors_[core], *transport_,
            [answer = std::move(prepare_answer), this, core](Txn2pcServer::PrepareAsk ask,
                                                             Txn2pcServer::ReplyFn reply) {
                prepared_[core] = ask;
                reply(answer);
            },
            [answer = std::move(decide_answer), this, core](Txn2pcServer::DecideAsk ask,
                                                            Txn2pcServer::ReplyFn reply) {
                decided_[core] = ask;
                reply(answer);
            });
        Txn2pcServer* raw = server.get();
        servers_.push_back(std::move(server));
        ASSERT_TRUE(reactors_[core]
                        ->RegisterMessageHandler(
                            sched::RingMessageKind::kTxnPrepareRequest,
                            [raw](const sched::MessageHeader& header,
                                  std::span<const std::byte> payload) {
                                raw->OnPrepare(header, payload);
                            })
                        .ok());
        ASSERT_TRUE(reactors_[core]
                        ->RegisterMessageHandler(
                            sched::RingMessageKind::kTxnDecideRequest,
                            [raw](const sched::MessageHeader& header,
                                  std::span<const std::byte> payload) {
                                raw->OnDecide(header, payload);
                            })
                        .ok());
    }

    void Pump(int iterations = 20) {
        for (int i = 0; i < iterations; ++i) {
            for (auto& reactor : reactors_) reactor->RunOnce();
        }
    }

    sched::ManualClock clock_;
    sched::NullIoBackend io_;
    std::optional<sched::RealRingTransport> transport_;
    std::vector<std::unique_ptr<sched::Scheduler>> reactors_;
    std::vector<std::unique_ptr<Txn2pcServer>> servers_;
    std::optional<Txn2pcClient> client_;
    std::map<std::uint32_t, Txn2pcServer::PrepareAsk> prepared_;
    std::map<std::uint32_t, Txn2pcServer::DecideAsk> decided_;
};

TEST_F(Txn2pcCoordinatorTest, EveryParticipantAnswersAndThePhaseSettlesPrepared) {
    InstallParticipant(1, Status::OK());
    InstallParticipant(2, Status::OK());
    const std::vector<std::uint32_t> participants{1, 2};

    ASSERT_TRUE(client_->Prepare(/*request_id=*/1, kSession, kTxn, participants).ok());
    EXPECT_EQ(client_->prepare_messages(), 2u);
    EXPECT_FALSE(client_->Settled(1)) << "nothing has answered yet";
    Pump();

    ASSERT_TRUE(client_->Settled(1));
    const TxnPhaseOutcome* phase = client_->Find(1);
    ASSERT_NE(phase, nullptr);
    EXPECT_TRUE(phase->AllPrepared());
    // The identity crossed intact, in both directions.
    ASSERT_EQ(prepared_.size(), 2u);
    EXPECT_EQ(prepared_[1].coordinator, 0u);
    EXPECT_EQ(prepared_[1].session_id, kSession);
    EXPECT_EQ(prepared_[2].transaction_id, kTxn);
    client_->Close(1);
    EXPECT_EQ(client_->phase_timeouts(), 0u);

    // The participants' own side of the same round trip: one request in,
    // one reply out, per core.
    ASSERT_EQ(servers_.size(), 2u);
    for (const std::unique_ptr<Txn2pcServer>& server : servers_) {
        EXPECT_EQ(server->prepares(), 1u);
        EXPECT_EQ(server->decides(), 0u);
        EXPECT_EQ(server->replies(), 1u);
    }
}

TEST_F(Txn2pcCoordinatorTest, AParticipantsRefusalCrossesWithItsOwnCodeAndWords) {
    InstallParticipant(1, Status::OK());
    InstallParticipant(2, Status::TxnConflict("core 2 already holds 16 cross-owner "
                                              "transactions, its limit"));
    const std::vector<std::uint32_t> participants{1, 2};
    ASSERT_TRUE(client_->Prepare(1, kSession, kTxn, participants).ok());
    Pump();

    const TxnPhaseOutcome* phase = client_->Find(1);
    ASSERT_NE(phase, nullptr);
    EXPECT_FALSE(phase->AllPrepared());
    EXPECT_EQ(client_->prepare_refusals(), 1u);
    for (const TxnParticipantOutcome& participant : phase->participants) {
        ASSERT_TRUE(participant.replied);
        if (participant.core != 2) continue;
        EXPECT_EQ(participant.status.code(), StatusCode::kTxnConflict);
        EXPECT_NE(participant.status.message().find("its limit"), std::string::npos)
            << participant.status.message();
    }
}

TEST_F(Txn2pcCoordinatorTest, AParticipantThatNeverAnswersSettlesOnTheDeadline) {
    InstallParticipant(1, Status::OK());
    // Core 2 has no participant at all: nothing is registered for the kind,
    // so the request is dropped and the phase can only end on its deadline.
    const std::vector<std::uint32_t> participants{1, 2};
    ASSERT_TRUE(client_->Prepare(1, kSession, kTxn, participants).ok());
    Pump();
    EXPECT_FALSE(client_->Settled(1)) << "one participant is still owed";

    clock_.Advance(kTxnPhaseDeadlineNs + 1);
    EXPECT_TRUE(client_->Settled(1));
    const TxnPhaseOutcome* phase = client_->Find(1);
    ASSERT_NE(phase, nullptr);
    EXPECT_FALSE(phase->AllPrepared());
    client_->Close(1);
    EXPECT_EQ(client_->phase_timeouts(), 1u);
}

TEST_F(Txn2pcCoordinatorTest, TheDecisionCrossesAndIsAcknowledged) {
    InstallParticipant(1, Status::OK());
    InstallParticipant(2, Status::OK());
    const std::vector<std::uint32_t> participants{1, 2};

    ASSERT_TRUE(client_->Decide(9, kSession, kTxn, TxnDecision::kCommit, participants).ok());
    EXPECT_EQ(client_->decide_messages(), 2u);
    Pump();

    ASSERT_TRUE(client_->Settled(9));
    const TxnPhaseOutcome* phase = client_->Find(9);
    ASSERT_NE(phase, nullptr);
    EXPECT_TRUE(phase->AllPrepared()) << "every participant acknowledged";
    ASSERT_EQ(decided_.size(), 2u);
    EXPECT_EQ(decided_[1].decision, TxnDecision::kCommit);
    EXPECT_EQ(decided_[2].decision, TxnDecision::kCommit);
}

TEST_F(Txn2pcCoordinatorTest, AReplyFromTheOtherLegDoesNotWakeThisOne) {
    // Both legs of one transaction carry the same session and transaction
    // id, so the identity check cannot separate them - the phase can. This
    // is a decide *reply* arriving on a prepare waiter's request id, which
    // is what a prepare answer that lost its race with the deadline would
    // look like one phase later.
    InstallParticipant(1, Status::OK());
    const std::vector<std::uint32_t> participants{1};
    ASSERT_TRUE(client_->Prepare(5, kSession, kTxn, participants).ok());

    const TxnParticipantReplyPayload reply =
        TxnParticipantReplyOf(kSession, kTxn, Status::OK());
    sched::SubmitSendPod(*reactors_[1], *transport_, /*src_core=*/1, /*dst_core=*/0,
                         /*session_core=*/0, /*request_id=*/5,
                         sched::RingMessageKind::kTxnDecideReply, reply);
    Pump();

    EXPECT_EQ(client_->identity_mismatches(), 1u);
    const TxnPhaseOutcome* phase = client_->Find(5);
    ASSERT_NE(phase, nullptr);
    // The real prepare answer still landed, on its own leg.
    EXPECT_TRUE(phase->AllPrepared());
}

TEST_F(Txn2pcCoordinatorTest, AReplyThatOutlivesItsPhaseIsCountedRatherThanDelivered) {
    InstallParticipant(1, Status::OK());
    const std::vector<std::uint32_t> participants{1};
    ASSERT_TRUE(client_->Prepare(3, kSession, kTxn, participants).ok());
    // The coordinator gives up before the participant's answer is pumped.
    clock_.Advance(kTxnPhaseDeadlineNs + 1);
    ASSERT_TRUE(client_->Settled(3));
    client_->Close(3);
    Pump();

    EXPECT_EQ(client_->late_replies(), 1u);
    EXPECT_EQ(client_->phase_timeouts(), 1u);
    EXPECT_EQ(client_->identity_mismatches(), 0u);
}

TEST_F(Txn2pcCoordinatorTest, EveryShapeThatCannotBeAPhaseIsRefusedBeforeAnythingIsSent) {
    const std::vector<std::uint32_t> none;
    const std::vector<std::uint32_t> self{0};
    const std::vector<std::uint32_t> twice{1, 1};
    const std::vector<std::uint32_t> absent{9};
    const std::vector<std::uint32_t> ok{1};

    EXPECT_FALSE(client_->Prepare(1, kSession, kTxn, none).ok())
        << "a one-owner transaction takes the single-core path and enters no protocol";
    EXPECT_FALSE(client_->Prepare(1, kSession, kTxn, self).ok());
    EXPECT_FALSE(client_->Prepare(1, kSession, kTxn, twice).ok());
    EXPECT_FALSE(client_->Prepare(1, kSession, kTxn, absent).ok());
    EXPECT_FALSE(client_->Prepare(1, kSession, /*transaction_id=*/0, ok).ok());
    EXPECT_FALSE(client_->Decide(1, kSession, kTxn, TxnDecision::kUnset, ok).ok());

    // Nothing left a waiter or a message behind, which is what makes each
    // of those refusals safe to hand a client verbatim.
    EXPECT_EQ(client_->waiting(), 0u);
    EXPECT_EQ(client_->prepare_messages(), 0u);
    EXPECT_EQ(client_->decide_messages(), 0u);

    // And one id carries one phase: the second open on it is refused.
    ASSERT_TRUE(client_->Prepare(1, kSession, kTxn, ok).ok());
    EXPECT_FALSE(client_->Prepare(1, kSession, kTxn, ok).ok());
}

// ---- The coordinator's COMMIT, end to end ------------------------------------
//
// The dispatcher's half: `COMMIT` on a transaction that enrolled
// participants runs D4's two phases, and one that did not runs the path it
// always ran. The participant here is a stub on a second reactor - what is
// under test is the coordinator's sequence, and the real participant has its
// own fixture above.

class Txn2pcCommitTest : public ::testing::Test {
protected:
    static constexpr std::size_t kSegmentSize = 1 << 20;

    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(kSegmentSize);
        ASSERT_TRUE(device.ok()) << device.status().message();
        log_device_ = std::move(device.value());
        wal::WalManagerConfig config;
        config.ring_capacity = wal::kMinRingCapacity;
        auto wal = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0, config);
        ASSERT_TRUE(wal.ok()) << wal.status().message();
        wal_ = std::move(wal.value());

        auto boot = bootstrap::BootstrapDatabase(store_, /*now_unix_seconds=*/1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
        ids_.emplace(boot_->superblock);
        undo_.emplace(store_, wal_.get());
        txns_.emplace(*ids_, *undo_, store_, wal_.get());
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr, &clock_,
                            wal_.get(), wal::DurabilityClass::kGroup, exec::Budget(),
                            /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*txns_);

        auto transport = sched::RealRingTransport::Create(
            /*core_count=*/2, 16, sched::kCoreRingPayloadBytes);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        transport_.emplace(std::move(transport.value()));
        coordinator_.emplace(clock_, io0_);
        participant_.emplace(clock_, io1_);
        ASSERT_TRUE(coordinator_->AttachTransport(&*transport_, 0).ok());
        ASSERT_TRUE(participant_->AttachTransport(&*transport_, 1).ok());

        client_.emplace(/*core_id=*/0, *coordinator_, *transport_, clock_);
        ASSERT_TRUE(client_->RegisterReplyReceivers().ok());
        dispatcher_->SetTxn2pc(&*client_);

        ASSERT_EQ(Local("CREATE TABLE t (id int64, v int64)").rfind("CREATED", 0), 0u);
    }

    void TearDown() override {
        // The dispatcher outlives the client here only by declaration
        // order; withdrawing the borrow is what the servers do at teardown
        // and what keeps this fixture honest about it.
        if (dispatcher_.has_value()) dispatcher_->SetTxn2pc(nullptr);
    }

    void InstallParticipant(Status prepare_answer) {
        server_.emplace(
            /*core_id=*/1, *participant_, *transport_,
            [this, answer = std::move(prepare_answer)](Txn2pcServer::PrepareAsk ask,
                                                       Txn2pcServer::ReplyFn reply) {
                prepared_ = ask;
                reply(answer);
            },
            [this](Txn2pcServer::DecideAsk ask, Txn2pcServer::ReplyFn reply) {
                decided_ = ask;
                reply(Status::OK());
            });
        ASSERT_TRUE(participant_
                        ->RegisterMessageHandler(sched::RingMessageKind::kTxnPrepareRequest,
                                                 [this](const sched::MessageHeader& header,
                                                        std::span<const std::byte> payload) {
                                                     server_->OnPrepare(header, payload);
                                                 })
                        .ok());
        ASSERT_TRUE(participant_
                        ->RegisterMessageHandler(sched::RingMessageKind::kTxnDecideRequest,
                                                 [this](const sched::MessageHeader& header,
                                                        std::span<const std::byte> payload) {
                                                     server_->OnDecide(header, payload);
                                                 })
                        .ok());
    }

    std::string Local(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Rows() { return Local("SELECT * FROM t"); }

    // The client's statement, run the way a served connection runs it - the
    // only entry point that may park, which is what the protocol needs.
    DispatchOutcome RunAsync(const std::string& sql, Session& session, int turns = 64) {
        auto out = std::make_shared<DispatchOutcome>();
        auto done = std::make_shared<bool>(false);
        coordinator_->Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground,
            dispatcher_->DispatchAsync(sql, &session, out.get()),
            [done](const Status&) { *done = true; }));
        for (int i = 0; i < turns && !*done; ++i) {
            (void)wal_->DrainOnce();
            coordinator_->RunOnce();
            participant_->RunOnce();
        }
        EXPECT_TRUE(*done) << "the statement never finished: " << sql;
        return *out;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    sched::ManualClock clock_;
    sched::NullIoBackend io0_;
    sched::NullIoBackend io1_;
    std::unique_ptr<wal::MemoryLogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
    std::optional<bootstrap::BootstrapResult> boot_;
    std::optional<txn::TrxIdSequence> ids_;
    std::optional<txn::UndoLog> undo_;
    std::optional<txn::TransactionManager> txns_;
    std::optional<CommandDispatcher> dispatcher_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::Scheduler> coordinator_;
    std::optional<sched::Scheduler> participant_;
    std::optional<Txn2pcClient> client_;
    std::optional<Txn2pcServer> server_;
    std::optional<Txn2pcServer::PrepareAsk> prepared_;
    std::optional<Txn2pcServer::DecideAsk> decided_;
};

TEST_F(Txn2pcCommitTest, AOneOwnerCommitSendsNoPrepareAndTakesThePathItAlwaysTook) {
    // D1's fast path, asserted the way the work order's §5 asks for it: by
    // counting prepare messages, which is zero.
    Session client_session;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &client_session).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO t VALUES (7)", &client_session)
                  .response.rfind("INSERTED", 0),
              0u);
    ASSERT_FALSE(client_session.has_participants());

    const DispatchOutcome out = RunAsync("COMMIT", client_session);
    EXPECT_EQ(out.response.rfind("COMMIT trx_id=", 0), 0u) << out.response;
    EXPECT_EQ(client_->prepare_messages(), 0u);
    EXPECT_EQ(client_->decide_messages(), 0u);
    EXPECT_NE(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcCommitTest, ACrossOwnerCommitPreparesThenDecidesAndTheClientSeesCommit) {
    InstallParticipant(Status::OK());
    Session session;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO t VALUES (7)", &session)
                  .response.rfind("INSERTED", 0),
              0u);
    // What R6-8 will do at the ship site, done here by hand: a statement
    // that ran on core 1 inside this transaction enrolled it.
    session.set_ship_id(11);
    session.EnrolParticipant(1);

    const DispatchOutcome out = RunAsync("COMMIT", session);
    EXPECT_EQ(out.response.rfind("COMMIT trx_id=", 0), 0u) << out.response;

    ASSERT_TRUE(prepared_.has_value());
    EXPECT_EQ(prepared_->session_id, 11u);
    EXPECT_NE(prepared_->transaction_id, 0u);
    ASSERT_TRUE(decided_.has_value());
    EXPECT_EQ(decided_->decision, TxnDecision::kCommit);
    // The decide names the same transaction the prepare did.
    EXPECT_EQ(decided_->transaction_id, prepared_->transaction_id);
    EXPECT_EQ(client_->prepare_messages(), 1u);
    EXPECT_EQ(client_->decide_messages(), 1u);
    // Both phases closed behind them: a waiter left standing is a leak the
    // next statement's request id would trip over.
    EXPECT_EQ(client_->waiting(), 0u);
    EXPECT_FALSE(session.in_explicit_txn());
    EXPECT_FALSE(session.has_participants());
    EXPECT_NE(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcCommitTest, ARefusedPrepareAbortsEverythingAndSaysWhoRefused) {
    InstallParticipant(Status::TxnConflict("core 1 holds no transaction for this session"));
    Session session;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO t VALUES (7)", &session)
                  .response.rfind("INSERTED", 0),
              0u);
    session.set_ship_id(11);
    session.EnrolParticipant(1);

    const DispatchOutcome out = RunAsync("COMMIT", session);
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    // The participant's own words, and its own retryable bit: nothing
    // committed anywhere, so a retry is safe and the client may take it.
    EXPECT_NE(out.response.find("core 1 holds no transaction"), std::string::npos)
        << out.response;
    EXPECT_EQ(StatusFromErrorReply(out.response).code(), StatusCode::kTxnConflict);

    // Told to abort, not left hanging.
    ASSERT_TRUE(decided_.has_value());
    EXPECT_EQ(decided_->decision, TxnDecision::kAbort);
    // And this core's own half is gone with it.
    EXPECT_FALSE(session.in_explicit_txn());
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();
    EXPECT_EQ(client_->waiting(), 0u);
}

TEST_F(Txn2pcCommitTest, AParticipantThatNeverAnswersAbortsTheTransaction) {
    // No participant installed on core 1 at all: the prepare is dropped and
    // the phase ends on its deadline. A prepare timeout is an **abort** -
    // no decision was written, so nothing committed anywhere.
    Session session;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO t VALUES (7)", &session)
                  .response.rfind("INSERTED", 0),
              0u);
    session.set_ship_id(11);
    session.EnrolParticipant(1);

    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    coordinator_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("COMMIT", &session, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 8 && !*done; ++i) {
        coordinator_->RunOnce();
        participant_->RunOnce();
    }
    ASSERT_FALSE(*done) << "the commit answered before its participant's deadline";

    // **Two deadlines, not one, and the test says so rather than hiding it
    // in a longer pump.** The prepare phase ends the transaction; the
    // decide phase then tells the same silent participant to abort and
    // waits out its own ceiling for an acknowledgement that changes no
    // outcome. So a client whose participant is gone waits `2 ×
    // kTxnPhaseDeadlineNs` for an answer that was decided at the first.
    // Bounded, never silent (HP3), and the cost is named here because R6-5
    // owns the ceiling this would be shortened by.
    clock_.Advance(kTxnPhaseDeadlineNs + 1);
    for (int i = 0; i < 8 && !*done; ++i) {
        coordinator_->RunOnce();
        participant_->RunOnce();
    }
    EXPECT_FALSE(*done) << "the decide leg waits its own ceiling for the same silent core";
    clock_.Advance(kTxnPhaseDeadlineNs + 1);
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        coordinator_->RunOnce();
        participant_->RunOnce();
    }
    ASSERT_TRUE(*done);
    EXPECT_EQ(out->response.rfind("ERR ", 0), 0u) << out->response;
    EXPECT_NE(out->response.find("did not answer prepare"), std::string::npos) << out->response;
    // One per phase - the count reads the doubled wait the comment above
    // describes, which is what makes it visible from outside the process.
    EXPECT_EQ(client_->phase_timeouts(), 2u);
    EXPECT_FALSE(session.in_explicit_txn());
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcCommitTest, ACrossOwnerCommitFromAPathThatCannotParkIsRefusedBeforeAnythingIsSent) {
    InstallParticipant(Status::OK());
    Session session;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &session).response.rfind("BEGIN", 0), 0u);
    session.set_ship_id(11);
    session.EnrolParticipant(1);

    // The synchronous entry point: no reactor to await participants on.
    const DispatchOutcome out = dispatcher_->Dispatch("COMMIT", &session);
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_TRUE(StatusFromErrorReply(out.response).retryable()) << out.response;
    EXPECT_EQ(client_->prepare_messages(), 0u) << "nothing may be asked of a participant here";
    // The transaction is untouched, which is what makes that refusal safe:
    // the client may COMMIT again on a served connection, or roll back.
    EXPECT_TRUE(session.in_explicit_txn());
    EXPECT_EQ(dispatcher_->Dispatch("ROLLBACK", &session).response.rfind("ROLLBACK", 0), 0u);
}

}  // namespace
}  // namespace kds::server
