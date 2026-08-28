#include "kds/txn/recovery_undo.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/server/superblock.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/txn/undo_page.hpp"
#include "kds/wal/analysis.hpp"

// RC05 - recovery's undo phase (docs/workplan-wal-recovery.md).
//
// Three properties carry it, and the third is the one the plan added after
// `origin/main`'s EXPLICIT key mode landed:
//
//   - a loser's writes are reached through its **own** chain, from the head
//     a checkpoint recorded, not from the records in the replay range;
//   - running undo twice changes nothing, because a crash during undo
//     re-runs it;
//   - a record whose row has moved **fails the mount** rather than
//     compensating a row the transaction never wrote (§4a).
//
// The store is driven directly rather than through a dispatcher: the phase
// is a function of an undo chain and a page, and building a real
// transaction to produce one would test the write path instead.

namespace kds::txn {
namespace {

constexpr PageId kHeapPage = server::kFirstUserPageId;
constexpr PageId kUndoPage = server::kFirstUserPageId + 1;
constexpr std::uint64_t kLoser = 77;

// A tuple payload whose Keystone word carries `pk`, padded out so the page
// has something of a realistic width to move around. The word goes in by
// hand because there is no store-side helper for it - `KeystoneIdOfPayload`
// is the read half and the write half belongs to the row codec, which this
// test deliberately does not drag in.
std::vector<std::byte> Payload(std::uint64_t pk, unsigned char fill) {
    std::vector<std::byte> out(32, static_cast<std::byte>(fill));
    auto word = Keystone::Encode(pk, /*flags=*/0, /*reserved=*/0);
    EXPECT_TRUE(word.ok()) << word.status().message();
    const std::uint64_t w = word.ok() ? word.value() : 0;
    std::memcpy(out.data(), &w, sizeof(w));
    return out;
}

class RecoveryUndoTest : public ::testing::Test {
protected:
    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    UndoLog undo_{store_};  // no WalManager: compensations are unlogged here

    std::span<std::byte, kPageSize> Heap() {
        auto p = store_.Get(kHeapPage);
        EXPECT_TRUE(p.ok()) << p.status().message();
        return p.value().bytes();
    }

    void SetUp() override {
        auto heap = store_.CreateAt(kHeapPage);
        ASSERT_TRUE(heap.ok()) << heap.status().message();
        auto view = heap::PageView::CreateEmpty(heap.value().bytes(), /*min_key=*/1);
        ASSERT_TRUE(view.ok()) << view.status().message();

        auto undo_page = store_.CreateAt(kUndoPage);
        ASSERT_TRUE(undo_page.ok()) << undo_page.status().message();
        ASSERT_TRUE(FormatUndoPage(undo_page.value().bytes(), kLoser, kInvalidPageId).ok());
    }

    // A var-heap page in the same store, so a loser's spill has somewhere
    // to have landed. Returns the slot the value took.
    std::uint16_t Spill(PageId page, std::string_view value) {
        std::vector<std::byte> bytes;
        for (char c : value) bytes.push_back(static_cast<std::byte>(c));
        auto p = store_.Get(page);
        EXPECT_TRUE(p.ok()) << p.status().message();
        auto slot = varheap::PageAppend(p.value().bytes(), bytes);
        EXPECT_TRUE(slot.ok()) << slot.status().message();
        return slot.ok() ? slot.value() : 0;
    }

    // The var-heap counterpart of `Record`: the target names a kVarHeap
    // page and slot rather than a row's, which is the whole difference.
    std::uint64_t VarHeapRecord(PageId page, std::uint16_t slot, std::uint64_t pk,
                                std::uint64_t prev) {
        UndoRecordFields rec{};
        rec.prior_trx_id = 0;  // an append supersedes nothing
        rec.prior_undo_ptr = kNoUndoPtr;
        rec.target_page_id = page;
        rec.target_slot = slot;
        rec.type = static_cast<std::uint8_t>(UndoRecordType::kVarHeapAppend);
        rec.txn_prev_undo_ptr = prev;
        rec.pk = pk;
        auto ptr = undo_.Append(kLoser, rec, {});
        EXPECT_TRUE(ptr.ok()) << ptr.status().message();
        return ptr.ok() ? ptr.value() : kNoUndoPtr;
    }

    // Appends one undo record for `kLoser`, linking it to `prev`, and
    // returns its pointer - the shape `TransactionManager::AppendUndo`
    // produces at runtime.
    std::uint64_t Record(UndoRecordType type, std::uint16_t slot, std::uint64_t pk,
                         std::uint64_t prev, std::span<const std::byte> image = {},
                         std::uint64_t prior_trx_id = 5) {
        UndoRecordFields rec{};
        rec.prior_trx_id = prior_trx_id;
        rec.prior_undo_ptr = kNoUndoPtr;
        rec.target_page_id = kHeapPage;
        rec.target_slot = slot;
        rec.type = static_cast<std::uint8_t>(type);
        rec.txn_prev_undo_ptr = prev;
        rec.pk = pk;
        auto ptr = undo_.Append(kLoser, rec, image);
        EXPECT_TRUE(ptr.ok()) << ptr.status().message();
        return ptr.ok() ? ptr.value() : kNoUndoPtr;
    }

    wal::AnalysisResult Losing(std::uint64_t head) {
        wal::AnalysisResult a;
        a.transactions[kLoser] = wal::TxnState{wal::TxnOutcome::kLoser, head};
        a.losers = 1;
        return a;
    }
};

// ---- The chain, walked from the head -------------------------------------

TEST_F(RecoveryUndoTest, AnInsertsSlotIsRetired) {
    heap::PageView view(Heap());
    ASSERT_TRUE(view.InsertTuple(Payload(42, 0xA1), kLoser, kNoUndoPtr).ok());
    const std::uint64_t head = Record(UndoRecordType::kInsert, 0, 42, kNoUndoPtr);

    RecoveryUndo undo(undo_);
    ASSERT_TRUE(undo.RollBack(store_, Losing(head)).ok());
    EXPECT_EQ(undo.compensations(), 1u);
    EXPECT_EQ(undo.transactions(), 1u);

    heap::PageView after(Heap());
    EXPECT_FALSE(after.PayloadAt(0, after.slot_count()).ok()) << "the slot was not retired";
}

TEST_F(RecoveryUndoTest, AnOverwriteIsRestoredByteForByte) {
    const auto before = Payload(42, 0xB0);
    heap::PageView view(Heap());
    ASSERT_TRUE(view.InsertTuple(before, /*trx_id=*/5, kNoUndoPtr).ok());
    // The loser's version, over the top of it.
    ASSERT_TRUE(view.OverwriteTuple(0, Payload(42, 0xEE), kLoser, kNoUndoPtr).ok());

    const std::uint64_t head = Record(UndoRecordType::kOverwrite, 0, 42, kNoUndoPtr, before);

    RecoveryUndo undo(undo_);
    ASSERT_TRUE(undo.RollBack(store_, Losing(head)).ok());

    heap::PageView after(Heap());
    auto tuple = after.ReadTuple(0);
    ASSERT_TRUE(tuple.ok()) << tuple.status().message();
    EXPECT_EQ(std::vector<std::byte>(tuple.value().payload.begin(), tuple.value().payload.end()),
              before);
    EXPECT_EQ(tuple.value().trx_id, 5u) << "the pre-loser writer was not restored";
}

TEST_F(RecoveryUndoTest, ADeleteMarkIsCleared) {
    heap::PageView view(Heap());
    ASSERT_TRUE(view.InsertTuple(Payload(42, 0xC3), /*trx_id=*/5, kNoUndoPtr).ok());
    ASSERT_TRUE(view.DeleteMark(0, kLoser).ok());

    const std::uint64_t head = Record(UndoRecordType::kDeleteMark, 0, 42, kNoUndoPtr);

    RecoveryUndo undo(undo_);
    ASSERT_TRUE(undo.RollBack(store_, Losing(head)).ok());

    heap::PageView after(Heap());
    auto tuple = after.ReadTuple(0);
    ASSERT_TRUE(tuple.ok()) << tuple.status().message() << " - the row is still deleted";
    EXPECT_EQ(tuple.value().trx_id, 5u);
}

TEST_F(RecoveryUndoTest, EveryRecordInTheChainIsCompensated) {
    // Three writes by one transaction, linked newest-last, walked
    // newest-first. This is the property the whole of RV10 exists for: the
    // chain reaches all three whatever the replay range contained.
    heap::PageView view(Heap());
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(view.InsertTuple(Payload(100 + i, 0xD0), kLoser, kNoUndoPtr).ok());
    }
    std::uint64_t prev = kNoUndoPtr;
    for (int i = 0; i < 3; ++i) {
        prev = Record(UndoRecordType::kInsert, static_cast<std::uint16_t>(i),
                      100 + static_cast<std::uint64_t>(i), prev);
    }

    RecoveryUndo undo(undo_);
    ASSERT_TRUE(undo.RollBack(store_, Losing(prev)).ok());
    EXPECT_EQ(undo.compensations(), 3u);

    heap::PageView after(Heap());
    for (std::uint16_t i = 0; i < 3; ++i) {
        EXPECT_FALSE(after.PayloadAt(i, after.slot_count()).ok()) << "slot " << i << " survived";
    }
}

// ---- Re-running, which is what a crash during undo causes ----------------

TEST_F(RecoveryUndoTest, RunningTwiceIsAByteForByteNoOp) {
    const auto before = Payload(42, 0xB0);
    heap::PageView view(Heap());
    ASSERT_TRUE(view.InsertTuple(before, /*trx_id=*/5, kNoUndoPtr).ok());
    ASSERT_TRUE(view.OverwriteTuple(0, Payload(42, 0xEE), kLoser, kNoUndoPtr).ok());
    ASSERT_TRUE(view.InsertTuple(Payload(43, 0xF0), kLoser, kNoUndoPtr).ok());

    std::uint64_t prev = Record(UndoRecordType::kOverwrite, 0, 42, kNoUndoPtr, before);
    prev = Record(UndoRecordType::kInsert, 1, 43, prev);

    RecoveryUndo first(undo_);
    ASSERT_TRUE(first.RollBack(store_, Losing(prev)).ok());
    auto page = store_.Get(kHeapPage);
    ASSERT_TRUE(page.ok());
    const std::vector<std::byte> after_first(page.value().bytes().begin(), page.value().bytes().end());

    RecoveryUndo second(undo_);
    ASSERT_TRUE(second.RollBack(store_, Losing(prev)).ok()) << "a re-run must not fail";
    auto again = store_.Get(kHeapPage);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(std::vector<std::byte>(again.value().bytes().begin(), again.value().bytes().end()), after_first);

    // And it recognised the retired slot rather than reading it as a
    // missing row - the difference between resuming and refusing.
    EXPECT_EQ(second.already_done(), 1u);
}

// ---- Who undo owes nothing to -------------------------------------------

TEST_F(RecoveryUndoTest, AWinnerAndAnAbortedTransactionAreLeftAlone) {
    heap::PageView view(Heap());
    ASSERT_TRUE(view.InsertTuple(Payload(42, 0xA1), 11, kNoUndoPtr).ok());
    const std::uint64_t head = Record(UndoRecordType::kInsert, 0, 42, kNoUndoPtr);

    wal::AnalysisResult a;
    a.transactions[11] = wal::TxnState{wal::TxnOutcome::kWinner, head};
    a.transactions[12] = wal::TxnState{wal::TxnOutcome::kAborted, head};

    RecoveryUndo undo(undo_);
    ASSERT_TRUE(undo.RollBack(store_, a).ok());
    EXPECT_EQ(undo.compensations(), 0u);
    EXPECT_EQ(undo.transactions(), 0u);

    heap::PageView after(Heap());
    EXPECT_TRUE(after.PayloadAt(0, after.slot_count()).ok()) << "a winner's row was retired";
}

TEST_F(RecoveryUndoTest, ALoserThatWroteNothingIsStillFinished) {
    // Head of kNoUndoPtr: nothing to compensate, but it still counts as
    // rolled back so the TXN_ABORT is written and a second recovery stops
    // calling it a loser.
    RecoveryUndo undo(undo_);
    ASSERT_TRUE(undo.RollBack(store_, Losing(kNoUndoPtr)).ok());
    EXPECT_EQ(undo.compensations(), 0u);
    EXPECT_EQ(undo.transactions(), 1u);
}

// ---- §4a: a row that moved refuses the mount -----------------------------

TEST_F(RecoveryUndoTest, ARecordWhoseRowMovedIsCorruptionRatherThanAWrongWrite) {
    // The leaf-division case, reproduced by putting a *different* row where
    // the record says the loser's was. Compensating here would overwrite a
    // row this transaction never touched - main's own words for why the
    // live path refuses it without a locator, and recovery has none.
    heap::PageView view(Heap());
    ASSERT_TRUE(view.InsertTuple(Payload(999, 0x11), /*trx_id=*/5, kNoUndoPtr).ok());
    const std::uint64_t head = Record(UndoRecordType::kOverwrite, 0, /*pk=*/42, kNoUndoPtr,
                                      Payload(42, 0xB0));

    RecoveryUndo undo(undo_);
    auto s = undo.RollBack(store_, Losing(head));
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
    EXPECT_NE(s.message().find("999"), std::string::npos)
        << "the message should name what it found: " << s.message();

    // And it wrote nothing.
    heap::PageView after(Heap());
    auto tuple = after.ReadTuple(0);
    ASSERT_TRUE(tuple.ok());
    EXPECT_EQ(tuple.value().trx_id, 5u);
}

// ---- A loser's spills (VC-B3) --------------------------------------------

TEST_F(RecoveryUndoTest, ALoserThatCrashedBetweenTheAppendAndTheTupleWriteLeavesNoOrphan) {
    // **The case this record type exists for.** VARHEAP_APPEND precedes the
    // HEAP_INSERT whose cell points at it, so a crash between the two
    // leaves a value nothing references.
    // `docs/rules/rule-fixed-length-tuple.md` §5 used to hand that orphan
    // to "purge's reclamation sweep", which never existed. The append is a
    // link in the loser's own chain now, so undo reaches it like any other
    // write - and the chain here holds *only* the spill, which is exactly
    // what a crash before the tuple write leaves behind.
    constexpr PageId kVarHeapPage = server::kFirstUserPageId + 2;
    auto page = store_.CreateAt(kVarHeapPage);
    ASSERT_TRUE(page.ok()) << page.status().message();
    ASSERT_TRUE(varheap::FormatPage(page.value().bytes(), /*owner_oid=*/9).ok());

    const std::uint16_t kept = Spill(kVarHeapPage, "a committed value");
    const std::uint16_t doomed = Spill(kVarHeapPage, "the loser's value");

    const std::uint64_t head = VarHeapRecord(kVarHeapPage, doomed, /*pk=*/1, kNoUndoPtr);

    RecoveryUndo undo(undo_);
    ASSERT_TRUE(undo.RollBack(store_, Losing(head)).ok());
    EXPECT_EQ(undo.compensations(), 1u);

    auto after = store_.GetForRead(kVarHeapPage);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(varheap::PageRead(after.value().bytes(), doomed).status().code(),
              StatusCode::kNotFound);
    // And only the loser's: a release that swept the page would take a
    // committed value with it.
    auto survivor = varheap::PageRead(after.value().bytes(), kept);
    ASSERT_TRUE(survivor.ok()) << survivor.status().message();
    EXPECT_EQ(survivor.value().size(), std::string("a committed value").size());
}

TEST_F(RecoveryUndoTest, ASpillWhoseAppendWasNeverRedoneIsWorkAlreadyDone) {
    // **The exposure phase B's own ordering opens.** The
    // `UNDO_WRITE{kVarHeapAppend}` is written *before* the
    // PAGE_INIT/VARHEAP_APPEND that fill the slot, so a log whose readable
    // prefix ends between them leaves the loser's chain naming a slot redo
    // never created. The WAL rule guarantees only the converse - a flushed
    // page image implies its records are durable - so this is a state the
    // engine must survive, not a torn-write hypothesis.
    //
    // Refusing the mount over it would fail a recovery in which the WAL
    // behaved correctly. An append that was never redone has nothing to
    // undo, so it counts as work already finished.
    constexpr PageId kVarHeapPage = server::kFirstUserPageId + 2;
    auto page = store_.CreateAt(kVarHeapPage);
    ASSERT_TRUE(page.ok());
    ASSERT_TRUE(varheap::FormatPage(page.value().bytes(), /*owner_oid=*/9).ok());
    // The page exists but the append never landed: slot 0 is past its
    // (empty) directory.
    const std::uint64_t head = VarHeapRecord(kVarHeapPage, /*slot=*/0, /*pk=*/1, kNoUndoPtr);

    RecoveryUndo undo(undo_);
    ASSERT_TRUE(undo.RollBack(store_, Losing(head)).ok())
        << "the mount was refused over an append redo never applied";
    EXPECT_EQ(undo.already_done(), 1u);
    EXPECT_EQ(undo.compensations(), 0u);
}

TEST_F(RecoveryUndoTest, ASpillWhosePageWasNeverRedoneIsWorkAlreadyDone) {
    // The same state one page out: on a chain-growing spill the PAGE_INIT
    // is written with the append, so a prefix ending before both leaves the
    // record naming a page that does not exist either.
    const std::uint64_t head =
        VarHeapRecord(server::kFirstUserPageId + 900, /*slot=*/0, /*pk=*/1, kNoUndoPtr);

    RecoveryUndo undo(undo_);
    ASSERT_TRUE(undo.RollBack(store_, Losing(head)).ok())
        << "the mount was refused over a var-heap page redo never created";
    EXPECT_EQ(undo.already_done(), 1u);
}

TEST_F(RecoveryUndoTest, ASpillNamingANonVarHeapPageStillFailsTheMount) {
    // The distinction the arm above must not swallow: "the append was never
    // applied" is a legitimate recovery state, "this record names a page of
    // the wrong class" is damage. The heap page is the dangerous case -
    // it holds `nr_slots` at the same body offset - so it is the one tested.
    heap::PageView view(Heap());
    ASSERT_TRUE(view.InsertTuple(Payload(42, 0xA1), kLoser, kNoUndoPtr).ok());
    const std::uint64_t head = VarHeapRecord(kHeapPage, /*slot=*/0, /*pk=*/42, kNoUndoPtr);

    RecoveryUndo undo(undo_);
    EXPECT_EQ(undo.RollBack(store_, Losing(head)).code(), StatusCode::kCorruption);
}

TEST_F(RecoveryUndoTest, ARunTwiceOverASpillIsAByteForByteNoOp) {
    // A crash *during* undo replays a prefix of the compensations, so each
    // must be a no-op the second time (recovery_undo.hpp - no CLR). A
    // release writes a zero over a slot offset, so the second run writes
    // the same zero; this asserts the whole page, not just the status.
    constexpr PageId kVarHeapPage = server::kFirstUserPageId + 2;
    auto page = store_.CreateAt(kVarHeapPage);
    ASSERT_TRUE(page.ok());
    ASSERT_TRUE(varheap::FormatPage(page.value().bytes(), /*owner_oid=*/9).ok());
    const std::uint16_t doomed = Spill(kVarHeapPage, "twice");
    const std::uint64_t head = VarHeapRecord(kVarHeapPage, doomed, /*pk=*/1, kNoUndoPtr);

    RecoveryUndo first(undo_);
    ASSERT_TRUE(first.RollBack(store_, Losing(head)).ok());
    std::array<std::byte, kPageSize> once{};
    {
        auto snapshot = store_.GetForRead(kVarHeapPage);
        ASSERT_TRUE(snapshot.ok());
        std::memcpy(once.data(), snapshot.value().bytes().data(), kPageSize);
    }

    RecoveryUndo second(undo_);
    ASSERT_TRUE(second.RollBack(store_, Losing(head)).ok());
    auto after = store_.GetForRead(kVarHeapPage);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(std::memcmp(after.value().bytes().data(), once.data(), kPageSize), 0)
        << "the second undo run changed the var-heap page";
}

TEST_F(RecoveryUndoTest, AChainThatDoesNotTerminateIsCorruptionRatherThanAHang) {
    // A record linked to itself. Undo must report it, not spin: a damaged
    // chain is the one input a mount cannot be allowed to hang on.
    heap::PageView view(Heap());
    ASSERT_TRUE(view.InsertTuple(Payload(42, 0xA1), kLoser, kNoUndoPtr).ok());

    UndoRecordFields rec{};
    rec.target_page_id = kHeapPage;
    rec.target_slot = 0;
    rec.type = static_cast<std::uint8_t>(UndoRecordType::kDeleteMark);
    rec.pk = 42;
    // Appended first, then pointed at itself. Predicting the pointer instead
    // - `EncodeUndoPtr(kUndoPage, kUndoRecordsOffset)` - guesses which page
    // `UndoLog::Append` will allocate, and it does not use the fixture's
    // undo page at all: it calls `CreateNew()`, which lands wherever the
    // store's cursor is. The self-link is the point of the test; where the
    // record physically sits is not.
    auto ptr = undo_.Append(kLoser, rec, {});
    ASSERT_TRUE(ptr.ok()) << ptr.status().message();

    rec.txn_prev_undo_ptr = ptr.value();
    auto landed = store_.Get(UndoPtrPageId(ptr.value()));
    ASSERT_TRUE(landed.ok()) << landed.status().message();
    ASSERT_TRUE(UndoPageWriteAt(landed.value().bytes(), UndoPtrOffset(ptr.value()), rec, {}).ok());

    RecoveryUndo undo(undo_);
    auto s = undo.RollBack(store_, Losing(ptr.value()));
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kCorruption) << s.message();
}

}  // namespace
}  // namespace kds::txn
