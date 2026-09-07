#include "kds/txn/manager.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/txn/trx_id.hpp"
#include "kds/txn/undo_log.hpp"

// docs/spec/txn.md sections 1, 5 and 6: the two levels' read views,
// first-updater-wins, and rollback's compensations.
//
// Deterministic and socket-free (rules.md section 4): an InMemoryPageStore
// and no WalManager, which is the unlogged path. The *logged* half - that
// the compensation records reach the device - is txn_rollback_test.cpp's.

namespace kds::txn {
namespace {

constexpr PageId kFirstUserPageId = 128;

// A well-formed tuple payload: the Keystone word carrying `pk`, then the
// test's text. Rollback identifies a row by the id in that word before it
// compensates (txn/manager.hpp's RowLocator), so a payload without one is
// not a row this manager could ever be asked to unwind - these used to be
// bare text, and the check had nothing to read.
std::vector<std::byte> BytesOf(std::string_view s, std::uint64_t pk = 1) {
    std::vector<std::byte> out(kKeystoneWordSize + s.size());
    const std::uint64_t word = Keystone::Encode(pk, 0, 0).value();
    std::memcpy(out.data(), &word, kKeystoneWordSize);
    std::memcpy(out.data() + kKeystoneWordSize, s.data(), s.size());
    return out;
}

// The text half, without the Keystone word the payload opens with.
std::string StringOf(std::span<const std::byte> b) {
    if (b.size() < kKeystoneWordSize) return std::string();
    std::string out(b.size() - kKeystoneWordSize, '\0');
    std::memcpy(out.data(), b.data() + kKeystoneWordSize, out.size());
    return out;
}

class TxnManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        superblock_ = server::SuperBlock::CreateFresh(/*now_unix_seconds=*/1000);
        ids_ = std::make_unique<TrxIdSequence>(superblock_);
        undo_ = std::make_unique<UndoLog>(store_, /*wal=*/nullptr);
        mgr_ = std::make_unique<TransactionManager>(*ids_, *undo_, store_, /*wal=*/nullptr);
    }

    Transaction* Begin(IsolationLevel level = IsolationLevel::kReadCommitted) {
        auto txn = mgr_->Begin(level);
        EXPECT_TRUE(txn.ok()) << txn.status().message();
        return txn.ok() ? txn.value() : nullptr;
    }

    // A page with one tuple on it, so the compensation tests have something
    // real to put back.
    struct Row {
        PageId page_id = kInvalidPageId;
        std::uint16_t slot = 0;
    };

    Row PlaceRow(std::string_view payload, std::uint64_t writer, std::uint64_t pk = 1) {
        auto created = store_.CreateNew();
        EXPECT_TRUE(created.ok());
        auto page = heap::PageView::CreateEmpty(created.value().second.bytes(), /*min_key=*/0);
        EXPECT_TRUE(page.ok());
        auto slot = page.value().InsertTuple(BytesOf(payload, pk), writer, kNoUndoPtr);
        EXPECT_TRUE(slot.ok());
        return Row{created.value().first, slot.value()};
    }

    std::string PayloadAt(const Row& row) {
        auto bytes = store_.Get(row.page_id);
        EXPECT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        auto tuple = page.ReadTuple(row.slot);
        EXPECT_TRUE(tuple.ok());
        return tuple.ok() ? StringOf(tuple.value().payload) : std::string();
    }

    heap::PageView::Tuple TupleAt(const Row& row) {
        auto bytes = store_.Get(row.page_id);
        EXPECT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        auto tuple = page.ReadTuple(row.slot);
        EXPECT_TRUE(tuple.ok());
        return tuple.value();
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    server::SuperBlock superblock_;
    std::unique_ptr<TrxIdSequence> ids_;
    std::unique_ptr<UndoLog> undo_;
    std::unique_ptr<TransactionManager> mgr_;
};

// ---- Isolation levels (section 1) ----------------------------------------

TEST_F(TxnManagerTest, ReadCommittedResnapshotsPerStatement) {
    Transaction* reader = Begin(IsolationLevel::kReadCommitted);
    ASSERT_NE(reader, nullptr);
    const std::uint64_t at_begin = reader->view().snapshot_lsn;

    Transaction* writer = Begin();
    ASSERT_NE(writer, nullptr);
    const std::uint64_t writer_id = writer->id();
    EXPECT_FALSE(reader->view().Visible(writer_id))
        << "the writer had not started when the reader's view was taken";

    ASSERT_TRUE(mgr_->Commit(*writer, wal::DurabilityClass::kRelaxed).ok());
    mgr_->Release(*writer);
    EXPECT_FALSE(reader->view().Visible(writer_id))
        << "the statement's view is pinned until the next statement boundary";

    ASSERT_TRUE(mgr_->StartStatement(*reader).ok());
    EXPECT_GT(reader->view().snapshot_lsn, at_begin);
    EXPECT_TRUE(reader->view().Visible(writer_id))
        << "a committed writer is visible to the next statement";
}

TEST_F(TxnManagerTest, RepeatableReadHoldsOneViewForTheWholeTransaction) {
    Transaction* reader = Begin(IsolationLevel::kRepeatableRead);
    ASSERT_NE(reader, nullptr);
    const ReadView at_begin = reader->view();

    Transaction* writer = Begin();
    ASSERT_NE(writer, nullptr);
    const std::uint64_t writer_id = writer->id();
    ASSERT_TRUE(mgr_->Commit(*writer, wal::DurabilityClass::kRelaxed).ok());
    mgr_->Release(*writer);

    ASSERT_TRUE(mgr_->StartStatement(*reader).ok());
    EXPECT_EQ(reader->view().snapshot_lsn, at_begin.snapshot_lsn);
    EXPECT_FALSE(reader->view().Visible(writer_id))
        << "a transaction committed after BEGIN must stay invisible under REPEATABLE READ";
}

TEST_F(TxnManagerTest, ATransactionAlwaysSeesItsOwnWrites) {
    Transaction* txn = Begin();
    ASSERT_NE(txn, nullptr);
    EXPECT_TRUE(txn->view().Visible(txn->id()));
    // ...and a view minted beside it does not, which is the same arm from
    // the other side.
    EXPECT_FALSE(mgr_->MintReadView(kNoTrxId).Visible(txn->id()));
}

TEST_F(TxnManagerTest, ALiveTransactionIsInvisibleToEveryOtherView) {
    Transaction* live = Begin();
    ASSERT_NE(live, nullptr);
    Transaction* other = Begin();
    ASSERT_NE(other, nullptr);
    EXPECT_FALSE(other->view().Visible(live->id()));
}

TEST_F(TxnManagerTest, TheBootstrapTransactionIsVisibleToEveryTransaction) {
    Transaction* txn = Begin();
    ASSERT_NE(txn, nullptr);
    EXPECT_TRUE(txn->view().Visible(kAlwaysVisibleTrxId));
}

// The 64-entry bound on live transactions went with the in-flight array
// it was the width of (AN-S2, AN-3 C): a view is a snapshot LSN that
// tracks no set, so the sixty-fifth `BEGIN` is admitted and every view
// still answers every other live transaction invisible.
TEST_F(TxnManagerTest, MoreThanSixtyFourLiveTransactionsAreAdmitted) {
    std::vector<Transaction*> held;
    for (std::size_t i = 0; i < 65; ++i) {
        auto txn = mgr_->Begin(IsolationLevel::kReadCommitted);
        ASSERT_TRUE(txn.ok()) << "at " << i << ": " << txn.status().message();
        held.push_back(txn.value());
    }
    EXPECT_EQ(mgr_->ActiveCount(), 65u);
    const ReadView last = held.back()->view();
    for (std::size_t i = 0; i + 1 < held.size(); ++i) {
        EXPECT_FALSE(last.Visible(held[i]->id())) << "live transaction " << i;
    }
    for (Transaction* txn : held) {
        ASSERT_TRUE(mgr_->Commit(*txn, wal::DurabilityClass::kRelaxed).ok());
    }
    const ReadView after = mgr_->MintReadView(kNoTrxId);
    for (Transaction* txn : held) EXPECT_TRUE(after.Visible(txn->id()));
}

// ---- First-updater-wins (section 5) --------------------------------------

TEST_F(TxnManagerTest, TheFourArmsOfTheConflictTable) {
    Transaction* mine = Begin();
    ASSERT_NE(mine, nullptr);
    Transaction* other = Begin();
    ASSERT_NE(other, nullptr);

    // 1. A pre-existing or catalog-stamped row: proceed.
    EXPECT_TRUE(mgr_->CheckWriteConflict(*mine, kAlwaysVisibleTrxId, 42).ok());
    // 2. My own earlier write: proceed.
    EXPECT_TRUE(mgr_->CheckWriteConflict(*mine, mine->id(), 42).ok());
    // 4. Still in flight: conflict.
    Status conflict = mgr_->CheckWriteConflict(*mine, other->id(), 42);
    EXPECT_EQ(conflict.code(), StatusCode::kTxnConflict);
    // The message is part of the wire contract (section 5), not a
    // diagnostic - the client's retry loop reads the shape.
    EXPECT_NE(conflict.message().find("row id=42"), std::string::npos) << conflict.message();
    EXPECT_NE(conflict.message().find("was written by transaction " +
                                       std::to_string(other->id())),
              std::string::npos)
        << conflict.message();
    EXPECT_TRUE(IsRetryable(conflict.code()));

    // 3. Committed before my read view: proceed. Commit `other`, then take
    // a fresh view over it.
    ASSERT_TRUE(mgr_->Commit(*other, wal::DurabilityClass::kRelaxed).ok());
    const std::uint64_t other_id = other->id();
    mgr_->Release(*other);
    ASSERT_TRUE(mgr_->StartStatement(*mine).ok());
    EXPECT_TRUE(mgr_->CheckWriteConflict(*mine, other_id, 42).ok());
}

// Under REPEATABLE READ the loser stays a loser: the view does not move, so
// a transaction that committed after BEGIN is never visible and never
// becomes writable.
TEST_F(TxnManagerTest, RepeatableReadKeepsConflictingWithACommittedWriter) {
    Transaction* mine = Begin(IsolationLevel::kRepeatableRead);
    Transaction* other = Begin();
    ASSERT_NE(mine, nullptr);
    ASSERT_NE(other, nullptr);

    const std::uint64_t other_id = other->id();
    ASSERT_TRUE(mgr_->Commit(*other, wal::DurabilityClass::kRelaxed).ok());
    mgr_->Release(*other);

    ASSERT_TRUE(mgr_->StartStatement(*mine).ok());
    EXPECT_EQ(mgr_->CheckWriteConflict(*mine, other_id, 7).code(), StatusCode::kTxnConflict);
}

// ---- Rollback (section 6) ------------------------------------------------

TEST_F(TxnManagerTest, RollbackRestoresTheBytesAnOverwriteReplaced) {
    const Row row = PlaceRow("original", kAlwaysVisibleTrxId);

    Transaction* txn = Begin();
    ASSERT_NE(txn, nullptr);
    const heap::PageView::Tuple before = TupleAt(row);
    mgr_->NoteOverwrite(*txn, /*rel_oid=*/9, row.page_id, row.slot, /*pk=*/1, before.trx_id,
                        before.undo_ptr, BytesOf("original"));

    auto bytes = store_.Get(row.page_id);
    ASSERT_TRUE(bytes.ok());
    heap::PageView page(bytes.value().bytes());
    ASSERT_TRUE(page.OverwriteTuple(row.slot, BytesOf("modified"), txn->id(), kNoUndoPtr).ok());
    EXPECT_EQ(PayloadAt(row), "modified");

    ASSERT_TRUE(mgr_->Abort(*txn).ok());
    EXPECT_EQ(PayloadAt(row), "original");
    EXPECT_EQ(TupleAt(row).trx_id, kAlwaysVisibleTrxId)
        << "the writer must be restored too, not just the bytes";
}

TEST_F(TxnManagerTest, RollbackClearsTheMarkADeleteSet) {
    const Row row = PlaceRow("still here", kAlwaysVisibleTrxId);

    Transaction* txn = Begin();
    ASSERT_NE(txn, nullptr);
    const heap::PageView::Tuple before = TupleAt(row);
    mgr_->NoteDeleteMark(*txn, /*rel_oid=*/9, row.page_id, row.slot, /*pk=*/1, before.trx_id,
                        before.undo_ptr);

    auto bytes = store_.Get(row.page_id);
    ASSERT_TRUE(bytes.ok());
    heap::PageView page(bytes.value().bytes());
    ASSERT_TRUE(page.DeleteMark(row.slot, txn->id()).ok());
    EXPECT_TRUE(TupleAt(row).deleted);

    ASSERT_TRUE(mgr_->Abort(*txn).ok());
    EXPECT_FALSE(TupleAt(row).deleted);
    EXPECT_EQ(TupleAt(row).trx_id, kAlwaysVisibleTrxId);
    EXPECT_EQ(PayloadAt(row), "still here");
}

TEST_F(TxnManagerTest, RollbackRetiresTheSlotAnInsertTook) {
    const Row row = PlaceRow("inserted", kAlwaysVisibleTrxId);

    Transaction* txn = Begin();
    ASSERT_NE(txn, nullptr);
    mgr_->NoteInsert(*txn, /*rel_oid=*/9, row.page_id, row.slot, /*pk=*/1);
    ASSERT_TRUE(mgr_->Abort(*txn).ok());

    auto bytes = store_.Get(row.page_id);
    ASSERT_TRUE(bytes.ok());
    heap::PageView page(bytes.value().bytes());
    // Physically retired, which is a different thing from delete-marked:
    // the slot reports NotFound rather than a deleted tuple.
    EXPECT_EQ(page.ReadTuple(row.slot).status().code(), StatusCode::kNotFound);
    auto info = page.DebugSlotInfo(row.slot);
    ASSERT_TRUE(info.ok());
    EXPECT_TRUE(info.value().dead);
}

TEST_F(TxnManagerTest, AMultiRowTransactionUnwindsInReverse) {
    const Row a = PlaceRow("a0", kAlwaysVisibleTrxId);
    const Row b = PlaceRow("b0", kAlwaysVisibleTrxId);

    Transaction* txn = Begin();
    ASSERT_NE(txn, nullptr);

    // Two overwrites of the *same* row plus one of another, so "in reverse"
    // is observable: the first entry for `a` holds "a0", the second holds
    // "a1". Replaying forwards would leave "a1".
    for (const auto& [row, before_bytes, after_bytes] :
         std::vector<std::tuple<Row, std::string, std::string>>{
             {a, "a0", "a1"}, {b, "b0", "b1"}, {a, "a1", "a2"}}) {
        const heap::PageView::Tuple before = TupleAt(row);
        mgr_->NoteOverwrite(*txn, 9, row.page_id, row.slot, 1, before.trx_id, before.undo_ptr,
                            BytesOf(before_bytes));
        auto bytes = store_.Get(row.page_id);
        ASSERT_TRUE(bytes.ok());
        heap::PageView page(bytes.value().bytes());
        ASSERT_TRUE(
            page.OverwriteTuple(row.slot, BytesOf(after_bytes), txn->id(), kNoUndoPtr).ok());
    }
    EXPECT_EQ(PayloadAt(a), "a2");
    EXPECT_EQ(PayloadAt(b), "b1");

    ASSERT_TRUE(mgr_->Abort(*txn).ok());
    EXPECT_EQ(PayloadAt(a), "a0") << "unwinding forwards would have left a1";
    EXPECT_EQ(PayloadAt(b), "b0");
}

TEST_F(TxnManagerTest, CommitDropsTheTrailAndAbortingAfterwardsIsANoOp) {
    const Row row = PlaceRow("committed", kAlwaysVisibleTrxId);

    Transaction* txn = Begin();
    ASSERT_NE(txn, nullptr);
    mgr_->NoteOverwrite(*txn, 9, row.page_id, row.slot, 1, kAlwaysVisibleTrxId, kNoUndoPtr,
                        BytesOf("original"));
    ASSERT_TRUE(mgr_->Commit(*txn, wal::DurabilityClass::kRelaxed).ok());
    EXPECT_TRUE(txn->trail().empty());
    EXPECT_FALSE(txn->active());

    // A committed write needs no compensation, so aborting after the fact
    // must not put anything back.
    ASSERT_TRUE(mgr_->Abort(*txn).ok());
    EXPECT_EQ(PayloadAt(row), "committed");
}

// A transaction ended but not yet released is invisible to every read view,
// which is what lets the caller keep the handle long enough to reply.
TEST_F(TxnManagerTest, AnEndedButUnreleasedTransactionIsNotInFlight) {
    Transaction* ended = Begin();
    ASSERT_NE(ended, nullptr);
    ASSERT_TRUE(mgr_->Commit(*ended, wal::DurabilityClass::kRelaxed).ok());

    Transaction* fresh = Begin();
    ASSERT_NE(fresh, nullptr);
    EXPECT_TRUE(fresh->view().Visible(ended->id()));
    EXPECT_FALSE(fresh->view().in_flight_at_mint)
        << "an ended transaction is not a contemporary of the next view";

    mgr_->Release(*ended);
}

// ---- Reader registration (docs/workplan-reader-registration.md) -----------
//
// The horizon is an LSN since AN-S2 (AN-R3): the oldest `snapshot_lsn` any
// live view holds, instance-wide. What an active transaction's *own* undo
// needs is not the horizon but the floor, which its `oldest_unresolved`
// bound holds below its id; the last cell of this group is that.

TEST_F(TxnManagerTest, TheHorizonIsUnboundedWithNoReaders) {
    EXPECT_EQ(mgr_->ReadHorizon(), UINT64_MAX);
}

TEST_F(TxnManagerTest, AnActiveTransactionBoundsTheHorizonAtItsSnapshot) {
    Transaction* txn = Begin();
    ASSERT_NE(txn, nullptr);
    EXPECT_EQ(mgr_->ReadHorizon(), txn->view().snapshot_lsn);

    ASSERT_TRUE(mgr_->Commit(*txn, wal::DurabilityClass::kRelaxed).ok());
    // Ended-but-unreleased already binds nothing: no rollback is coming
    // and no further statement reads through it.
    EXPECT_EQ(mgr_->ReadHorizon(), UINT64_MAX);
    mgr_->Release(*txn);
}

TEST_F(TxnManagerTest, ALeasedReaderBoundsTheHorizonUntilItsLeaseDies) {
    const ReadView view = mgr_->MintReadView(kNoTrxId);
    const std::uint64_t bound = view.snapshot_lsn;

    auto lease = mgr_->RegisterReader(view);
    ASSERT_TRUE(lease.ok());
    EXPECT_TRUE(lease.value().held());
    EXPECT_EQ(mgr_->ReadHorizon(), bound);

    lease.value().Release();
    EXPECT_FALSE(lease.value().held());
    EXPECT_EQ(mgr_->ReadHorizon(), UINT64_MAX);
    // Idempotent: a second release withdraws nothing twice.
    lease.value().Release();
    EXPECT_EQ(mgr_->ReadHorizon(), UINT64_MAX);
}

TEST_F(TxnManagerTest, AnInFlightWriterKeepsBindingThroughAViewThatSawIt) {
    Transaction* writer = Begin();
    ASSERT_NE(writer, nullptr);
    const std::uint64_t writer_id = writer->id();

    // An autocommit view minted while the writer runs has a snapshot below
    // the writer's eventual commit, so the view's bound holds the horizon
    // under that commit.
    const ReadView view = mgr_->MintReadView(kNoTrxId);
    auto lease = mgr_->RegisterReader(view);
    ASSERT_TRUE(lease.ok());

    ASSERT_TRUE(mgr_->Commit(*writer, wal::DurabilityClass::kRelaxed).ok());
    mgr_->Release(*writer);

    // The writer is gone from live_, but the leased view still cannot see
    // it - the lease is what keeps the horizon honest about that, and what
    // keeps the floor from passing the writer's entry.
    EXPECT_FALSE(view.Visible(writer_id));
    EXPECT_EQ(mgr_->ReadHorizon(), view.snapshot_lsn);
    EXPECT_FALSE(mgr_->ResolvedForEveryReader(writer_id));
    lease.value().Release();
    EXPECT_EQ(mgr_->ReadHorizon(), UINT64_MAX);
    EXPECT_TRUE(mgr_->ResolvedForEveryReader(writer_id));
}

TEST_F(TxnManagerTest, AMovedLeaseKeepsTheRegistrationAndTheHuskDropsNothing) {
    const ReadView view = mgr_->MintReadView(kNoTrxId);
    const std::uint64_t bound = view.snapshot_lsn;

    ReaderLease moved;
    {
        auto lease = mgr_->RegisterReader(view);
        ASSERT_TRUE(lease.ok());
        moved = std::move(lease.value());
        // The moved-from husk dies here; the registration must survive it.
    }
    EXPECT_TRUE(moved.held());
    EXPECT_EQ(mgr_->ReadHorizon(), bound);

    moved.Release();
    EXPECT_EQ(mgr_->ReadHorizon(), UINT64_MAX);
}

TEST_F(TxnManagerTest, MoveAssigningOverAHeldLeaseReleasesTheOverwrittenSlot) {
    const ReadView view = mgr_->MintReadView(kNoTrxId);

    auto first = mgr_->RegisterReader(view);
    auto second = mgr_->RegisterReader(view);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());

    // The overwritten registration must be withdrawn, not leaked - a leak
    // here is invisible until slot exhaustion, which is why this is pinned.
    first.value() = std::move(second.value());
    EXPECT_TRUE(first.value().held());
    EXPECT_FALSE(second.value().held());

    first.value().Release();
    EXPECT_EQ(mgr_->ReadHorizon(), UINT64_MAX);
}

TEST_F(TxnManagerTest, SlotExhaustionIsOutOfSpaceAndAReleaseReopensIt) {
    const ReadView view = mgr_->MintReadView(kNoTrxId);

    std::vector<ReaderLease> held;
    held.reserve(kMaxRegisteredReaders);
    for (std::size_t i = 0; i < kMaxRegisteredReaders; ++i) {
        auto lease = mgr_->RegisterReader(view);
        ASSERT_TRUE(lease.ok()) << "slot " << i;
        held.push_back(std::move(lease.value()));
    }

    auto refused = mgr_->RegisterReader(view);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kOutOfSpace);

    held.pop_back();
    auto reopened = mgr_->RegisterReader(view);
    EXPECT_TRUE(reopened.ok());
}

// ---- The undo purge, end to end (docs/inflight/in-progress/workplan-undo-purge.md D1) -----------
//
// The manager installs its ReadHorizon() on the log at construction, so
// the two mechanisms compose without a call site: while a writer runs,
// its own id bounds the horizon and its pages hold; once it resolves,
// the next writer's growth recycles them.
TEST_F(TxnManagerTest, UndoPagesRecycleOnceTheirWritersClearTheHorizon) {
    const std::vector<std::byte> image(kUndoPageCapacity / 4 - kUndoRecordHeaderSize,
                                       std::byte{0x5A});
    UndoRecordFields fields{};
    fields.type = static_cast<std::uint8_t>(UndoRecordType::kOverwrite);
    fields.prior_trx_id = kAlwaysVisibleTrxId;
    fields.prior_undo_ptr = kNoUndoPtr;
    fields.target_page_id = 300;
    fields.target_slot = 2;

    Transaction* a = Begin();
    ASSERT_NE(a, nullptr);
    // Two pages and one growth while `a` runs: its own id bounds the
    // horizon, so nothing may recycle - a rollback could still need it.
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(mgr_->AppendUndo(*a, fields, /*pk=*/1, image).ok());
    }
    EXPECT_EQ(undo_->PagesRecycled(), 0u)
        << "a running transaction's undo was recycled under it";
    ASSERT_TRUE(mgr_->Commit(*a, wal::DurabilityClass::kRelaxed).ok());
    mgr_->Release(*a);

    // The next writer's growths find `a`'s pages settled and reuse them:
    // the chain plateaus instead of growing without bound.
    Transaction* b = Begin();
    ASSERT_NE(b, nullptr);
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(mgr_->AppendUndo(*b, fields, /*pk=*/1, image).ok());
    }
    EXPECT_GE(undo_->PagesRecycled(), 1u);
    EXPECT_LE(undo_->LivePages(), 3u) << "undo grew where it should have recycled";
    ASSERT_TRUE(mgr_->Commit(*b, wal::DurabilityClass::kRelaxed).ok());
    mgr_->Release(*b);
}

// **The retention duty that changed hands at AN-S2** (AN-R3). Over the
// trx-id predicate an active transaction's own id was folded into the
// horizon, which is what kept its own undo from being retired under it. An
// LSN horizon over snapshots says nothing about that; what holds the floor
// below a live transaction now is its core's `oldest_unresolved` bound
// (AN-R8). This is that bound doing the duty: a purge pass taken while `a`
// runs - another writer's growth - leaves `a`'s records readable, and
// `a`'s rollback then finds them.
//
// **Mutation**: publish `kUnboundedBound` for the oldest unresolved id and
// the floor passes `a`; its pages recycle under it and the read below
// returns another writer's bytes.
TEST_F(TxnManagerTest, ALiveTransactionsUndoSurvivesAPurgePassTakenWhileItRuns) {
    const std::vector<std::byte> mine(kUndoPageCapacity / 4 - kUndoRecordHeaderSize,
                                      std::byte{0xA1});
    const std::vector<std::byte> theirs(kUndoPageCapacity / 4 - kUndoRecordHeaderSize,
                                        std::byte{0xB2});
    UndoRecordFields fields{};
    fields.type = static_cast<std::uint8_t>(UndoRecordType::kOverwrite);
    fields.prior_trx_id = kAlwaysVisibleTrxId;
    fields.prior_undo_ptr = kNoUndoPtr;
    fields.target_page_id = 300;
    fields.target_slot = 2;

    Transaction* a = Begin();
    ASSERT_NE(a, nullptr);
    auto first = mgr_->AppendUndo(*a, fields, /*pk=*/1, mine);
    ASSERT_TRUE(first.ok()) << first.status().message();
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(mgr_->AppendUndo(*a, fields, 1, mine).ok());

    // Another writer grows the log twice over while `a` is live, and every
    // growth runs the purge.
    Transaction* b = Begin();
    ASSERT_NE(b, nullptr);
    for (int i = 0; i < 12; ++i) ASSERT_TRUE(mgr_->AppendUndo(*b, fields, 1, theirs).ok());
    ASSERT_TRUE(mgr_->Commit(*b, wal::DurabilityClass::kRelaxed).ok());
    mgr_->Release(*b);
    Transaction* c = Begin();
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 8; ++i) ASSERT_TRUE(mgr_->AppendUndo(*c, fields, 1, theirs).ok());

    EXPECT_EQ(undo_->PagesRecycled(), 0u) << "a live transaction's undo was recycled under it";
    auto record = undo_->Read(first.value());
    ASSERT_TRUE(record.ok()) << record.status().message();
    EXPECT_EQ(record.value().image.size(), mine.size());
    EXPECT_TRUE(std::equal(record.value().image.begin(), record.value().image.end(),
                           mine.begin()))
        << "the record at a's first undo pointer holds another writer's bytes";

    ASSERT_TRUE(mgr_->Abort(*a).ok());
    ASSERT_TRUE(mgr_->Commit(*c, wal::DurabilityClass::kRelaxed).ok());
    mgr_->Release(*c);
}

// An unlogged instance has no commit record to take an order from, so the
// window assigns one: each commit takes the next position, and a view
// minted after it sits exactly one above a view minted before.
TEST_F(TxnManagerTest, AnUnloggedCommitTakesTheNextPositionInCommitOrder) {
    const ReadView before = mgr_->MintReadView(kNoTrxId);
    Transaction* txn = Begin();
    ASSERT_NE(txn, nullptr);
    ASSERT_TRUE(mgr_->Commit(*txn, wal::DurabilityClass::kRelaxed).ok());
    const ReadView after = mgr_->MintReadView(kNoTrxId);
    EXPECT_EQ(after.snapshot_lsn, before.snapshot_lsn + 1);
    EXPECT_FALSE(before.Visible(txn->id()));
    EXPECT_TRUE(after.Visible(txn->id()));
    mgr_->Release(*txn);
}

// ---- Isolation-level parsing ----------------------------------------------

TEST(IsolationLevelTest, ParsesTheSpellingsAConfigFileInvites) {
    for (std::string_view text : {"read committed", "READ COMMITTED", "read-committed",
                                  "read_committed", "rc"}) {
        auto parsed = ParseIsolationLevel(text);
        ASSERT_TRUE(parsed.ok()) << text << ": " << parsed.status().message();
        EXPECT_EQ(parsed.value(), IsolationLevel::kReadCommitted) << text;
    }
    for (std::string_view text : {"repeatable read", "REPEATABLE READ", "repeatable-read", "rr"}) {
        auto parsed = ParseIsolationLevel(text);
        ASSERT_TRUE(parsed.ok()) << text << ": " << parsed.status().message();
        EXPECT_EQ(parsed.value(), IsolationLevel::kRepeatableRead) << text;
    }
}

TEST(IsolationLevelTest, TheNameRoundTripsThroughTheParser) {
    for (IsolationLevel level : {IsolationLevel::kReadCommitted, IsolationLevel::kRepeatableRead}) {
        auto parsed = ParseIsolationLevel(IsolationLevelName(level));
        ASSERT_TRUE(parsed.ok());
        EXPECT_EQ(parsed.value(), level);
    }
}

// SERIALIZABLE is out of scope and **not** [OPEN] (section 1), so the
// refusal says why rather than "unknown level".
TEST(IsolationLevelTest, SerializableIsRefusedWithItsReason) {
    auto parsed = ParseIsolationLevel("serializable");
    EXPECT_EQ(parsed.status().code(), StatusCode::kUnsupported);
    EXPECT_NE(parsed.status().message().find("predicate locking"), std::string::npos)
        << parsed.status().message();
}

TEST(IsolationLevelTest, AnUnknownLevelNamesWhatWasGiven) {
    auto parsed = ParseIsolationLevel("snapshot");
    EXPECT_EQ(parsed.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(parsed.status().message().find("snapshot"), std::string::npos);
}

}  // namespace
}  // namespace kds::txn
