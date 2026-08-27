#include "kds/wal/analysis.hpp"

#include <cstddef>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/txn/undo_page.hpp"
#include "kds/wal/memory_log_device.hpp"
#include "kds/wal/payload.hpp"
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
Lsn AppendCheckpointBegin(WalStream& stream, std::span<const CheckpointActiveTxn> active_txns,
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
    std::unique_ptr<MemoryLogDevice> device_ =
        std::move(MemoryLogDevice::Create(kSegmentSize).value());

    StatusOr<AnalysisResult> Run(Lsn redo_start = 0, Lsn anchor_durable = 0) {
        return Analyze((*device_), /*core_id=*/0, AnalysisStart{redo_start, anchor_durable});
    }
};

// ---- The three-way split -------------------------------------------------

TEST_F(AnalysisTest, WinnersLosersAndAbortedAreSplitExactly) {
    {
        auto s = WalStream::Open(device_.get(), 0);
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
    EXPECT_EQ(r.value().transactions.at(10).outcome, TxnOutcome::kWinner);
    EXPECT_EQ(r.value().transactions.at(20).outcome, TxnOutcome::kAborted);
    EXPECT_EQ(r.value().transactions.at(30).outcome, TxnOutcome::kLoser);
    EXPECT_EQ(r.value().winners, 1u);
    EXPECT_EQ(r.value().aborted, 1u);
    EXPECT_EQ(r.value().losers, 1u);
}

TEST_F(AnalysisTest, ACommitIsNotDowngradedByALaterRecordNamingIt) {
    // The generic "this record names a transaction" note must never
    // overwrite a terminal outcome - which it would if it wrote kLoser
    // unconditionally.
    {
        auto s = WalStream::Open(device_.get(), 0);
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
    EXPECT_EQ(r.value().transactions.at(7).outcome, TxnOutcome::kWinner);
}

// ---- Seeding from the checkpoint ----------------------------------------

TEST_F(AnalysisTest, TheCheckpointSeedsBothTables) {
    Lsn checkpoint_lsn = 0;
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        WalStream& w = *s.value();

        // Transaction 42 began before the checkpoint, so no TXN_BEGIN for
        // it appears in the scanned range - the checkpoint's active list
        // is the only thing that knows it exists.
        const CheckpointActiveTxn active[] = {{42, /*last_undo_ptr=*/0}};
        const CheckpointDirtyPage dirty[] = {{700, 4096 + 64}};
        checkpoint_lsn = AppendCheckpointBegin(w, active, dirty);
        ASSERT_NE(checkpoint_lsn, 0u);
        ASSERT_TRUE(w.Sync().ok());
    }

    auto r = Run(checkpoint_lsn);
    ASSERT_TRUE(r.ok()) << r.status().message();

    ASSERT_TRUE(r.value().transactions.count(42) == 1);
    EXPECT_EQ(r.value().transactions.at(42).outcome, TxnOutcome::kLoser)
        << "a transaction live at the checkpoint and never terminated is a loser";
    ASSERT_TRUE(r.value().dirty_pages.count(700) == 1);
    EXPECT_EQ(r.value().dirty_pages.at(700), 4096u + 64u)
        << "the checkpoint's recLSN must survive, not be replaced by the record's LSN";
}

TEST_F(AnalysisTest, APageHandoffRemovesThePageFromTheDirtyPageTable) {
    // PW1c-2, page-lsn-cross-stream.md section 9 rule 3: the page
    // left this stream at the handoff LSN, and rule 1a's flush means
    // everything this stream logged for it before is already in the
    // durable image - so this stream's redo owes the page nothing, and an
    // entry here would point redo at a page another stream owns from that
    // LSN on. (Supersedes PW1c-1's skip-only direction pin.)
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapOverwrite, 5, 800}).ok());

        std::array<std::byte, kPageHandoffPayloadSize> handoff{};
        ASSERT_TRUE(EncodePageHandoff(handoff, PageHandoffPayload{1}).ok());
        // One for the already-dirty page, one for a page this stream never
        // otherwise touched.
        ASSERT_TRUE(s.value()->Append({RecordType::kPageHandoff, kNoTxnId, 800}, handoff).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kPageHandoff, kNoTxnId, 900}, handoff).ok());
        // Twice for one page: erase is idempotent, a repeated handoff (a
        // republished grant) changes nothing.
        ASSERT_TRUE(s.value()->Append({RecordType::kPageHandoff, kNoTxnId, 800}, handoff).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().dirty_pages.count(800), 0u)
        << "a handed-off page must leave the dirty page table";
    EXPECT_EQ(r.value().dirty_pages.count(900), 0u);
}

TEST_F(AnalysisTest, ATransactionalPageHandoffIsCorruption) {
    // A handoff is an ownership event and belongs to no transaction
    // (log_page_handoff.hpp hardcodes 0); a nonzero txn_id would mint a
    // phantom loser, so it is refused as corruption, never interpreted.
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        std::array<std::byte, kPageHandoffPayloadSize> handoff{};
        ASSERT_TRUE(EncodePageHandoff(handoff, PageHandoffPayload{1}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kPageHandoff, /*txn=*/7, 800}, handoff).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
}

TEST_F(AnalysisTest, ATransactionalPageHandoffNamingNoPageIsCorruptionToo) {
    // The 25059bf review's pin for the hoist: the phantom loser is minted
    // from the transaction id, not the page, so the refusal must fire even
    // when the handoff names no page at all.
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        std::array<std::byte, kPageHandoffPayloadSize> handoff{};
        ASSERT_TRUE(EncodePageHandoff(handoff, PageHandoffPayload{1}).ok());
        ASSERT_TRUE(
            s.value()->Append({RecordType::kPageHandoff, /*txn=*/7, kInvalidPageId}, handoff).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.status().code(), StatusCode::kCorruption) << r.status().message();
}

TEST_F(AnalysisTest, APageThatReturnsAfterAHandoffReentersAtItsPostReturnLsn) {
    // A -> B -> A: the page comes back (the handoff *to* this core lives
    // in the other stream, so this stream never sees it) and the
    // re-acquiring write re-dirties it. recLSN must be the post-return
    // record: the pre-handoff history is in the durable image, and
    // replaying it over the returned page is exactly the stale re-apply
    // the PL spec's section 3 describes.
    Lsn returned = 0;
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapOverwrite, 5, 800}).ok());
        std::array<std::byte, kPageHandoffPayloadSize> handoff{};
        ASSERT_TRUE(EncodePageHandoff(handoff, PageHandoffPayload{1}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kPageHandoff, kNoTxnId, 800}, handoff).ok());
        auto lsn = s.value()->Append({RecordType::kHeapOverwrite, 6, 800});
        ASSERT_TRUE(lsn.ok());
        returned = lsn.value();
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run();
    ASSERT_TRUE(r.ok()) << r.status().message();
    ASSERT_EQ(r.value().dirty_pages.count(800), 1u);
    EXPECT_EQ(r.value().dirty_pages.at(800), returned);
}

TEST_F(AnalysisTest, AHandoffRemovesACheckpointSeededEntryToo) {
    // The removal must reach entries analysis never saw dirtied in the
    // scanned range: the checkpoint's dirty-page table seeded the page,
    // and the handoff after the checkpoint is the only record that knows
    // it left.
    Lsn checkpoint_lsn = 0;
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        WalStream& w = *s.value();
        const CheckpointDirtyPage dirty[] = {{700, 64}};
        checkpoint_lsn = AppendCheckpointBegin(w, {}, dirty);
        ASSERT_NE(checkpoint_lsn, 0u);
        std::array<std::byte, kPageHandoffPayloadSize> handoff{};
        ASSERT_TRUE(EncodePageHandoff(handoff, PageHandoffPayload{2}).ok());
        ASSERT_TRUE(w.Append({RecordType::kPageHandoff, kNoTxnId, 700}, handoff).ok());
        ASSERT_TRUE(w.Sync().ok());
    }
    auto r = Run(checkpoint_lsn);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().dirty_pages.count(700), 0u)
        << "a checkpoint-seeded entry must not survive a later handoff";
}

TEST_F(AnalysisTest, ARecLsnIsTheFirstTimeAPageWasDirtiedNotTheLast) {
    std::vector<Lsn> lsns;
    {
        auto s = WalStream::Open(device_.get(), 0);
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
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        for (PageId page : {PageId{10}, PageId{11}, PageId{12}}) {
            auto lsn = s.value()->Append({RecordType::kHeapInsert, 1, page});
            ASSERT_TRUE(lsn.ok());
            lsns.push_back(lsn.value());
        }
        const CheckpointActiveTxn active[] = {{1, /*last_undo_ptr=*/0}};
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
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 1, 11}).ok());
        auto lsn = s.value()->Append({RecordType::kHeapInsert, 1, 11});
        ASSERT_TRUE(lsn.ok());
        second = lsn.value();
        const CheckpointActiveTxn active[] = {{1, /*last_undo_ptr=*/0}};
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
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnBegin, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 1, 128}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnCommit, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }
    auto r = Run(/*redo_start=*/0, /*anchor_durable=*/0);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().records, 3u);
    EXPECT_EQ(r.value().transactions.at(1).outcome, TxnOutcome::kWinner);
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
        auto s = WalStream::Open(device_.get(), 0);
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
        auto s = WalStream::Open(device_.get(), 0);
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
        auto s = WalStream::Open(device_.get(), 0);
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
        auto s = WalStream::Open(device_.get(), 0);
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
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kTxnBegin, 1, kInvalidPageId}).ok());
        ASSERT_TRUE(s.value()->Append({RecordType::kHeapInsert, 1, 128}).ok());
        auto lsn = s.value()->Append({RecordType::kTxnCommit, 1, kInvalidPageId});
        ASSERT_TRUE(lsn.ok());
        last_lsn = lsn.value();
        ASSERT_TRUE(s.value()->Sync().ok());
        segment.resize(static_cast<std::size_t>(kSegmentSize));
        ASSERT_TRUE(device_->ReadAt(0, 0, segment).ok());
    }

    // Wipe the commit record: the transaction becomes a loser, which is
    // the whole point of a durable commit being the thing that decides.
    auto torn_owned = MemoryLogDevice::Create(kSegmentSize);
    ASSERT_TRUE(torn_owned.ok()) << torn_owned.status().message();
    MemoryLogDevice& torn = *torn_owned.value();
    ASSERT_TRUE(torn.CreateSegment(0).ok());
    for (std::size_t i = static_cast<std::size_t>(last_lsn); i < segment.size(); ++i) {
        segment[i] = std::byte{0};
    }
    ASSERT_TRUE(torn.WriteAt(0, 0, segment).ok());

    auto r = Analyze(torn, 0, AnalysisStart{});
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().transactions.at(1).outcome, TxnOutcome::kLoser)
        << "a commit that did not survive is not a commit";
    EXPECT_EQ(r.value().end_lsn, last_lsn);
}

// ---- RV10: the undo-chain head each loser is walked from ----------------

TEST_F(AnalysisTest, ACheckpointSeedsALosersUndoChainHead) {
    // The case the scan alone cannot answer: the transaction's write is
    // below the redo start, so no record in range names it, and the head
    // the checkpoint recorded is the only route to it.
    const std::uint64_t kHead = 0x00020038ull;
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        const CheckpointActiveTxn active[] = {{77, kHead}};
        const CheckpointDirtyPage dirty[] = {{500, 0}};
        AppendCheckpointBegin(*s.value(), active, dirty);
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    auto r = Run();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().transactions.at(77).outcome, TxnOutcome::kLoser);
    EXPECT_EQ(r.value().transactions.at(77).last_undo_ptr, kHead)
        << "the head a checkpoint recorded is what undo starts from";
}

TEST_F(AnalysisTest, AnUndoWriteInRangeAdvancesTheHeadPastTheCheckpoints) {
    // The scan is forward, so the newest record wins: a checkpoint's head
    // describes what the transaction wrote *before* the scan, and anything
    // it wrote inside the scan is newer.
    const std::uint64_t kStale = 0x00020038ull;
    constexpr PageId kUndoPage = 9;
    constexpr std::uint16_t kOffset = 56;
    {
        auto s = WalStream::Open(device_.get(), 0);
        ASSERT_TRUE(s.ok());
        const CheckpointActiveTxn active[] = {{77, kStale}};
        AppendCheckpointBegin(*s.value(), active, {});

        std::vector<std::byte> tail(txn::UndoRecordTailSize(0), std::byte{0});
        txn::UndoRecordFields rec{};
        rec.target_page_id = 300;
        rec.target_slot = 1;
        rec.type = static_cast<std::uint8_t>(txn::UndoRecordType::kInsert);
        rec.pk = 4242;
        ASSERT_TRUE(txn::EncodeUndoRecordTail(tail, rec, {}).ok());

        std::vector<std::byte> buf(kUndoWriteFixedSize + tail.size(), std::byte{0});
        const UndoWritePayload p{0, 0, kOffset, static_cast<std::uint16_t>(tail.size())};
        auto n = EncodeUndoWrite(buf, p, tail);
        ASSERT_TRUE(n.ok()) << n.status().message();
        ASSERT_TRUE(s.value()
                        ->Append({RecordType::kUndoWrite, 77, kUndoPage},
                                 std::span(buf).first(n.value()))
                        .ok());
        ASSERT_TRUE(s.value()->Sync().ok());
    }

    auto r = Run();
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r.value().transactions.at(77).last_undo_ptr,
              txn::EncodeUndoPtr(kUndoPage, kOffset))
        << "the newest record this transaction wrote is the head";
    EXPECT_NE(r.value().transactions.at(77).last_undo_ptr, kStale);
}

TEST(AnalysisUndoPtrTest, TheDuplicatedShiftMatchesTheRealOne) {
    // analysis.hpp repeats txn::kUndoPtrPageIdShift because wal/ sits below
    // txn/ and cannot include it. This is the pin that catches a change to
    // either - the one thing standing between the duplication and a silent
    // divergence that would make every recovered chain head name the wrong
    // page.
    constexpr PageId kPage = 1234;
    constexpr std::uint16_t kOffset = 4321;
    const std::uint64_t theirs = txn::EncodeUndoPtr(kPage, kOffset);
    const std::uint64_t ours =
        (static_cast<std::uint64_t>(kPage) << kAnalysisUndoPtrPageIdShift) | kOffset;
    EXPECT_EQ(ours, theirs);
}

}  // namespace
}  // namespace kds::wal
