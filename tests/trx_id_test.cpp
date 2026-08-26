#include "kds/txn/trx_id.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"
#include "kds/server/superblock.hpp"

// docs/spec/txn.md section 10-2: ids are monotonic, never 1, never reissued
// across a simulated restart, a crash burns the block remainder, and past
// kMaxTxnId is OutOfRange.

namespace kds::txn {
namespace {

// A superblock that survives a "restart" the way a real one does: encoded
// to a page, decoded back. Anything the sequence did not persist is gone,
// which is the whole point of the restart cases below.
class Persisted {
public:
    Persisted() : superblock_(server::SuperBlock::CreateFresh(/*now_unix_seconds=*/1000)) {}

    server::SuperBlock& superblock() noexcept { return superblock_; }

    // What TrxIdSequence's `persist` callback does: write the page.
    Status Write() {
        superblock_.Encode(std::span<std::byte, kPageSize>(page_));
        ++writes_;
        return Status::OK();
    }

    // Reboot: read the last *written* image back. A raise that never
    // reached the page is lost, exactly as a crash loses it.
    void Reboot() {
        auto decoded = server::SuperBlock::Decode(std::span<const std::byte, kPageSize>(page_));
        EXPECT_TRUE(decoded.ok()) << decoded.status().message();
        if (decoded.ok()) superblock_ = decoded.value();
    }

    int writes() const noexcept { return writes_; }

private:
    server::SuperBlock superblock_;
    std::array<std::byte, kPageSize> page_{};
    int writes_ = 0;
};

TEST(TrxIdSequenceTest, AFreshDatabaseStartsAtTheFirstUserId) {
    server::SuperBlock sb = server::SuperBlock::CreateFresh(1000);
    EXPECT_EQ(sb.next_trx_id(), catalog::kFirstUserTrxId);
    EXPECT_EQ(catalog::kFirstUserTrxId, 2u);

    TrxIdSequence seq(sb);
    auto first = seq.Next();
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), 2u);
}

// The one id that must never be issued: every read view trusts it
// unconditionally and permanently (section 4.2).
TEST(TrxIdSequenceTest, TheBootstrapIdIsNeverIssued) {
    server::SuperBlock sb = server::SuperBlock::CreateFresh(1000);
    TrxIdSequence seq(sb);
    for (int i = 0; i < 1000; ++i) {
        auto id = seq.Next();
        ASSERT_TRUE(id.ok());
        EXPECT_NE(id.value(), catalog::kBootstrapXid);
        EXPECT_GT(id.value(), catalog::kBootstrapXid);
    }
}

TEST(TrxIdSequenceTest, IdsAreMonotonicAndUnique) {
    server::SuperBlock sb = server::SuperBlock::CreateFresh(1000);
    TrxIdSequence seq(sb);

    std::set<std::uint64_t> seen;
    std::uint64_t previous = 0;
    for (int i = 0; i < 10000; ++i) {
        auto id = seq.Next();
        ASSERT_TRUE(id.ok());
        EXPECT_GT(id.value(), previous);
        EXPECT_TRUE(seen.insert(id.value()).second);
        previous = id.value();
    }
}

// One durable write per block, not per id - which is the whole reason the
// block exists (bench/results-keystone-alloc.md measured 2629x for the
// alternative).
TEST(TrxIdSequenceTest, OneDurableWritePerBlockNotPerId) {
    Persisted p;
    TrxIdSequence seq(p.superblock(), [&] { return p.Write(); });

    for (std::uint64_t i = 0; i < kTrxIdBlockSize; ++i) {
        ASSERT_TRUE(seq.Next().ok());
    }
    EXPECT_EQ(p.writes(), 1) << "a whole block came out of one persisted ceiling";

    ASSERT_TRUE(seq.Next().ok());
    EXPECT_EQ(p.writes(), 2) << "the next block raised the ceiling again";
}

TEST(TrxIdSequenceTest, IdsAreNotReissuedAcrossARestart) {
    Persisted p;
    std::uint64_t last = 0;
    {
        TrxIdSequence seq(p.superblock(), [&] { return p.Write(); });
        for (int i = 0; i < 10; ++i) {
            auto id = seq.Next();
            ASSERT_TRUE(id.ok());
            last = id.value();
        }
    }

    p.Reboot();
    TrxIdSequence after(p.superblock(), [&] { return p.Write(); });
    auto first = after.Next();
    ASSERT_TRUE(first.ok());
    EXPECT_GT(first.value(), last) << "an id issued before the restart was issued again";
}

// The accepted cost of bump-ahead, asserted rather than described: the
// unspent remainder of the block is gone. Ids are unique and monotonic,
// never gapless (section 4.2).
TEST(TrxIdSequenceTest, ACrashBurnsTheBlockRemainder) {
    Persisted p;
    std::uint64_t last = 0;
    {
        TrxIdSequence seq(p.superblock(), [&] { return p.Write(); });
        for (int i = 0; i < 3; ++i) {
            auto id = seq.Next();
            ASSERT_TRUE(id.ok());
            last = id.value();
        }
        EXPECT_EQ(seq.ceiling(), catalog::kFirstUserTrxId + kTrxIdBlockSize);
    }

    p.Reboot();
    TrxIdSequence after(p.superblock(), [&] { return p.Write(); });
    auto first = after.Next();
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), catalog::kFirstUserTrxId + kTrxIdBlockSize)
        << "the whole block was burned, not just the ids spent";
    EXPECT_GT(first.value() - last, 1u) << "a gap is expected here, and is not a defect";
}

// Exhaustion is OutOfRange and never wrapped: a wrapped id would make an
// old row's writer look like a live one.
TEST(TrxIdSequenceTest, ExhaustionIsOutOfRangeRatherThanAWrap) {
    server::SuperBlock sb = server::SuperBlock::CreateFresh(1000);
    ASSERT_TRUE(sb.SetNextTrxId(kMaxTrxId).ok());

    TrxIdSequence seq(sb);
    auto last = seq.Next();
    ASSERT_TRUE(last.ok());
    EXPECT_EQ(last.value(), kMaxTrxId);

    auto past = seq.Next();
    EXPECT_FALSE(past.ok());
    EXPECT_EQ(past.status().code(), StatusCode::kOutOfRange);
}

TEST(TrxIdSequenceTest, TheLastBlockIsClampedRatherThanOverflowed) {
    server::SuperBlock sb = server::SuperBlock::CreateFresh(1000);
    ASSERT_TRUE(sb.SetNextTrxId(kMaxTrxId - 2).ok());

    TrxIdSequence seq(sb);
    ASSERT_TRUE(seq.Next().ok());
    EXPECT_EQ(seq.ceiling(), kMaxTrxId + 1) << "a block past the top is clamped to it";
}

// ---- The superblock's half ------------------------------------------------

TEST(SuperBlockTrxIdTest, TheCeilingRoundTripsThroughAPage) {
    server::SuperBlock sb = server::SuperBlock::CreateFresh(1000);
    ASSERT_TRUE(sb.SetNextTrxId(123456).ok());

    std::array<std::byte, kPageSize> page{};
    sb.Encode(std::span<std::byte, kPageSize>(page));
    auto decoded = server::SuperBlock::Decode(std::span<const std::byte, kPageSize>(page));
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(decoded.value().next_trx_id(), 123456u);
}

// A ceiling that moved backwards would reissue ids already stamped on
// tuples - two versions written by "the same" transaction, which no later
// check could detect.
TEST(SuperBlockTrxIdTest, TheCeilingRefusesToMoveBackwards) {
    server::SuperBlock sb = server::SuperBlock::CreateFresh(1000);
    ASSERT_TRUE(sb.SetNextTrxId(500).ok());
    EXPECT_EQ(sb.SetNextTrxId(499).code(), StatusCode::kInvalidArgument);
    EXPECT_EQ(sb.next_trx_id(), 500u);
    EXPECT_TRUE(sb.SetNextTrxId(500).ok()) << "unchanged is not backwards";
}

// A version-8 image holds zeroes where this field now lives, and a zero
// here would hand a real transaction id 0 and then reissue kBootstrapXid.
// The version bump is what makes that unreachable; this pins the second
// line of defence.
TEST(SuperBlockTrxIdTest, AZeroCeilingIsRefusedAtTheDoor) {
    server::SuperBlock sb = server::SuperBlock::CreateFresh(1000);
    std::array<std::byte, kPageSize> page{};
    sb.Encode(std::span<std::byte, kPageSize>(page));

    const std::uint64_t zero = 0;
    std::memcpy(page.data() + server::kSuperBlockBodyOffset + server::kNextTrxIdOffset, &zero,
                sizeof(zero));

    auto decoded = server::SuperBlock::Decode(std::span<const std::byte, kPageSize>(page));
    EXPECT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.status().code(), StatusCode::kCorruption);
}

}  // namespace
}  // namespace kds::txn
