#include "kds/exec/leaf_cursor.hpp"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "kds/storage/btree/btree.hpp"
#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/visit.hpp"

// `LeafCursor` walks a relation's leaf chain a slice at a time for the
// index, assertion and transition builds. Its bound is the walk's cycle
// guard, kept across slices - never a length (the audit's G19): a 2^20-leaf
// budget refused a build over a relation past 8 GiB of leaves. The cells
// drive `Step` with a synthetic `per_leaf`, so a chain far longer than any
// store could hold costs only the hops.

namespace kds::exec {
namespace {

class LeafCursorTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto created = store_.CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        root_ = created.value().first;
        ASSERT_TRUE(btree::FormatRoot(created.value().second.bytes(), /*owner_oid=*/0).ok());
        created.value().second.Release();
        ASSERT_TRUE(cursor_.Start(store_, root_).ok());
        ASSERT_FALSE(cursor_.walked());
    }

    storage::InMemoryPageStore store_{128};
    PageId root_ = kInvalidPageId;
    LeafCursor cursor_;
};

TEST_F(LeafCursorTest, AChainLongerThanTheOldBudgetIsWalkedToItsEnd) {
    // Past 2^20 leaves, the budget the walk was refused at: every id is new,
    // so the walk ends at the chain's end, not at a count.
    const PageId last = root_ + storage::kMaxPageWalkLength + 2;
    const Status s = cursor_.Step(storage::kMaxPageWalkLength + 8, [&](PageId leaf) -> StatusOr<PageId> {
        return leaf < last ? leaf + 1 : kInvalidPageId;
    });
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_TRUE(cursor_.walked());
}

TEST_F(LeafCursorTest, ACycleIsFoundAcrossSlicesWhereverItReentersTheChain) {
    // The guard is the whole walk's: a cycle longer than one slice is found
    // only if the slices share it. Re-entering at the origin and in the
    // middle are the two shapes the guard's first visit treats differently.
    for (const PageId reenter : {PageId{0}, PageId{37}}) {
        SCOPED_TRACE(reenter);
        LeafCursor cursor;
        ASSERT_TRUE(cursor.Start(store_, root_).ok());
        const PageId back_to = root_ + reenter;
        const PageId tail = root_ + 299;
        const auto per_leaf = [&](PageId leaf) -> StatusOr<PageId> {
            return leaf == tail ? back_to : leaf + 1;
        };
        Status s = Status::OK();
        int slices = 0;
        while (s.ok() && slices < 100) {
            s = cursor.Step(64, per_leaf);
            ++slices;
        }
        EXPECT_EQ(s.code(), StatusCode::kCorruption) << "a cycle spread over slices ran unbounded";
        EXPECT_NE(s.message().find("revisits"), std::string::npos) << s.message();
        EXPECT_LE(slices, 20) << "found, but not within three times the cycle's length";
    }
}

}  // namespace
}  // namespace kds::exec
