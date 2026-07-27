#include "kds/server/superblock.hpp"

#include <array>
#include <span>

#include <gtest/gtest.h>

namespace kds::server {
namespace {

using PageBuf = std::array<std::byte, kPageSize>;

std::span<std::byte, kPageSize> AsSpan(PageBuf& buf) { return std::span<std::byte, kPageSize>(buf); }
std::span<const std::byte, kPageSize> AsConstSpan(const PageBuf& buf) {
    return std::span<const std::byte, kPageSize>(buf);
}

TEST(SuperBlockTest, CreateFreshSetsExpectedDefaults) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);

    EXPECT_EQ(sb.version(), kSuperBlockVersion);
    EXPECT_EQ(sb.max_page_id(), kFirstUserPageId);
    EXPECT_EQ(sb.last_commit_page_id(), 0u);
    EXPECT_EQ(sb.create_time(), 1000u);
    EXPECT_EQ(sb.last_mount_time(), 1000u);
    EXPECT_EQ(sb.last_fsync_time(), 0u);
    EXPECT_EQ(sb.total_pages(), 0u);
    EXPECT_EQ(sb.free_pages(), 0u);
}

TEST(SuperBlockTest, EncodeDecodeRoundTrip) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    sb.AllocatePageId();
    sb.AllocatePageId();
    sb.SetPageCounts(500, 100);
    sb.SetAllocRange(200, 56);
    sb.MarkFsynced(2000);

    PageBuf buf{};
    sb.Encode(AsSpan(buf));

    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok());

    EXPECT_EQ(decoded.value().version(), sb.version());
    EXPECT_EQ(decoded.value().max_page_id(), sb.max_page_id());
    EXPECT_EQ(decoded.value().last_commit_page_id(), sb.last_commit_page_id());
    EXPECT_EQ(decoded.value().create_time(), sb.create_time());
    EXPECT_EQ(decoded.value().last_fsync_time(), sb.last_fsync_time());
    EXPECT_EQ(decoded.value().total_pages(), 500u);
    EXPECT_EQ(decoded.value().free_pages(), 100u);
    EXPECT_EQ(decoded.value().alloc_range(), (std::pair<PageId, std::uint64_t>{200, 56}));
}

TEST(SuperBlockTest, DecodeRejectsBadMagic) {
    PageBuf buf{};  // zero-initialized: magic == 0, never a valid superblock
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

TEST(SuperBlockTest, AllocatePageIdIsMonotonicAndUnique) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    PageId first = sb.AllocatePageId();
    PageId second = sb.AllocatePageId();
    PageId third = sb.AllocatePageId();

    EXPECT_EQ(first, kFirstUserPageId);
    EXPECT_EQ(second, first + 1);
    EXPECT_EQ(third, first + 2);
    EXPECT_EQ(sb.max_page_id(), first + 3);
}

TEST(SuperBlockTest, AllocatePageIdBatchReservesAContiguousRange) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    PageId batch_start = sb.AllocatePageIdBatch(10);
    EXPECT_EQ(batch_start, kFirstUserPageId);
    EXPECT_EQ(sb.max_page_id(), kFirstUserPageId + 10);

    // The next single allocation must not reuse anything in the batch.
    PageId next = sb.AllocatePageId();
    EXPECT_EQ(next, kFirstUserPageId + 10);
}

TEST(SuperBlockTest, MarkFsyncedAdvancesLastCommitToMaxPageId) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    sb.AllocatePageId();
    sb.AllocatePageId();

    sb.MarkFsynced(1500);
    EXPECT_EQ(sb.last_fsync_time(), 1500u);
    EXPECT_EQ(sb.last_commit_page_id(), sb.max_page_id());
}

TEST(SuperBlockTest, EncodeZeroesReservedTail) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    PageBuf buf;
    buf.fill(std::byte{0xAB});  // poison the buffer first
    sb.Encode(AsSpan(buf));

    for (std::size_t i = kSuperBlockUsedSize; i < kPageSize; ++i) {
        ASSERT_EQ(buf[i], std::byte{0}) << "byte " << i << " should be zeroed";
    }
}

}  // namespace
}  // namespace kds::server
