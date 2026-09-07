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

// ---- The log topology (AR0 M0, work order AL's AL-R3) --------------------

// Builds the page an existing database has: a superblock this build wrote,
// with the topology word forced to what every image predating the field
// holds. Poking the byte rather than calling a setter is deliberate -
// nothing in the API can set the topology yet, and the compatibility claim
// is about *bytes on a page*, not about a value in a struct.
PageBuf PageWithTopology(std::uint32_t topology) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    // Zeroed, not indeterminate: `Encode` reads the page's type byte to
    // decide whether to format the header, so an uninitialized buffer is a
    // real indeterminate-value read and a nondeterministic test.
    PageBuf buf{};
    sb.Encode(AsSpan(buf));
    std::memcpy(buf.data() + kSuperBlockBodyOffset + kLogTopologyOffset, &topology,
                sizeof(topology));
    return buf;
}

std::uint32_t TopologyByteOf(const PageBuf& buf) {
    std::uint32_t out = 0;
    std::memcpy(&out, buf.data() + kSuperBlockBodyOffset + kLogTopologyOffset, sizeof(out));
    return out;
}

// The claim is about the *byte*, not about a value in a struct: a fresh
// image says single-stream on the page, so the word a later mount reads is
// the word this build wrote rather than a default it fell back to.
TEST(SuperBlockTopologyTest, AFreshImageSaysSingleStreamOnThePage) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    PageBuf buf;
    buf.fill(std::byte{0xAB});  // so a byte left unwritten would fail here
    sb.Encode(AsSpan(buf));

    EXPECT_EQ(TopologyByteOf(buf), kSingleStream);

    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
}

// **The door AM-S4(d)'s collapse is proved against.** Every per-core-stream
// branch in the engine was reachable only through this word, so this cell
// is what makes deleting them a deletion of dead code. `kPerCoreStreams` is
// a value a version-17 image cannot legally hold - `CreateFresh` writes
// `kSingleStream` and takes no parameter - so reaching it means a corrupted
// or hand-edited word, and it gets the refusal a bad version gets.
TEST(SuperBlockTopologyTest, APerCoreStreamVolumeIsRefusedAtDecode) {
    PageBuf buf = PageWithTopology(kPerCoreStreams);

    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
    EXPECT_NE(decoded.status().message().find("topology"), std::string::npos);
    // Named rather than merely refused: an operator holding a pre-M0
    // volume is told there is no migration, not that their file is
    // damaged in some unspecified way.
    EXPECT_NE(decoded.status().message().find("recreate"), std::string::npos);
}

// Decode *and* re-encode. Reading alone would pass on an `Encode` that
// wrote a hard zero, since the value under test would be the one poked
// into the buffer rather than the one the encoder produced.
TEST(SuperBlockTopologyTest, ANonZeroTopologySurvivesBothHalvesOfTheCodec) {
    PageBuf buf = PageWithTopology(kSingleStream);

    // A successful `Decode` *is* the topology assertion now (AM-S4(d)):
    // there is no accessor, because there is one answer, and the refusal
    // above is what carries the fact.
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();

    PageBuf again;
    again.fill(std::byte{0xAB});
    decoded.value().Encode(AsSpan(again));
    EXPECT_EQ(TopologyByteOf(again), kSingleStream) << "Encode dropped the topology";
}

// Guessing the topology would guess how many streams recovery must find.
TEST(SuperBlockTopologyTest, AnUnknownTopologyIsRefusedRatherThanDefaulted) {
    PageBuf buf = PageWithTopology(7);

    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
    EXPECT_NE(decoded.status().message().find("topology"), std::string::npos);
}

TEST(SuperBlockTopologyTest, UnderOneStreamOnlySlotZeroTakesAnAnchor) {
    PageBuf buf = PageWithTopology(kSingleStream);
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok());
    SuperBlock one = std::move(decoded.value());

    EXPECT_TRUE(one.SetWalAnchor(0, WalAnchorFields{4096, 8192, 12288, 0}).ok());

    Status refused = one.SetWalAnchor(1, WalAnchorFields{1, 2, 3, 0});
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(refused.message().find("one WAL stream"), std::string::npos);

    // And the refusal path mutated nothing: not the slot, and not the
    // count recovery compares against the core count.
    EXPECT_EQ(one.wal_anchor(1).checkpoint_lsn, 0u);
    EXPECT_EQ(one.wal_anchor_count(), 1u);
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

// Slot 0 alone, because slot 0 alone is legal: `SetWalAnchor` refuses
// every other, so what has to survive the codec is the one slot plus the
// count field beside it. The 64-slot array stays in the on-disk layout -
// shrinking it would be a second format event for no reader's benefit.
TEST(SuperBlockWalAnchorTest, AnchorsSurviveEncodeDecode) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);
    ASSERT_TRUE(sb.SetWalAnchor(0, WalAnchorFields{4096, 8192, 12288, 0}).ok());

    PageBuf buf{};
    sb.Encode(AsSpan(buf));
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok());

    EXPECT_EQ(decoded.value().wal_anchor_count(), 1u);
    EXPECT_EQ(decoded.value().wal_anchor(0).checkpoint_lsn, 4096u);
    EXPECT_EQ(decoded.value().wal_anchor(0).redo_start_lsn, 8192u);
    EXPECT_EQ(decoded.value().wal_anchor(0).durable_lsn, 12288u);
    EXPECT_EQ(decoded.value().wal_anchor(0).segment_no, 0u);

    // Every other slot reads "never checkpointed", which is now the only
    // thing any of them can say.
    EXPECT_EQ(decoded.value().wal_anchor(3).redo_start_lsn, 0u);
    EXPECT_EQ(decoded.value().wal_anchor(7).redo_start_lsn, 0u);
}

// Slot 0 is the only slot (AM-S4(d)), so this is the refusal that says so.
// It replaces `AnchorsAreIndexedIndependentlyPerCore` and
// `CountTracksTheHighestCoreEverPublished`, whose subject - a table indexed
// by core, with a count tracking the highest one - the collapse removed.
TEST(SuperBlockWalAnchorTest, EveryCoreButZeroIsRefusedASlot) {
    SuperBlock sb = SuperBlock::CreateFresh(1000);

    for (std::uint32_t core = 1; core < kMaxWalCores; ++core) {
        Status s = sb.SetWalAnchor(core, WalAnchorFields{core + 1, core + 2, core + 3, core});
        EXPECT_FALSE(s.ok()) << "core " << core << " was given a slot";
        EXPECT_EQ(s.code(), StatusCode::kInvalidArgument);
        EXPECT_NE(s.message().find("one WAL stream"), std::string::npos);
    }

    // And nothing was written by any of them: a refusal that had raised the
    // count would send the next mount looking for anchors nobody published.
    EXPECT_EQ(sb.wal_anchor_count(), 0u);

    // Slot 0 still takes one, and the count is then 1 and stays 1.
    ASSERT_TRUE(sb.SetWalAnchor(0, WalAnchorFields{4, 5, 6, 0}).ok());
    EXPECT_EQ(sb.wal_anchor_count(), 1u);
    ASSERT_TRUE(sb.SetWalAnchor(0, WalAnchorFields{7, 8, 9, 0}).ok());
    EXPECT_EQ(sb.wal_anchor_count(), 1u);
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
    // Slot 0, because slot 0 is the only one a publish can reach
    // (AM-S4(d)). The claim under test is the *offset*, not the slot: an
    // eight-core count must not have shifted the anchor table's bytes.
    ASSERT_TRUE(sb.SetWalAnchor(0, WalAnchorFields{11, 22, 33, 44}).ok());

    PageBuf buf{};
    sb.Encode(AsSpan(buf));
    auto decoded = SuperBlock::Decode(AsConstSpan(buf));
    ASSERT_TRUE(decoded.ok());

    EXPECT_EQ(decoded.value().core_count(), 8u);
    EXPECT_EQ(decoded.value().wal_anchor(0).checkpoint_lsn, 11u);
    EXPECT_EQ(decoded.value().wal_anchor(0).redo_start_lsn, 22u);
    EXPECT_EQ(decoded.value().wal_anchor(0).durable_lsn, 33u);
    EXPECT_EQ(decoded.value().wal_anchor(0).segment_no, 44u);
    EXPECT_EQ(decoded.value().next_trx_id(), sb.next_trx_id());
}

}  // namespace
}  // namespace kds::server
