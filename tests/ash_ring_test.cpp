#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "kds/stats/ash_record.hpp"
#include "kds/stats/ash_ring.hpp"

// CJ's WE06 (`instructions/v3.2.0/workorder-cj-wait-events-ash.md` §2.7): the
// 64-byte record's round trip and offsets, and the ring's wrap-around.

namespace kds::stats {
namespace {

AshRecord Sample(std::uint64_t n) {
    AshRecord r;
    r.sample_ts_us = 1'000'000 * n;
    r.session_id = static_cast<std::uint32_t>(100 + n);
    r.core_id = 3;
    r.state = 2 | kAshBackground;
    r.wait_class = 5;
    r.event_id = 14;
    r.sched_group = 1;
    r.flags = kAshLate | kAshArgHashValid;
    r.fingerprint = 0x1122334455667788ull + n;
    r.step_id = 7;
    r.origin_session = 9;
    r.trx_id = 0x0000AABBCCDDEEFFull;
    r.p1 = 0xFFFFFFFFFFFFFFFFull;
    r.p2 = 0x80000001u;
    r.p3 = 42;
    r.wait_elapsed_us = 0xFFFFFFFFu;
    return r;
}

TEST(AshRecord, ARecordIs64BytesAndRoundTripsEveryField) {
    static_assert(sizeof(AshBytes) == 64);
    const AshRecord r = Sample(1);
    EXPECT_EQ(DecodeAsh(EncodeAsh(r)), r);
}

TEST(AshRecord, FieldsSitAtTheSpecsOffsetsLittleEndian) {
    AshRecord r;
    r.sample_ts_us = 0x0102030405060708ull;
    r.event_id = 0x0A0B;
    r.p3 = 0x0C0D0E0Fu;
    const AshBytes b = EncodeAsh(r);
    EXPECT_EQ(b[0], 0x08);
    EXPECT_EQ(b[7], 0x01);
    EXPECT_EQ(b[16], 0x0B);
    EXPECT_EQ(b[17], 0x0A);
    EXPECT_EQ(b[56], 0x0F);
    EXPECT_EQ(b[59], 0x0C);
}

TEST(AshRecord, ATrxIdKeepsItsLow48BitsOnly) {
    AshRecord r;
    r.trx_id = 0xFFFF123456789ABCull;
    EXPECT_EQ(DecodeAsh(EncodeAsh(r)).trx_id, 0x0000123456789ABCull);
}

TEST(AshRing, AnUnwrappedRingReturnsWhatWasWrittenOldestFirst) {
    AshRing ring(5 * kAshRecordBytes);
    for (std::uint64_t i = 1; i <= 3; ++i) ring.Record(Sample(i));
    const auto got = ring.Snapshot();
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], Sample(1));
    EXPECT_EQ(got[2], Sample(3));
}

TEST(AshRing, AWrappedRingKeepsTheNewestCapacityRecordsInOrder) {
    AshRing ring(4 * kAshRecordBytes);
    ASSERT_EQ(ring.capacity(), 4u);
    for (std::uint64_t i = 1; i <= 4; ++i) ring.Record(Sample(i));
    EXPECT_EQ(ring.Snapshot().front(), Sample(1)) << "exactly full: nothing lost yet";
    ring.Record(Sample(5));  // the boundary: the first record is overwritten
    auto got = ring.Snapshot();
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got.front(), Sample(2));
    EXPECT_EQ(got.back(), Sample(5));
    for (std::uint64_t i = 6; i <= 11; ++i) ring.Record(Sample(i));
    got = ring.Snapshot();
    ASSERT_EQ(got.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) EXPECT_EQ(got[i], Sample(8 + i));
    EXPECT_EQ(ring.recorded(), 11u);
}

TEST(AshRing, ASizeUnderOneRecordStillHoldsOne) {
    AshRing ring(10);
    EXPECT_EQ(ring.capacity(), 1u);
    ring.Record(Sample(1));
    ring.Record(Sample(2));
    ASSERT_EQ(ring.Snapshot().size(), 1u);
    EXPECT_EQ(ring.Snapshot()[0], Sample(2));
}

TEST(AshRing, ASnapshotDuringWritingHoldsWholeRecordsInOrder) {
    AshRing ring(64 * 50);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (std::uint64_t n = 1; !stop.load(); ++n) ring.Record(Sample(n));
    });
    for (int i = 0; i < 200; ++i) {
        const auto got = ring.Snapshot();
        for (std::size_t k = 0; k < got.size(); ++k) {
            EXPECT_EQ(got[k], Sample(got[k].sample_ts_us / 1'000'000)) << "a torn record";
            if (k > 0) EXPECT_EQ(got[k].sample_ts_us, got[k - 1].sample_ts_us + 1'000'000);
        }
    }
    stop.store(true);
    writer.join();
}

}  // namespace
}  // namespace kds::stats
