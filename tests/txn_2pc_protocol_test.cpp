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
#include "kds/txn/lock_table.hpp"
#include "kds/server/shipped_statement_executor.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"
// R6-6: the stop sequence's checkpoint, and the analysis of the mount that
// follows it.
#include "kds/wal/analysis.hpp"
#include "kds/wal/checkpointer.hpp"
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
//   1. a prepared context is not rolled back - not by the lifetime sweep, not by
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

    // The cap of the lock table this fixture wants, or `nullopt` for no
    // lock family at all - which is what every cell but the AO ones wants,
    // and what keeps them measuring an engine with no table rather than
    // one with an empty table.
    virtual std::optional<std::size_t> LockCap() const { return std::nullopt; }

    // **Declared here, above every member below, and it has to be**:
    // `~TransactionManager` releases the borrows of every still-live
    // transaction through this table (AO-R6's other end), so `txns_` must
    // die first and a member dies before the ones declared above it. Moving
    // this down beside `txns_` is a use-after-free at teardown, not a tidy.
    // Both production owners order it the same way (`expeditor.hpp`,
    // `core_runtime.hpp`).
    std::unique_ptr<txn::LockTable> locks_;

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
        // **The lock family goes into the manager as well as the
        // dispatcher, and it has to** (AO-S6c). The manager is what
        // releases a transaction's borrows at its decide
        // (`txn/manager.hpp`, AO-R6), so a fixture that wires the table
        // into the dispatcher alone builds a family that takes borrows and
        // never gives them back: entries accumulate across statements and
        // the next statement conflicts with rows whose writers committed
        // long ago. Production passes it to both - `core_runtime.cpp`
        // constructs the manager with it and then calls `set_locks` - and
        // this fixture was passing it to one. That stayed invisible while
        // only UPDATE and DELETE borrowed and each cell ran a single
        // write; it stopped being invisible the moment AO-S6c gave INSERT
        // its borrow, and the cell that caught it read `entries=5` after
        // four committed autocommit inserts.
        if (const std::optional<std::size_t> cap = LockCap(); cap.has_value()) {
            auto table = txn::LockTable::Create(/*core_count=*/1, *cap);
            ASSERT_TRUE(table.ok()) << table.status().message();
            locks_ = std::move(table.value());
        }
        txns_.emplace(*ids_, *undo_, store_, wal_.get(), /*visibility=*/nullptr, /*core=*/0,
                      locks_.get());
        dispatcher_.emplace(boot_->superblock, boot_->catalog, store_, /*log=*/nullptr, &clock_,
                            wal_.get(), Durability(), exec::Budget(),
                            /*recorder=*/nullptr, /*replay_enabled=*/false,
                            /*access_statistics=*/false, /*cabins=*/nullptr, &*txns_);
        dispatcher_->set_locks(locks_.get());
        scheduler_.emplace(clock_, io_);
        executor_.emplace(/*core_id=*/1, *dispatcher_, *scheduler_, clock_, /*log=*/nullptr,
                          wal_.get());

        // **HEAP, pinned since SUS-1** (AS-R5, `instructions/v3.0.0/
        // workorder-as-sus1-heap-suspended.md`). `t` is this chain's only
        // heap relation, and `MidWalkWaitTest` below spells out why it has
        // to stay one: it resumes its walk **by position** while `tb`
        // resumes **by key**, so on the flipped default the two arms would
        // be the same walk run twice and the position arm - AO-S3b's, the
        // one that already shipped a wrong answer once - would execute
        // nowhere. The test binary lifts the suspension
        // (`tests/heap_suspension_env.cpp`), which is what lets this word
        // stand.
        ASSERT_EQ(Local("CREATE TABLE t (id int64, v int64) HEAP").rfind("CREATED", 0), 0u);
    }

    // `group` by default, because that is what a server runs and what makes
    // the decide leg's commit take the group committer rather than an
    // inline sync. Overridden by the strict fixture below, which is XE1's
    // control: the ack timing this order moved is D2's alone.
    virtual wal::DurabilityClass Durability() const { return wal::DurabilityClass::kGroup; }

    std::string Local(const std::string& sql) { return dispatcher_->Dispatch(sql).response; }
    std::string Rows() { return Local("SELECT * FROM t"); }

    struct Answer {
        bool answered = false;
        Status status;
        std::string text;

        // What the executor and the log said **at the moment this answer
        // was produced** (XE3). Filled by every seam callback below, because
        // the only way to test what precedes a reply rather than what merely
        // follows it is to read it from inside the reply.
        std::size_t enrolled_at_ack = 0;
        std::size_t in_doubt_at_ack = 0;
        std::uint64_t committed_at_ack = 0;
        wal::Lsn appended_at_ack = 0;
        wal::Lsn durable_at_ack = 0;
    };

    // The reactor's turn, plus the drain a reactor's post-task hook runs:
    // a prepare parks on `IsDurable`, and nothing makes a record durable
    // without one (`WalManager::DrainOnce`).
    //
    // **`drain = false` is the instrument XE1's contract is read with**:
    // nothing else in this fixture can make a record durable, so an answer
    // that arrives without the drain is an answer that did not wait for one.
    void Pump(const std::shared_ptr<Answer>& answer, bool drain = true, int turns = 64) {
        for (int i = 0; i < turns && !answer->answered; ++i) {
            if (drain) (void)wal_->DrainOnce();
            scheduler_->RunOnce();
        }
    }

    // Stamped into an `Answer` from inside a seam callback. Shared with the
    // callback rather than captured by reference: a seam keeps the lambda
    // and may call it after the turn limit gave up - which is exactly what
    // a *failing* cell does - so a pointer into a helper's frame would be
    // written through after that frame is gone.
    void StampAck(Answer& answer) {
        answer.enrolled_at_ack = executor_->enrolled();
        answer.in_doubt_at_ack = executor_->in_doubt();
        answer.committed_at_ack = executor_->decides_committed();
        answer.appended_at_ack = wal_->appended_lsn();
        answer.durable_at_ack = wal_->durable_lsn();
    }

    Answer Ship(const std::string& sql, std::uint64_t sequence, bool in_txn = true,
                bool join = false,
                std::optional<txn::IsolationLevel> isolation = std::nullopt,
                bool drain = true) {
        StatementShipServer::ShippedStatement statement;
        statement.requester = kCoordinator;
        statement.session_id = kSession;
        statement.sequence = sequence;
        statement.role = Role::kReadWrite;
        statement.in_txn = in_txn;
        statement.join = join;
        statement.isolation = isolation;
        statement.text = sql;

        auto answer = std::make_shared<Answer>();
        executor_->Seam()(std::move(statement),
                          [this, answer](const Status& status, std::string_view text) {
                              StampAck(*answer);
                              answer->answered = true;
                              answer->status = status;
                              answer->text.assign(text);
                          });
        last_ship_ = answer;
        Pump(answer, drain);
        return *answer;
    }

    // Resumes whatever `Ship` last left unanswered, with the drain this
    // time. A second `Ship` on the same session cannot do this - the
    // executor refuses one while a statement is still running for that
    // session, which is its own rule and not this fixture's - so a cell
    // that withholds the drain has to finish the statement it started.
    Answer FinishLastShip() {
        Pump(last_ship_);
        return *last_ship_;
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
                  std::uint64_t session_id = kSession, bool retry = false,
                  bool drain = true) {
        Txn2pcServer::DecideAsk ask;
        ask.coordinator = kCoordinator;
        ask.session_id = session_id;
        ask.transaction_id = transaction_id;
        ask.decision = decision;
        ask.retry = retry;
        auto answer = std::make_shared<Answer>();
        executor_->DecideSeam()(ask, [this, answer](const Status& status) {
            StampAck(*answer);
            answer->answered = true;
            answer->status = status;
        });
        Pump(answer, drain);
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
    // The last `Ship`'s answer, for `FinishLastShip`.
    std::shared_ptr<Answer> last_ship_;
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
    clock_.Advance(kTxnLifetimeCeilingNs * 4);
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
    clock_.Advance(kTxnLifetimeCeilingNs * 2);
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

// ---- XE1: the ack is at the COMMIT append under D2 --------------------------
//
// `instructions/v2.7.1/workorder-xd.md` XE3, enacting
// `instructions/v2.7.1/ratification-xd1.md`. The contract is
// `docs/spec/cross-owner-txn.md` section 2: a participant's own terminal
// record is a redo shortcut, not part of what the client is promised, so
// under D2 it acknowledges the decide when the record is appended and lets
// the next drain carry it.
//
// **The instrument is the drain, withheld.** `PumpNoDrain` turns the
// reactor and nothing else, and nothing else in this fixture can make a
// record durable - so an ack that arrives under it is an ack that did not
// wait for one. Before XE1 this test runs out its turn limit rather than
// failing an expectation, which is why the answered check comes first.

TEST_F(Txn2pcParticipantTest, UnderGroupTheDecideIsAcknowledgedWithItsCommitStillInTheRing) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());
    const wal::Lsn durable_before = wal_->durable_lsn();

    const Answer acked = Decide(TxnDecision::kCommit, kCoordinatorTxn, kSession,
                                /*retry=*/false, /*drain=*/false);
    ASSERT_TRUE(acked.answered) << "the decide was not acknowledged without a drain";
    EXPECT_TRUE(acked.status.ok()) << acked.status.message();

    // The whole of XE1, stated as the log saw it: bytes were appended past
    // the durable point and the ack went out anyway.
    EXPECT_GT(acked.appended_at_ack, acked.durable_at_ack)
        << "nothing was left unsynced, so this cell proves nothing about ack timing";
    EXPECT_EQ(acked.durable_at_ack, durable_before)
        << "the durable point moved without a drain, which this fixture cannot do";

    // And the drain still carries it - the record is not orphaned by the
    // early ack, it is deferred. `Commit(kGroup)` registered it with the
    // group at the append, so one tick is enough with nobody parked.
    (void)wal_->DrainOnce();
    EXPECT_GE(wal_->durable_lsn(), acked.appended_at_ack)
        << "the deferred COMMIT never reached the device";
}

// **The blast radius, which is the cell that would catch the flag leaking.**
// `CommitAck` is stamped per statement, and the same participant session also
// runs ordinary shipped autocommit writes whose D2 acknowledgement still means
// "durable". So this one is the negative: with the drain withheld it must
// **not** answer. Nothing but the code's structure prevents that regression,
// which is exactly why it is worth a cell.
TEST_F(Txn2pcParticipantTest, AnOrdinaryShippedWriteStillWaitsForDurabilityBeforeItAnswers) {
    const Answer autocommit = Ship("INSERT INTO t VALUES (8)", 1, /*in_txn=*/false,
                                   /*join=*/false, /*isolation=*/std::nullopt,
                                   /*drain=*/false);
    EXPECT_FALSE(autocommit.answered)
        << "an autocommit shipped write answered with its commit still in the ring";

    // And it is the drain it was waiting for, not something else that
    // broke: give the **same** statement one and it completes.
    const Answer drained = FinishLastShip();
    ASSERT_TRUE(drained.answered) << "the write never answered even with the drain";
    EXPECT_TRUE(drained.status.ok()) << drained.status.message();
    EXPECT_NE(Rows().find(",8"), std::string::npos) << Rows();
}

TEST_F(Txn2pcParticipantTest, TheEarlierAckDoesNotMoveTheBookkeepingItFollows) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());
    ASSERT_EQ(executor_->in_doubt(), 1u);

    // Read *inside* the reply callback, so these are the values a
    // coordinator's ack is concurrent with rather than values settled
    // afterwards. Every one of them was already true before XE1; the point
    // of the cell is that moving the ack earlier did not drag them with it.
    const Answer acked = Decide(TxnDecision::kCommit, kCoordinatorTxn, kSession,
                                /*retry=*/false, /*drain=*/false);
    ASSERT_TRUE(acked.answered);
    EXPECT_EQ(acked.enrolled_at_ack, 0u) << "the context outlived its own ack";
    EXPECT_EQ(acked.in_doubt_at_ack, 0u) << "the transaction was still in doubt at its ack";
    EXPECT_EQ(acked.committed_at_ack, 1u) << "the counter trailed the ack it describes";
    EXPECT_NE(Rows().find(",7"), std::string::npos) << Rows();
}

// **The control**, and the reason the fixture's class is a virtual: under
// D1 the sync happens *inside* `WalManager::Commit` before it returns
// (`wal/manager.cpp`), so there is no post-append wait for XE1 to move and
// the record is on the device by the time the ack is written. Same seam,
// same instrument, opposite reading - which is what makes the D2 cell above
// a statement about D2 rather than about this fixture.
class Txn2pcParticipantStrictTest : public Txn2pcParticipantTest {
protected:
    wal::DurabilityClass Durability() const override { return wal::DurabilityClass::kStrict; }
};

TEST_F(Txn2pcParticipantStrictTest, UnderStrictTheCommitIsAlreadyDurableWhenTheDecideIsAcked) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    const Answer acked = Decide(TxnDecision::kCommit, kCoordinatorTxn, kSession,
                                /*retry=*/false, /*drain=*/false);
    ASSERT_TRUE(acked.answered)
        << "strict's decide needs no drain either, and did not get one";
    EXPECT_TRUE(acked.status.ok()) << acked.status.message();
    EXPECT_EQ(acked.durable_at_ack, acked.appended_at_ack)
        << "strict acknowledged a decide with bytes still unsynced";
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

    // A decide naming no transaction is refused over a target that
    // prepared - the id is what an in-doubt ask resolves by - and admitted
    // over intent holders alone, who never ask (AO-S5(b) C2: the refusal
    // arm's decide and `ReleaseIntentsWithoutWaiting` name none by design,
    // and were refused here until then).
    EXPECT_FALSE(client_->Decide(2, kSession, /*transaction_id=*/0, TxnDecision::kAbort, ok).ok());
    EXPECT_EQ(client_->decide_messages(), 0u);

    // And one id carries one phase: the second open on it is refused.
    ASSERT_TRUE(client_->Prepare(1, kSession, kTxn, ok).ok());
    EXPECT_FALSE(client_->Prepare(1, kSession, kTxn, ok).ok());

    ASSERT_TRUE(client_->Decide(3, kSession, /*transaction_id=*/0, TxnDecision::kAbort, ok,
                                /*intent_only=*/ok)
                    .ok());
    EXPECT_EQ(client_->decide_messages(), 1u) << "the intent-only decide did not leave";
    client_->Close(3);
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

TEST(CrossOwnerJoinTest, HasParticipantIsWhatTheJoinBitIsReadFrom) {
    Session session;
    EXPECT_FALSE(session.HasParticipant(1));
    session.EnrolParticipant(1);
    EXPECT_TRUE(session.HasParticipant(1));
    EXPECT_FALSE(session.HasParticipant(2));
}

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

// ---- R6-5: the in-doubt ask, and D5's bounded block ---------------------------
//
// Three fixtures again, for the three sides D5 has.
//
// **The coordinator's memory** (`Txn2pcResolveTest`): what a core answers a
// participant that asks. Three answers and no fourth - the decision, "not
// yet", and "cannot be established here" - and the last of those is
// terminal, so the arm that produces it is the one worth being sure about.
//
// **The participant's wait** (`Txn2pcInDoubtTest`): one ask per ceiling and
// the answer applied. What is pinned is that the ask is a *resend* by
// construction - R6-0's bit set - because the whole safety of answering it
// from a record rather than by re-deciding rests on that.
//
// **The writer's block** (`Txn2pcBlockedWriterTest`): a statement that wants
// a row an in-doubt transaction holds waits and then runs, or waits out the
// ceiling and is refused **by name, retryably, and not `UnknownOutcome`**.

class Txn2pcResolveTest : public ::testing::Test {
protected:
    static constexpr std::uint64_t kSession = 7;
    static constexpr std::uint64_t kTxn = 1234;

    void SetUp() override {
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

        // The participant's half is the transport alone here: what is under
        // test is what the coordinator answers, so the seam simply records.
        server_.emplace(
            /*core_id=*/1, *participant_, *transport_,
            [](Txn2pcServer::PrepareAsk, Txn2pcServer::ReplyFn reply) { reply(Status::OK()); },
            [](Txn2pcServer::DecideAsk, Txn2pcServer::ReplyFn reply) { reply(Status::OK()); },
            [this](Txn2pcServer::ResolveAnswer answer) { answers_.push_back(answer); });
        ASSERT_TRUE(server_->RegisterResolveReplyReceiver().ok());
        // The decide leg too, so a phase this fixture closes can be one
        // that was actually acknowledged - which is the difference the
        // record's lifetime turns on.
        ASSERT_TRUE(participant_
                        ->RegisterMessageHandler(sched::RingMessageKind::kTxnDecideRequest,
                                                 [this](const sched::MessageHeader& header,
                                                        std::span<const std::byte> payload) {
                                                     server_->OnDecide(header, payload);
                                                 })
                        .ok());
    }

    void Pump(int iterations = 20) {
        for (int i = 0; i < iterations; ++i) {
            coordinator_->RunOnce();
            participant_->RunOnce();
        }
    }

    sched::ManualClock clock_;
    sched::NullIoBackend io0_;
    sched::NullIoBackend io1_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::Scheduler> coordinator_;
    std::optional<sched::Scheduler> participant_;
    std::optional<Txn2pcClient> client_;
    std::optional<Txn2pcServer> server_;
    std::vector<Txn2pcServer::ResolveAnswer> answers_;
};

TEST_F(Txn2pcResolveTest, ADecidedTransactionIsAnsweredWithItsDecisionAndNotReDecided) {
    const std::vector<std::uint32_t> participants{1};
    ASSERT_TRUE(client_->Prepare(1, kSession, kTxn, participants).ok());
    Pump();
    client_->Close(1);
    ASSERT_TRUE(client_->Decide(2, kSession, kTxn, TxnDecision::kCommit, participants).ok());
    // **The record is written before the sends**, so the ask below is
    // answered even though this decide phase is never pumped or closed -
    // which is the shape of a decide message the ring lost.
    server_->Ask(/*coordinator=*/0, kSession, kTxn);
    Pump();

    ASSERT_EQ(answers_.size(), 1u);
    EXPECT_TRUE(answers_[0].status.ok()) << answers_[0].status.message();
    EXPECT_EQ(answers_[0].decision, TxnDecision::kCommit);
    EXPECT_EQ(answers_[0].transaction_id, kTxn);
    EXPECT_EQ(client_->resolutions_answered(), 1u);
    EXPECT_EQ(client_->resolutions_unknown(), 0u);
}

TEST_F(Txn2pcResolveTest, APreparedButUndecidedTransactionIsToldToAskAgainRatherThanUnknown) {
    // The window this exists for: the participant has prepared and the
    // coordinator is still waiting on a *sibling* participant. Answering
    // `UnknownOutcome` here would be terminal - the participant would stop
    // asking about a transaction whose decision is milliseconds away.
    const std::vector<std::uint32_t> participants{1};
    ASSERT_TRUE(client_->Prepare(1, kSession, kTxn, participants).ok());
    server_->Ask(/*coordinator=*/0, kSession, kTxn);
    Pump();

    ASSERT_EQ(answers_.size(), 1u);
    EXPECT_FALSE(answers_[0].status.ok());
    EXPECT_TRUE(answers_[0].status.retryable()) << answers_[0].status.message();
    EXPECT_EQ(answers_[0].decision, TxnDecision::kUnset);
    EXPECT_EQ(client_->resolutions_undecided(), 1u);
    EXPECT_EQ(client_->resolutions_unknown(), 0u);
}

TEST_F(Txn2pcResolveTest, ATransactionThisCoreHasNoRecordOfIsUnknownAndNeverGuessed) {
    server_->Ask(/*coordinator=*/0, kSession, /*transaction_id=*/999);
    Pump();

    ASSERT_EQ(answers_.size(), 1u);
    EXPECT_EQ(answers_[0].status.code(), StatusCode::kUnknownOutcome);
    EXPECT_EQ(answers_[0].decision, TxnDecision::kUnset);
    EXPECT_EQ(client_->resolutions_unknown(), 1u);
    // **Never `retryable`**: this is D5's terminal answer and a participant
    // that retried it would ask for ever about a record that is gone.
    EXPECT_FALSE(answers_[0].status.retryable());
}

TEST_F(Txn2pcResolveTest, AnAcknowledgedDecidePhaseKeepsNoRecordAndAnUnacknowledgedOneDoes) {
    const std::vector<std::uint32_t> participants{1};
    ASSERT_TRUE(client_->Decide(1, kSession, kTxn, TxnDecision::kCommit, participants).ok());
    EXPECT_EQ(client_->decisions_held(), 1u);
    Pump();
    ASSERT_TRUE(client_->Settled(1));
    client_->Close(1);
    // Every participant acknowledged, so nobody is left to ask and the
    // record would only be a map node held for the retention on the healthy
    // path - which is every cross-owner transaction.
    EXPECT_EQ(client_->decisions_held(), 0u);

    // The unacknowledged case keeps it: the participant that did not
    // acknowledge is exactly the one that will ask.
    const std::vector<std::uint32_t> silent{1};
    ASSERT_TRUE(client_->Decide(2, kSession, kTxn + 1, TxnDecision::kAbort, silent).ok());
    clock_.Advance(kTxnPhaseDeadlineNs + 1);
    ASSERT_TRUE(client_->Settled(2));
    client_->Close(2);
    EXPECT_EQ(client_->decisions_held(), 1u);
}

TEST_F(Txn2pcResolveTest, TheRetentionForgetsADecisionAndTheAnswerBecomesUnknown) {
    const std::vector<std::uint32_t> participants{1};
    ASSERT_TRUE(client_->Decide(1, kSession, kTxn, TxnDecision::kCommit, participants).ok());
    clock_.Advance(kTxnDecisionRetentionNs + 1);

    server_->Ask(/*coordinator=*/0, kSession, kTxn);
    Pump();
    ASSERT_EQ(answers_.size(), 1u);
    EXPECT_EQ(answers_[0].status.code(), StatusCode::kUnknownOutcome);
    EXPECT_EQ(client_->decisions_forgotten(), 1u);
    EXPECT_EQ(client_->decisions_held(), 0u);
}

TEST_F(Txn2pcResolveTest, AnAskWithTheRetryBitClearIsRefusedRatherThanAnswered) {
    // R6-0's contract has one live sender and this is it: an ask is a
    // resend by construction. A sender that leaves the bit clear does not
    // know that, and answering it as though it did is how the guarantee
    // would be lost quietly - so the ask is refused rather than served from
    // a record.
    const std::vector<std::uint32_t> participants{1};
    ASSERT_TRUE(client_->Decide(1, kSession, kTxn, TxnDecision::kCommit, participants).ok());

    TxnResolveRequestPayload ask{};
    ask.session_id = kSession;
    ask.transaction_id = kTxn;
    ask.retry = 0;
    sched::SubmitSendPod(*participant_, *transport_, /*src_core=*/1, /*dst_core=*/0,
                         /*session_core=*/0, /*request_id=*/77,
                         sched::RingMessageKind::kTxnResolveRequest, ask);
    Pump();

    ASSERT_EQ(answers_.size(), 1u);
    EXPECT_EQ(answers_[0].status.code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(client_->resolve_refusals(), 1u);
    EXPECT_EQ(client_->resolutions_answered(), 0u);
}

// ---- The participant's wait ---------------------------------------------------

class Txn2pcInDoubtTest : public Txn2pcParticipantTest {
protected:
    void SetUp() override {
        Txn2pcParticipantTest::SetUp();
        auto transport = sched::RealRingTransport::Create(
            /*core_count=*/2, 16, sched::kCoreRingPayloadBytes);
        ASSERT_TRUE(transport.ok()) << transport.status().message();
        transport_.emplace(std::move(transport.value()));
        ASSERT_TRUE(scheduler_->AttachTransport(&*transport_, 1).ok());
        coordinator_reactor_.emplace(clock_, coordinator_io_);
        ASSERT_TRUE(coordinator_reactor_->AttachTransport(&*transport_, 0).ok());

        // This core's own 2PC transport, which is what its sweep asks
        // through. The prepare and decide seams are the executor's own, as
        // in a server - the ask is the only leg this fixture drives.
        server_.emplace(/*core_id=*/1, *scheduler_, *transport_, executor_->PrepareSeam(),
                        executor_->DecideSeam(), executor_->ResolveSeam());
        ASSERT_TRUE(server_->RegisterResolveReplyReceiver().ok());
        executor_->SetTxn2pcServer(&*server_);

        // The coordinator's half: a real client, so what answers the ask is
        // the code a coordinator runs and not a stub.
        client_.emplace(/*core_id=*/0, *coordinator_reactor_, *transport_, clock_);
        ASSERT_TRUE(client_->RegisterReplyReceivers().ok());
    }

    void TearDown() override {
        if (executor_.has_value()) executor_->SetTxn2pcServer(nullptr);
    }

    void PumpBoth(int iterations = 20) {
        for (int i = 0; i < iterations; ++i) {
            (void)wal_->DrainOnce();
            scheduler_->RunOnce();
            coordinator_reactor_->RunOnce();
        }
    }

    sched::NullIoBackend coordinator_io_;
    std::optional<sched::RealRingTransport> transport_;
    std::optional<sched::Scheduler> coordinator_reactor_;
    std::optional<Txn2pcServer> server_;
    std::optional<Txn2pcClient> client_;
};

TEST_F(Txn2pcInDoubtTest, AnInDoubtParticipantAsksOncePerCeilingAndNotBefore) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());
    ASSERT_EQ(executor_->in_doubt(), 1u);

    // Under the ceiling: nothing is asked. The prepared context is not the
    // idle sweep's either, so this tick does nothing at all.
    clock_.Advance(kTxnInDoubtCeilingNs / 2);
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->in_doubt_asks(), 0u);

    clock_.Advance(kTxnInDoubtCeilingNs);
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->in_doubt_asks(), 1u);
    // And not again on the very next tick: one ask per ceiling.
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->in_doubt_asks(), 1u);
    clock_.Advance(kTxnInDoubtCeilingNs);
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->in_doubt_asks(), 2u);

    // The transaction is still prepared and still holds its rows: asking
    // changes nothing about the participant's own state.
    EXPECT_EQ(executor_->in_doubt(), 1u);
    EXPECT_EQ(executor_->enrolled(), 1u);
}

TEST_F(Txn2pcInDoubtTest, TheCoordinatorsAnswerCommitsTheTransactionTheDecideNeverReached) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    // The coordinator decided and its decide message never arrived - the
    // failure D5 is written for. `Decide` records before it sends, so the
    // record is there for the ask even though this fixture never delivers
    // the message to the participant's seam.
    const std::vector<std::uint32_t> participants{1};
    ASSERT_TRUE(client_->Decide(/*request_id=*/1, kSession, kCoordinatorTxn,
                                TxnDecision::kCommit, participants)
                    .ok());

    clock_.Advance(kTxnInDoubtCeilingNs + 1);
    executor_->ExpireEnrolled();
    EXPECT_EQ(executor_->in_doubt_asks(), 1u);
    PumpBoth(64);

    EXPECT_EQ(executor_->in_doubt_resolved_committed(), 1u);
    EXPECT_EQ(executor_->in_doubt(), 0u);
    EXPECT_EQ(executor_->enrolled(), 0u);
    EXPECT_NE(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcInDoubtTest, AnUnknownAnswerLeavesTheTransactionInDoubtRatherThanGuessing) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    // No record on the coordinator at all: the ask is answered
    // `UnknownOutcome`, which is terminal. Nothing is applied - committing
    // would be on no authority and aborting would contradict a coordinator
    // that may have committed - so the row stays invisible and the context
    // stays prepared for the next mount to resolve (R6-4).
    clock_.Advance(kTxnInDoubtCeilingNs + 1);
    executor_->ExpireEnrolled();
    PumpBoth(64);

    EXPECT_EQ(executor_->in_doubt_resolved_unknown(), 1u);
    EXPECT_EQ(executor_->in_doubt(), 1u);
    EXPECT_EQ(executor_->enrolled(), 1u);
    EXPECT_EQ(executor_->decides_committed(), 0u);
    EXPECT_EQ(executor_->decides_aborted(), 0u);
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();
}

TEST_F(Txn2pcInDoubtTest, AnUnknownAnswerEndsTheAskingRatherThanRepeatingItForEver) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    clock_.Advance(kTxnInDoubtCeilingNs + 1);
    executor_->ExpireEnrolled();
    PumpBoth(64);
    ASSERT_EQ(executor_->in_doubt_asks(), 1u);
    ASSERT_EQ(executor_->in_doubt_resolved_unknown(), 1u);

    // **`UnknownOutcome` is terminal** (D5), and terminal has to mean the
    // asking stops. The coordinator holds no record and may not re-decide,
    // so every later ask draws the same answer - two ring messages and a
    // Warn line per ceiling for the life of the process, against a
    // transaction only the next mount can finish (R6-4). It would also
    // make `in_doubt_resolved_unknown()` a count of *asks* rather than of
    // the transactions its accessor documents, which is the number
    // `SHOW META`'s `txn_in_doubt_unresolved` is read as.
    for (int i = 0; i < 4; ++i) {
        clock_.Advance(kTxnInDoubtCeilingNs + 1);
        executor_->ExpireEnrolled();
        PumpBoth(16);
    }
    EXPECT_EQ(executor_->in_doubt_asks(), 1u);
    EXPECT_EQ(executor_->in_doubt_resolved_unknown(), 1u);
    // Stopping the asks hides nothing: the transaction is still prepared,
    // still in doubt, and still holding its rows.
    EXPECT_EQ(executor_->in_doubt(), 1u);
    EXPECT_EQ(executor_->enrolled(), 1u);
}

TEST_F(Txn2pcInDoubtTest, AnAbortAnswerUnwindsTheParticipantsHalf) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());
    const std::vector<std::uint32_t> participants{1};
    ASSERT_TRUE(client_->Decide(/*request_id=*/1, kSession, kCoordinatorTxn,
                                TxnDecision::kAbort, participants)
                    .ok());

    clock_.Advance(kTxnInDoubtCeilingNs + 1);
    executor_->ExpireEnrolled();
    PumpBoth(64);

    EXPECT_EQ(executor_->in_doubt_resolved_aborted(), 1u);
    EXPECT_EQ(executor_->in_doubt(), 0u);
    EXPECT_EQ(Rows().find(",7"), std::string::npos) << Rows();
}

// ---- The blocked writer -------------------------------------------------------
//
// D5's ratified `[OPEN]`: a writer of a row an in-doubt transaction holds
// **blocks** under a ceiling rather than being refused up front. The
// participant fixture is reused because the in-doubt transaction has to be
// a real one - `Transaction::prepared` is raised by the prepare path, and a
// test that set it by hand would prove nothing about the path that sets it.

class Txn2pcBlockedWriterTest : public Txn2pcParticipantTest {
protected:
    void SetUp() override {
        Txn2pcParticipantTest::SetUp();
        dispatcher_->set_in_doubt_ceiling_ns(kTxnInDoubtCeilingNs);
    }

    // A local client's statement on the served path - the only entry point
    // that may park, which is what the block needs.
    //
    // **It fails rather than returning a half-outcome.** Before AO-S3 only
    // an in-doubt row made a statement park, so a caller could assume this
    // returned a finished one; now any undecided holder does, and a caller
    // asserting on `response` would read the pre-wait reply
    // `DispatchAndStage` left there and pass whether or not the statement
    // ever completed. Every cell that means to observe a *wait* asserts on
    // its own `done` flag instead of calling this.
    DispatchOutcome RunAsync(const std::string& sql, Session& session, int turns = 64) {
        auto out = std::make_shared<DispatchOutcome>();
        auto done = std::make_shared<bool>(false);
        scheduler_->Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground,
            dispatcher_->DispatchAsync(sql, &session, out.get()),
            [done](const Status&) { *done = true; }));
        for (int i = 0; i < turns && !*done; ++i) {
            (void)wal_->DrainOnce();
            scheduler_->RunOnce();
        }
        EXPECT_TRUE(*done) << "the statement was still parked after " << turns
                           << " turns, so what follows would read a stale reply: " << sql;
        return *out;
    }
};

TEST_F(Txn2pcBlockedWriterTest, AWriterOfAnInDoubtRowWaitsAndThenRunsWhenTheDecisionArrives) {
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_TRUE(Ship("UPDATE t SET v = 2 WHERE id = 7", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    // A local client wants the same row. It is held by a transaction this
    // core prepared, so the write blocks instead of being refused.
    Session local;
    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("UPDATE t SET v = 3 WHERE id = 7", &local, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 16 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_FALSE(*done) << "the writer answered instead of waiting: " << out->response;

    // The decision arrives well inside the ceiling, and the blocked
    // statement then runs against the row the commit left.
    ASSERT_TRUE(Decide(TxnDecision::kCommit).status.ok());
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_TRUE(*done);
    EXPECT_EQ(out->response.rfind("UPDATED", 0), 0u) << out->response;
    EXPECT_NE(Rows().find(",3"), std::string::npos) << Rows();
}

TEST_F(Txn2pcBlockedWriterTest, AtTheFaultNetTheWriterIsAbortedAndTheRefusalNamesTheNet) {
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_TRUE(Ship("UPDATE t SET v = 2 WHERE id = 7", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    Session local;
    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("UPDATE t SET v = 3 WHERE id = 7", &local, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 16 && !*done; ++i) scheduler_->RunOnce();
    ASSERT_FALSE(*done);

    // **AO-S3 moved this bound from a ceiling to a fault net** (AO-R8).
    // 200 ms used to be an ordinary outcome: wait a little, then refuse.
    // A wait that ends by clock reintroduces a refusal *after* the work was
    // done (AR2-R10), so the wait now ends when the holder decides, and
    // this bound fires only when something is broken. Past the old ceiling
    // the statement is still waiting, which is the behaviour change stated.
    clock_.Advance(kTxnInDoubtCeilingNs + 1);
    for (int i = 0; i < 64 && !*done; ++i) scheduler_->RunOnce();
    ASSERT_FALSE(*done) << "the old 200 ms ceiling still ended the wait: " << out->response;

    // Past the net it is aborted, because a hang is not the alternative
    // (HP3) - the net exists so a missed cycle or a stuck holder cannot
    // block forever.
    clock_.Advance(txn::kLockWaitFaultNetNs);
    for (int i = 0; i < 64 && !*done; ++i) scheduler_->RunOnce();
    ASSERT_TRUE(*done) << "the wait has no bound at all, which is the hang HP3 forbids";

    const Status refused = StatusFromErrorReply(out->response);
    // Retryable, so a client's loop reads the bit the engine means; *not*
    // `UnknownOutcome`, which would tell a client to read back data its
    // statement never touched; and named as the **net**, so an operator
    // meeting it looks for the fault rather than concluding the row was
    // busy.
    EXPECT_TRUE(refused.retryable()) << out->response;
    EXPECT_NE(refused.code(), StatusCode::kUnknownOutcome);
    EXPECT_EQ(refused.code(), StatusCode::kTxnConflict);
    EXPECT_NE(out->response.find("fault net"), std::string::npos) << out->response;
    // The carried status too: a KWP client reads that one, and it held the
    // pre-wait conflict rather than the fault the net exists to report.
    EXPECT_NE(out->status.message().find("fault net"), std::string::npos)
        << out->status.message();
    EXPECT_EQ(out->response.find("coordinator"), std::string::npos)
        << "the net is the lock family's, not 2PC's: " << out->response;
    // The holder is untouched by the refusal - R1 aborts the waiter and
    // never the holder - so it is still prepared and still holding the row.
    EXPECT_EQ(executor_->in_doubt(), 1u);
}

TEST_F(Txn2pcBlockedWriterTest, TheCeilingKnobNoLongerEndsTheWritersWait) {
    // **D5's "refuse at once" branch is gone, and this cell records it**
    // rather than leaving a key that reads as an off-switch. `0` used to
    // mean "refuse retryably up front", the branch the operator did not
    // ratify, kept reachable by configuration so the two could be measured
    // against each other. AO-S3 ends the wait on the holder's decide
    // instead of on any clock, so the knob reaches nothing: the writer
    // waits at `0` exactly as it waits at 200.
    //
    // Left as an inert key rather than refused at startup, so a
    // configuration carrying it still mounts; AO-R8 gives its re-scope to
    // M3, and AO-0 item 7 is where the operator's word on that sits.
    dispatcher_->set_in_doubt_ceiling_ns(0);
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_TRUE(Ship("UPDATE t SET v = 2 WHERE id = 7", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    Session local;
    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("UPDATE t SET v = 3 WHERE id = 7", &local, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    EXPECT_FALSE(*done) << "a ceiling of 0 still ended the wait, so the key is not inert after "
                           "all: " << out->response;

    // And it ends the way every wait now ends - on the decision.
    ASSERT_TRUE(Decide(TxnDecision::kCommit).status.ok());
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    EXPECT_TRUE(*done);
    EXPECT_EQ(out->response.rfind("UPDATED", 0), 0u) << out->response;
}

TEST_F(Txn2pcBlockedWriterTest, AWriterInsideATransactionIsNotPoisonedWhileItIsStillWaiting) {
    // The block withholds the poison `EndWrite` would otherwise apply,
    // because a poisoned session cannot run the statement again - and a
    // wait whose end is a forced ROLLBACK is not a wait. The poison lands
    // only if the ceiling is reached, at which point the statement has
    // genuinely failed.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_TRUE(Ship("UPDATE t SET v = 2 WHERE id = 7", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    Session local;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &local).response.rfind("BEGIN", 0), 0u);
    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("UPDATE t SET v = 3 WHERE id = 7", &local, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 16 && !*done; ++i) scheduler_->RunOnce();
    ASSERT_FALSE(*done);
    EXPECT_FALSE(local.failed()) << "a waiting statement has not failed yet";

    ASSERT_TRUE(Decide(TxnDecision::kCommit).status.ok());
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_TRUE(*done);
    EXPECT_EQ(out->response.rfind("UPDATED", 0), 0u) << out->response;
    EXPECT_FALSE(local.failed());
    EXPECT_EQ(dispatcher_->Dispatch("COMMIT", &local).response.rfind("COMMIT", 0), 0u);
}

TEST_F(Txn2pcBlockedWriterTest, ThePathThatCannotWaitPoisonsExactlyAsItAlwaysDid) {
    // **The poison is withheld for the wait, so where there is no wait it
    // must stand.** `Dispatch()` has no reactor to park on and answers the
    // conflict itself; a failed statement inside an explicit transaction
    // poisons the session whatever refused it (txn.md section 6 -
    // failure atomicity is per transaction), and a session left unpoisoned
    // here would tell the client `ERR` and then let its COMMIT succeed
    // without the statement. The same holds for a dispatcher with no clock,
    // which is the other arm the block cannot be taken on.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_TRUE(Ship("UPDATE t SET v = 2 WHERE id = 7", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    Session local;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &local).response.rfind("BEGIN", 0), 0u);
    const DispatchOutcome out = dispatcher_->Dispatch("UPDATE t SET v = 3 WHERE id = 7", &local);
    EXPECT_EQ(out.response.rfind("ERR ", 0), 0u) << out.response;
    EXPECT_EQ(StatusFromErrorReply(out.response).code(), StatusCode::kTxnConflict);
    // Its own words, not the block's: nothing waited, so nothing may claim
    // to have waited (HP4 - a pre-existing refusal keeps its spelling).
    EXPECT_EQ(out.response.find("coordinator"), std::string::npos) << out.response;
    EXPECT_TRUE(local.failed()) << "the failed statement left the transaction committable";
    EXPECT_NE(dispatcher_->Dispatch("COMMIT", &local).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(dispatcher_->Dispatch("ROLLBACK", &local).response.rfind("ROLLBACK", 0), 0u);
}

// ---- AO-S4a: the wait-for graph and the victim ---------------------------

// The same fixture with a lock table handed to the dispatcher, which is
// what turns AO-S3's narrow rule ("only a transaction holding nothing may
// wait") into AO-S4a's ("any transaction may wait, and the one that closes
// a cycle is aborted").

// AO-S6a's borrow cap, on a table sized so three rows reach it: one entry
// for the relation `IX` and one per row, so a cap of 3 admits the relation
// and two rows and stops on the third.
//
// **The write proceeds past the cap** (the operator's decision of
// 2026-09-08). At S6a a borrow protects nothing - a `TryAcquire` the
// dispatcher declines to wait for changes no verdict - so a ledger that
// has run out must not fail a statement that would otherwise succeed. It
// stops recording and says so through `borrow_cap_stops()`, which is what
// keeps the truncation visible rather than silent. **S6b propagates
// instead**, where the lock is the wait and an incomplete ledger is an
// incorrect one.
class LockCapTest : public Txn2pcBlockedWriterTest {
protected:
    std::optional<std::size_t> LockCap() const override { return 3; }
};

TEST_F(LockCapTest, AWritePastTheBorrowCapIsRefusedAndNamesTheCap) {
    // **The cap refuses since AO-S6c-c.** It was swallowed while the borrow
    // was advisory, on the operator's decision of 2026-09-08: a ledger that
    // guards nothing must not fail a statement that would otherwise
    // succeed. That condition has lapsed - the lock is the wait now, and a
    // fence stops an insert - so a truncated ledger would be rows with no
    // fence over them and no wait behind them, which is AO-R10's reason for
    // refusing instead.
    //
    // The predicate names no pk window, which is the shape item 14's
    // declared units deliberately do not reach, so it still accumulates one
    // entry per row - and is the one shape that can meet the cap at all.
    Session session;
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (3, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (4, 0)").rfind("INSERTED", 0), 0u);

    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE v = 0", session);
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted) << out.response;
    EXPECT_FALSE(IsRetryable(out.status.code()))
        << "a retry meets the same cap, so the wire's retryable bit must not invite one";
    EXPECT_EQ(out.resource_detail, static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap))
        << "the client's fix for this is a shorter transaction, not a smaller statement, which "
           "is why protocol.md gave it a detail of its own - and it had no setter until now";
    EXPECT_GE(dispatcher_->borrow_cap_stops(), 1u);
}

TEST_F(LockCapTest, AWriteInsideTheBorrowCapCountsNoTruncation) {
    Session session;
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);

    // Two rows plus the relation is exactly the cap, and the cap refuses
    // the entry *at* it rather than past it - so two rows fit in three.
    // Per-row for the reason the cell above states.
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE v = 0", session);
    EXPECT_EQ(out.response, "UPDATED 2") << out.response;
    EXPECT_EQ(dispatcher_->borrow_cap_stops(), 0u)
        << "a statement inside the cap must record every borrow it takes";
}

// ---- AO-0 item 14, marked 2026-09-08: the unit a bulk write declares ----
//
// Both cells are sized so the *difference* is what fails them: four rows
// under a cap of three, where per-row accumulation reaches the cap and a
// declared coarse unit does not. Without the declaration each of these
// counts a truncation.

TEST_F(LockCapTest, AWhereLessWriteDeclaresTheRelationAndTakesOneBorrow) {
    Session session;
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (3, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (4, 0)").rfind("INSERTED", 0), 0u);

    // A write with no predicate touches every row there is, so it declares
    // the relation: one entry for four rows, and a cap of three is never
    // approached.
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9", session);
    EXPECT_EQ(out.response, "UPDATED 4") << out.response;
    EXPECT_EQ(dispatcher_->borrow_cap_stops(), 0u)
        << "a declared relation unit is one borrow; accumulating per row would reach the cap "
           "at the third of these four rows";
}

// ---- AO-S6c-a: every writer takes its borrow, and gives it back --------

TEST_F(LockCapTest, AnAutocommitStatementGivesItsBorrowsBackAtItsDecide) {
    // **The cell that would have caught the half-wired fixture.** Borrows
    // are released by the *manager* at a decide (AO-R6), not by the
    // dispatcher, so a table wired into one and not the other takes
    // tenancies and never gives them back - and the next statement then
    // conflicts with rows whose writers committed long ago. That is what
    // this fixture did until AO-S6c, invisibly, because only UPDATE and
    // DELETE borrowed and no cell ran two writes over the same rows.
    ASSERT_EQ(locks_->EntryCount(), 0u) << "nothing is held before the first statement";
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);
    EXPECT_EQ(locks_->EntryCount(), 0u)
        << "two committed autocommit inserts left tenancies in the table; the manager is what "
           "releases them and it must have been given the table";

    Session session;
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE v = 0", session);
    ASSERT_EQ(out.response, "UPDATED 2") << out.response;
    EXPECT_EQ(locks_->EntryCount(), 0u) << "and an autocommit UPDATE's borrows go the same way";
}

// A cap of one, so the *declaration itself* is what the cap refuses.
class LockCapOfOneTest : public Txn2pcBlockedWriterTest {
protected:
    std::optional<std::size_t> LockCap() const override { return 1; }
};

TEST_F(LockCapOfOneTest, ADeclaredUnitTheCapRefusesEndsTheStatement) {
    // The behaviour that replaced `ARefusedDeclarationFallsBackToThePerRowBorrow`
    // when AO-S6c-c removed the demotion, and the review's B4: under a cap
    // of one the relation `IX` fills the ledger and the range under it is
    // refused, so the statement ends there having written nothing rather
    // than falling back to a per-row walk that would meet the same cap on
    // its first row.
    //
    // No row is seeded, and none is needed: under this cap no INSERT can
    // succeed either, and the declaration is borrowed **before** the walk -
    // so what the cell measures happens whether or not the relation is
    // empty, which is itself the point.
    Session session;
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE id > 0", session);
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted) << out.response;
    EXPECT_EQ(out.resource_detail, static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap))
        << out.response;
}

TEST_F(LockCapOfOneTest, AnInsertTakesTheBorrowOfTheRowItWrites) {
    // AO-S6c-a. Until this sub-stage an INSERT borrowed nothing, which was
    // not a gap but the thing standing between AO-S6 and its own goal: the
    // wait's blocker is moving from the tuple header to the lock table, and
    // a row whose only writer was an INSERT would have a header `trx_id`
    // and no entry - so the table alone would answer that the row is free.
    //
    // A cap of one is what makes the borrow observable without a second
    // session: the relation `IX` fills the ledger, so the row's own tuple
    // is refused by the cap and counted. A statement that borrowed nothing
    // would count nothing.
    // A cap of one admits the relation `IX` and nothing under it, so an
    // insert that borrows its row is refused and an insert that borrows
    // nothing sails through. Since AO-S6c-c that refusal is the statement's,
    // which is a sharper observation than the counter this used to read.
    // **Observed on the outcome, not on the rendered line.** A text-only
    // check passed while the cap's refusal was reaching a KWP client as
    // `INVALID_ARGUMENT` carrying the cap's detail - the review's B1 - so
    // the category and the detail are what this asserts.
    Session session;
    const DispatchOutcome out = RunAsync("INSERT INTO t VALUES (1, 0)", session);
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted)
        << "the INSERT took no borrow, so the wait's blocker cannot come from the table: "
        << out.response;
    EXPECT_EQ(out.resource_detail, static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap))
        << out.response;
}

TEST_F(LockCapOfOneTest, ASortedFillBorrowsTheIdBlockItCarved) {
    // The INSERT path that routes **past** `InsertOneRow`: `t` is HEAP with
    // no var-heap, no index, no cabin and no assertion, so a multi-row
    // VALUES whose every row omits its pk goes to `SortedFillInner`. Until
    // AO-S6c-a it wrote rows and borrowed nothing at all - the one writer
    // left in the "header `trx_id`, no table entry" shape the sub-stage
    // exists to remove, and the review is what found it.
    //
    // One `Range` over the carved block rather than an entry per row, so
    // the observation is the same as any coarse unit's under a cap of one:
    // the relation `IX` fills the ledger and the range itself is refused
    // and counted. A run that borrowed nothing would count nothing.
    // As the single-row cell above: the cap admits the relation `IX` and
    // refuses the range under it, so a fill that borrows its block is
    // refused and one that borrows nothing is not.
    Session session;
    const DispatchOutcome out = RunAsync("INSERT INTO t VALUES (5), (6)", session);
    EXPECT_EQ(out.status.code(), StatusCode::kResourceExhausted)
        << "the sorted fill placed rows without borrowing the ids it carved: " << out.response;
    EXPECT_EQ(out.resource_detail, static_cast<std::uint16_t>(wire::ResourceDetail::kLockCap))
        << out.response;
}

TEST_F(LockCapTest, ARangePredicateDeclaresItsWindowRatherThanAccumulatingRows) {
    Session session;
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (3, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (4, 0)").rfind("INSERTED", 0), 0u);

    // `id > 1` is a pk window of three rows. Declared, it is the relation
    // `IX` plus one range - two entries under a cap of three. Per-row it
    // would be the `IX` plus three tuples, which reaches the cap on the
    // last of them.
    const DispatchOutcome out = RunAsync("UPDATE t SET v = 9 WHERE id > 1", session);
    EXPECT_EQ(out.response, "UPDATED 3") << out.response;
    EXPECT_EQ(dispatcher_->borrow_cap_stops(), 0u)
        << "a range-shaped predicate declares one window; accumulating its rows would reach "
           "the cap";
}

class LockDeadlockTest : public Txn2pcBlockedWriterTest {
protected:
    std::optional<std::size_t> LockCap() const override { return txn::kMaxLocksPerTxnDefault; }

    // Starts a statement on the served path and returns its handles; the
    // caller decides what it is waiting to observe, which is the only way
    // to tell a park from a slow grant.
    //
    // **It owns the statement text, and that is not tidiness.**
    // `DispatchAsync` takes a `std::string_view` and is a coroutine, so the
    // view is copied into the frame while the characters are not: the
    // caller must keep them alive until the statement finishes. Every cell
    // that calls `DispatchAsync` directly passes a string *literal*, which
    // has static storage and hides the requirement; a helper taking
    // `const std::string&` binds a temporary that dies at the end of the
    // caller's statement, long before the first `Pump`, and the parked
    // coroutine then parses freed memory. That reads as `ERR unknown
    // command` from a statement that is plainly a valid `UPDATE`.
    struct Started {
        std::shared_ptr<std::string> sql = std::make_shared<std::string>();
        std::shared_ptr<DispatchOutcome> out = std::make_shared<DispatchOutcome>();
        std::shared_ptr<bool> done = std::make_shared<bool>(false);
    };

    Started Start(std::string sql, Session& session) {
        Started s;
        *s.sql = std::move(sql);
        scheduler_->Submit(sched::MakeCoroTask(
            sched::SchedulingGroup::kForeground,
            dispatcher_->DispatchAsync(*s.sql, &session, s.out.get()),
            [d = s.done](const Status&) { *d = true; }));
        return s;
    }

    void Pump(int turns = 64) {
        for (int i = 0; i < turns; ++i) {
            (void)wal_->DrainOnce();
            scheduler_->RunOnce();
        }
    }
};

// ---- AO-S6e-b: the read borrow, and the DDL that waits for one ---------

// The oid an INSERT reports, which is how a cell names a relation the
// dispatcher created (`drop-table.md` DT2 uses the same handle).
std::uint64_t OidIn(const std::string& insert_reply) {
    const auto at = insert_reply.find("oid=");
    EXPECT_NE(at, std::string::npos) << insert_reply;
    return at == std::string::npos ? 0 : std::strtoull(insert_reply.c_str() + at + 4, nullptr, 10);
}

TEST_F(LockDeadlockTest, AReadDeclaresItsPositionAndGivesItBack) {
    // AR2 §3's `SELECT` row: the borrow is **the statement's**. Taken at
    // the bind since AT-S1 (`step_compiler.cpp`'s seam), re-reported into
    // by the walk, and released when the statement ends,
    // which is what `EntryCount` reads here - a read that kept its position
    // would leave the relation entry standing and a later `DROP TABLE`
    // would wait for a reader that finished long ago.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO rb VALUES (1, 1)").rfind("INSERTED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    const std::string rows = Local("SELECT * FROM rb");
    EXPECT_NE(rows.rfind("ERR", 0), 0u) << rows;
    EXPECT_EQ(dispatcher_->read_borrows(), before + 1)
        << "the walk declared no position at all";
    EXPECT_EQ(locks_->EntryCount(), 0u) << "a statement-scoped borrow outlived its statement";
}

TEST_F(LockDeadlockTest, ADropWaitsForAPositionedReaderAndThenRunsAgain) {
    // The AO-S6 row's cell. The reader's borrow is held here directly
    // rather than by a running `SELECT`, and that is a property of the
    // engine rather than of the cell: a local read is synchronous, so a
    // statement on this core cannot observe another one mid-walk. What the
    // holder stands for is exactly what a walk takes -
    // `Relation(oid)` in `IS` under a read holder id - and
    // `AReadDeclaresItsPositionAndGivesItBack` above is what says a real
    // read takes it.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 7;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    Session s;
    Started drop = Start("DROP TABLE rb", s);
    Pump();
    ASSERT_FALSE(*drop.done) << "the drop ran over a positioned reader: " << drop.out->response;

    // And the wait ends where the position does. The re-run is a whole
    // statement - the drop had written nothing when it was refused - so
    // what the client gets is the reply the first attempt would have given.
    locks_->Release(reader_id, reader);
    Pump();
    ASSERT_TRUE(*drop.done) << "the drop never resumed after the reader released";
    EXPECT_EQ(drop.out->response.rfind("DROPPED TABLE rb", 0), 0u) << drop.out->response;
    EXPECT_EQ(locks_->EntryCount(), 0u) << "the wake registration outlived its wait";
}

TEST_F(LockDeadlockTest, ADropWithNoReactorToParkOnNamesThePositionedReader) {
    // The synchronous path has no reactor, so its honest answer is the
    // refusal itself - the same division `index_window` and `write_block`
    // draw. What it must not do is name a transaction: the holder is a
    // statement's read borrow, and an operator sent looking for
    // "transaction 9223372036854775815" would be looking for something that
    // never existed.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 7;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    const std::string refused = Local("DROP TABLE rb");
    EXPECT_EQ(refused.rfind("ERR", 0), 0u) << refused;
    EXPECT_NE(refused.find("a positioned reader"), std::string::npos) << refused;

    locks_->Release(reader_id, reader);
    EXPECT_EQ(Local("DROP TABLE rb").rfind("DROPPED TABLE rb", 0), 0u);
}

TEST_F(LockDeadlockTest, AWholeRelationWriteIsNotHeldUpByAPositionedReader) {
    // **The rule at the unit where it actually lands** (the AO-S6e-b
    // review's B2). `ConflictingOverlap` skips intention holders, so a
    // reader's slice `IS` passes a declared range - but a `WHERE`-less
    // write used to collapse onto the **relation** unit, where the conflict
    // is decided by the entry's own compatibility test and `IS` against `X`
    // is a refusal. So the rule was written on one path and the traffic ran
    // down another: a scan of the relation would have refused
    // `DELETE FROM rb`, and refused rather than waited, because a read
    // borrow's holder is not a transaction and nothing can wait for it.
    //
    // Since AO-S6e-b a `WHERE`-less write declares the whole id space as a
    // range: the relation unit means the relation as an object, which is
    // what DDL claims, and a write's claim is over keys.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 13;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);
    // And the slice under it, which is the other half of what a walk holds.
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id,
                              txn::LockKey::Slice(static_cast<catalog::Oid>(oid), 1,
                                                  catalog::kIdSpaceEnd),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    const std::string deleted = Local("DELETE FROM rb");
    EXPECT_EQ(deleted.rfind("DELETED", 0), 0u)
        << "a positioned reader held up a write of every row, which MVCC answers it from its "
           "snapshot: "
        << deleted;

    const std::string rows = Local("SELECT * FROM rb");
    EXPECT_NE(rows.rfind("ERR", 0), 0u) << "the read that follows it is refused: " << rows;

    locks_->Release(reader_id, reader);
}

TEST_F(LockDeadlockTest, ADropInsideATransactionWaitsWithoutPoisoningIt) {
    // The shape the autocommit cells do not reach, and the one that would
    // have made the wait worse than the refusal it replaced: a `DROP TABLE`
    // inside an explicit transaction (DT5) that meets a reader. `EndWrite`
    // poisons a transaction whose statement failed - so without the rule
    // that withholds it while a wait is still possible, the re-run would
    // answer "transaction is aborted", non-retryable, where the client used
    // to get its relation dropped. It is `blocking_writer_`'s own rule
    // applied to the second failure that a wait can get past.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 9;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    Session s;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &s).response.rfind("BEGIN", 0), 0u);
    Started drop = Start("DROP TABLE rb", s);
    Pump();
    ASSERT_FALSE(*drop.done) << "the drop ran over a positioned reader: " << drop.out->response;

    locks_->Release(reader_id, reader);
    Pump();
    ASSERT_TRUE(*drop.done) << "the drop never resumed";
    EXPECT_EQ(drop.out->response.rfind("DROPPED TABLE rb", 0), 0u) << drop.out->response;

    // And the transaction is still a transaction: a poisoned one answers
    // this with "transaction is aborted".
    EXPECT_EQ(dispatcher_->Dispatch("COMMIT", &s).response.rfind("COMMIT", 0), 0u);
}

TEST_F(LockDeadlockTest, ADropWhoseWaitReachesTheFaultNetPoisonsItsTransaction) {
    // **The exit the second review found untested, and it was wrong.**
    // `EndWrite` withholds the poison a failed statement owes an explicit
    // transaction while a wait is still possible - or the re-run would
    // answer "transaction is aborted" instead of dropping the relation.
    // Two of the three exits restore it (the victim through
    // `RefuseParkedWrite`, a failing re-run through `EndWrite` itself, the
    // field being empty by then); the fault net is the third, and without
    // it an eleven-second refusal left the transaction usable and
    // committable - §6's failure atomicity broken by a wait that is
    // supposed to be invisible when it works.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings reader;
    const std::uint64_t reader_id = txn::kReadHolderBit | 21;
    ASSERT_TRUE(locks_
                    ->Acquire(reader_id, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kIntentionShared, reader)
                    .value()
                    .granted);

    Session s;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &s).response.rfind("BEGIN", 0), 0u);
    Started drop = Start("DROP TABLE rb", s);
    Pump();
    ASSERT_FALSE(*drop.done) << drop.out->response;

    // The holder never releases. The net is the only thing that can end
    // this, and reaching it is a defect report rather than an outcome
    // (AO-R8) - which does not make it any less a failed statement.
    clock_.Advance(txn::kLockWaitFaultNetNs + 1);
    Pump();
    ASSERT_TRUE(*drop.done) << "the wait outlived its own fault net";
    EXPECT_EQ(drop.out->response.rfind("ERR", 0), 0u) << drop.out->response;
    const std::string commit = dispatcher_->Dispatch("COMMIT", &s).response;
    EXPECT_EQ(commit.rfind("ERR", 0), 0u)
        << "the transaction committed after a statement of it failed: " << commit;

    locks_->Release(reader_id, reader);
}

// ---- AT-S3: the sys.tables row has no contender the page latch does not serialise --
//
// E13 asked whether the relation's `sys.tables` row becomes borrowable at
// the tuple unit - `X` on the row - so a named-key `INSERT` waits instead
// of shipping. The answer is no, and this cell pins why. Admitting a named
// key writes the row's mark or flips its key order **outside the caller's
// transaction** (`wal::kNoTxnId`; `heap-and-tuple.md` §4.1: "both writes
// outlive a rollback"), under the page latch - a monotone in-place
// overwrite, not a row version. A transaction-length `X` on that row would
// serialise every named-key insert into a relation for the length of each
// transaction, and protect nothing the latch does not. What a peer waits
// on to stop shipping is the page write itself, which is `MayWrite`'s last
// arm and AT-S5's.

TEST_F(LockDeadlockTest, TwoTransactionsNamedKeysIntoOneRelationDoNotWaitOnItsCatalogRow) {
    ASSERT_EQ(Local("CREATE TABLE nk (id int64, v int64)").rfind("CREATED", 0), 0u);

    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO nk VALUES (7, 1)", &a).response.rfind("INSERTED", 0),
              0u);
    // `a` holds its user row's `X` and has moved the relation's mark. A
    // second transaction naming another key admits at once: the only thing
    // it shares with `a` is the catalog row, and nothing borrows that.
    Session b;
    Started second = Start("INSERT INTO nk VALUES (8, 1)", b);
    Pump();
    ASSERT_TRUE(*second.done) << "a named key waited on another transaction's catalog-row write";
    EXPECT_EQ(second.out->response.rfind("INSERTED", 0), 0u) << second.out->response;

    // And the ledger holds `a`'s two borrows on the user relation - the
    // relation `IX` and row 7's `X` (AO-S6a) - and nothing keyed on the
    // catalog relation, which is the third entry a row `X` would have added.
    // Probed, not only counted: an `X` on the catalog relation is granted
    // at once, which it could not be if anything stood there.
    EXPECT_EQ(locks_->EntryCount(), 2u) << "a borrow stands on something other than a's relation and row";
    txn::LockHoldings probe;
    auto free = locks_->TryAcquire(/*txn=*/4242, txn::LockKey::Relation(catalog::kSysTablesTable),
                                   txn::LockMode::kExclusive, probe);
    ASSERT_TRUE(free.ok());
    EXPECT_TRUE(free.value()) << "something holds the catalog relation while a's key is admitted";
    locks_->Release(4242, probe);
    // This fixture is one core over an in-memory store, where no page latch
    // is armed: it proves the ledger claim and nothing about the latch.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(locks_->EntryCount(), 0u);
}

// ---- AT-S1: the declaration moves to the bind ---------------------------
//
// AT-R1 (`instructions/v3.0.0/workorder-at-m3-uniformity.md`). Until this
// stage a statement declared its relation from the **outermost walk**, at
// `step_vm.cpp`'s `index == 0` guard - so a join's inner relation, a
// subquery's relation and a write's own relation were resolved and executed
// against with no `IS` held, and a DDL's `X` was granted over them. The
// declaration is now made by the compiler at the bind, between the name and
// the schema, which is what closes the window `ar0-5-amendment-uniformity.md`
// §8 names: a DDL cannot commit and release between this statement reading a
// layout and holding a position over it.
//
// `read_borrows()` counts **granted** asks, one per relation per statement,
// which is what these cells read. The defence is one-directional and
// `read_borrow.hpp` says why: a refused ask is read on, and
// `ARefusedReadBorrowLeavesTheReaderReadingOn` below already pins that.
// A remote step's half is `RemoteStepServiceTest`'s.

TEST_F(LockDeadlockTest, AJoinDeclaresItsInnerRelationAtTheBind) {
    // Two relations bound, two declarations. Before AT-S1 this was one:
    // `step_vm.cpp:1960`'s guard reports only for `index == 0`, so the
    // inner side of a walked join declared nothing at all and its schema
    // was read under no borrow.
    ASSERT_EQ(Local("CREATE TABLE jo (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE ji (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO jo VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO ji VALUES (1, 1)").rfind("INSERTED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    const std::string rows = Local("SELECT jo.v, ji.v FROM jo JOIN ji ON jo.id = ji.id");
    ASSERT_NE(rows.rfind("ERR", 0), 0u) << rows;
    EXPECT_EQ(dispatcher_->read_borrows(), before + 2)
        << "the inner relation of a join was bound without being declared";
    EXPECT_EQ(locks_->EntryCount(), 0u) << "a statement-scoped borrow outlived its statement";
}

TEST_F(LockDeadlockTest, ASubqueryRelationIsDeclaredAtItsOwnBind) {
    // The nested block binds through the same loop
    // (`step_compiler.cpp`'s "the one site that covers FROM, every JOIN and
    // every subquery block"), so the declaration reaches it without the
    // compiler knowing anything about locks.
    ASSERT_EQ(Local("CREATE TABLE so (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE si (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO so VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO si VALUES (1, 1)").rfind("INSERTED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    const std::string rows = Local("SELECT v FROM so WHERE id IN (SELECT id FROM si)");
    ASSERT_NE(rows.rfind("ERR", 0), 0u) << rows;
    EXPECT_EQ(dispatcher_->read_borrows(), before + 2)
        << "a subquery's relation was resolved under no borrow";
    EXPECT_EQ(locks_->EntryCount(), 0u);
}

TEST_F(LockDeadlockTest, AnInsertDeclaresItsRelationAtResolveToo) {
    // The third write verb, which the first draft of this stage missed: an
    // INSERT resolves its relation's layout exactly as the others do, and
    // its own borrows are taken rows later.
    ASSERT_EQ(Local("CREATE TABLE ins (id int64, v int64)").rfind("CREATED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    ASSERT_EQ(Local("INSERT INTO ins VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    EXPECT_EQ(dispatcher_->read_borrows(), before + 1)
        << "an INSERT resolved its relation under no borrow";
    EXPECT_EQ(locks_->EntryCount(), 0u) << "the insert's read borrow outlived its statement";
}

TEST_F(LockDeadlockTest, AWriteDeclaresItsRelationAtResolveToo) {
    // A write resolves a schema exactly as a read does and carries the same
    // window - its own `IX` is taken well after the compile. The
    // declaration is an `IS`, which is compatible with that `IX`, so it
    // costs the writer nothing and gives the compile a `SELECT`'s
    // protection.
    ASSERT_EQ(Local("CREATE TABLE wr (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO wr VALUES (1, 1)").rfind("INSERTED", 0), 0u);

    const std::uint64_t before = dispatcher_->read_borrows();
    ASSERT_EQ(Local("UPDATE wr SET v = 2 WHERE id = 1").rfind("UPDATED", 0), 0u);
    EXPECT_EQ(dispatcher_->read_borrows(), before + 1)
        << "an UPDATE resolved its relation under no borrow";
    EXPECT_EQ(locks_->EntryCount(), 0u) << "the write's read borrow outlived its statement";
}

TEST_F(LockDeadlockTest, ADropThatWouldCloseACycleIsTheVictimRatherThanWaiting) {
    // The wait's other half, and the reason it draws an edge at all. A
    // relation `X` is refused by the `IX` of any *writer* of that relation,
    // not only by a reader's `IS` - so a transaction that holds rows and
    // then drops a relation somebody else is writing can be one half of a
    // cycle, and the holder it waits for can be waiting on a row it holds.
    // An autocommit drop holds nothing and registers nothing; this one
    // holds a row, so the registration is what refuses it (AO-R7: the
    // waiter that closes the cycle is the victim).
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO rb VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);

    // A holds a row of `t`; B holds a row of `rb` - and so `IX` on `rb`.
    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 1 WHERE id = 5", &a).response, "UPDATED 1");
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE rb SET v = 2 WHERE id = 1", &b).response, "UPDATED 1");

    // B waits for A's row: one edge, `B -> A`.
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 5", b);
    Pump();
    ASSERT_FALSE(*wb.done) << "B did not wait for A: " << wb.out->response;

    // A now drops the relation B holds `IX` on. The edge `A -> B` closes
    // the cycle, so A's statement is refused naming deadlock instead of
    // parking on a slot nothing would flip until the fault net.
    //
    // On the served path, because that is where a wait exists to be
    // refused: the synchronous `Dispatch` registers no wake and takes no
    // edge, and its honest answer is the plain conflict.
    Started drop = Start("DROP TABLE rb", a);
    Pump();
    ASSERT_TRUE(*drop.done) << "the victim parked instead of being refused";
    EXPECT_NE(drop.out->response.find("deadlock"), std::string::npos) << drop.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 1u) << "the victim's edge outlived its refusal";

    // The survivor proceeds on A's rollback, which is what the victim's
    // message tells the client to do.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &a).response.rfind("ROLL", 0), 0u);
    Pump();
    EXPECT_TRUE(*wb.done) << "B never resumed after A rolled back";
}

TEST_F(LockDeadlockTest, AReadIsNotRefusedByADropThatHoldsTheRelation) {
    // **A read borrow never refuses a read and never makes one wait.** The
    // reader needs no protection to be correct - DT1 leaves its pages
    // allocated and its oid never reissued - so a refused ask leaves it
    // holding nothing and reading on. It is also what keeps a reader out of
    // the wait-for graph, which is why a DDL waiting for one cannot be in a
    // cycle through it.
    ASSERT_EQ(Local("CREATE TABLE rb (id int64, v int64)").rfind("CREATED", 0), 0u);
    const std::uint64_t oid = OidIn(Local("INSERT INTO rb VALUES (1, 1)"));
    ASSERT_NE(oid, 0u);

    txn::LockHoldings ddl;
    ASSERT_TRUE(locks_
                    ->Acquire(4242, txn::LockKey::Relation(static_cast<catalog::Oid>(oid)),
                              txn::LockMode::kExclusive, ddl)
                    .value()
                    .granted);

    const std::uint64_t before = dispatcher_->read_borrows();
    const std::string rows = Local("SELECT * FROM rb");
    EXPECT_NE(rows.rfind("ERR", 0), 0u) << "a read waited for or was refused by a DDL: " << rows;
    EXPECT_EQ(dispatcher_->read_borrows(), before)
        << "the ask was refused, so no position was declared - and the read ran anyway";
    locks_->Release(4242, ddl);
}

// ---- AO-S6c-b: the lock is what the statement waits for ----------------

TEST_F(LockDeadlockTest, AWriteBlockedByARangeFenceWaitsForItsHolder) {
    // **The wait the tuple header could not express.** A declares a window
    // over `(4, ...)` whose second conjunct matches nothing, so it holds
    // `Range(t, 5, ...)` and writes **no row** - which is the whole point:
    // row 5's header still names the committed inserter, so the MVCC check
    // passes it and first-updater-wins has nothing to say. Before AO-S6c-b
    // the blocker came from that header, so B had nobody to wait for and
    // the fence either refused it outright or, worse, was not consulted at
    // the wait at all.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    const DispatchOutcome fenced =
        dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 4 AND v = 99", &a);
    ASSERT_EQ(fenced.response, "UPDATED 0") << fenced.response;

    // B writes a row inside A's window. Nothing about the row itself
    // refuses it; the declared range does.
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 5", b);
    Pump();
    ASSERT_FALSE(*wb.done) << "B did not wait on the fence: " << wb.out->response;

    // And the wait ends where every wait in this family ends - at the
    // holder's decide, with the statement run again.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*wb.done) << "B never resumed after the fence was released";
    EXPECT_EQ(wb.out->response, "UPDATED 1") << wb.out->response;
}

TEST_F(LockDeadlockTest, AnInsertIntoAFencedWindowWaitsForItsHolder) {
    // AO-S6c-c's first piece, and the gap AO-S6c-b's review named: until
    // this cell an INSERT wrote straight through a range fence, because the
    // insert sites took their borrow and never read the answer. There is no
    // header here to fall back on - the row does not exist yet - so the
    // borrow is the *only* thing that can refuse it, which is why a fence
    // guarded its holder against writers of rows that exist and not against
    // rows that appear.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    const DispatchOutcome fenced =
        dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 4 AND v = 99", &a);
    ASSERT_EQ(fenced.response, "UPDATED 0") << fenced.response;

    // Above the relation's high-water mark, because `t` is heap-clustered
    // and its chain grows only at the tail - a key below the mark is a
    // btree-only shape and would be refused before the borrow is reached.
    Started wb = Start("INSERT INTO t VALUES (100, 1)", b);
    Pump();
    ASSERT_FALSE(*wb.done) << "the insert went through A's fence: " << wb.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*wb.done) << "the insert never resumed after the fence was released";
    EXPECT_EQ(wb.out->response.rfind("INSERTED", 0), 0u) << wb.out->response;
}

TEST_F(LockDeadlockTest, AnIllegalKeyIsRefusedWithoutWaitingOnAFence) {
    // The regression AO-S6c-c's own first draft introduced and its review
    // caught. Moving the borrow ahead of `AdmitExplicitRowId` was right for
    // the re-run, but that call is the **only** validation a caller-named
    // key ever gets - `TableAccess` carries no `next_id` on purpose - so it
    // also put the borrow ahead of every reason to refuse the key at all.
    // An insert that could never succeed then waited out a fence, burned
    // ledger entries, and inside an explicit transaction could be killed as
    // a deadlock victim for a statement with no future.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 4 AND v = 99", &a).response,
              "UPDATED 0");

    // Key 5 sits inside A's window and below the relation's high-water
    // mark, so on a heap relation it can never be written by anyone. It
    // must be told so at once.
    Started wb = Start("INSERT INTO t VALUES (5, 9)", b);
    Pump();
    ASSERT_TRUE(*wb.done) << "an illegal key waited on a fence it could never write past";
    EXPECT_NE(wb.out->response.find("high-water mark"), std::string::npos) << wb.out->response;
}

TEST_F(LockDeadlockTest, ASortedFillWaitsOnAFenceOverTheBlockItCarves) {
    // The other insert path, which routes past `InsertOneRow` entirely and
    // whose park had no cell of its own until here - the review's C5. Every
    // row omits its key, so the fill carves one contiguous block and
    // borrows it as a range.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 4 AND v = 99", &a).response,
              "UPDATED 0");

    // The block is carved from the mark, which sits inside A's window, and
    // the window runs to the end of the id space - so a re-run that carves
    // a fresh block is still covered.
    Started wb = Start("INSERT INTO t VALUES (1), (2)", b);
    Pump();
    ASSERT_FALSE(*wb.done) << "the fill wrote through A's fence: " << wb.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*wb.done) << "the fill never resumed after the fence was released";
    EXPECT_EQ(wb.out->response.rfind("INSERTED", 0), 0u) << wb.out->response;
}

TEST_F(LockDeadlockTest, ATwoCycleAbortsTheWaiterThatClosedItAndTheOtherProceeds) {
    // AO-5's S4a cell. Without a detector this is the deadlock AO-S3's
    // guard exists to prevent; with one, the guard lifts and the cycle is
    // resolved at the instant it closes.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (8, 1)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &a)
                  .response.rfind("UPDATED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 8", &b)
                  .response.rfind("UPDATED", 0),
              0u);

    // A wants B's row. It holds rows, so under AO-S3 alone it would have
    // been refused; with a detector it parks, and one edge is recorded.
    Started wa = Start("UPDATE t SET v = 3 WHERE id = 8", a);
    Pump();
    ASSERT_FALSE(*wa.done) << "A did not wait, so the guard did not lift: " << wa.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 1u);

    // B now wants A's row, which closes the cycle. B is the waiter that
    // closed it, so B is the victim - deterministically, not by a race.
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 7", b);
    Pump();
    ASSERT_TRUE(*wb.done) << "the cycle was not detected; only the 11 s net would end this";
    const Status victim = StatusFromErrorReply(wb.out->response);
    EXPECT_EQ(victim.code(), StatusCode::kTxnConflict) << wb.out->response;
    EXPECT_TRUE(victim.retryable()) << "the survivor will have released by the time it retries";
    EXPECT_NE(wb.out->response.find("deadlock"), std::string::npos)
        << "an operator meeting this needs to know to look for a lock-order bug rather than "
           "for contention: " << wb.out->response;
    // **And on the carried `Status`, which is the one a KWP client reads**
    // (`KwpSession::OnStatementComplete` prefers `outcome.status` over the
    // rendered line). A deadlock reported only in the text arm is reported
    // to nobody on the default port.
    EXPECT_NE(wb.out->status.message().find("deadlock"), std::string::npos)
        << "the carried status still holds the pre-wait conflict: " << wb.out->status.message();

    // A is untouched - a detector aborts the waiter, never the holder - and
    // proceeds the moment B's transaction lets go.
    EXPECT_FALSE(*wa.done);
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &b).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    EXPECT_TRUE(*wa.done) << "the survivor never proceeded, so the cycle was broken at both ends";
    EXPECT_EQ(wa.out->response.rfind("UPDATED", 0), 0u) << wa.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u) << "every wait ended, so no edge is left behind";
}

TEST_F(LockDeadlockTest, AThreeCycleIsCaughtByTheTransitiveWalk) {
    // The chain the walk has to follow: A waits for B, B waits for C, and
    // C's wait for A is what closes it. A cycle test that only looked one
    // edge deep would miss this and leave three transactions for the net.
    for (int id = 1; id <= 3; ++id) {
        ASSERT_EQ(Local("INSERT INTO t VALUES (" + std::to_string(id) + ", 1)")
                      .rfind("INSERTED", 0),
                  0u);
    }
    Session a;
    Session b;
    Session c;
    Session* sessions[] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(dispatcher_->Dispatch("BEGIN", sessions[i]).response.rfind("BEGIN", 0), 0u);
        ASSERT_EQ(dispatcher_
                      ->Dispatch("UPDATE t SET v = 2 WHERE id = " + std::to_string(i + 1),
                                 sessions[i])
                      .response.rfind("UPDATED", 0),
                  0u);
    }

    Started wa = Start("UPDATE t SET v = 3 WHERE id = 2", a);  // A -> B
    Pump();
    ASSERT_FALSE(*wa.done);
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 3", b);  // B -> C
    Pump();
    ASSERT_FALSE(*wb.done);
    EXPECT_EQ(locks_->WaitEdgeCount(), 2u);

    Started wc = Start("UPDATE t SET v = 3 WHERE id = 1", c);  // C -> A closes it
    Pump();
    ASSERT_TRUE(*wc.done) << "the three-cycle was not detected";
    EXPECT_NE(wc.out->response.find("deadlock"), std::string::npos) << wc.out->response;
    EXPECT_FALSE(*wa.done) << "A and B are untouched; only the closer is aborted";
    EXPECT_FALSE(*wb.done);
}

TEST_F(LockDeadlockTest, AChainThatDoesNotCloseIsNotADeadlock) {
    // The false positive a naive detector would produce: A waits for B and
    // B waits for C, which is three transactions and two edges and no
    // cycle. Both waits must stand.
    for (int id = 1; id <= 3; ++id) {
        ASSERT_EQ(Local("INSERT INTO t VALUES (" + std::to_string(id) + ", 1)")
                      .rfind("INSERTED", 0),
                  0u);
    }
    Session a;
    Session b;
    Session c;
    Session* sessions[] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(dispatcher_->Dispatch("BEGIN", sessions[i]).response.rfind("BEGIN", 0), 0u);
        ASSERT_EQ(dispatcher_
                      ->Dispatch("UPDATE t SET v = 2 WHERE id = " + std::to_string(i + 1),
                                 sessions[i])
                      .response.rfind("UPDATED", 0),
                  0u);
    }

    Started wa = Start("UPDATE t SET v = 3 WHERE id = 2", a);  // A -> B
    Pump();
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 3", b);  // B -> C, no cycle
    Pump();
    EXPECT_FALSE(*wa.done) << wa.out->response;
    EXPECT_FALSE(*wb.done) << wb.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 2u);

    // C decides, and the chain unwinds from the far end.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", sessions[2]).response.rfind("COMMIT", 0), 0u);
    Pump();
    EXPECT_TRUE(*wb.done) << wb.out->response;
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", sessions[1]).response.rfind("COMMIT", 0), 0u);
    Pump();
    EXPECT_TRUE(*wa.done) << wa.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u);
}


TEST_F(LockDeadlockTest, AStatementThatHasWrittenRowsNowWaitsInsteadOfBeingRefused) {
    // **The line AO-S3b moved, and this cell is where it now sits.**
    //
    // It used to read the other way: a statement that had already written
    // rows was refused even with a detector present, because a re-run would
    // have written those rows a second time and there are no savepoints to
    // undo them with. AO-S3b does not re-run it - the walk stops at the held
    // row, the scope and the position stay with the session, and the resume
    // carries on from there - so restartability stops being the thing that
    // forbids the wait.
    //
    // What did *not* move is the guard below it: the wait is offered only
    // where a detector exists, because a waiter holding rows can join a
    // cycle (`WithoutATableTheNarrowGuardIsWhatKeepsTheStageSafe`).
    ASSERT_EQ(Local("INSERT INTO t VALUES (1, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (2, 1)").rfind("INSERTED", 0), 0u);

    // A holder takes row 2 and stays open.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 2", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    // A multi-row UPDATE that writes row 1 and then meets row 2.
    //
    // **The predicate names no pk window, and since AO-S6c-c that is what
    // keeps this cell about a mid-walk park.** `WHERE id <= 2` would now
    // declare `Range(t, 0, 3)` (item 14), which overlaps the holder's tuple
    // and is refused *before* the walk starts - so the statement would wait
    // having written nothing, hold no rows, and correctly register no edge
    // at all. That is the better behaviour and it is why the declaration
    // exists; it is simply not the shape this cell measures. Both rows
    // are matched by `v > 0` whichever value the page carries - `apply`
    // evaluates the WHERE against the page's current bytes, so row 2 reads
    // as the holder's uncommitted `9` rather than as the `1` this writer's
    // view would resolve.
    Session writer;
    Started w = Start("UPDATE t SET v = 3 WHERE v > 0", writer);
    Pump();
    ASSERT_FALSE(*w.done) << "the statement was refused rather than parked: " << w.out->response;

    // **And it is in the graph.** An autocommit statement parked mid-walk
    // holds rows, so it can be the other half of a cycle - which is why its
    // waiter identity comes from the parked scope rather than from
    // `session.transaction()`, which is null here. An edge that went
    // unregistered would be a deadlock only the fault net could end.
    EXPECT_EQ(locks_->WaitEdgeCount(), 1u)
        << "a parked autocommit writer holding rows registered no edge, so a cycle through it "
           "would be invisible to the detector";

    // The holder rolls back, which restores the row's prior writer id along
    // with its bytes - so the parked statement's own unchanged view admits
    // the write it stopped on, and the statement finishes.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended after the holder decided";
    EXPECT_EQ(w.out->response.rfind("UPDATED 2", 0), 0u)
        << "both rows, counted across the park: " << w.out->response;
    // And the edge is gone, however the wait ended.
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u);
}

TEST_F(LockDeadlockTest, WithoutATableTheNarrowGuardIsWhatKeepsTheStageSafe) {
    // The two states stated side by side. Take the table away and the same
    // transaction that waited above is refused instead, because nothing
    // would catch the cycle it could join - which is AO-S3's rule, and why
    // it is a guard rather than a limitation.
    dispatcher_->set_locks(nullptr);
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (8, 1)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &a)
                  .response.rfind("UPDATED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 8", &b)
                  .response.rfind("UPDATED", 0),
              0u);

    Started wa = Start("UPDATE t SET v = 3 WHERE id = 8", a);
    Pump();
    ASSERT_TRUE(*wa.done) << "a transaction holding rows waited with no detector present";
    EXPECT_EQ(StatusFromErrorReply(wa.out->response).code(), StatusCode::kTxnConflict)
        << wa.out->response;
    EXPECT_EQ(wa.out->response.find("deadlock"), std::string::npos)
        << "and it is an ordinary conflict, not a deadlock report: " << wa.out->response;
}

// ---- AO-S6d item 17: the repeatable-read wait, made exact ---------------
//
// `ARepeatableReadWriterIsRefusedRatherThanOfferedANarrowerWait` above is
// the case that stands: the blocker there is the row's own writer, and a
// commit makes the row invisible to the waiter's view for the rest of its
// transaction, so the wait could only ever pay off on the abort arm. These
// two are the case that does not - a holder of a *unit* over the key that
// has written no version of it. Its commit changes what this view admits
// only for rows it wrote, and the row in hand was written by somebody the
// view has already judged.

TEST_F(LockDeadlockTest, ARepeatableReadWriterWaitsOnAFenceHolderThatWroteNoRow) {
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);

    Session rr;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &rr)
                  .response.rfind("ERR", 0),
              std::string::npos)
        << "the level must be settable for this cell to mean anything";
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &rr).response.rfind("BEGIN", 0), 0u);

    // A holds `[2, 100)` and writes nothing inside it, so row 5's header
    // still names the committed inserter - a version this view can see.
    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 1 AND id < 100 AND v = 99",
                                    &a)
                  .response,
              "UPDATED 0");

    Started w = Start("UPDATE t SET v = 3 WHERE id = 5", rr);
    Pump();
    ASSERT_FALSE(*w.done) << "the repeatable-read writer was refused instead of waiting on a "
                             "holder that wrote no version of its row: "
                          << w.out->response;

    // **The mutation**: restore the whole-level exclusion in
    // `NoteBlockingWriter` and this is refused at once, with the assertion
    // above failing rather than the one below.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_EQ(w.out->response, "UPDATED 1") << w.out->response;
    // Read back through the waiter's own commit: it is an explicit
    // transaction, so until it decides its write is nobody else's to see.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &rr).response.rfind("COMMIT", 0), 0u);
    EXPECT_NE(Rows().find(",3"), std::string::npos) << Rows();
}

TEST_F(LockDeadlockTest, ARepeatableReadWaiterIsStillRefusedIfTheFenceHolderWritesTheRow) {
    // The half the operator ruled on (2026-09-09, AO-0 item 17): the wait
    // is offered on what the holder has done *so far*, and a holder is free
    // to write the row afterwards. The re-run then meets a header naming a
    // transaction that committed after this view was minted, and is refused
    // first-updater-wins - the correct repeatable-read answer, reached
    // after a wait rather than instead of one. What this pins is that the
    // wait did not widen what the level admits.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);

    Session rr;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &rr)
                  .response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &rr).response.rfind("BEGIN", 0), 0u);

    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id > 1 AND id < 100 AND v = 99",
                                    &a)
                  .response,
              "UPDATED 0");

    Started w = Start("UPDATE t SET v = 3 WHERE id = 5", rr);
    Pump();
    ASSERT_FALSE(*w.done) << w.out->response;

    // Now A writes the row the waiter wants - its own fence does not block
    // it - and commits.
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 5", &a).response,
              "UPDATED 1");
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_EQ(StatusFromErrorReply(w.out->response).code(), StatusCode::kTxnConflict)
        << w.out->response;
    EXPECT_NE(Rows().find(",9"), std::string::npos)
        << "the holder's value is what stands: " << Rows();
}

TEST_F(LockDeadlockTest, ARepeatableReadInsertThatWaitsOnAFenceStillCannotDuplicateAKey) {
    // The boundary the lift has to be checked against, because it is the
    // one shape where "the view cannot see the holder's commit" would be a
    // wrong *answer* rather than a refusal: an INSERT of a caller-named key
    // that waits on a fence whose holder then inserts that very key.
    //
    // It is safe, and not by luck. The uniqueness proof is **physical** -
    // `btree.cpp`'s descent scans the one leaf that may hold the key and
    // `heap_chain.cpp`'s scans the tail page, neither of them through a
    // read view - so a version this repeatable-read session cannot see is
    // still a key it cannot take. The refusal is `AlreadyExists`, which is
    // not retryable and says so.
    ASSERT_EQ(Local("CREATE TABLE tb (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO tb VALUES (10, 0)").rfind("INSERTED", 0), 0u);

    Session rr;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &rr)
                  .response.rfind("ERR", 0),
              std::string::npos);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &rr).response.rfind("BEGIN", 0), 0u);

    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("DELETE FROM tb WHERE id > 1 AND id < 100 AND v = 999", &a)
                  .response,
              "DELETED 0");

    Started w = Start("INSERT INTO tb VALUES (50, 1)", rr);
    Pump();
    ASSERT_FALSE(*w.done) << w.out->response;

    // The holder takes the key the waiter wants - its own fence does not
    // block it - and commits.
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO tb VALUES (50, 2)", &a)
                  .response.rfind("INSERTED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_NE(w.out->response.find("duplicate primary key"), std::string::npos)
        << "a repeatable-read insert wrote a key its own view could not see: "
        << w.out->response;
}

// ---- AO-S6d item 15: a failed commit unwinds instead of leaking ---------
//
// `CommitLocal` has aborted on a failed commit since the DT9 review, and
// says at the site why merely reporting it is not enough. `EndWrite`'s
// autocommit arm did not, and the two are the same transaction state: a
// commit fails only *before* `PublishCommit`, so the transaction is still
// active and `Release` refuses to free an active one. What the leak cost
// stopped being memory at AO-S6c-a, which gave every writer a borrow: the
// leaked transaction's tenancies were never released either, so every
// later writer of a row it touched waited out the fault net and was told a
// defect report.

class FailedCommitTest : public LockDeadlockTest {
protected:
    // **Strict, because it is the class whose commit record is synced
    // inside `WalManager::Commit`.** Under `kGroup` the commit stages and
    // the sync happens at a later drain, where its failure is nobody's
    // statement; `FailNextSync` under `kStrict` fails the one call
    // `TransactionManager::Commit` can fail on, which is this arm.
    wal::DurabilityClass Durability() const override { return wal::DurabilityClass::kStrict; }
};

TEST_F(FailedCommitTest, AnAutocommitWriteWhoseCommitFailsReleasesEverythingItHeld) {
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    const std::size_t held_before = locks_->EntryCount();

    log_device_->FailNextSync(Status::IoError("injected"));
    const DispatchOutcome out = dispatcher_->Dispatch("UPDATE t SET v = 1 WHERE id = 5");
    ASSERT_EQ(out.response.rfind("ERR ", 0), 0u)
        << "the injected sync failure did not reach the statement: " << out.response;
    EXPECT_NE(out.response.find("injected"), std::string::npos) << out.response;

    // The three things the leak was: a transaction still in flight, its
    // borrows still held, and its rows still written.
    EXPECT_EQ(txns_->ActiveCount(), 0u)
        << "the transaction whose commit failed is still active";
    EXPECT_EQ(locks_->EntryCount(), held_before)
        << "the failed commit left its borrows in the table";
    EXPECT_NE(Rows().find(",0"), std::string::npos)
        << "the write the commit never made durable was not compensated: " << Rows();

    // **The mutation**: reinstate the release-only arm and this second
    // writer parks on a holder nothing will ever decide, reaching the
    // 11-second fault net and failing here rather than below.
    Session b;
    Started wb = Start("UPDATE t SET v = 3 WHERE id = 5", b);
    Pump();
    ASSERT_TRUE(*wb.done) << "a later writer of the row is waiting on a transaction that no "
                             "longer exists: " << wb.out->response;
    EXPECT_EQ(wb.out->response, "UPDATED 1") << wb.out->response;
}

TEST_F(FailedCommitTest, AFailedStatementInsideATransactionStillPoisonsAndStillHolds) {
    // The other arm of `EndWrite`, pinned so the fix above is seen not to
    // widen: inside an explicit transaction a failed statement does **not**
    // unwind. Failure atomicity is per transaction (txn.md §6), so the rows
    // already written stay, the borrows stay with them, and the client must
    // ROLLBACK.
    ASSERT_EQ(Local("INSERT INTO t VALUES (5, 0)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (6, 0)").rfind("INSERTED", 0), 0u);
    const std::size_t held_before = locks_->EntryCount();

    Session a;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 5", &a).response,
              "UPDATED 1");
    // A key below the relation's high-water mark: refused, on a heap
    // relation, after the write scope is open.
    const DispatchOutcome refused = dispatcher_->Dispatch("INSERT INTO t VALUES (5, 9)", &a);
    ASSERT_EQ(refused.response.rfind("ERR ", 0), 0u) << refused.response;

    EXPECT_GT(locks_->EntryCount(), held_before)
        << "the poisoned transaction gave its borrows back before ROLLBACK";
    EXPECT_EQ(dispatcher_->Dispatch("SELECT * FROM t", &a).response.rfind("ERR ", 0), 0u)
        << "the session was not poisoned";

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &a).response.rfind("ROLLBACK", 0), 0u);
    EXPECT_EQ(locks_->EntryCount(), held_before);
    EXPECT_NE(Rows().find(",0"), std::string::npos) << Rows();
}

// ---- AO-S6e-c: the bounded false rejection becomes a wait ---------------
//
// `assertion.md` §6.2 listed four properties of the admission protocol and
// accepted a **bounded false rejection** as one of them: "a statement can
// be rejected due to a reservation of a transaction that later aborts".
// The owner of that reservation is knowable, so census row 11 turns the
// rejection into a wait for its decide. What the wait rides is the channel
// every other wait in this family uses, which is what gives it the wait-for
// graph - two transactions can each hold a reservation the other's
// admission needs, and that cycle is real.

TEST_F(LockDeadlockTest, AnAssertionsFalseRejectionWaitsAndIsAdmittedWhenTheReserverAborts) {
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);

    // The reserver takes 60 of the 100 and holds it undecided.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (7, 60)", &holder)
                  .response.rfind("INSERTED", 0),
              0u);

    // 50 more would be 110. The aggregate that refuses is 60 of somebody
    // else's reservation, so the refusal is one a decide can undo.
    //
    // **The mutation**: drop the `NoteBlockingWriter` at the admission
    // site and this is `ERR ASSERTION_VIOLATION` here instead - which is
    // what `AssertionEnforceTest`'s own cell still asserts on the
    // synchronous path, where nothing can park.
    Session rival;
    Started w = Start("INSERT INTO trades VALUES (7, 50)", rival);
    Pump();
    ASSERT_FALSE(*w.done) << "the false rejection was answered instead of waited: "
                          << w.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_EQ(w.out->response.rfind("INSERTED", 0), 0u) << w.out->response;
}

TEST_F(LockDeadlockTest, AnAssertionsRejectionIsFinalWhenTheReserverCommits) {
    // The other decide, and the reason the wait is worth having: the same
    // statement gets two different *correct* answers depending on how the
    // reservation ends, and only one of them is the violation. A wait that
    // ended in the violation either way would be a stall bought for
    // nothing.
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (7, 60)", &holder)
                  .response.rfind("INSERTED", 0),
              0u);

    Session rival;
    Started w = Start("INSERT INTO trades VALUES (7, 50)", rival);
    Pump();
    ASSERT_FALSE(*w.done) << w.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*w.done) << "the wait never ended";
    EXPECT_NE(w.out->response.find("ASSERTION_VIOLATION"), std::string::npos)
        << "the reservation became the group's real weight, so the refusal is now true: "
        << w.out->response;
}

TEST_F(LockDeadlockTest, AnAssertionRejectionWithNothingReservedIsRefusedAtOnce) {
    // The third answer, and the one that says the wait is not unconditional:
    // where the aggregate that refuses is **settled**, no decide can change
    // it and `ReserverOn` names nobody, so the violation is delivered now.
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("INSERT INTO trades VALUES (7, 60)").rfind("INSERTED", 0), 0u);

    Session rival;
    Started w = Start("INSERT INTO trades VALUES (7, 50)", rival);
    Pump();
    ASSERT_TRUE(*w.done) << "a refusal nothing can undo was waited on anyway";
    EXPECT_NE(w.out->response.find("ASSERTION_VIOLATION"), std::string::npos) << w.out->response;
}

TEST_F(LockDeadlockTest, AnUpdateThatHasAlreadyReservedIsRefusedRatherThanWaited) {
    // **The review's B1, and the cell that would have caught it.**
    // `AdmitAndReserveUpdate` is per *assertion* and not atomic across
    // them: when the second refuses, the first is already applied to its
    // cabin and appended to its chain. A wait re-runs the whole statement,
    // and the first assertion is reserved a second time - permanently,
    // because a reservation never enters the transaction's trail, so
    // `EndWrite`'s re-runnability test cannot see it and both entries are
    // `kAssertReserve` records a rebuild reproduces.
    //
    // So a call that has reserved hands back no reserver, and the statement
    // gets the violation it always gave. **The mutation**: drop
    // `reserved_any`, and the `UPDATE` below waits instead of answering -
    // and after the holder rolls back, `wide`'s aggregate is 30 too high
    // for the life of the relation.
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    // `wide` is asked first (creation order) and admits the +30; `tight`
    // is asked second and refuses it. The two bounds are 10 apart so that
    // `wide`'s aggregate is observable *below* `tight`'s ceiling - with a
    // far-apart pair, `tight` shadows `wide` and a double count in `wide`
    // could not be seen at all.
    ASSERT_EQ(Local("CREATE ASSERTION wide ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION tight ON trades GROUP BY (account) CHECK SUM(qty) <= 90")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("INSERT INTO trades VALUES (7, 10)").rfind("INSERTED", 0), 0u);

    // A reservation of 60 that has not decided: both groups read 70.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (7, 60)", &holder)
                  .response.rfind("INSERTED", 0),
              0u);

    // 10 -> 40 is +30: `wide` admits it and **reserves**, `tight` refuses.
    Session rival;
    Started w = Start("UPDATE trades SET qty = 40 WHERE id = 1", rival);
    Pump();
    ASSERT_TRUE(*w.done) << "the update waited after it had already reserved: " << w.out->response;
    EXPECT_NE(w.out->response.find("ASSERTION_VIOLATION"), std::string::npos) << w.out->response;

    // And the refusal left `wide` where it was. The holder rolls back, so
    // account 7's only weight is the committed 10 - the update was refused,
    // so row 1 is still qty 10. 65 more is 75: inside `tight`'s 90 and
    // inside `wide`'s 100. Had the refused attempt's `wide` reservation
    // been counted a second time, `wide` would read 40 and refuse this.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    const std::string probe = Local("INSERT INTO trades VALUES (7, 65)");
    EXPECT_EQ(probe.rfind("INSERTED", 0), 0u)
        << "`wide` counted the refused attempt's reservation a second time: " << probe;
}

TEST_F(LockDeadlockTest, TwoTransactionsWaitingOnEachOthersReservationAreADeadlockAndOneIsTheVictim) {
    // **The claim the whole sub-stage rests on**, and it had no cell:
    // riding the family's channel is what gives an assertion wait the
    // wait-for graph, and the reason that matters is that two transactions
    // can each hold a reservation the other's admission needs. That cycle
    // is real, and AO-S4a's detector is what ends it.
    ASSERT_EQ(Local("CREATE TABLE trades (id int64, account int64, qty int64) BTREE")
                  .rfind("CREATED", 0),
              0u);
    ASSERT_EQ(Local("CREATE ASSERTION cap ON trades GROUP BY (account) CHECK SUM(qty) <= 100")
                  .rfind("CREATED", 0),
              0u);

    // Each takes 60 of a different account's 100, so neither refuses the
    // other yet - and each holds rows, which is what makes an edge *out* of
    // a holder possible at all.
    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (7, 60)", &a)
                  .response.rfind("INSERTED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO trades VALUES (8, 60)", &b)
                  .response.rfind("INSERTED", 0),
              0u);

    // A now wants B's account and B wants A's: each is refused by the
    // other's undecided 60, and each waits for it.
    Started wa = Start("INSERT INTO trades VALUES (8, 50)", a);
    Pump();
    ASSERT_FALSE(*wa.done) << "A did not wait on B's reservation: " << wa.out->response;

    Started wb = Start("INSERT INTO trades VALUES (7, 50)", b);
    Pump();
    // **The mutation**: take the lock table away and neither edge is
    // recorded - both stall to the fault net and both are refused as defect
    // reports, which is the hang AO-R7 exists to convert into a verdict.
    ASSERT_TRUE(*wb.done) << "the waiter that closed the cycle was not refused";
    EXPECT_NE(wb.out->response.find("deadlock"), std::string::npos)
        << "the cycle was ended by something other than the detector: " << wb.out->response;
    EXPECT_EQ(StatusFromErrorReply(wb.out->response).code(), StatusCode::kTxnConflict)
        << wb.out->response;

    // The victim is the waiter that closed it; the survivor's wait ends
    // when that transaction rolls back, which is the contract every other
    // failed statement inside a transaction has.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &b).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*wa.done) << "A never resumed after its holder rolled back";
    EXPECT_EQ(wa.out->response.rfind("INSERTED", 0), 0u) << wa.out->response;
}

// ---- AO-S3b: the mid-statement wait --------------------------------------
//
// Everything above waits *between* statements: the statement that meets a
// held row has written nothing, is refused, and is run again from the top
// once the holder decides. A statement that has already written rows could
// not do that - there are no savepoints, so its rows cannot be rolled back
// to a statement boundary - and so was answered with its conflict. These
// cells are the other shape: the walk stops at the held row, the statement
// keeps its scope and its position, and the resume carries on from there.

class MidWalkWaitTest : public LockDeadlockTest {
protected:
    // Ten rows, ids 1..10, every one of them v = 0. Inserted in id order,
    // so the walk meets them in that order too - which is what lets the
    // cell name "row 7" and mean the seventh row the walk reaches.
    void SetUp() override {
        LockDeadlockTest::SetUp();
        // A second relation in the other storage form, ten rows like `t`.
        // **The two arms resume by different means** - `t` by position and
        // `tb` by key - so a cell that ran only against the fixture's
        // default would leave one of them unexercised.
        ASSERT_EQ(Local("CREATE TABLE tb (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
        for (int id = 1; id <= 10; ++id) {
            const std::string vals = " VALUES (" + std::to_string(id) + ", 0)";
            ASSERT_EQ(Local("INSERT INTO t" + vals).rfind("INSERTED", 0), 0u) << "id " << id;
            ASSERT_EQ(Local("INSERT INTO tb" + vals).rfind("INSERTED", 0), 0u) << "id " << id;
        }
    }

    // **Why every cell here writes `WHERE v >= 0` rather than no predicate
    // or a pk one.** A `WHERE`-less write declares the relation (AO-0 item
    // 14) and a pk-shaped one declares its range, and a declared unit is
    // borrowed **before the walk starts** - so such a statement waits
    // having written nothing and re-runs cleanly, which is the better
    // behaviour and has its own cell
    // (`ACoarseDeclarationWaitsBeforeItWritesAnythingAndThenSucceeds`).
    // The mid-walk park these cells exist to measure is what a predicate
    // naming no pk window still takes. Every row is inserted with `v = 0`
    // and a holder's row carries `9`, so `v >= 0` matches all ten whichever
    // value the page holds - `apply` evaluates the WHERE against the page's
    // current bytes, not against the version the view would resolve.
    //
    // How many rows carry the new value. The point of every cell here is
    // *which* rows the statement wrote, so this is what they assert on.
    int RowsWith(int v) {
        // `Rows()` renders "id,v" per row and a header, so a row's value is
        // whatever follows its last comma. **Rows are separated by a
        // literal backslash-n**, not by a newline character - the reply is
        // one line on the wire, which is why every other cell in this file
        // writes its expectation with an escaped separator.
        const std::string rows = Rows();
        const std::string want = std::to_string(v);
        const std::string sep = "\\n";
        int n = 0;
        std::size_t at = 0;
        while (at <= rows.size()) {
            const std::size_t eol = rows.find(sep, at);
            const std::string line = rows.substr(at, eol == std::string::npos ? eol : eol - at);
            const std::size_t comma = line.rfind(',');
            if (comma != std::string::npos && line.substr(comma + 1) == want) ++n;
            if (eol == std::string::npos) break;
            at = eol + sep.size();
        }
        return n;
    }
};

TEST_F(MidWalkWaitTest, ATenRowUpdateMeetingAHeldRowKeepsWhatItWroteAndWaits) {
    // AO-5's S3b cell. The holder takes row 7 and does not decide; the
    // ten-row UPDATE walks into it having written six rows.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
        // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE t SET v = 1 WHERE v >= 0", w);
    Pump();

    // **It waited rather than answering**, which is the whole stage: before
    // AO-S3b a statement that had written rows was refused outright, because
    // re-running it would have written them twice.
    ASSERT_FALSE(*walk.done) << "the statement did not park: " << walk.out->response;

    // **And it kept what it wrote.** Six rows carry the new value while the
    // statement is parked - the rows before the held one - which is what
    // makes this a mid-statement wait and not a re-run.
    // **What another session sees while it waits: nothing.** The parked
    // statement's six rows are written but uncommitted, so ordinary MVCC
    // hides them - "keeps rows 1-6" is a claim about the statement's own
    // transaction, and the explicit-transaction cell below is where it is
    // observable, on the trail.
    EXPECT_EQ(RowsWith(1), 0) << "a parked statement's writes must not be visible to anyone else";

    // The holder aborts, so the row's prior writer id comes back with its
    // bytes and the parked statement's own view - unchanged across the
    // park - admits the write it refused. That is why the abort arm
    // completes rather than re-refusing.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();

    ASSERT_TRUE(*walk.done) << "the wait never ended after the holder decided";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u)
        << "all ten rows under one snapshot: " << walk.out->response;
    EXPECT_EQ(RowsWith(1), 10);
}

TEST_F(MidWalkWaitTest, ACoarseDeclarationWaitsBeforeItWritesAnythingAndThenSucceeds) {
    // **The behaviour item 14's declared units buy, marked by the operator
    // on 2026-09-08 against `txn.md` §5's ratified refusal.** A `WHERE`-less
    // write declares the whole id space - the relation until AO-S6e-b,
    // which moved the declaration off the relation entry so that a
    // positioned reader's `IS` there does not refuse it - and borrows it
    // before the walk, so it waits having written nothing - and an autocommit statement's re-run
    // mints a fresh view, so it succeeds where the same statement used to
    // write six rows, meet the seventh, and be refused `TxnConflict` with
    // those six compensated. That is AR2-A §1's "refusal → wait" delivered,
    // and it is a real semantic change: for autocommit this shape is now
    // closer to PostgreSQL's READ COMMITTED than to the stricter rule §5
    // states. An explicit transaction keeps its view across the re-run and
    // is refused exactly as before.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    Started walk = Start("UPDATE t SET v = 1", w);
    Pump();
    ASSERT_FALSE(*walk.done) << walk.out->response;
    EXPECT_EQ(RowsWith(1), 0)
        << "the declaration is borrowed before the walk, so nothing is written while it waits";

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    Pump();

    ASSERT_TRUE(*walk.done) << "the wait never ended after the holder committed";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u)
        << "the re-run mints a fresh view and writes every row: " << walk.out->response;
    EXPECT_EQ(RowsWith(1), 10);
}

TEST_F(MidWalkWaitTest, TheCommitArmRefusesAndUnwindsWhatTheStatementHadWritten) {
    // The other decide. `txn.md` section 5 already ratifies this as
    // deliberate - stricter than PostgreSQL's READ COMMITTED, which would
    // re-read and update the new version - so what this cell pins is that
    // the refusal arrives with the statement's own six rows **unwound**,
    // not left behind as a partial UPDATE.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
        // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE t SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << walk.out->response;
    ASSERT_EQ(RowsWith(1), 0) << "uncommitted rows must stay invisible";

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    Pump();

    ASSERT_TRUE(*walk.done) << "the wait never ended after the holder committed";
    EXPECT_EQ(StatusFromErrorReply(walk.out->response).code(), StatusCode::kTxnConflict)
        << walk.out->response;
    // Autocommit, so the statement is the whole transaction and its failure
    // is atomic: the six rows it had written are compensated.
    EXPECT_EQ(RowsWith(1), 0) << "a refused autocommit statement left rows behind";
}

TEST_F(MidWalkWaitTest, InsideAnExplicitTransactionTheRowsAreWrittenOnceNotTwice) {
    // The same statement inside a transaction, and the cell that tells a
    // **resume** from a **re-run**: the trail is this statement's undo
    // record per row, so a resumed walk leaves ten entries where a re-run
    // would leave sixteen - the six it redid plus the ten it then wrote.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &w).response.rfind("BEGIN", 0), 0u);
        // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE t SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << walk.out->response;

    ASSERT_NE(w.transaction(), nullptr);
    EXPECT_EQ(w.transaction()->trail().size(), 6u)
        << "the parked statement should have written exactly the rows before the held one";

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();

    ASSERT_TRUE(*walk.done) << "the wait never ended";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u) << walk.out->response;
    EXPECT_EQ(w.transaction()->trail().size(), 10u)
        << "sixteen would mean the statement re-ran from the top instead of resuming";
    EXPECT_FALSE(w.failed()) << "a park is not a failure, so the session must not be poisoned";
}

TEST_F(MidWalkWaitTest, ADeleteParksInTheMiddleOfItsWalkToo) {
    // The same mechanism on the other verb - `DeleteInner` carries its own
    // copy of the stop, and a stage that built it for UPDATE alone would
    // leave DELETE answering a partial count.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
        // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("DELETE FROM t WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << "the DELETE did not park: " << walk.out->response;

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*walk.done) << "the wait never ended";
    EXPECT_EQ(walk.out->response.rfind("DELETED 10", 0), 0u) << walk.out->response;
}

TEST_F(MidWalkWaitTest, AClusteredBtreeParksAndResumesByKey) {
    // **The arm SUS-1 makes load-bearing.** A btree cannot resume from a
    // remembered leaf, because a split moves the upper half of a leaf to a
    // new sibling and would carry already-written rows into a page the walk
    // has yet to visit. It resumes by descending to whichever leaf now
    // holds the key it stopped at, so the cell that matters is that the
    // resumed walk finishes the relation exactly once.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE tb SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &w).response.rfind("BEGIN", 0), 0u);
        // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE tb SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << "the btree walk did not park: " << walk.out->response;
    ASSERT_NE(w.transaction(), nullptr);
    EXPECT_EQ(w.transaction()->trail().size(), 6u)
        << "the parked btree walk should hold exactly the keys below the held one";

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*walk.done) << "the wait never ended";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u) << walk.out->response;
    // Ten, not sixteen: the four keys at and above the held one were the
    // only ones the resume wrote.
    EXPECT_EQ(w.transaction()->trail().size(), 10u)
        << "the key-ordered skip did not hold - rows below the resume key were written twice";
}

TEST_F(MidWalkWaitTest, ThePointLookupArmStillReportsTheConflictItCannotWriteThrough) {
    // **The arm that does not walk at all.** A bare `WHERE id = k` on a
    // btree relation is answered by a descent, not by `VisitRelation` - so
    // it never reaches the stop the walk carries, and it renders its reply
    // straight from the row counter. AO-S3b made `apply` answer OK for a
    // row it declined to write, which turned that reply into `UPDATED 0`
    // for a row a writer holds: a success line for a write that never
    // happened, with the autocommit commit behind it.
    //
    // One row is in scope, so nothing was written and there is nothing to
    // resume from: the answer is the conflict, and the wait is the
    // whole-statement one AO-S3 already built.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE tb SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    Started point = Start("UPDATE tb SET v = 1 WHERE id = 7", w);
    Pump();
    ASSERT_FALSE(*point.done)
        << "the point UPDATE answered instead of waiting: " << point.out->response;

    // The holder commits and the wait ends. **This is AO-S3's arm, not
    // AO-S3b's**: the statement wrote nothing, so it is re-run whole under
    // a *fresh* snapshot, which sees the committed version and writes
    // through it. One row, written once - the outcome the premature
    // `UPDATED 0` replaced with a silent no-op.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*point.done) << "the wait never ended";
    EXPECT_EQ(point.out->response.rfind("UPDATED 1", 0), 0u) << point.out->response;
    // Read back off `tb` itself - the fixture's `Rows()` helper selects
    // from `t`, so it would answer this question about the wrong relation.
    EXPECT_NE(Local("SELECT * FROM tb WHERE id = 7").find(",1"), std::string::npos)
        << "the row the wait was for was never written";

    // And the DELETE arm beside it, which renders its count the same way.
    Session holder2;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder2).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE tb SET v = 9 WHERE id = 3", &holder2)
                  .response.rfind("UPDATED", 0),
              0u);
    Session d;
    Started point_delete = Start("DELETE FROM tb WHERE id = 3", d);
    Pump();
    ASSERT_FALSE(*point_delete.done)
        << "the point DELETE answered instead of waiting: " << point_delete.out->response;
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder2).response.rfind("COMMIT", 0), 0u);
    Pump();
    ASSERT_TRUE(*point_delete.done);
    EXPECT_EQ(point_delete.out->response.rfind("DELETED 1", 0), 0u)
        << "a held row was answered as a delete of zero rows: " << point_delete.out->response;
}

TEST_F(MidWalkWaitTest, ABtreeResumeSurvivesALeafSplitUnderThePark) {
    // The hazard the key-ordered resume exists for, made to happen: while
    // the statement is parked, another session inserts enough keys to split
    // the leaf it stopped in. A positional cursor would come back to a page
    // whose upper half - including rows this statement already wrote - now
    // lives in a new sibling it has yet to visit, and would write them
    // twice. Descending by key cannot see that difference.
    //
    // **The keys are sparse, and that is the whole setup.** A split divides
    // a leaf at its middle key, so filling `tb` (ids 1..10) with ids above
    // 100 divides it far above the resume key and leaves every row this
    // walk wrote exactly where it was - a cell that meets a split and never
    // meets the hazard, which a positional cursor passes. The relation here
    // is keyed 1000, 2000 .. 10000 and the fillers are all *below* the held
    // key, so the division falls between the rows the walk already wrote
    // and the ones it has yet to reach. That is the case that tells the two
    // resume shapes apart.
    ASSERT_EQ(Local("CREATE TABLE ts (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    for (int id = 1000; id <= 10000; id += 1000) {
        ASSERT_EQ(Local("INSERT INTO ts VALUES (" + std::to_string(id) + ", 0)")
                      .rfind("INSERTED", 0),
                  0u)
            << "id " << id;
    }

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE ts SET v = 9 WHERE id = 7000", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &w).response.rfind("BEGIN", 0), 0u);
    // `v >= 0` rather than no predicate: a `WHERE`-less write declares the
    // relation (item 14) and is borrowed before the walk, so it would wait
    // having written nothing and there would be no parked cursor for a
    // split to straddle. Every row of `ts` is inserted with `v = 0`.
    Started walk = Start("UPDATE ts SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_FALSE(*walk.done) << walk.out->response;
    ASSERT_EQ(w.transaction()->trail().size(), 6u)
        << "the park must be at the seventh key for the split to straddle it";

    // **The leaf count before and after, so the cell cannot pass
    // vacuously.** `DESCRIBE`'s whole line will not do: it carries
    // `ids_issued`, which moves with every insert whether or not a leaf
    // ever divided, so comparing the lines would assert nothing about the
    // tree. The assertion is on `leaves=` itself.
    const auto leaf_count = [&]() -> int {
        const std::string shape = Local("DESCRIBE ts");
        const std::size_t at = shape.find("leaves=");
        EXPECT_NE(at, std::string::npos) << shape;
        if (at == std::string::npos) return -1;
        return std::atoi(shape.c_str() + at + std::string("leaves=").size());
    };
    const int leaves_before = leaf_count();
    ASSERT_EQ(leaves_before, 1)
        << "the ten rows must start in one leaf for it to be the leaf the split divides";

    // Enough keys *below* the held one to force the division there. They
    // are all invisible to the parked walk's snapshot, so none of them may
    // appear in its count.
    for (int id = 1; id <= 400; ++id) {
        ASSERT_EQ(Local("INSERT INTO ts VALUES (" + std::to_string(id) + ", 7)")
                      .rfind("INSERTED", 0),
                  0u)
            << "id " << id;
    }
    EXPECT_GT(leaf_count(), leaves_before)
        << "no leaf ever divided, so this cell would pass without exercising the hazard it is "
           "named for";

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Pump();
    ASSERT_TRUE(*walk.done) << "the wait never ended after a split under the park";
    EXPECT_EQ(walk.out->response.rfind("UPDATED 10", 0), 0u)
        << "the resumed walk must see its own ten rows and none of the 400 inserted after its "
           "snapshot: " << walk.out->response;
    EXPECT_EQ(w.transaction()->trail().size(), 10u)
        << "a row below the resume key was written a second time after the split";
}

TEST_F(MidWalkWaitTest, ADeadlockBetweenTwoMidWalkParksUnwindsTheVictimsOpenScope) {
    // A cycle in which **both** transactions are parked mid-walk holding
    // rows, which no other cell in this file produces: every existing
    // deadlock cell uses `WHERE id = k` statements that write nothing
    // before conflicting.
    //
    // **What it does not prove**, stated because the first version of this
    // comment claimed it did: it does not exercise `RefuseParkedWrite`'s
    // scope cleanup. Both victims here are inside explicit transactions,
    // where `EndWrite`'s unowned arm only poisons - which `RefuseParkedWrite`
    // does anyway - so the cell passes with the cleanup compiled out. It was
    // checked that way. The cleanup's own cell is the autocommit one below,
    // where the scope is *owned* and someone has to end it.
    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);

    // **The anchors carry distinguishing values, and since AO-S6c-c that is
    // what makes the shape reachable at all.** The cell used to give the two
    // walkers disjoint pk windows (`id >= 5`, `id >= 2`); a pk-shaped
    // predicate now declares a range and is borrowed before the walk, so
    // neither would write anything before conflicting. A non-pk predicate
    // keeps the per-row path - but both walkers then start at row 1, and
    // the second meets the first's rows immediately and writes nothing,
    // which is the coverage this cell exists to add. So the anchors' values
    // are what carve the two walks apart: A takes row 10 and B row 5, and
    // each walker's predicate selects the rows the other has not touched.
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 1 WHERE id = 10", &a)
                  .response.rfind("UPDATED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 5", &b)
                  .response.rfind("UPDATED", 0),
              0u);

    // A walks 1..4, writes them, and meets row 5 - B's. It parks holding
    // four rows, and the graph gets the edge A -> B.
    Started wa = Start("UPDATE t SET v = 3 WHERE v >= 0", a);
    Pump();
    ASSERT_FALSE(*wa.done) << "A did not park mid-walk: " << wa.out->response;
    EXPECT_EQ(locks_->WaitEdgeCount(), 1u);

    // B walks 6..9, writes them, and meets row 10 - which A is holding.
    // That edge closes the cycle, so B is the victim by AO-R7's rule, and
    // B has an open scope with four rows of its own already written.
    //
    // `v <= 1` selects exactly those: rows 1..4 now read as A's uncommitted
    // `3`, row 5 as B's own `2`, and row 10 as A's anchor `1` - `apply`
    // evaluates the WHERE against the page's current bytes, so the
    // uncommitted values are what the predicate sees.
    Started wb = Start("UPDATE t SET v = 4 WHERE v <= 1", b);
    Pump();
    ASSERT_TRUE(*wb.done) << "the cycle was not detected; only the 11 s net would end this";
    const Status victim = StatusFromErrorReply(wb.out->response);
    EXPECT_EQ(victim.code(), StatusCode::kTxnConflict) << wb.out->response;
    EXPECT_NE(wb.out->response.find("deadlock"), std::string::npos) << wb.out->response;

    // **The scope was ended, not abandoned.** Inside an explicit
    // transaction that means poisoned rather than unwound - the rows stay
    // and the client must ROLLBACK - which is exactly what `EndWrite`'s
    // unowned arm does for every other failed statement.
    EXPECT_TRUE(b.failed()) << "the victim's scope was left open and its session unpoisoned";
    EXPECT_EQ(dispatcher_->Dispatch("SELECT id, v FROM t", &b).response,
              "ERR current transaction is aborted; commands are ignored until ROLLBACK");

    // And the ROLLBACK is clean: it takes back both B's anchor and the
    // three rows its parked statement had written.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &b).response.rfind("ROLLBACK", 0), 0u);
    Pump();

    // A was never touched - a detector aborts the waiter, never the holder
    // - and proceeds once B's rollback releases row 10.
    ASSERT_TRUE(*wa.done) << "the survivor never proceeded, so the cycle was broken at both ends";
    EXPECT_EQ(wa.out->response.rfind("UPDATED 10", 0), 0u)
        << "every row, counted across A's park: " << wa.out->response;
    // Non-vacuous only because B genuinely wrote rows 6..9 before it was
    // refused: the assertion below is what proves they went with its
    // rollback, and it says nothing at all if B never wrote any.
    ASSERT_EQ(RowsWith(2), 0) << "B's anchor outlived its rollback";
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &a).response.rfind("COMMIT", 0), 0u);
    EXPECT_EQ(locks_->WaitEdgeCount(), 0u);
    // B's writes are gone and A's are all there: nothing of the victim's
    // open scope survived its refusal.
    EXPECT_EQ(RowsWith(4), 0) << "the victim's rows outlived its rollback";
    EXPECT_EQ(RowsWith(3), 10);
}

TEST_F(MidWalkWaitTest, ARefusedAutocommitParkDoesNotLeaveItsScopeForTheNextStatement) {
    // **`RefuseParkedWrite`'s scope cleanup, and the hazard it exists for.**
    // An autocommit statement parked mid-walk owns its transaction: the
    // session does not know about it (`session.transaction()` is null and
    // `in_explicit_txn()` is false), so if the refusal does not end that
    // scope, nothing does. Worse than the leak is what it leaves behind -
    // `session.parked_write()` stays set, and the **next** statement on this
    // session takes the resume branch at the top of `HandleUpdate`, picking
    // up a dead transaction and a stale cursor and skipping every row below
    // it.
    //
    // The wait is ended by the fault net rather than a decide, because that
    // is the refusal arm reachable with a holder that never decides.
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 10", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
    Started walk = Start("UPDATE t SET v = 3 WHERE id >= 5", w);
    Pump();
    ASSERT_FALSE(*walk.done) << "the autocommit statement did not park: " << walk.out->response;

    // Past the net. The holder never decides, so this is the arm that ends
    // it - and AO-R8 calls reaching it a defect report rather than an
    // outcome, which is what this cell is standing in for.
    clock_.Advance(txn::kLockWaitFaultNetNs + 1);
    Pump();
    ASSERT_TRUE(*walk.done) << "the fault net never fired";
    EXPECT_EQ(StatusFromErrorReply(walk.out->response).code(), StatusCode::kTxnConflict)
        << walk.out->response;

    // **The decisive assertion.** A fresh statement on the same session must
    // be a *fresh* statement. With the cleanup compiled out it resumes from
    // the refused statement's cursor instead and answers `UPDATED 6`,
    // covering only rows 5..10 - the rows below the stale cursor silently
    // skipped, which is the wrong answer this block prevents.
    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    Started after = Start("UPDATE t SET v = 5 WHERE id >= 1", w);
    Pump();
    ASSERT_TRUE(*after.done) << after.out->response;
    EXPECT_EQ(after.out->response.rfind("UPDATED 10", 0), 0u)
        << "the next statement resumed into the refused one's scope and cursor: "
        << after.out->response;
    EXPECT_EQ(RowsWith(5), 10);
}

TEST_F(MidWalkWaitTest, WithoutADetectorTheMidWalkParkIsNotOffered) {
    // **The guard is unchanged and this is where it shows.** A statement
    // parked mid-walk is a waiter that holds rows, which is exactly what
    // AO-S3's narrow rule excludes when there is no detector - so with the
    // table taken away the ten-row UPDATE is refused at row 7 rather than
    // parking, and its own rows are unwound. AO-S3b widens what a waiter
    // may be, never where it may wait.
    dispatcher_->set_locks(nullptr);
    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 9 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session w;
        // `v >= 0` for the fixture's reason: the per-row path.
    Started walk = Start("UPDATE t SET v = 1 WHERE v >= 0", w);
    Pump();
    ASSERT_TRUE(*walk.done) << "it parked with no detector to end a cycle it could join";
    EXPECT_EQ(StatusFromErrorReply(walk.out->response).code(), StatusCode::kTxnConflict)
        << walk.out->response;
    EXPECT_EQ(RowsWith(1), 0) << "the refused statement left rows behind";
}

// ---- What AO-S3 deliberately does not wait for ---------------------------

TEST_F(Txn2pcBlockedWriterTest, ATransactionThatAlreadyWroteIsRefusedRatherThanWaited) {
    // **The guard that makes this stage deadlock-free without AO-S4a's
    // detector.** A cycle needs an edge out of a holder, so if every waiter
    // holds nothing, the wait-for graph runs waiters -> holders and cannot
    // close. Restricting the wait to transactions that have written nothing
    // buys exactly that, at the price of keeping the old refusal for a
    // transaction that already holds rows - which AO-S4a lifts once there
    // is a detector to catch what it lets in.
    //
    // Without the guard this cell is the classic deadlock: A holds 7 and
    // wants 8, B holds 8 and wants 7, and both stall until the 11 s fault
    // net aborts them - two clients that used to get an instant retryable
    // conflict now make no progress at all, which is the shape AR2-R10
    // forbids.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_EQ(Local("INSERT INTO t VALUES (8, 1)").rfind("INSERTED", 0), 0u);

    Session a;
    Session b;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &a).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &b).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &a)
                  .response.rfind("UPDATED", 0),
              0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 8", &b)
                  .response.rfind("UPDATED", 0),
              0u);

    // Each now wants the other's row. Both have written, so neither waits.
    auto out_a = std::make_shared<DispatchOutcome>();
    auto done_a = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("UPDATE t SET v = 3 WHERE id = 8", &a, out_a.get()),
        [done_a](const Status&) { *done_a = true; }));
    for (int i = 0; i < 64 && !*done_a; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_TRUE(*done_a) << "a transaction holding rows waited, which is a deadlock edge and "
                            "there is no detector until AO-S4a";
    EXPECT_EQ(StatusFromErrorReply(out_a->response).code(), StatusCode::kTxnConflict)
        << out_a->response;
    EXPECT_TRUE(StatusFromErrorReply(out_a->response).retryable());
}

TEST_F(Txn2pcBlockedWriterTest, ARepeatableReadWriterIsRefusedRatherThanOfferedANarrowerWait) {
    // The second guard. Under `kRepeatableRead` the view is minted at
    // `BEGIN` and never re-minted, so a holder that **commits** after it
    // stays invisible and the re-run refuses on the ground it refused on
    // the first time - a stall ending in the refusal already owed.
    //
    // **The exclusion is conservative rather than exact**, and the cell is
    // named for that. A holder that *aborts* restores the row's prior
    // writer id, and the same view would then admit the write; so a wait
    // here would help in one of the two decides. The level is excluded
    // whole because a wait that pays off only on a rollback is a narrower
    // promise than this stage makes everywhere else, and offering it
    // without saying so would be the convenient answer rather than the
    // true one. The cell below exercises an undecided holder, which is the
    // case both readings agree on.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session rr;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &rr)
                  .response.rfind("ERR", 0),
              std::string::npos)
        << "the level must be settable for this cell to mean anything";
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &rr).response.rfind("BEGIN", 0), 0u);

    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("UPDATE t SET v = 3 WHERE id = 7", &rr, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_TRUE(*done) << "a repeatable-read writer waited for a decision its own view will "
                          "never see: " << out->response;
    EXPECT_EQ(StatusFromErrorReply(out->response).code(), StatusCode::kTxnConflict)
        << out->response;
}

// ---- AO-S3's cutover, on the foreign-key forward check -------------------

TEST_F(Txn2pcBlockedWriterTest, AChildInsertWaitsOutAnInFlightParentAndPassesWhenItCommits) {
    // AO-3 B row 3, the same-core half (the shipped probe's is AO-S5). The
    // forward check answered `kBusy` -> `TxnConflict` because F3 said there
    // was nothing to wait on under a single-writer core. There is now: the
    // check runs at the dispatch fork, before any row work, so the
    // statement has written nothing and the wait is the ordinary statement
    // restart.
    //
    // **The answer genuinely depends on how the parent ends**, which is why
    // refusing was the wrong shape: commit makes the child legal, abort
    // makes it a violation, and the client could not tell which by retrying
    // blindly.
    ASSERT_EQ(Local("CREATE TABLE accounts (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE orders (id int64, account_id int64 REFERENCES accounts) BTREE")
                  .rfind("CREATED", 0),
              0u);

    // A parent row inserted by a transaction that has not decided.
    Session parent;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &parent).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO accounts VALUES (5, 1)", &parent)
                  .response.rfind("INSERTED", 0),
              0u);

    // The child insert cannot answer until the parent does.
    Session child;
    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("INSERT INTO orders VALUES (1, 5)", &child, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_FALSE(*done) << "the child was refused instead of waiting: " << out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &parent).response.rfind("COMMIT", 0), 0u);
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_TRUE(*done) << "the wait never ended";
    EXPECT_EQ(out->response.rfind("INSERTED", 0), 0u) << out->response;
}

TEST_F(Txn2pcBlockedWriterTest, ARepeatableReadChildWaitsOutItsParentBecauseTheCheckViewIsFresh) {
    // AO-S6d's item 17, on the forward check - and the correction the
    // second `critics-developer` pass made to its first build, which had
    // labelled this wait futile on the ground that a repeatable-read
    // transaction's check view is minted at `BEGIN`.
    //
    // It is not. `CheckView` says so in its own first sentence - a
    // constraint check reads latest state, so it mints a view of *now* -
    // and every caller takes it from there whatever the level. So the wait
    // pays off in both arms: the parent's commit makes it visible to the
    // re-run, and its abort makes the answer a terminal `FkViolation`
    // rather than the retryable conflict that sends a client into a loop.
    //
    // It is also what keeps one statement's answer independent of where its
    // parent lives: the cross-owner half of this check parks on the
    // parent's core with no isolation test at all
    // (`fk_probe_service.cpp`), so excluding the level here answered the
    // same `INSERT` differently on a two-core instance.
    ASSERT_EQ(Local("CREATE TABLE accounts (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE orders (id int64, account_id int64 REFERENCES accounts) BTREE")
                  .rfind("CREATED", 0),
              0u);

    Session parent;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &parent).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO accounts VALUES (5, 1)", &parent)
                  .response.rfind("INSERTED", 0),
              0u);

    Session child;
    ASSERT_EQ(dispatcher_->Dispatch("SET ISOLATION LEVEL REPEATABLE READ", &child)
                  .response.rfind("ERR", 0),
              std::string::npos)
        << "the level must be settable for this cell to mean anything";
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &child).response.rfind("BEGIN", 0), 0u);

    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("INSERT INTO orders VALUES (1, 5)", &child, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    // **The mutation**: label this site `kFutile` and the child is refused
    // `TxnConflict` here, where the same statement at READ COMMITTED waits.
    ASSERT_FALSE(*done) << "the repeatable-read child was refused instead of waiting for its "
                           "parent: " << out->response;

    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &parent).response.rfind("COMMIT", 0), 0u);
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_TRUE(*done) << "the wait never ended";
    EXPECT_EQ(out->response.rfind("INSERTED", 0), 0u)
        << "the parent committed before the check view was minted, so the child is legal: "
        << out->response;
}

TEST_F(Txn2pcBlockedWriterTest, AChildInsertWaitingOnAParentThatRollsBackIsAViolation) {
    // The other decide, and the reason the wait is worth having: the same
    // child statement gets two different *correct* answers depending on how
    // the parent ends, and neither is `TxnConflict`. A client retrying a
    // refusal would have discovered this too, eventually, at the cost of a
    // round trip per attempt and a schedule nobody chose.
    ASSERT_EQ(Local("CREATE TABLE accounts (id int64, v int64) BTREE").rfind("CREATED", 0), 0u);
    ASSERT_EQ(Local("CREATE TABLE orders (id int64, account_id int64 REFERENCES accounts) BTREE")
                  .rfind("CREATED", 0),
              0u);

    Session parent;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &parent).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("INSERT INTO accounts VALUES (5, 1)", &parent)
                  .response.rfind("INSERTED", 0),
              0u);

    Session child;
    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("INSERT INTO orders VALUES (1, 5)", &child, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_FALSE(*done);

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &parent).response.rfind("ROLLBACK", 0), 0u);
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_TRUE(*done);
    const Status answered = StatusFromErrorReply(out->response);
    EXPECT_EQ(answered.code(), StatusCode::kFkViolation)
        << "the parent never existed, so the child references nothing: " << out->response;
    EXPECT_FALSE(answered.retryable())
        << "and a retry cannot fix it, which is the difference from the conflict this used to be";
}

// ---- AO-S3's cutover, on an ordinary local holder ------------------------

TEST_F(Txn2pcBlockedWriterTest, AnAutocommitWriterWaitsOutALocalHolderAndThenSeesItsValue) {
    // AO-5's S3 row, in full: "an autocommit `UPDATE` against a row an open
    // transaction holds returns after its `COMMIT` with the new value". No
    // 2PC anywhere in it - just two sessions on one core, which is the
    // shape every OLTP client meets and the one that used to answer
    // `TXN_CONFLICT` and hand the waiting back to the client.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session waiter;
    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("UPDATE t SET v = 3 WHERE id = 7", &waiter, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_FALSE(*done) << "the writer was refused instead of waiting: " << out->response;

    // The holder decides. The waiter's next poll finds it gone, re-runs the
    // statement, and writes over the value the commit left - which is the
    // re-check being mandatory: it does not resume with the answer it had
    // when it parked.
    ASSERT_EQ(dispatcher_->Dispatch("COMMIT", &holder).response.rfind("COMMIT", 0), 0u);
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_TRUE(*done) << "the wait never ended";
    EXPECT_EQ(out->response.rfind("UPDATED", 0), 0u) << out->response;
    EXPECT_NE(Rows().find(",3"), std::string::npos) << Rows();
}

TEST_F(Txn2pcBlockedWriterTest, AWriterWaitingOutAHolderThatRollsBackWritesOverThePriorVersion) {
    // The other decide. The holder's compensations put the old value back
    // before its borrows go (AO-R6's ordering), so the waiter re-runs
    // against the version that was there all along rather than a
    // half-undone one.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);

    Session holder;
    ASSERT_EQ(dispatcher_->Dispatch("BEGIN", &holder).response.rfind("BEGIN", 0), 0u);
    ASSERT_EQ(dispatcher_->Dispatch("UPDATE t SET v = 2 WHERE id = 7", &holder)
                  .response.rfind("UPDATED", 0),
              0u);

    Session waiter;
    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("UPDATE t SET v = 3 WHERE id = 7", &waiter, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_FALSE(*done);

    ASSERT_EQ(dispatcher_->Dispatch("ROLLBACK", &holder).response.rfind("ROLLBACK", 0), 0u);
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    ASSERT_TRUE(*done) << "an abort must end the wait exactly as a commit does";
    EXPECT_EQ(out->response.rfind("UPDATED", 0), 0u) << out->response;
    EXPECT_NE(Rows().find(",3"), std::string::npos) << Rows();
}

TEST_F(Txn2pcBlockedWriterTest, AnOrdinaryInFlightWriterIsNowWaitedOnToo) {
    // **AO-S3 inverted this cell's claim, which is the cutover.** It used
    // to assert the narrowness that made R6-5's block safe: only an
    // *in-doubt* transaction was worth waiting for, and an ordinary
    // in-flight writer got first-updater-wins immediately, on the argument
    // that it "ends on its own" so the client's retry would find the row
    // free.
    //
    // That argument is what AR2-A §1's first axis rejects. The client's
    // retry loop *is* a wait, written in the wrong place: it spins, it
    // burns a round trip per attempt, and it gives up on a schedule nobody
    // chose. A holder that has not decided is one thing whether or not it
    // is prepared (AO-3 B rows 1 and 2), so the wait now covers both and
    // the in-doubt case is subsumed rather than special-cased.
    ASSERT_EQ(Local("INSERT INTO t VALUES (7, 1)").rfind("INSERTED", 0), 0u);
    ASSERT_TRUE(Ship("UPDATE t SET v = 2 WHERE id = 7", 1).status.ok());
    // No prepare: the shipped transaction is open and not in doubt.

    Session local;
    auto out = std::make_shared<DispatchOutcome>();
    auto done = std::make_shared<bool>(false);
    scheduler_->Submit(sched::MakeCoroTask(
        sched::SchedulingGroup::kForeground,
        dispatcher_->DispatchAsync("UPDATE t SET v = 3 WHERE id = 7", &local, out.get()),
        [done](const Status&) { *done = true; }));
    for (int i = 0; i < 64 && !*done; ++i) {
        (void)wal_->DrainOnce();
        scheduler_->RunOnce();
    }
    EXPECT_FALSE(*done) << "an ordinary in-flight holder was still answered with a refusal: "
                        << out->response;
}

// ---- R6-6: a prepared transaction across a graceful stop ----------------------
//
// R6-3 leaves a prepared context standing at shutdown and R6-4 resolves one at
// the next mount, and each was tested against its own half. **What was never
// tested is the joint** — and the joint is where PW3b's shutdown checkpoint
// meets R6-4's floor, which is the one interaction that can lose the
// transaction silently.
//
// The stop sequence ends on every core in `CoreRuntime::ShutdownCheckpoint`,
// which flushes the core's pages and only then checkpoints. **A flush before
// a checkpoint empties the dirty table**, and an empty dirty table would
// otherwise make the redo start the `CHECKPOINT_BEGIN`'s own LSN — past the
// `TXN_PREPARE`, so the next mount would scan from after the record that says
// "do not decide this", read the active-list entry as an ordinary loser, and
// roll back a transaction the coordinator may have committed. The floor is
// what stops that, and this is the fixture that models the case exactly: a
// target whose dirty table is empty *because* the flush already happened.
//
// **Where `RollbackAllEnrolled` falls relative to that checkpoint differs by
// core**, and this fixture models core 0's order — rollback first
// (`expeditor.cpp:1827`), then the final `Sync()` and `Checkpoint()` that end
// `Serve`. A **peer** runs them the other way round: `Serve` calls
// `core->ShutdownCheckpoint()` inside its per-core loop (`:1861`) and the
// peer's `RollbackAllEnrolled` runs later still, in `~CoreRuntime`
// (`core_runtime.cpp:46`) when `cores_.clear()` destroys it. The property
// under test is the same under either order, and that is the point worth
// stating rather than assuming: `RollbackAllEnrolled` never touches a
// prepared context (D4, `APreparedTransactionIsLeftInDoubtAtShutdown...`
// above), so `OldestPreparedLsn()` reports the same LSN on both sides of it
// and the checkpoint reads the same number whichever side it runs on.

class Txn2pcShutdownTest : public Txn2pcParticipantTest {
protected:
    // The shutdown checkpoint's target: every page already written back, so
    // the dirty table contributes no recLSN at all. Scripted rather than a
    // real store's, because what is under test is what the checkpointer does
    // when the dirty table has nothing to say — which on the shutdown path
    // is always.
    class FlushedTarget final : public wal::CheckpointTarget {
    public:
        std::vector<wal::CheckpointDirtyPage> DirtyTable() const override { return {}; }
        Status FlushPages(std::span<const PageId>) override { return Status::OK(); }
    };
};

TEST_F(Txn2pcShutdownTest, APreparedTransactionSurvivesTheStopSequenceAndTheMountAfterIt) {
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());

    // The prepare's LSN, as the transaction manager reports it — which is
    // the wiring claim R6-4's unit test could not make: that a real prepare
    // through the executor reaches `OldestPreparedLsn`, rather than a
    // scripted `ActiveTransactions` answering a number a test chose.
    const wal::Lsn prepared_at = txns_->OldestPreparedLsn();
    ASSERT_NE(prepared_at, 0u) << "the prepare never reached the transaction manager";

    // Step 1 of the stop, in core 0's order: the executor leaves it. D4
    // forbids the unilateral abort, so this core stops still owing an
    // answer. (A peer reaches the same state one step later - see the
    // fixture's note.)
    executor_->RollbackAllEnrolled();
    ASSERT_EQ(executor_->left_in_doubt_at_stop(), 1u);
    ASSERT_EQ(executor_->enrolled(), 1u);

    // Step 2: the shutdown checkpoint, over a dirty table the flush emptied.
    wal::InMemoryCheckpointAnchor anchor;
    FlushedTarget target;
    wal::Checkpointer checkpointer(*wal_, target, *txns_, anchor);
    ASSERT_TRUE(checkpointer.RunToCompletion().ok());
    ASSERT_EQ(anchor.publishes(), 1u);

    // **The published redo start is at or below the prepare**, which with an
    // empty dirty table is true only because of the floor.
    EXPECT_LE(anchor.anchor().redo_start_lsn, prepared_at)
        << "the shutdown checkpoint outran the record that says this transaction is not "
           "this core's to decide";

    // Step 3: the mount after the stop, scanning from exactly the anchor the
    // stop published. The prepare is inside the range and the transaction is
    // the fourth outcome - not a loser, which is what undo would unwind.
    auto analysis = wal::Analyze(*log_device_, /*core_id=*/1,
                                 wal::AnalysisStart{anchor.anchor().redo_start_lsn,
                                                    anchor.anchor().durable_lsn});
    ASSERT_TRUE(analysis.ok()) << analysis.status().message();
    EXPECT_EQ(analysis.value().prepared, 1u)
        << "the mount after a graceful stop no longer finds the prepared transaction";
    EXPECT_EQ(analysis.value().losers, 0u) << "a prepared transaction is not a loser";
    ASSERT_EQ(analysis.value().prepared_txns.size(), 1u);
    const wal::PreparedTxn& found = analysis.value().prepared_txns.begin()->second;
    // And it names the coordinator it was prepared for, which is what the
    // resolution at that mount looks the decision up by (R6-4).
    EXPECT_EQ(found.coordinator_core, kCoordinator);
    EXPECT_EQ(found.coordinator_session_id, kSession);
    EXPECT_EQ(found.coordinator_txn_id, kCoordinatorTxn);
}

TEST_F(Txn2pcShutdownTest, AStopWithNothingPreparedPublishesTheAnchorItAlwaysDid) {
    // The other half, and the one every existing stream is: with nothing
    // prepared the floor contributes nothing, so a shutdown checkpoint over
    // an empty dirty table publishes the `CHECKPOINT_BEGIN` LSN exactly as
    // it did before R6-4 - the property that keeps PW3b's measured
    // "2 records, redo 0" restart bound intact.
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    executor_->RollbackAllEnrolled();
    ASSERT_EQ(executor_->left_in_doubt_at_stop(), 0u) << "nothing was prepared";
    ASSERT_EQ(txns_->OldestPreparedLsn(), 0u);

    wal::InMemoryCheckpointAnchor anchor;
    FlushedTarget target;
    wal::Checkpointer checkpointer(*wal_, target, *txns_, anchor);
    ASSERT_TRUE(checkpointer.RunToCompletion().ok());
    EXPECT_EQ(anchor.anchor().redo_start_lsn, checkpointer.last_checkpoint_lsn())
        << "with nothing prepared the redo start is the checkpoint's own LSN";
}

TEST_F(Txn2pcShutdownTest, TheFloorHoldsAcrossASecondCheckpointWhilstStillInDoubt) {
    // A cadence checkpoint fires, then the stop's. The floor is applied per
    // checkpoint from the live transaction, so a transaction still in doubt
    // pins **every** one of them - which is the price R6-4 named (an
    // in-doubt transaction pins the log's redo start). What this
    // discriminates is a floor applied only on the checkpoint that first
    // saw the prepare: the second checkpoint's own `RedoStartFrom` would
    // then publish its `CHECKPOINT_BEGIN` LSN and the anchor would outrun
    // the record. (It does *not* discriminate a floor computed once and
    // cached - that would answer the same LSN here.)
    ASSERT_TRUE(Ship("INSERT INTO t VALUES (7)", 1).status.ok());
    ASSERT_TRUE(Prepare().status.ok());
    const wal::Lsn prepared_at = txns_->OldestPreparedLsn();
    ASSERT_NE(prepared_at, 0u);

    wal::InMemoryCheckpointAnchor anchor;
    FlushedTarget target;
    wal::Checkpointer checkpointer(*wal_, target, *txns_, anchor);
    ASSERT_TRUE(checkpointer.RunToCompletion().ok());
    // A precondition, not this fixture's subject - the fixture above owns
    // that property. `ASSERT` so that a broken floor fails there and stops
    // here, rather than reporting the *second* checkpoint as what dropped
    // a floor the first one never held.
    ASSERT_LE(anchor.anchor().redo_start_lsn, prepared_at);

    ASSERT_EQ(Local("INSERT INTO t VALUES (9, 1)").rfind("INSERTED", 0), 0u);
    executor_->RollbackAllEnrolled();
    ASSERT_TRUE(checkpointer.RunToCompletion().ok());
    EXPECT_EQ(anchor.publishes(), 2u);
    EXPECT_LE(anchor.anchor().redo_start_lsn, prepared_at)
        << "the second checkpoint dropped the floor the first one held";
}

}  // namespace
}  // namespace kds::server
