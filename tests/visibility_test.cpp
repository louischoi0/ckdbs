#include "kds/txn/visibility.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/txn/read_view.hpp"

// docs/spec/txn.md section 10-3, the whole group: kBootstrapXid always visible,
// own writes visible, a commit above the snapshot LSN invisible, a live
// writer invisible, undo_ptr == 0 with an invisible writer means no version,
// a delete-mark read from both sides, a three-version chain read from three
// views yielding three payloads, and garbage in the tuple header's two free
// bytes changing nothing - the mechanized form of "no xmax anywhere".
//
// **The views here are hand-built over an `InstanceVisibility`** (AN-S2):
// a view is a snapshot LSN plus the instance it reads the floor and the
// window through. `Committed()` publishes every id up to `kLastCommitted`
// at an LSN equal to its id, so `ViewAt(n)` sees writers below `n` and not
// those at or above it - exactly what the number meant over the trx-id
// predicate, which is why the chain cells below read unchanged. A cell
// about a *live* writer builds its own instance and leaves that id out.

namespace kds::txn {
namespace {

constexpr PageId kFirstUserPageId = 128;
constexpr std::uint64_t kLastCommitted = 200;

std::vector<std::byte> BytesOf(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::string StringOf(std::span<const std::byte> b) {
    std::string out(b.size(), '\0');
    std::memcpy(out.data(), b.data(), b.size());
    return out;
}

// One instance in which ids 2..kLastCommitted are committed at LSN = id.
// Function-local so a view built in any cell can point at it for the
// binary's life; nothing here ever reclaims it (no cursor is published, so
// the floor stays at 0 and every answer goes through the window).
const InstanceVisibility& Committed() {
    static const InstanceVisibility* const vis = [] {
        auto* built = new InstanceVisibility();
        for (std::uint64_t id = 2; id <= kLastCommitted; ++id) built->PublishCommit(id, id);
        return built;
    }();
    return *vis;
}

// A view over `vis` that sees every commit at an LSN below `up_to`.
ReadView ViewOver(const InstanceVisibility& vis, std::uint64_t up_to,
                  std::uint64_t own = kNoTrxId) {
    ReadView v;
    v.snapshot_lsn = up_to == 0 ? 0 : up_to - 1;
    v.own_trx_id = own;
    v.visibility = &vis;
    return v;
}

ReadView ViewAt(std::uint64_t up_to, std::uint64_t own = kNoTrxId) {
    return ViewOver(Committed(), up_to, own);
}

// ---- ReadView::Visible, on its own ---------------------------------------

TEST(ReadViewTest, TheBootstrapTransactionIsVisibleToEveryView) {
    // Including a view taken before it would have started, which is the
    // point: it is not a low id that happens to be committed, it is
    // permanent (txn.md section 4.2).
    EXPECT_TRUE(ViewAt(0).Visible(kAlwaysVisibleTrxId));
    EXPECT_TRUE(ViewAt(1000).Visible(kAlwaysVisibleTrxId));

    // And over an instance that has published nothing at all - the window
    // empty, the floor at zero - which is what "unconditional" means: the
    // always-visible arm comes first, so nothing can shadow it.
    InstanceVisibility empty;
    EXPECT_TRUE(ViewOver(empty, 1000).Visible(kAlwaysVisibleTrxId));
}

TEST(ReadViewTest, AViewSeesItsOwnWritesIncludingUncommittedOnes) {
    // 50 is live: it has no window entry anywhere.
    InstanceVisibility vis;
    const ReadView v = ViewOver(vis, /*up_to=*/50, /*own=*/50);
    EXPECT_TRUE(v.Visible(50));
    EXPECT_FALSE(ViewOver(vis, 50).Visible(50)) << "and nobody else sees it";
}

TEST(ReadViewTest, ACommitAboveTheSnapshotIsInvisible) {
    const ReadView v = ViewAt(50);
    EXPECT_TRUE(v.Visible(49));
    EXPECT_FALSE(v.Visible(50));
    EXPECT_FALSE(v.Visible(51));
}

// A writer with no window entry is live, aborted or never issued, and the
// snapshot cannot tell which - all three are "not committed before me".
// The trx-id predicate found this case in an in-flight set the view
// carried; the commit-LSN one finds it by absence from the window.
TEST(ReadViewTest, ALiveWriterIsInvisibleWhateverItsId) {
    InstanceVisibility vis;
    vis.PublishCommit(11, 11);
    vis.PublishCommit(99, 99);
    const ReadView v = ViewOver(vis, 100);
    for (std::uint64_t live : {std::uint64_t{10}, std::uint64_t{25}, std::uint64_t{40}}) {
        EXPECT_FALSE(v.Visible(live)) << live;
    }
    EXPECT_TRUE(v.Visible(11));
    EXPECT_TRUE(v.Visible(99));
}

// AN-R3's third branch: below the floor, a version still on a page was
// written by a winner, whatever the window says. The floor is read
// through the instance at every call rather than copied into the view -
// so a view minted *before* the floor rose answers the same as one minted
// after it, which is the property that lets reclamation drop entries.
TEST(ReadViewTest, BelowTheFloorEveryWriterOnAPageIsAWinner) {
    InstanceVisibility vis;
    vis.PublishCommit(10, 10);
    const ReadView minted_early = ViewOver(vis, 5);
    EXPECT_FALSE(minted_early.Visible(10)) << "committed at 10, snapshot at 4";
    EXPECT_FALSE(minted_early.Visible(7)) << "no entry, above the floor";

    // The floor rises past both and the window entry goes with it.
    vis.PublishBounds(/*core=*/0, kUnboundedBound, /*cursor=*/200);
    ASSERT_EQ(vis.Reclaim(), 1u);
    ASSERT_EQ(vis.Floor(), 200u);
    EXPECT_TRUE(minted_early.Visible(10)) << "a reclaimed entry never hides a committed row";
    EXPECT_TRUE(minted_early.Visible(7)) << "below the floor, an id on a page is resolved";
    EXPECT_FALSE(minted_early.Visible(201)) << "above it, nothing changed";
}

// A view built with no instance behind it admits nothing but the two
// unconditional arms - the safe default, and the one `CheckVisibility`
// reads as "every writer busy".
TEST(ReadViewTest, AViewOverNoInstanceAdmitsOnlyTheUnconditionalArms) {
    ReadView v;
    v.own_trx_id = 7;
    EXPECT_TRUE(v.Visible(kAlwaysVisibleTrxId));
    EXPECT_TRUE(v.Visible(7));
    EXPECT_FALSE(v.Visible(2));
}

// A dispatcher with no transaction manager holds this view, and under it
// the predicate must admit exactly what the engine admitted before MVCC
// existed. That is what makes wiring the predicate in a no-op - and it is
// the one view that admits a writer no window has ever heard of.
TEST(ReadViewTest, TheEverythingViewAdmitsEveryWriter) {
    const ReadView v = ReadView::Everything();
    EXPECT_TRUE(v.Visible(kAlwaysVisibleTrxId));
    EXPECT_TRUE(v.Visible(2));
    EXPECT_TRUE(v.Visible(1u << 20));
    EXPECT_EQ(v.visibility, nullptr) << "it consults nothing";
}

// ---- Classify: phase 1, no page fetch ------------------------------------

TEST(ClassifyTest, AVisibleWriterIsDecidedWithoutTheChain) {
    const ReadView v = ViewAt(50);
    EXPECT_EQ(Classify(v, /*trx_id=*/10, /*deleted=*/false, /*undo_ptr=*/kNoUndoPtr),
              Visibility::kVisible);
    // Even with a chain hanging off it: a visible writer's version *is*
    // the answer, so the chain is never walked.
    EXPECT_EQ(Classify(v, 10, false, EncodeUndoPtr(9, 1024)), Visibility::kVisible);
}

TEST(ClassifyTest, ADeleteMarkByAVisibleDeleterIsNoVersion) {
    EXPECT_EQ(Classify(ViewAt(50), /*trx_id=*/10, /*deleted=*/true, kNoUndoPtr),
              Visibility::kNoVersion);
}

TEST(ClassifyTest, AnInvisibleWriterWithNoPredecessorIsNoVersion) {
    // An insert by a transaction I cannot see - and the reason INSERT
    // writes no undo record at all (section 3.6).
    EXPECT_EQ(Classify(ViewAt(50), /*trx_id=*/77, /*deleted=*/false, kNoUndoPtr),
              Visibility::kNoVersion);
}

TEST(ClassifyTest, AnInvisibleWriterWithAPredecessorNeedsTheChain) {
    EXPECT_EQ(Classify(ViewAt(50), 77, false, EncodeUndoPtr(9, 1024)),
              Visibility::kNeedsUndoWalk);
}

// ---- ResolveThroughUndo: phase 2, the walk -------------------------------

class VisibilityTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = storage::MemoryPageDevice::Create(/*extent_pages=*/64,
                                                        /*initial_pages=*/64);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());

        auto store = storage::DevicePageStore::Open(*device_, kFirstUserPageId);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());

        undo_ = std::make_unique<UndoLog>(*store_, /*wal=*/nullptr);
    }

    // Appends one before-image and returns the undo_ptr a tuple would carry.
    std::uint64_t Supersede(std::uint64_t writer, std::uint64_t prior_trx_id,
                            std::uint64_t prior_undo_ptr, UndoRecordType type,
                            std::string_view image) {
        UndoRecordFields r{};
        r.prior_trx_id = prior_trx_id;
        r.prior_undo_ptr = prior_undo_ptr;
        r.target_page_id = 300;
        r.target_slot = 1;
        r.type = static_cast<std::uint8_t>(type);
        auto ptr = undo_->Append(writer, r, BytesOf(image));
        EXPECT_TRUE(ptr.ok()) << ptr.status().message();
        return ptr.ok() ? ptr.value() : kNoUndoPtr;
    }

    // The full predicate over one tuple, as a caller runs it: classify,
    // and only on kNeedsUndoWalk copy and walk.
    StatusOr<Visibility> Resolve(const ReadView& view, std::uint64_t trx_id, bool deleted,
                                 std::uint64_t undo_ptr, std::string_view current_payload,
                                 std::string* out) {
        const Visibility first = Classify(view, trx_id, deleted, undo_ptr);
        if (first != Visibility::kNeedsUndoWalk) {
            if (first == Visibility::kVisible && out != nullptr) *out = current_payload;
            return first;
        }
        std::vector<std::byte> scratch = BytesOf(current_payload);
        auto verdict = ResolveThroughUndo(view, *undo_, trx_id, deleted, undo_ptr, scratch);
        if (verdict.ok() && verdict.value() == Visibility::kVisible && out != nullptr) {
            *out = StringOf(scratch);
        }
        return verdict;
    }

    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> store_;
    std::unique_ptr<UndoLog> undo_;
};

TEST_F(VisibilityTest, AnInvisibleOverwriteStepsBackToThePriorVersion) {
    // Transaction 60 overwrote "old" with "new"; a view taken before 60
    // committed must see "old".
    const std::uint64_t ptr = Supersede(/*writer=*/60, /*prior_trx_id=*/kAlwaysVisibleTrxId,
                                        kNoUndoPtr, UndoRecordType::kOverwrite, "old");
    std::string seen;
    auto verdict = Resolve(ViewAt(60), /*trx_id=*/60, /*deleted=*/false, ptr, "new", &seen);
    ASSERT_TRUE(verdict.ok()) << verdict.status().message();
    EXPECT_EQ(verdict.value(), Visibility::kVisible);
    EXPECT_EQ(seen, "old");
}

// A delete-mark changes no tuple bytes, so stepping back over one keeps
// the payload - and if a later overwrite changed them, the newer record
// already restored them on the way down (section 4.3's step 4).
TEST_F(VisibilityTest, ADeleteMarkByAnInvisibleDeleterYieldsThePriorVersion) {
    const std::uint64_t ptr = Supersede(/*writer=*/60, /*prior_trx_id=*/kAlwaysVisibleTrxId,
                                        kNoUndoPtr, UndoRecordType::kDeleteMark, "");
    std::string seen;
    auto verdict = Resolve(ViewAt(60), /*trx_id=*/60, /*deleted=*/true, ptr, "row", &seen);
    ASSERT_TRUE(verdict.ok()) << verdict.status().message();
    EXPECT_EQ(verdict.value(), Visibility::kVisible);
    EXPECT_EQ(seen, "row") << "the bytes are the tuple's own; the mark carried none";
}

TEST_F(VisibilityTest, ADeleteMarkByAVisibleDeleterIsNoVersion) {
    const std::uint64_t ptr = Supersede(60, kAlwaysVisibleTrxId, kNoUndoPtr,
                                        UndoRecordType::kDeleteMark, "");
    // A view taken after 60 committed: the row is gone.
    auto verdict = Resolve(ViewAt(61), /*trx_id=*/60, /*deleted=*/true, ptr, "row", nullptr);
    ASSERT_TRUE(verdict.ok());
    EXPECT_EQ(verdict.value(), Visibility::kNoVersion);
}

TEST_F(VisibilityTest, AnInsertRecordEndsTheChainWithNoVersion) {
    // kInsert is never written today, but the reader handles it so that
    // persisting the in-memory insert trail is a code change and not a
    // format-version event (section 3.6).
    const std::uint64_t ptr =
        Supersede(60, /*prior_trx_id=*/59, kNoUndoPtr, UndoRecordType::kInsert, "");
    auto verdict = Resolve(ViewAt(59), /*trx_id=*/60, false, ptr, "new", nullptr);
    ASSERT_TRUE(verdict.ok());
    EXPECT_EQ(verdict.value(), Visibility::kNoVersion);
}

// txn.md section 10-3's headline case: one tuple, three versions, three
// read views, three different payloads.
TEST_F(VisibilityTest, AThreeVersionChainReadFromThreeViewsYieldsThreePayloads) {
    // v1 written by the bootstrap transaction, then 60 overwrites it, then
    // 70 overwrites that. The live tuple holds "v3", written by 70.
    const std::uint64_t to_v1 =
        Supersede(/*writer=*/60, /*prior_trx_id=*/kAlwaysVisibleTrxId, kNoUndoPtr,
                  UndoRecordType::kOverwrite, "v1");
    const std::uint64_t to_v2 =
        Supersede(/*writer=*/70, /*prior_trx_id=*/60, to_v1, UndoRecordType::kOverwrite, "v2");

    struct Case {
        std::uint64_t up_to;
        const char* expect;
    };
    for (const Case& c : {Case{60, "v1"}, Case{70, "v2"}, Case{71, "v3"}}) {
        std::string seen;
        auto verdict = Resolve(ViewAt(c.up_to), /*trx_id=*/70, false, to_v2, "v3", &seen);
        ASSERT_TRUE(verdict.ok()) << verdict.status().message();
        EXPECT_EQ(verdict.value(), Visibility::kVisible) << "up_to " << c.up_to;
        EXPECT_EQ(seen, c.expect) << "up_to " << c.up_to;
    }
}

// A live writer is invisible even though its id is below every id the view
// can see committed - which is what distinguishes "started before me" from
// "committed before me", and what the window's absence answers.
TEST_F(VisibilityTest, ALiveWriterIsSteppedOverEvenBelowCommittedIds) {
    const std::uint64_t ptr = Supersede(60, kAlwaysVisibleTrxId, kNoUndoPtr,
                                        UndoRecordType::kOverwrite, "committed");
    InstanceVisibility vis;
    vis.PublishCommit(99, 99);  // something above 60 is committed; 60 is not
    const ReadView v = ViewOver(vis, /*up_to=*/100);

    std::string seen;
    auto verdict = Resolve(v, /*trx_id=*/60, false, ptr, "uncommitted", &seen);
    ASSERT_TRUE(verdict.ok());
    EXPECT_EQ(verdict.value(), Visibility::kVisible);
    EXPECT_EQ(seen, "committed");
}

TEST_F(VisibilityTest, AWriterSeesItsOwnUncommittedOverwrite) {
    const std::uint64_t ptr = Supersede(60, kAlwaysVisibleTrxId, kNoUndoPtr,
                                        UndoRecordType::kOverwrite, "committed");
    InstanceVisibility vis;
    const ReadView v = ViewOver(vis, /*up_to=*/100, /*own=*/60);

    std::string seen;
    auto verdict = Resolve(v, /*trx_id=*/60, false, ptr, "mine", &seen);
    ASSERT_TRUE(verdict.ok());
    EXPECT_EQ(verdict.value(), Visibility::kVisible);
    EXPECT_EQ(seen, "mine");
}

TEST_F(VisibilityTest, ADamagedChainIsCorruptionRatherThanAWrongAnswer) {
    std::vector<std::byte> scratch = BytesOf("new");
    auto verdict = ResolveThroughUndo(ViewAt(50), *undo_, /*trx_id=*/77, /*deleted=*/false,
                                      /*undo_ptr=*/EncodeUndoPtr(0, 1024), scratch);
    EXPECT_EQ(verdict.status().code(), StatusCode::kCorruption);
}

// ---- No xmax anywhere -----------------------------------------------------
//
// The tuple header's `reserved` byte and the slot's spare bits are the two
// places an xmax could hide. Writing garbage into them must change nothing,
// because nothing reads them - which is the mechanized form of invariant
// 12's "there is no xmax".
TEST(TupleHeaderTest, GarbageInTheHeadersFreeBytesChangesNothing) {
    std::array<std::byte, kPageSize> buf{};
    auto page = heap::PageView::CreateEmpty(std::span<std::byte, kPageSize>(buf), /*min_key=*/0);
    ASSERT_TRUE(page.ok()) << page.status().message();

    const std::vector<std::byte> payload = BytesOf("the row");
    auto slot = page.value().InsertTuple(payload, /*trx_id=*/60, /*undo_ptr=*/kNoUndoPtr);
    ASSERT_TRUE(slot.ok()) << slot.status().message();

    auto before = page.value().ReadTuple(slot.value());
    ASSERT_TRUE(before.ok());
    const auto tuple_offset = page.value().DebugSlotInfo(slot.value());
    ASSERT_TRUE(tuple_offset.ok());

    // The one byte the tuple header reserves, scribbled on.
    const auto garbage = std::byte{0xFF};
    std::memcpy(buf.data() + tuple_offset.value().offset + heap::kTupleReservedOffset, &garbage,
                1);

    auto after = page.value().ReadTuple(slot.value());
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().trx_id, before.value().trx_id);
    EXPECT_EQ(after.value().undo_ptr, before.value().undo_ptr);
    EXPECT_EQ(after.value().deleted, before.value().deleted);
    EXPECT_EQ(StringOf(after.value().payload), "the row");

    // And the predicate reads it identically.
    EXPECT_EQ(Classify(ViewAt(61), after.value()), Classify(ViewAt(61), before.value()));
    EXPECT_EQ(Classify(ViewAt(60), after.value()), Classify(ViewAt(60), before.value()));
}

}  // namespace
}  // namespace kds::txn
