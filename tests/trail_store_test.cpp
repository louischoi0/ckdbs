#include "kds/stats/trail_store.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// Writing and reading one instance's trail (docs/inflight/in-progress/waystone-workplan.md P08).
//
// The round-trip is the least interesting thing here - the page codec has
// its own tests. What is pinned below is the three ways a trail must
// *refuse* to be something it is not: it is never partial, never merged
// with an older execution's, and never somebody else's.

namespace kds::stats {
namespace {

class TrailStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto root = CreateDirPage(store_);
        ASSERT_TRUE(root.ok());
        root_ = root.value();
    }

    // One plausible observation. `n` varies every field that identifies a
    // tuple, so a mixed-up entry shows as a wrong value rather than as a
    // coincidence.
    static exec::TouchedTuple Touched(std::uint64_t n) {
        exec::TouchedTuple t;
        t.rel_oid = 4000 + n;
        t.pk = 100 + n;
        t.page_id = static_cast<PageId>(500 + n);
        t.slot = static_cast<std::uint16_t>(n);
        t.step_id = static_cast<std::uint16_t>(n % 4);
        return t;
    }

    static std::vector<exec::TouchedTuple> Trail(std::size_t count) {
        std::vector<exec::TouchedTuple> out;
        for (std::size_t i = 0; i < count; ++i) out.push_back(Touched(i));
        return out;
    }

    storage::InMemoryPageStore store_{128};
    PageId root_ = kInvalidPageId;
    static constexpr int kDepth = 1;
};

TEST_F(TrailStoreTest, ATrailRoundTripsEveryFieldInExecutionOrder) {
    const InstanceKey key{0xAAAA, 0xBBBB};
    const auto touched = Trail(3);
    ASSERT_TRUE(WriteTrail(store_, root_, kDepth, key, touched, /*recorded_ts=*/77).ok());

    auto read = ReadTrail(store_, root_, kDepth, key);
    ASSERT_TRUE(read.ok());
    ASSERT_EQ(read.value().size(), 3u);

    for (std::size_t i = 0; i < touched.size(); ++i) {
        EXPECT_EQ(read.value()[i].pk, touched[i].pk);
        EXPECT_EQ(read.value()[i].rel_oid, touched[i].rel_oid) << "an entry must say which "
                                                                  "relation it came from";
        EXPECT_EQ(read.value()[i].page_id, touched[i].page_id);
        EXPECT_EQ(read.value()[i].slot, touched[i].slot);
        // The join position. Without it a cross-relation trail is an
        // unordered bag of tuples from three tables (spec section 6).
        EXPECT_EQ(read.value()[i].step_id, touched[i].step_id);
        EXPECT_NE(read.value()[i].flags & kWaystoneEntryValid, 0);
    }
}

TEST_F(TrailStoreTest, PageEpochIsRecordedAsZeroBecauseNoEpochExists) {
    // Documenting a gap, not endorsing it. This engine has no page epoch
    // (storage/page_header.hpp reserves the field), so spec section 2's
    // replay rule 2 has nothing to check against and a replayer must not
    // pretend otherwise. It is safe *today* only because a tuple's address
    // is stable for life - the fixed-length rule stops an UPDATE migrating
    // one and nothing relayouts. When relayout lands, the epoch must too.
    const InstanceKey key{1, 2};
    ASSERT_TRUE(WriteTrail(store_, root_, kDepth, key, Trail(1), 0).ok());

    auto read = ReadTrail(store_, root_, kDepth, key);
    ASSERT_TRUE(read.ok());
    ASSERT_EQ(read.value().size(), 1u);
    EXPECT_EQ(read.value()[0].page_epoch, 0u);
}

TEST_F(TrailStoreTest, ReRecordingReplacesTheTrailRatherThanMergingIt) {
    const InstanceKey key{7, 9};
    ASSERT_TRUE(WriteTrail(store_, root_, kDepth, key, Trail(5), 1).ok());
    ASSERT_TRUE(WriteTrail(store_, root_, kDepth, key, Trail(2), 2).ok());

    auto read = ReadTrail(store_, root_, kDepth, key);
    ASSERT_TRUE(read.ok());
    // Wholesale, per P08: a merge would accumulate rows from the earlier
    // execution that no longer qualify, and nothing at this layer can tell
    // those from rows that still do.
    EXPECT_EQ(read.value().size(), 2u);
}

TEST_F(TrailStoreTest, ATrailTooLongForOnePageIsRefusedWholeRatherThanTruncated) {
    const InstanceKey key{11, 13};
    ASSERT_TRUE(WriteTrail(store_, root_, kDepth, key, Trail(kMaxTrailEntries), 1).ok())
        << "exactly a full page must still fit";

    // One more than fits. **Not truncated**: a truncated trail covers only
    // the first rows of an execution and no reader can tell it from a
    // complete one, so replay would serve a partial answer believing it
    // whole. Refusing leaves the instance with no trail, which is the state
    // every instance starts in and which every consumer already handles.
    Status s = WriteTrail(store_, root_, kDepth, key, Trail(kMaxTrailEntries + 1), 2);
    EXPECT_EQ(s.code(), StatusCode::kOutOfSpace);

    // And the refusal left the previous trail intact rather than half
    // overwriting it.
    auto read = ReadTrail(store_, root_, kDepth, key);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().size(), kMaxTrailEntries);
}

TEST_F(TrailStoreTest, AnEmptyTrailIsRefused) {
    // Writing one would replace a populated trail with nothing, which is a
    // worse answer than keeping the previous execution's.
    const InstanceKey key{3, 4};
    EXPECT_EQ(WriteTrail(store_, root_, kDepth, key, {}, 0).code(),
              StatusCode::kInvalidArgument);
}

TEST_F(TrailStoreTest, AnInstanceWithNoTrailReadsEmptyRatherThanFailing) {
    // The ordinary case for anything nobody has recorded, and it must not
    // be an error: a caller does the same thing here as for a stale entry -
    // fall through to the authoritative path.
    auto read = ReadTrail(store_, root_, kDepth, InstanceKey{99, 99});
    ASSERT_TRUE(read.ok());
    EXPECT_TRUE(read.value().empty());
}

TEST_F(TrailStoreTest, ACollidingInstanceReadsAsAMissAndNeverAsAForeignTrail) {
    // Two instances of one pattern whose arg_hashes agree on every bit the
    // depth-1 walk consumes (the low 11), so they resolve to the same page.
    const InstanceKey recorded{0x5150, 0x0000};
    const InstanceKey colliding{0x5150, 0x0000 + (1ull << 11)};
    ASSERT_EQ(DirIndexAt(recorded.arg_hash, kDepth, 0), DirIndexAt(colliding.arg_hash, kDepth, 0))
        << "the test needs a real collision to be testing anything";

    ASSERT_TRUE(WriteTrail(store_, root_, kDepth, recorded, Trail(3), 1).ok());

    // **The load-bearing assertion of this file.** The directory is keyed
    // by a hash, so a collision leads a reader to a real, valid, *wrong*
    // trail. The waystone header's own copy of the instance is what turns
    // that into a miss instead of somebody else's rows.
    auto read = ReadTrail(store_, root_, kDepth, colliding);
    ASSERT_TRUE(read.ok());
    EXPECT_TRUE(read.value().empty());

    // The recorded one is unaffected by having been asked about.
    auto original = ReadTrail(store_, root_, kDepth, recorded);
    ASSERT_TRUE(original.ok());
    EXPECT_EQ(original.value().size(), 3u);
}

TEST_F(TrailStoreTest, ACollidingWriteDisplacesTheOccupant) {
    // The `[PROPOSED]` collision policy (trail_store.hpp): displace. The
    // victim gets a header mismatch, a miss, and re-records on its next
    // execution - self-healing, and safe by invariant 8. Pinned so that
    // switching to drop or chain is a visible change rather than a silent
    // one.
    ASSERT_TRUE(TrailDisplacesOnCollision());

    const InstanceKey first{0x6161, 0x0000};
    const InstanceKey second{0x6161, 0x0000 + (1ull << 11)};
    ASSERT_TRUE(WriteTrail(store_, root_, kDepth, first, Trail(3), 1).ok());
    ASSERT_TRUE(WriteTrail(store_, root_, kDepth, second, Trail(2), 2).ok());

    auto evicted = ReadTrail(store_, root_, kDepth, first);
    ASSERT_TRUE(evicted.ok());
    EXPECT_TRUE(evicted.value().empty()) << "the displaced instance reads as a miss";

    auto winner = ReadTrail(store_, root_, kDepth, second);
    ASSERT_TRUE(winner.ok());
    EXPECT_EQ(winner.value().size(), 2u);
}

}  // namespace
}  // namespace kds::stats
