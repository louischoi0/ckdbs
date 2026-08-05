#include "kds/server/superblock.hpp"

#include <array>
#include <cstring>
#include <span>
#include <string>

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
    EXPECT_EQ(sb.create_time(), 1000u);
    EXPECT_EQ(sb.last_mount_time(), 1000u);
    EXPECT_EQ(sb.wal_anchor_count(), 0u);
}

TEST(SuperBlockTest, EncodeDecodeRoundTrip) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    sb.MarkMounted(2000);
    ASSERT_TRUE(sb.SetWalAnchor(0, WalAnchorFields{4096, 8192, 12288, 0}).ok());

    PageBuf buf{};
    sb.Encode(AsSpan(buf));

    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok());

    EXPECT_EQ(decoded.value().version(), sb.version());
    EXPECT_EQ(decoded.value().create_time(), sb.create_time());
    EXPECT_EQ(decoded.value().last_mount_time(), 2000u);
    EXPECT_EQ(decoded.value().wal_anchor_count(), 1u);
    EXPECT_EQ(decoded.value().wal_anchor(0).redo_start_lsn, 8192u);
}

TEST(SuperBlockTest, DecodeRejectsBadMagic) {
    PageBuf buf{};  // zero-initialized: magic == 0, never a valid superblock
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

TEST(SuperBlockTest, EncodeZeroesReservedTail) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    PageBuf buf;
    buf.fill(std::byte{0xAB});  // poison the buffer first
    sb.Encode(AsSpan(buf));

    // Offsets in the layout constants are body-relative, so the tail starts
    // past the body, not past byte kSuperBlockUsedSize of the page.
    for (std::size_t i = kSuperBlockBodyOffset + kSuperBlockUsedSize; i < kPageSize; ++i) {
        ASSERT_EQ(buf[i], std::byte{0}) << "byte " << i << " should be zeroed";
    }
}

// ---- Per-core WAL anchors (wal.md section 14-3) --------------------------

TEST(SuperBlockWalAnchorTest, FreshDatabaseHasNoAnchors) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);

    EXPECT_EQ(sb.wal_anchor_count(), 0u);
    for (std::uint32_t core = 0; core < kMaxWalCores; ++core) {
        // Zero is not a legal record LSN, so this reads unambiguously as
        // "no checkpoint yet" and sends recovery to the start of the stream.
        EXPECT_EQ(sb.wal_anchor(core).redo_start_lsn, 0u);
        EXPECT_EQ(sb.wal_anchor(core).checkpoint_lsn, 0u);
        EXPECT_EQ(sb.wal_anchor(core).durable_lsn, 0u);
        EXPECT_EQ(sb.wal_anchor(core).segment_no, 0u);
    }
}

TEST(SuperBlockWalAnchorTest, AnchorsSurviveEncodeDecode) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    ASSERT_TRUE(sb.SetWalAnchor(0, WalAnchorFields{4096, 8192, 12288, 0}).ok());
    ASSERT_TRUE(sb.SetWalAnchor(7, WalAnchorFields{1 << 20, 1 << 21, 1 << 22, 3}).ok());

    PageBuf buf{};
    sb.Encode(AsSpan(buf));
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok());

    EXPECT_EQ(decoded.value().wal_anchor_count(), 8u);
    EXPECT_EQ(decoded.value().wal_anchor(0).checkpoint_lsn, 4096u);
    EXPECT_EQ(decoded.value().wal_anchor(0).redo_start_lsn, 8192u);
    EXPECT_EQ(decoded.value().wal_anchor(0).durable_lsn, 12288u);
    EXPECT_EQ(decoded.value().wal_anchor(0).segment_no, 0u);
    EXPECT_EQ(decoded.value().wal_anchor(7).redo_start_lsn, 1u << 21);
    EXPECT_EQ(decoded.value().wal_anchor(7).segment_no, 3u);

    // A slot between two published ones is still "never checkpointed".
    EXPECT_EQ(decoded.value().wal_anchor(3).redo_start_lsn, 0u);
}

TEST(SuperBlockWalAnchorTest, AnchorsAreIndexedIndependentlyPerCore) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    for (std::uint32_t core = 0; core < kMaxWalCores; ++core) {
        ASSERT_TRUE(sb.SetWalAnchor(core, WalAnchorFields{core + 1, core + 2, core + 3, core}).ok());
    }

    PageBuf buf{};
    sb.Encode(AsSpan(buf));
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok());

    EXPECT_EQ(decoded.value().wal_anchor_count(), kMaxWalCores);
    for (std::uint32_t core = 0; core < kMaxWalCores; ++core) {
        EXPECT_EQ(decoded.value().wal_anchor(core).redo_start_lsn, core + 2)
            << "core " << core << " read another core's entry";
    }
}

TEST(SuperBlockWalAnchorTest, CountTracksTheHighestCoreEverPublished) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    ASSERT_TRUE(sb.SetWalAnchor(5, WalAnchorFields{1, 2, 3, 0}).ok());
    EXPECT_EQ(sb.wal_anchor_count(), 6u);

    // A lower core publishing later does not shrink it: the count says how
    // wide the table is, not which entry was written last.
    ASSERT_TRUE(sb.SetWalAnchor(1, WalAnchorFields{4, 5, 6, 0}).ok());
    EXPECT_EQ(sb.wal_anchor_count(), 6u);
}

TEST(SuperBlockWalAnchorTest, CoreIdBeyondTheTableIsRefusedNotWrapped) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);

    Status s = sb.SetWalAnchor(kMaxWalCores, WalAnchorFields{1, 2, 3, 0});
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
    // And nothing was written: wrapping would have landed on core 0.
    EXPECT_EQ(sb.wal_anchor(0).redo_start_lsn, 0u);
    EXPECT_EQ(sb.wal_anchor_count(), 0u);
}

TEST(SuperBlockWalAnchorTest, ReadingPastTheTableIsNoAnchorNotACrash) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    EXPECT_EQ(sb.wal_anchor(kMaxWalCores).redo_start_lsn, 0u);
    EXPECT_EQ(sb.wal_anchor(1'000'000).redo_start_lsn, 0u);
}

TEST(SuperBlockTest, DecodeRefusesAnyVersionButThisBuilds) {
    for (const std::uint32_t version : {kSuperBlockVersion - 1, kSuperBlockVersion + 1}) {
        SuperBlock sb = SuperBlock::CreateFresh(1000);
        PageBuf buf{};
        sb.Encode(AsSpan(buf));
        std::memcpy(buf.data() + kSuperBlockBodyOffset + kVersionOffset, &version,
                    sizeof(version));

        auto decoded = SuperBlock::Decode(AsConstSpan(buf));
        EXPECT_FALSE(decoded.ok()) << "version " << version << " should not mount";
        EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
    }
}

// ---- The pinned core count (workplan-crosscore.md M6) -----------------

TEST(SuperBlockTest, TheCoreCountIsPinnedAndRoundTrips) {
    SuperBlock sb = SuperBlock::CreateFresh(1000, storage::kDefaultInlineCellWidth,
                                            /*core_count=*/4);
    EXPECT_EQ(sb.core_count(), 4u);

    PageBuf buf{};
    sb.Encode(AsSpan(buf));
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().core_count(), 4u);
}

TEST(SuperBlockTest, ADefaultFreshSuperBlockIsSingleCore) {
    EXPECT_EQ(SuperBlock::CreateFresh(1000).core_count(), 1u);
}

TEST(SuperBlockTest, AZeroCoreCountIsCorruptionRatherThanADefault) {
    // The state a version-9 image would decode to, reading the field out of
    // what used to be the reserved tail. "Boot with no cores" is not
    // something to carry forward, and the version bump is what normally
    // catches it - this is the belt to that braces.
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    PageBuf buf{};
    sb.Encode(AsSpan(buf));
    const std::uint32_t zero = 0;
    std::memcpy(buf.data() + kSuperBlockBodyOffset + kCoreCountOffset, &zero, sizeof(zero));

    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

TEST(SuperBlockTest, ACoreCountAboveTheAnchorTableIsRefused) {
    // kMaxWalCores is a hard ceiling, not a preference: the anchor table is
    // indexed by core_id and a core above it has nowhere to publish from.
    EXPECT_TRUE(CheckCoreCount(kMaxWalCores).ok());
    EXPECT_EQ(CheckCoreCount(kMaxWalCores + 1).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(CheckCoreCount(0).code(), StatusCode::kInvalidArgument);

    // And the ceiling is named, so an operator who hits it learns what
    // bounds them.
    EXPECT_NE(CheckCoreCount(kMaxWalCores + 1).message().find(std::to_string(kMaxWalCores)),
              std::string::npos);
}

TEST(SuperBlockTest, TheCoreCountDoesNotDisturbTheAnchorTable) {
    // It is appended past next_trx_id for exactly this reason: no existing
    // offset moves. Asserted rather than assumed, because the last two
    // format bumps both turned on this property holding.
    SuperBlock sb = SuperBlock::CreateFresh(1000, storage::kDefaultInlineCellWidth, 8);
    ASSERT_TRUE(sb.SetWalAnchor(3, WalAnchorFields{11, 22, 33, 44}).ok());

    PageBuf buf{};
    sb.Encode(AsSpan(buf));
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok());

    EXPECT_EQ(decoded.value().core_count(), 8u);
    EXPECT_EQ(decoded.value().wal_anchor(3).checkpoint_lsn, 11u);
    EXPECT_EQ(decoded.value().wal_anchor(3).redo_start_lsn, 22u);
    EXPECT_EQ(decoded.value().wal_anchor(3).durable_lsn, 33u);
    EXPECT_EQ(decoded.value().wal_anchor(3).segment_no, 44u);
    EXPECT_EQ(decoded.value().next_trx_id(), sb.next_trx_id());
}

}  // namespace
}  // namespace kds::server
