#include "kds/server/prepared_resolver.hpp"

#include <unistd.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/superblock.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/recovery_undo.hpp"
#include "kds/txn/undo_log.hpp"
#include "kds/wal/analysis.hpp"
#include "kds/wal/file_log_device.hpp"
#include "kds/wal/log_txn_prepare.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/recovery.hpp"

// R6-4: what a mount does with a transaction this core **prepared** and
// never heard the outcome of (`instructions/v2.4.0/2pc.md` D4).
//
// The property under test is one sentence: *a prepared transaction is
// neither rolled back nor published on this core's own authority.* Three
// ways to get that wrong, and each has its test here:
//
//   1. analysis calls it a loser, and undo rolls back a transaction the
//      coordinator committed and acknowledged to a client;
//   2. nothing resolves it, and redo publishes its writes uncommitted
//      (`txn.md` §8's gap, one protocol up);
//   3. the resolution guesses - at an absent coordinator stream, at a
//      stream with no decision in it - and is right half the time.
//
// The verdict comes from **one lookup in one stream**, never from ordering
// two streams' records against each other (`workplan-crosscore.md`
// guideline 3), which is what the fixture below is arranged to make
// visible: the two streams are written independently and neither knows the
// other's LSNs.

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

// ---- The resolution: one lookup in the coordinator's stream -----------------

class PreparedResolutionTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = (std::filesystem::temp_directory_path() /
                ("kds_prepared_recovery_" + std::string(info->name()) + "_" +
                 std::to_string(::getpid())))
                   .string();
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    // The coordinator's stream, written independently of the
    // participant's - which is the arrangement the test is making a point
    // of: neither knows the other's LSNs, and the resolution needs none.
    void WriteCoordinatorStream(bool commit, bool decide = true) {
        auto device = wal::FileLogDevice::Open(dir_, kCoordinatorCore, kSegment);
        ASSERT_TRUE(device.ok()) << device.status().message();
        wal::WalManagerConfig config;
        config.ring_capacity = wal::kMinRingCapacity;
        auto manager =
            wal::WalManager::Open(device.value().get(), clock_, kCoordinatorCore, config);
        ASSERT_TRUE(manager.ok()) << manager.status().message();
        // An unrelated transaction either way, so the scan has to match on
        // the id rather than on "the only terminal record in the file".
        ASSERT_TRUE(manager.value()->Commit(kCoordinatorTxn + 500,
                                            wal::DurabilityClass::kStrict)
                        .ok());
        if (decide) {
            auto decided = commit ? manager.value()->Commit(kCoordinatorTxn,
                                                            wal::DurabilityClass::kStrict)
                                  : manager.value()->Abort(kCoordinatorTxn);
            ASSERT_TRUE(decided.ok()) << decided.status().message();
        }
        ASSERT_TRUE(manager.value()->SyncAll().ok());
    }

    wal::PreparedTxn Prepared(std::uint32_t coordinator_core = kCoordinatorCore) const {
        wal::PreparedTxn out;
        out.coordinator_core = coordinator_core;
        out.coordinator_session_id = kCoordinatorSession;
        out.coordinator_txn_id = kCoordinatorTxn;
        out.prepare_lsn = 4096;
        return out;
    }

    // Two cores' anchors, both zeroed - "no checkpoint yet", which
    // disables the durable-point check the way it is disabled for a fresh
    // stream. The test that needs a real one sets it.
    std::vector<WalAnchorFields> Anchors() const { return std::vector<WalAnchorFields>(2); }

    CoordinatorStreamResolver Resolver(std::vector<WalAnchorFields> anchors = {}) {
        return CoordinatorStreamResolver(dir_, kSegment, kParticipantCore,
                                         anchors.empty() ? Anchors() : std::move(anchors));
    }

    // One prepared transaction, resolved. The batch shape wrapped for the
    // tests that are about the verdict rather than about the batching.
    StatusOr<wal::TxnOutcome> ResolveOne(CoordinatorStreamResolver& resolver,
                                         const wal::PreparedTxn& prepared,
                                         std::uint64_t participant_txn = kParticipantTxn) {
        std::map<std::uint64_t, wal::PreparedTxn> input{{participant_txn, prepared}};
        auto out = resolver.ResolveAll(input);
        if (!out.ok()) return out.status();
        auto found = out.value().find(participant_txn);
        if (found == out.value().end()) {
            return Status::InvalidArgument("the resolver answered no verdict");
        }
        return found->second;
    }

    sched::ManualClock clock_;
    std::string dir_;
};

TEST_F(PreparedResolutionTest, ACoordinatorThatCommittedMakesThePreparedTransactionAWinner) {
    WriteCoordinatorStream(/*commit=*/true);
    CoordinatorStreamResolver resolver = Resolver();

    auto verdict = ResolveOne(resolver, Prepared());
    ASSERT_TRUE(verdict.ok()) << verdict.status().message();
    EXPECT_EQ(verdict.value(), wal::TxnOutcome::kWinner);
    EXPECT_EQ(resolver.streams_read(), 1u);
    EXPECT_GT(resolver.records_scanned(), 0u);
}

TEST_F(PreparedResolutionTest, ACoordinatorThatAbortedMakesItALoser) {
    WriteCoordinatorStream(/*commit=*/false);
    CoordinatorStreamResolver resolver = Resolver();

    auto verdict = ResolveOne(resolver, Prepared());
    ASSERT_TRUE(verdict.ok()) << verdict.status().message();
    // A loser, not `kAborted`: this stream wrote no compensations, so undo
    // owes the rollback rather than redo having already replayed it.
    EXPECT_EQ(verdict.value(), wal::TxnOutcome::kLoser);
}

TEST_F(PreparedResolutionTest, ACoordinatorThatDecidedNothingAbortsIt) {
    // The coordinator crashed between the prepare and its decision. Its own
    // mount will roll that transaction back too - a transaction with no
    // terminal record is a loser in its own stream - so both sides reach
    // the same verdict independently, which is what makes this evidence
    // rather than a presumption.
    WriteCoordinatorStream(/*commit=*/false, /*decide=*/false);
    CoordinatorStreamResolver resolver = Resolver();

    auto verdict = ResolveOne(resolver, Prepared());
    ASSERT_TRUE(verdict.ok()) << verdict.status().message();
    EXPECT_EQ(verdict.value(), wal::TxnOutcome::kLoser);
}

TEST_F(PreparedResolutionTest, ACoordinatorStreamShortOfItsAnchorRefusesRatherThanAborting) {
    // The shape that would otherwise read as "no decision" and abort a
    // transaction its coordinator may have committed: the stream is intact
    // as far as it goes, but its anchor was published past where it now
    // ends, so the records the anchor depends on are gone. `Analyze`
    // refuses this on its own stream (analysis.hpp); a scan of somebody
    // else's owes the same check, and for a sharper reason.
    WriteCoordinatorStream(/*commit=*/false, /*decide=*/false);
    std::vector<WalAnchorFields> anchors(2);
    anchors[kCoordinatorCore].durable_lsn = 1ull << 40;  // far past the file
    CoordinatorStreamResolver resolver = Resolver(anchors);

    auto verdict = ResolveOne(resolver, Prepared());
    ASSERT_FALSE(verdict.ok());
    EXPECT_EQ(verdict.status().code(), StatusCode::kCorruption);
    EXPECT_NE(verdict.status().message().find("before the durable point"), std::string::npos)
        << verdict.status().message();
}

TEST_F(PreparedResolutionTest, OneCoordinatorsStreamIsScannedOnceForEveryTransactionOnIt) {
    // The batch's whole point: three of this core's transactions decided by
    // one coordinator cost one scan, not three.
    WriteCoordinatorStream(/*commit=*/true);
    CoordinatorStreamResolver resolver = Resolver();

    std::map<std::uint64_t, wal::PreparedTxn> input;
    for (std::uint64_t i = 0; i < 3; ++i) input.emplace(kParticipantTxn + i, Prepared());
    auto out = resolver.ResolveAll(input);
    ASSERT_TRUE(out.ok()) << out.status().message();
    EXPECT_EQ(out.value().size(), 3u);
    EXPECT_EQ(resolver.streams_read(), 1u) << "one stream, one scan";
    for (const auto& [txn_id, verdict] : out.value()) {
        (void)txn_id;
        EXPECT_EQ(verdict, wal::TxnOutcome::kWinner);
    }
}

TEST_F(PreparedResolutionTest, APrepareNamingACoreThisDatabaseDoesNotHaveIsCorruption) {
    CoordinatorStreamResolver resolver = Resolver();
    auto verdict = ResolveOne(resolver, Prepared(/*coordinator_core=*/9));
    ASSERT_FALSE(verdict.ok());
    EXPECT_EQ(verdict.status().code(), StatusCode::kCorruption);
    EXPECT_NE(verdict.status().message().find("this database has 2 core(s)"), std::string::npos)
        << verdict.status().message();
}

TEST_F(PreparedResolutionTest, AnAbsentCoordinatorStreamRefusesRatherThanAborting) {
    // Nothing was written for core 1 at all. Every core publishes a
    // completion checkpoint at every mount (RC08), so this is a log
    // directory that lost a file - and answering "abort" could discard a
    // transaction its coordinator committed and told a client about.
    CoordinatorStreamResolver resolver = Resolver();

    auto verdict = ResolveOne(resolver, Prepared());
    ASSERT_FALSE(verdict.ok());
    EXPECT_EQ(verdict.status().code(), StatusCode::kCorruption);
    EXPECT_NE(verdict.status().message().find("will not guess"), std::string::npos)
        << verdict.status().message();
}

TEST_F(PreparedResolutionTest, APrepareNamingThisCoreAsItsOwnCoordinatorIsCorruption) {
    // A coordinator never writes TXN_PREPARE; only a participant does.
    CoordinatorStreamResolver resolver = Resolver();
    auto verdict = ResolveOne(resolver, Prepared(/*coordinator_core=*/kParticipantCore));
    ASSERT_FALSE(verdict.ok());
    EXPECT_EQ(verdict.status().code(), StatusCode::kCorruption);
}

// ---- The mount: what the whole phase does -----------------------------------

class PreparedMountTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = (std::filesystem::temp_directory_path() /
                ("kds_prepared_mount_" + std::string(info->name()) + "_" +
                 std::to_string(::getpid())))
                   .string();
        std::filesystem::remove_all(dir_);

        // The participant's stream: one prepared, undecided transaction.
        auto device = wal::FileLogDevice::Open(dir_, kParticipantCore, kSegment);
        ASSERT_TRUE(device.ok()) << device.status().message();
        participant_ = std::move(device.value());
        wal::WalManagerConfig config;
        config.ring_capacity = wal::kMinRingCapacity;
        auto manager = wal::WalManager::Open(participant_.get(), clock_, kParticipantCore,
                                             config);
        ASSERT_TRUE(manager.ok()) << manager.status().message();
        wal_ = std::move(manager.value());
        auto lsn = wal::LogTxnPrepare(wal_.get(), kParticipantTxn, kCoordinatorCore,
                                      kCoordinatorSession, kCoordinatorTxn);
        ASSERT_TRUE(lsn.ok()) << lsn.status().message();
        ASSERT_TRUE(wal_->SyncAll().ok());
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    void WriteCoordinatorDecision(bool commit) {
        auto device = wal::FileLogDevice::Open(dir_, kCoordinatorCore, kSegment);
        ASSERT_TRUE(device.ok()) << device.status().message();
        wal::WalManagerConfig config;
        config.ring_capacity = wal::kMinRingCapacity;
        auto manager =
            wal::WalManager::Open(device.value().get(), clock_, kCoordinatorCore, config);
        ASSERT_TRUE(manager.ok()) << manager.status().message();
        auto decided =
            commit ? manager.value()->Commit(kCoordinatorTxn, wal::DurabilityClass::kStrict)
                   : manager.value()->Abort(kCoordinatorTxn);
        ASSERT_TRUE(decided.ok()) << decided.status().message();
        ASSERT_TRUE(manager.value()->SyncAll().ok());
    }

    StatusOr<wal::RecoveryReport> Recover(wal::PreparedResolver* resolver) {
        txn::UndoLog undo_log(store_, /*wal=*/nullptr);
        txn::RecoveryUndo undo(undo_log, /*wal=*/nullptr);
        return wal::RecoverCore(*participant_, kParticipantCore, store_, wal::AnalysisStart{},
                                &undo, /*clock=*/nullptr, resolver);
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    sched::ManualClock clock_;
    std::string dir_;
    std::unique_ptr<wal::FileLogDevice> participant_;
    std::unique_ptr<wal::WalManager> wal_;
};

TEST_F(PreparedMountTest, APreparedTransactionWithNoResolverRefusesTheMount) {
    // The refusal `RecoverCore` already makes for losers with no undo
    // phase, one protocol up: this core may neither roll the transaction
    // back nor publish it, so it will not open at all.
    auto report = Recover(/*resolver=*/nullptr);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(report.status().message().find("promised not to decide"), std::string::npos)
        << report.status().message();
}

TEST_F(PreparedMountTest, ACommittedVerdictLeavesTheTransactionStandingAndUndoUntouched) {
    WriteCoordinatorDecision(/*commit=*/true);
    CoordinatorStreamResolver resolver(dir_, kSegment, kParticipantCore,
                                      std::vector<WalAnchorFields>(2));

    auto report = Recover(&resolver);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_EQ(report.value().prepared, 1u);
    EXPECT_EQ(report.value().prepared_committed, 1u);
    EXPECT_EQ(report.value().prepared_aborted, 0u);
    // It joins the winners, so undo has nothing to do and never runs.
    EXPECT_EQ(report.value().analysis.winners, 1u);
    EXPECT_EQ(report.value().analysis.losers, 0u);
    EXPECT_EQ(report.value().analysis.prepared, 0u) << "resolved, so no longer open";
    EXPECT_FALSE(report.value().undo_ran);
}

TEST_F(PreparedMountTest, AnAbortedVerdictHandsTheTransactionToUndo) {
    WriteCoordinatorDecision(/*commit=*/false);
    CoordinatorStreamResolver resolver(dir_, kSegment, kParticipantCore,
                                      std::vector<WalAnchorFields>(2));

    auto report = Recover(&resolver);
    ASSERT_TRUE(report.ok()) << report.status().message();
    EXPECT_EQ(report.value().prepared, 1u);
    EXPECT_EQ(report.value().prepared_aborted, 1u);
    EXPECT_EQ(report.value().analysis.losers, 1u);
    EXPECT_EQ(report.value().analysis.winners, 0u);
    // Undo ran for it - and had nothing to compensate, because this
    // fixture's transaction wrote no rows. What is under test is which set
    // it joined.
    EXPECT_TRUE(report.value().undo_ran);
}

TEST_F(PreparedMountTest, ARefusedResolutionRefusesTheMountRatherThanDefaulting) {
    // The coordinator's stream is absent; the resolver refuses, and the
    // refusal is the mount's answer rather than a verdict.
    CoordinatorStreamResolver resolver(dir_, kSegment, kParticipantCore,
                                      std::vector<WalAnchorFields>(2));
    auto report = Recover(&resolver);
    ASSERT_FALSE(report.ok());
    EXPECT_EQ(report.status().code(), StatusCode::kCorruption);
    EXPECT_NE(report.status().message().find("resolving prepared transaction"),
              std::string::npos)
        << report.status().message();
}

}  // namespace
}  // namespace kds::server
