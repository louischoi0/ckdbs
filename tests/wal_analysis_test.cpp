#include "kds/wal/analysis.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/stream.hpp"

// RC02 - recovery's analysis phase (docs/workplan-wal-recovery.md).
//
// Analysis answers two questions and nothing else: which pages need
// replaying, and which transactions were left unfinished. Everything here
// is a scripted log rather than a crashed database, which is the point of
// the phase reading only the device.
//
// The case worth reading first is the three-way outcome split. A durable
// TXN_ABORT does not make a loser: rollback's compensations are ordinary
// logged mutations written *before* it (txn.md section 6), and a stream is
// a durable prefix, so redo replays them and undo owes that transaction
// nothing. Getting this wrong costs a second rollback over an already
// rolled-back transaction - which the page_lsn gate would absorb, silently,
// while doing work recovery does not need.

namespace kds::wal {
namespace {

constexpr std::uint64_t kSegmentSize = 16 * 1024;

// Appends a CHECKPOINT_BEGIN carrying the two tables, as the checkpointer
// does, and returns its LSN.
Lsn AppendCheckpointBegin(WalStream& stream, std::span<const std::uint64_t> active_txns,
                          std::span<const CheckpointDirtyPage> dirty_pages) {
    std::vector<std::byte> payload(
        CheckpointBeginSize(active_txns.size(), dirty_pages.size()), std::byte{0});
    auto encoded = EncodeCheckpointBegin(payload, active_txns, dirty_pages);
    EXPECT_TRUE(encoded.ok()) << encoded.status().message();
    auto lsn = stream.Append({RecordType::kCheckpointBegin, kNoTxnId, kInvalidPageId},
                             std::span(payload).first(encoded.value()));
    EXPECT_TRUE(lsn.ok()) << lsn.status().message();
    return lsn.ok() ? lsn.value() : 0;
}

class AnalysisTest : public ::testing::Test {
protected:
    MemoryLogDevice device_{kSegmentSize};

    StatusOr<AnalysisResult> Run(Lsn redo_start = 0, Lsn anchor_durable = 0) {
        return Analyze(device_, /*core_id=*/0, AnalysisStart{redo_start, anchor_durable});
    }
};

// ---- The three-way split -------------------------------------------------

TEST_F(AnalysisTest, WinnersLosersAndAbortedAreSplitExactly) {
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        WalStream& w = *s.value();

        // 10 commits. 20 aborts (compensated in the log before its abort
        // record). 30 is cut off by the crash.
        ASSERT_TRUE(w.Append({RecordType::kTxnBegin, 10, kInvalidPageId}).ok());
        ASSERT_TRUE(w.Append({RecordType::kHeapInsert, 10, 500}).ok());
        ASSERT_TRUE(w.Append({RecordType::kTxnCommit, 10, kInvalidPageId}).ok());

        ASSERT_TRUE(w.Append({RecordType::kTxnBegin, 20, kInvalidPageId}).ok());
        ASSERT_TRUE(w.Append({RecordType::kHeapInsert, 20, 501}).ok());
        ASSERT_TRUE(w.Append({RecordType::kSlotRetire, 20, 501}).ok());  // its compensation
        ASSERT_TRUE(w.Append({RecordType::kTxnAbort, 20, kInvalidPageId}).ok());

        ASSERT_TRUE(w.Append({RecordType::kTxnBegin, 30, kInvalidPageId}).ok());
        ASSERT_TRUE(w.Append({RecordType::kHeapInsert, 30, 502}).ok());

        ASSERT_TRUE(w.Sync().ok());
    }

    auto r = Run();
    ASSERT_TRUE(r.ok()) << r.status().message();

    ASSERT_EQ(r.value().transactions.size(), 3u);
    EXPECT_EQ(r.value().transactions.at(10), TxnOutcome::kWinner);
    EXPECT_EQ(r.value().transactions.at(20), TxnOutcome::kAborted);
    EXPECT_EQ(r.value().transactions.at(30), TxnOutcome::kLoser);
    EXPECT_EQ(r.value().winners, 1u);
    EXPECT_EQ(r.value().aborted, 1u);
    EXPECT_EQ(r.value().losers, 1u);
}

TEST_F(AnalysisTest, ACommitIsNotDowngradedByALaterRecordNamingIt) {
    // The generic "this record names a transaction" note must never
    // overwrite a terminal outcome - which it would if it wrote kLoser
    // unconditionally.
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnBegin, 7, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnCommit, 7, kInvalidPageId}).ok());
        // A rollback compensation from *another* transaction that happens
        // to carry the same id would be a bug; what is realistic is a
        // SLOT_RETIRE stamped with the committed id by a purge pass.
        ASSERT_TRUE(s.value()->Append({RecordType::kSlotRetire, 7, 900}).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().transactions.at(7), TxnOutcome::kWinner);
}

// ---- Seeding from the checkpoint ----------------------------------------

TEST_F(AnalysisTest, TheCheckpointSeedsBothTables) {
    Lsn checkpoint_lsn = 0;
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        WalStream& w = *s.value();

        // Transaction 42 began before the checkpoint, so no TXN_BEGIN for
        // it appears in the scanned range - the checkpoint's active list
        // is the only thing that knows it exists.
        const std::uint64_t active[] = {42};
        const CheckpointDirtyPage dirty[] = {{700, 4096 + 64}};
        checkpoint_lsn = AppendCheckpointBegin(w, active, dirty);
        ASSERT_NE(checkpoint_lsn, 0u);
        ASSERT_TRUE(w.Sync().ok());
    }

    auto r = Run(checkpoint_lsn);
    ASSERT_TRUE(r.ok()) << r.status().message();

    ASSERT_TRUE(r.value().transactions.count(42) == 1);
    EXPECT_EQ(r.value().transactions.at(42), TxnOutcome::kLoser)
        << "a transaction live at the checkpoint and never terminated is a loser";
    ASSERT_TRUE(r.value().dirty_pages.count(700) == 1);
    EXPECT_EQ(r.value().dirty_pages.at(700), 4096u + 64u)
        << "the checkpoint's recLSN must survive, not be replaced by the record's LSN";
}

TEST_F(AnalysisTest, ARecLsnIsTheFirstTimeAPageWasDirtiedNotTheLast) {
    std::vector<Lsn> lsns;
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        for (int i = 0; i < 3; ++i) {
            auto lsn = s.value()->Append({RecordType::kHeapOverwrite, 5, 800});
            ASSERT_TRUE(lsn.ok());
            lsns.push_back(lsn.value());
        }
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().dirty_pages.at(800), lsns.front())
        << "redo must start at the oldest record that makes the page whole";
}

// ---- The redo start ------------------------------------------------------

TEST_F(AnalysisTest, TheRedoStartIsTheOldestRecLsn) {
    // Real recLSNs, taken from records actually appended before the
    // checkpoint - which is the only shape a checkpoint's dirty table can
    // really have, and the shape that makes the floor argument checkable.
    std::vector<Lsn> lsns;
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        for (PageId page : {PageId{10}, PageId{11}, PageId{12}}) {
            auto lsn = s.value()->Append({RecordType::kHeapInsert, 1, page});
            ASSERT_TRUE(lsn.ok());
            lsns.push_back(lsn.value());
        }
        const std::uint64_t active[] = {1};
        // Pages 20-22 appear *only* in the checkpoint's table, so this
        // exercises the seeding path rather than re-stating what the scan
        // already saw. Not in recLSN order, so a min() that happened to
        // take the first entry would pass by luck.
        const CheckpointDirtyPage dirty[] = {
            {22, lsns[2]}, {20, lsns[0]}, {21, lsns[1]}};
        AppendCheckpointBegin(*s.value(), active, dirty);
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run(/*redo_start=*/4096);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().redo_start_lsn, lsns.front());
    // Six pages known dirty: three the scan saw, three the checkpoint
    // named. Both sources reach the table.
    EXPECT_EQ(r.value().dirty_pages.size(), 6u);
    EXPECT_EQ(r.value().dirty_pages.at(20), lsns[0]);
}

TEST_F(AnalysisTest, ARecLsnOfZeroIsSkippedAndDoesNotDragTheRedoStartToZero) {
    // wal.md section 11-3's rule, and the one a second copy loses. A page
    // dirty but described by no record must not make recovery replay the
    // whole stream.
    Lsn second = 0;
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 1, 11}).ok());
        auto lsn = s.value()->Append({RecordType::kHeapInsert, 1, 11});
        ASSERT_TRUE(lsn.ok());
        second = lsn.value();
        const std::uint64_t active[] = {1};
        const CheckpointDirtyPage dirty[] = {{10, 0}, {11, second}};
        AppendCheckpointBegin(*s.value(), active, dirty);
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    // Page 11's own first record is older than the checkpoint's recLSN for
    // it, and the older one wins - so the assertion is against that, and
    // page 10's zero must not appear at all.
    auto r = Run(/*redo_start=*/4096);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().dirty_pages.at(10), 0u);
    EXPECT_NE(r.value().redo_start_lsn, 0u) << "a recLSN of 0 was min()ed in";
    EXPECT_EQ(r.value().redo_start_lsn, 4096u) << "page 11's first record is the oldest";
    EXPECT_LT(second, r.value().end_lsn);
}

TEST(RedoStartFromTest, TheSharedRuleSkipsZeroAndFloorsAtTheCheckpoint) {
    // The rule itself, since two callers depend on it: the checkpointer
    // computing it forward and analysis recomputing it backward.
    EXPECT_EQ(RedoStartFrom(900, std::span<const CheckpointDirtyPage>{}), 900u);

    const CheckpointDirtyPage some[] = {{1, 0}, {2, 1200}, {3, 800}};
    EXPECT_EQ(RedoStartFrom(900, std::span<const CheckpointDirtyPage>(some, 3)), 800u);

    const CheckpointDirtyPage all_zero[] = {{1, 0}, {2, 0}};
    EXPECT_EQ(RedoStartFrom(900, std::span<const CheckpointDirtyPage>(all_zero, 2)), 900u);

    std::map<PageId, Lsn> as_map{{1, 0}, {2, 1200}, {3, 800}};
    EXPECT_EQ(RedoStartFrom(900, as_map), 800u) << "the two overloads must agree";
}

// ---- No checkpoint at all ------------------------------------------------

TEST_F(AnalysisTest, ALogWithNoCheckpointIsAnalyzedFromTheStart) {
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnBegin, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 1, 128}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnCommit, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run(/*redo_start=*/0, /*anchor_durable=*/0);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().records, 3u);
    EXPECT_EQ(r.value().transactions.at(1), TxnOutcome::kWinner);
    EXPECT_EQ(r.value().dirty_pages.size(), 1u);
}

// ---- The honesty check ---------------------------------------------------

TEST_F(AnalysisTest, AStreamShorterThanItsAnchorClaimsIsCorruption) {
    // The failure this check exists for: a log that lost the records its
    // anchor depends on scans to zero records, which is byte-identical to
    // a clean shutdown right after a checkpoint. Without the durable-point
    // comparison, recovery's quietest failure is a silent empty replay
    // onto a database that needed one.
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 1, 128}).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run(/*redo_start=*/4096, /*anchor_durable=*/1'000'000);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
    EXPECT_NE(r.status().message().find("before the durable point"), std::string::npos)
        << r.status().message();
}

TEST_F(AnalysisTest, AStreamThatReachesItsAnchorsDurablePointIsAccepted) {
    // The control: the same check must not refuse a healthy stream.
    Lsn durable = 0;
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 1, 128}).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
        durable = s.value()->durable_lsn();
    }
    auto r = Run(/*redo_start=*/4096, /*anchor_durable=*/durable);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().end_lsn, durable);
}

// ---- RV4's inputs --------------------------------------------------------

TEST_F(AnalysisTest, TheLargestPageAndTransactionIdAreReported) {
    // RV4: the superblock's high-water mark is unlogged, so a crash can
    // revert it below a page the log names. Recovery raises it past this.
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 4, 300}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 91, 4096}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 12, 77}).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().max_page_id, 4096u);
    EXPECT_EQ(r.value().max_txn_id, 91u);
}

TEST_F(AnalysisTest, RecordsWithNoPageDoNotEnterTheDirtyTable) {
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnBegin, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnCommit, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_TRUE(r.value().dirty_pages.empty());
    EXPECT_EQ(r.value().max_page_id, kInvalidPageId);
}

// ---- A torn tail is not a failure ---------------------------------------

TEST_F(AnalysisTest, ATornTailIsMeteredAndTheRecordsBeforeItStand) {
    std::vector<std::byte> segment;
    Lsn last_lsn = 0;
    {
        auto s = WalStream::Open(&device_, 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnBegin, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 1, 128}).ok());
        auto lsn = s.value()->Append({RecordType::kTxnCommit, 1, kInvalidPageId});
        ASSERT_TRUE(lsn.ok());
        last_lsn = lsn.value();
        ASSERT_TRUE(s.value()->Sync().ok());
        segment.resize(static_cast<std::size_t>(kSegmentSize));
        ASSERT_TRUE(device_.ReadAt(0, 0, segment).ok());
    }

    // Wipe the commit record: the transaction becomes a loser, which is
    // the whole point of a durable commit being the thing that decides.
    MemoryLogDevice torn(kSegmentSize);
    ASSERT_TRUE(torn.CreateSegment(0).ok());
    for (std::size_t i = static_cast<std::size_t>(last_lsn); i < segment.size(); ++i) {
        segment[i] = std::byte{0};
    }
    ASSERT_TRUE(torn.WriteAt(0, 0, segment).ok());

    auto r = Analyze(torn, 0, AnalysisStart{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().transactions.at(1), TxnOutcome::kLoser)
        << "a commit that did not survive is not a commit";
    EXPECT_EQ(r.value().end_lsn, last_lsn);
}

}  // namespace
}  // namespace kds::wal
