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

std::vector<std::uint64_t> Pks(const std::vector<stats::CabinEntry>* bucket) {
    std::vector<std::uint64_t> out;
    if (bucket != nullptr) {
        for (const stats::CabinEntry& entry : *bucket) out.push_back(entry.pk);
    }
    return out;
}

TEST(InnerBuildTest, PerKeyReplayIsWalkOrder) {
    // Two keys interleaved the way a walk interleaves them: each bucket
    // must hold its own rows in encounter order, unaffected by the other's.
    InnerBuild build;
    build.Add(Key(10), Entry(1));
    build.Add(Key(20), Entry(2));
    build.Add(Key(10), Entry(3));
    build.Add(Key(20), Entry(4));
    build.Add(Key(10), Entry(5));

    EXPECT_EQ(Pks(build.Find(Key(10))), (std::vector<std::uint64_t>{1, 3, 5}));
    EXPECT_EQ(Pks(build.Find(Key(20))), (std::vector<std::uint64_t>{2, 4}));
}

TEST(InnerBuildTest, AWalkReplaysWalkOrderNotPkOrder) {
    // Both key modes, one discriminating pin. Bucket one is an ASSIGNED
    // walk: ids ascend, so the walk encounters them ascending and replay
    // is ascending as a *consequence*. Bucket two is an EXPLICIT walk: a
    // caller-supplied id can be appended below existing ids
    // (docs/heap-and-tuple.md §4.1), so page-slot order diverges from pk
    // order — and walk order is the emission contract. A map that sorted
    // by pk (the Cabin recording's move, WalkAndRecord) would pass the
    // first bucket and every other test here while changing replies on an
    // EXPLICIT relation.
    InnerBuild build;
    build.Add(Key(7), Entry(2));
    build.Add(Key(7), Entry(5));
    build.Add(Key(7), Entry(9));
    build.Add(Key(8), Entry(5));
    build.Add(Key(8), Entry(2));
    build.Add(Key(8), Entry(9));

    EXPECT_EQ(Pks(build.Find(Key(7))), (std::vector<std::uint64_t>{2, 5, 9}));
    EXPECT_EQ(Pks(build.Find(Key(8))), (std::vector<std::uint64_t>{5, 2, 9}));
}

TEST(InnerBuildTest, AnUnknownKeyIsNullptr) {
    // And nullptr is the only "no rows" answer the type can express: Add
    // always pushes and nothing erases, so every bucket Find returns is
    // non-empty. What nullptr *means* is the caller's (inner_build.hpp).
    InnerBuild build;
    build.Add(Key(1), Entry(1));

    EXPECT_EQ(build.Find(Key(2)), nullptr);
    ASSERT_NE(build.Find(Key(1)), nullptr);
    EXPECT_FALSE(build.Find(Key(1))->empty());
}

TEST(InnerBuildTest, RowsCountsEveryEntryAcrossBuckets) {
    // What JB5's cap reads: entries, not values — the map's memory is per
    // entry (spec §7 counts rows, following aggregate_max_groups).
    InnerBuild build;
    EXPECT_EQ(build.rows(), 0u);
    build.Add(Key(1), Entry(1));
    build.Add(Key(1), Entry(2));
    build.Add(Key(2), Entry(3));

    EXPECT_EQ(build.rows(), 3u);
}

}  // namespace
}  // namespace kds::exec
