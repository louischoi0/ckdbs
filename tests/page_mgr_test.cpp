#include "kds/storage/page_mgr/page_mgr.hpp"

#include <cstring>

#include <gtest/gtest.h>

#include "kds/storage/in_memory_page_store.hpp"

namespace kds::storage {
namespace {

class BufferPoolTest : public ::testing::Test {
protected:
    InMemoryPageStore backing_;
};

TEST_F(BufferPoolTest, AllocNewThenLookupSeesSameBytes) {
    BufferPool pool(backing_, 4);

    auto allocated = pool.AllocNew(10);
    ASSERT_TRUE(allocated.ok());
    std::memset(allocated.value()->bytes().data(), 0x7A, 8);
    pool.Unpin(*allocated.value());

    auto found = pool.Lookup(10);
    ASSERT_TRUE(found.ok());
    EXPECT_EQ(found.value()->bytes()[0], std::byte{0x7A});
    pool.Unpin(*found.value());
}

TEST_F(BufferPoolTest, AllocNewMarksFrameDirtyByDefault) {
    BufferPool pool(backing_, 4);
    auto frame = pool.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    EXPECT_TRUE(frame.value()->is_dirty());
    pool.Unpin(*frame.value());
}

TEST_F(BufferPoolTest, FlushIsTheOnlyWayAFrameGoesClean) {
    BufferPool pool(backing_, 4);
    auto frame = pool.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    // MarkClean() is private (page.md section 8): a page is clean because
    // it was written, never because someone said so.
    ASSERT_TRUE(pool.Flush(*frame.value()).ok());
    EXPECT_FALSE(frame.value()->is_dirty());
    pool.Unpin(*frame.value());
}

TEST_F(BufferPoolTest, AllocNewRejectsAlreadyResidentPage) {
    BufferPool pool(backing_, 4);
    auto first = pool.AllocNew(5);
    ASSERT_TRUE(first.ok());
    pool.Unpin(*first.value());

    auto second = pool.AllocNew(5);
    EXPECT_FALSE(second.ok());
    EXPECT_EQ(second.status().code(), StatusCode::kAlreadyExists);
}

TEST_F(BufferPoolTest, LookupFailsOnColdMiss) {
    BufferPool pool(backing_, 4);
    auto found = pool.Lookup(123);
    EXPECT_FALSE(found.ok());
    EXPECT_EQ(found.status().code(), StatusCode::kNotFound);
}

TEST_F(BufferPoolTest, LookupOrLoadFailsIfBackingNeverCreatedThePage) {
    BufferPool pool(backing_, 4);
    auto found = pool.LookupOrLoad(999);
    EXPECT_FALSE(found.ok());
    EXPECT_EQ(found.status().code(), StatusCode::kNotFound);
}

TEST_F(BufferPoolTest, LookupOrLoadLoadsFromBackingOnMiss) {
    ASSERT_TRUE(backing_.CreateAt(7).ok());
    std::memset(backing_.Get(7).value().bytes().data(), 0x11, 4);

    BufferPool pool(backing_, 4);
    auto frame = pool.LookupOrLoad(7);
    ASSERT_TRUE(frame.ok());
    EXPECT_EQ(frame.value()->bytes()[0], std::byte{0x11});
    EXPECT_FALSE(frame.value()->is_dirty());  // loaded, not newly allocated
    pool.Unpin(*frame.value());
}

TEST_F(BufferPoolTest, PoolFullReturnsOutOfSpace) {
    BufferPool pool(backing_, 2);

    auto a = pool.AllocNew(1);
    auto b = pool.AllocNew(2);
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());

    auto c = pool.AllocNew(3);
    EXPECT_FALSE(c.ok());
    EXPECT_EQ(c.status().code(), StatusCode::kOutOfSpace);
}

TEST_F(BufferPoolTest, StatsReflectFreeAndValidFrames) {
    BufferPool pool(backing_, 3);
    auto stats0 = pool.stats();
    EXPECT_EQ(stats0.total, 3u);
    EXPECT_EQ(stats0.free, 3u);
    EXPECT_EQ(stats0.valid, 0u);

    auto frame = pool.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    pool.Unpin(*frame.value());

    auto stats1 = pool.stats();
    EXPECT_EQ(stats1.free, 2u);
    EXPECT_EQ(stats1.valid, 1u);
}

TEST_F(BufferPoolTest, PinCountTracksPinAndUnpin) {
    BufferPool pool(backing_, 2);
    auto frame = pool.AllocNew(1);
    ASSERT_TRUE(frame.ok());
    EXPECT_EQ(frame.value()->pin_count(), 1);  // AllocNew pins once

    pool.Pin(*frame.value());
    EXPECT_EQ(frame.value()->pin_count(), 2);

    pool.Unpin(*frame.value());
    pool.Unpin(*frame.value());
    EXPECT_EQ(frame.value()->pin_count(), 0);
}

TEST_F(BufferPoolTest, PageStoreInterfaceRoundTripsAndDoesNotLeakPins) {
    BufferPool pool(backing_, 4);
    PageStore& as_store = pool;

    auto created = as_store.CreateAt(50);
    ASSERT_TRUE(created.ok());
    std::memset(created.value().bytes().data(), 0x99, 2);

    auto fetched = as_store.Get(50);
    ASSERT_TRUE(fetched.ok());
    EXPECT_EQ(fetched.value().bytes()[0], std::byte{0x99});

    // Neither PageStore-interface call should have left a dangling pin.
    auto frame = pool.Lookup(50);
    ASSERT_TRUE(frame.ok());
    EXPECT_EQ(frame.value()->pin_count(), 1);  // only this Lookup() call's pin
    pool.Unpin(*frame.value());
}

}  // namespace
}  // namespace kds::storage
