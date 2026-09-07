#include <memory>
#include <utility>

#include <gtest/gtest.h>

#include "kds/sched/clock.hpp"
#include "kds/wal/analysis.hpp"
#include "kds/wal/log_txn_prepare.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"

// R6-4: what a mount does with a transaction this core **prepared** and
// never heard the outcome of (`instructions/v2.4.0/2pc.md` D4).
//
// The property under test is one sentence: *a prepared transaction is
// neither rolled back nor published on this core's own authority.* The way
// to get it wrong here is for analysis to call it a loser, so that undo
// rolls back a transaction the coordinator committed and acknowledged to a
// client - hence the fourth outcome, and hence these cells.
//
// **Only analysis is tested here.** The verdict itself is `RecoverCore`'s,
// and with one stream for the instance (AM-S4(d)) it is a lookup of the
// coordinator's transaction id in the table this same scan built:
// `wal_recovery_test.cpp` owns those cells. The `CoordinatorStreamResolver`
// that opened a second core's log, and the fixtures that exercised it, went
// with the topology that made a second log exist.

namespace kds::server {
namespace {

constexpr std::uint64_t kSegment = 1 << 20;
constexpr std::uint32_t kParticipantCore = 0;
constexpr std::uint32_t kCoordinatorCore = 1;
constexpr std::uint64_t kParticipantTxn = 4001;
constexpr std::uint64_t kCoordinatorTxn = 9002;
constexpr std::uint64_t kCoordinatorSession = 77;

// ---- Analysis: the fourth outcome -------------------------------------------

class PreparedAnalysisTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = wal::MemoryLogDevice::Create(kSegment);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        wal::WalManagerConfig config;
        config.ring_capacity = wal::kMinRingCapacity;
        auto manager =
            wal::WalManager::Open(device_.get(), clock_, kParticipantCore, config);
        ASSERT_TRUE(manager.ok()) << manager.status().message();
        wal_ = std::move(manager.value());
    }

    void LogPrepare(std::uint64_t participant_txn = kParticipantTxn) {
        auto lsn = wal::LogTxnPrepare(wal_.get(), participant_txn, kCoordinatorCore,
                                      kCoordinatorSession, kCoordinatorTxn);
        ASSERT_TRUE(lsn.ok()) << lsn.status().message();
    }

    wal::AnalysisResult Analyze() {
        EXPECT_TRUE(wal_->Flush().ok());
        auto out = wal::Analyze(*device_, kParticipantCore, wal::AnalysisStart{});
        EXPECT_TRUE(out.ok()) << out.status().message();
        return out.ok() ? out.value() : wal::AnalysisResult{};
    }

    sched::ManualClock clock_;
    std::unique_ptr<wal::MemoryLogDevice> device_;
    std::unique_ptr<wal::WalManager> wal_;
};

TEST_F(PreparedAnalysisTest, APreparedTransactionIsItsOwnOutcomeAndNotALoser) {
    LogPrepare();
    const wal::AnalysisResult out = Analyze();

    EXPECT_EQ(out.prepared, 1u);
    // **Not a loser**, which is the whole point: undo would roll back a
    // transaction this core promised not to abort.
    EXPECT_EQ(out.losers, 0u);
    EXPECT_EQ(out.winners, 0u);
    ASSERT_EQ(out.prepared_txns.size(), 1u);

    const auto& [txn_id, prepared] = *out.prepared_txns.begin();
    EXPECT_EQ(txn_id, kParticipantTxn) << "keyed by this stream's own transaction id (D2)";
    EXPECT_EQ(prepared.coordinator_core, kCoordinatorCore);
    EXPECT_EQ(prepared.coordinator_session_id, kCoordinatorSession);
    EXPECT_EQ(prepared.coordinator_txn_id, kCoordinatorTxn);
    EXPECT_NE(prepared.prepare_lsn, 0u);
    ASSERT_NE(out.transactions.find(kParticipantTxn), out.transactions.end());
    EXPECT_EQ(out.transactions.at(kParticipantTxn).outcome, wal::TxnOutcome::kPrepared);
}

TEST_F(PreparedAnalysisTest, APrepareThisStreamThenDecidedNeedsNoResolution) {
    // The ordinary path: the decide leg arrived and the participant wrote
    // its own terminal record. Nothing is owed to any other stream.
    LogPrepare();
    ASSERT_TRUE(wal_->Commit(kParticipantTxn, wal::DurabilityClass::kRelaxed).ok());

    const wal::AnalysisResult out = Analyze();
    EXPECT_EQ(out.prepared, 0u);
    EXPECT_EQ(out.winners, 1u);
    EXPECT_TRUE(out.prepared_txns.empty())
        << "a decided transaction is not an open question, whatever it prepared";
}

TEST_F(PreparedAnalysisTest, APrepareThisStreamThenAbortedIsAborted) {
    LogPrepare();
    ASSERT_TRUE(wal_->Abort(kParticipantTxn).ok());

    const wal::AnalysisResult out = Analyze();
    EXPECT_EQ(out.prepared, 0u);
    EXPECT_EQ(out.aborted, 1u);
    EXPECT_EQ(out.losers, 0u);
    EXPECT_TRUE(out.prepared_txns.empty());
}

TEST_F(PreparedAnalysisTest, TwoPreparedTransactionsAreBothCarried) {
    LogPrepare(kParticipantTxn);
    LogPrepare(kParticipantTxn + 1);
    const wal::AnalysisResult out = Analyze();
    EXPECT_EQ(out.prepared, 2u);
    EXPECT_EQ(out.prepared_txns.size(), 2u);
}

}  // namespace
}  // namespace kds::server
