#include "kds/exec/inner_build.hpp"

#include <vector>

#include <gtest/gtest.h>

// JB2 (docs/workplan-join-inner-build.md) — the statement-local inner
// build's map. The load-bearing property and its argument live in
// inner_build.hpp; these tests pin it on its own, as the workplan asks.
// Key-making refusals (kNull, kParam) and value-kind non-collision are
// `MakeValueKey`/`MakeCabinKey` behavior, owned and pinned by
// cabin_store_test.cpp — not re-tested through this caller.

namespace kds::exec {
namespace {

stats::CabinEntry Entry(std::uint64_t pk) {
    stats::CabinEntry entry;
    entry.pk = pk;
    return entry;
}

stats::CabinKey Key(std::int64_t v) {
    parser::AstValue value;
    value.type = parser::ValueType::kInt;
    value.int_val = v;
    return *stats::MakeValueKey(value);
}

// Every Add in these tests must store; a refusal is the index-limit
// backstop (inner_build.hpp), which no test here goes near, and swallowing
// one would let a dropped row read as an ordering bug three assertions
// later.
void Add(InnerBuild& build, const stats::CabinKey& key, const stats::CabinEntry& entry) {
    ASSERT_TRUE(build.Add(key, entry));
}

std::vector<std::uint64_t> Pks(const InnerBuild::Bucket& bucket) {
    std::vector<std::uint64_t> out;
    for (const stats::CabinEntry& entry : bucket) out.push_back(entry.pk);
    return out;
}

TEST(InnerBuildTest, PerKeyReplayIsWalkOrder) {
    // Two keys interleaved the way a walk interleaves them: each bucket
    // must hold its own rows in encounter order, unaffected by the other's.
    InnerBuild build;
    Add(build, Key(10), Entry(1));
    Add(build, Key(20), Entry(2));
    Add(build, Key(10), Entry(3));
    Add(build, Key(20), Entry(4));
    Add(build, Key(10), Entry(5));

    EXPECT_EQ(Pks(build.Find(Key(10))), (std::vector<std::uint64_t>{1, 3, 5}));
    EXPECT_EQ(Pks(build.Find(Key(20))), (std::vector<std::uint64_t>{2, 4}));
}

TEST(InnerBuildTest, AWalkReplaysWalkOrderNotPkOrder) {
    // Both key modes, one discriminating pin. Bucket one is an ASSIGNED
    // walk: ids ascend, so the walk encounters them ascending and replay
    // is ascending as a *consequence*. Bucket two is an EXPLICIT walk: a
    // caller-supplied id can be appended below existing ids
    // (docs/spec/heap-and-tuple.md §4.1), so page-slot order diverges from pk
    // order — and walk order is the emission contract. A map that sorted
    // by pk (the Cabin recording's move, WalkAndRecord) would pass the
    // first bucket and every other test here while changing replies on an
    // EXPLICIT relation.
    InnerBuild build;
    Add(build, Key(7), Entry(2));
    Add(build, Key(7), Entry(5));
    Add(build, Key(7), Entry(9));
    Add(build, Key(8), Entry(5));
    Add(build, Key(8), Entry(2));
    Add(build, Key(8), Entry(9));

    EXPECT_EQ(Pks(build.Find(Key(7))), (std::vector<std::uint64_t>{2, 5, 9}));
    EXPECT_EQ(Pks(build.Find(Key(8))), (std::vector<std::uint64_t>{5, 2, 9}));
}

TEST(InnerBuildTest, AnUnknownKeyIsAnEmptyBucket) {
    // And an empty Bucket is the only "no rows" answer the type can
    // express: Add always appends and nothing erases, so a key that was
    // bucketed at all comes back non-empty. What emptiness *means* is the
    // caller's (inner_build.hpp).
    InnerBuild build;
    Add(build, Key(1), Entry(1));

    EXPECT_TRUE(build.Find(Key(2)).empty());
    EXPECT_FALSE(build.Find(Key(1)).empty());
}

TEST(InnerBuildTest, ABucketSurvivesTheAddsThatExtendTheMap) {
    // The invalidation contract JB4's probe and JB6's resumed walk rely
    // on, pinned where it is stated (inner_build.hpp): a Bucket holds an
    // index, so growth of either arena vector - under its own key or any
    // other - leaves it walkable, and an append under its own key is
    // visible to a walk that has not yet passed the tail. A vector per
    // key could not promise the second half at all: push_back would
    // reallocate the very buffer a probe was iterating.
    InnerBuild build;
    Add(build, Key(1), Entry(1));
    InnerBuild::Bucket bucket = build.Find(Key(1));
    auto it = bucket.begin();
    EXPECT_EQ(it->pk, 1u);

    for (std::uint64_t pk = 2; pk <= 500; ++pk) Add(build, Key(pk % 7), Entry(pk));
    Add(build, Key(1), Entry(999));

    // The iterator taken before all of that still reads its own entry, and
    // walking on reaches the rows appended since.
    EXPECT_EQ(it->pk, 1u);
    std::vector<std::uint64_t> pks = Pks(bucket);
    ASSERT_FALSE(pks.empty());
    EXPECT_EQ(pks.front(), 1u);
    EXPECT_EQ(pks.back(), 999u);
}

TEST(InnerBuildTest, RowsCountsEveryEntryAcrossBuckets) {
    // What JB5's cap reads: entries, not values — the map's memory is per
    // entry (spec §7 counts rows, following aggregate_max_groups).
    InnerBuild build;
    EXPECT_EQ(build.rows(), 0u);
    Add(build, Key(1), Entry(1));
    Add(build, Key(1), Entry(2));
    Add(build, Key(2), Entry(3));

    EXPECT_EQ(build.rows(), 3u);
}

}  // namespace
}  // namespace kds::exec
