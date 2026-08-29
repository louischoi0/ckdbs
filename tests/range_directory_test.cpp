#include "kds/catalog/range_directory.hpp"

#include "kds/catalog/schema.hpp"

#include <gtest/gtest.h>

#include <vector>

// RD3's resolver (work order `instructions/v2.5.0/range-directory.md`
// RB1). **This file is deliberately its only unit caller** - the same
// discipline `range_eligible_test.cpp` states for RD4: the routing sites
// arrive at RB3 and RB4, and until then a call from a statement path is
// this row's shape leaking rather than a consumer.
//
// Everything here is pure over its arguments, so nothing needs a catalog
// or a store. The end-to-end half - that `TableAccess::ranges` is filled
// from `sys.ranges` and dies with the entry - lives in `catalog_test.cpp`
// where the rows can actually be written.

namespace kds::catalog {
namespace {

// Three ranges over the id space, boundaries at the row-id lease grant
// (D6's starting unit, 4,096): [0, 4096) core 0, [4096, 8192) core 1,
// [8192, end) core 2.
std::vector<RangeTarget> ThreeRanges() {
    std::vector<SysRangeRow> rows(3);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].rel_oid = 4000;
        rows[i].lo = static_cast<std::uint64_t>(i) * 4096;
        rows[i].owner_core = static_cast<std::uint32_t>(i);
        rows[i].entry_page = static_cast<PageId>(400 + i);
    }
    return RangeTargetsFrom(rows);
}

TEST(RangeDirectoryTest, HiIsDerivedFromTheNextRowAndTheLastOneEndsTheIdSpace) {
    const std::vector<RangeTarget> ranges = ThreeRanges();
    ASSERT_EQ(ranges.size(), 3u);

    EXPECT_EQ(ranges[0].lo, 0u);
    EXPECT_EQ(ranges[0].hi, 4096u);
    EXPECT_EQ(ranges[1].lo, 4096u);
    EXPECT_EQ(ranges[1].hi, 8192u);
    // The property that makes the rows a partition of the whole space
    // rather than of the ids that exist: nothing above the last boundary
    // is owned by nobody.
    EXPECT_EQ(ranges[2].lo, 8192u);
    EXPECT_EQ(ranges[2].hi, kIdSpaceEnd);

    // The row's two routing facts cross unchanged; `hi` is the only thing
    // this derivation adds.
    EXPECT_EQ(ranges[2].owner_core, 2u);
    EXPECT_EQ(ranges[2].entry_page, 402u);
}

TEST(RangeDirectoryTest, AnEmptyDirectoryIsRefusedRatherThanAnsweredWithOneRange) {
    // The zero-cost invariant, enforced (`range_directory.hpp`): this test
    // is what a caller that skipped `access.ranges.empty()` fails.
    auto refused = ResolveRanges({}, PkSpan::Whole());
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
    EXPECT_NE(refused.status().message().find("owner_core"), std::string::npos)
        << refused.status().message();
}

TEST(RangeDirectoryTest, AnEqualityNamesExactlyOneRange) {
    const std::vector<RangeTarget> ranges = ThreeRanges();

    auto one = ResolveRanges(ranges, PkSpan::Equality(5000));
    ASSERT_TRUE(one.ok()) << one.status().message();
    ASSERT_EQ(one.value().size(), 1u);
    EXPECT_EQ(one.value()[0].owner_core, 1u);
    EXPECT_EQ(one.value()[0].entry_page, 401u);
}

TEST(RangeDirectoryTest, ABoundaryIdBelongsToTheRangeAboveIt) {
    const std::vector<RangeTarget> ranges = ThreeRanges();

    // `[lo, hi)`: the boundary is the *first* id of the upper range, not
    // the last of the lower one. Getting this backwards routes every
    // block-aligned id - which is every id the allocator hands out first
    // (§6b) - to the wrong core.
    auto at = ResolveRanges(ranges, PkSpan::Equality(4096));
    ASSERT_TRUE(at.ok()) << at.status().message();
    ASSERT_EQ(at.value().size(), 1u);
    EXPECT_EQ(at.value()[0].owner_core, 1u);

    auto below = ResolveRanges(ranges, PkSpan::Equality(4095));
    ASSERT_TRUE(below.ok()) << below.status().message();
    ASSERT_EQ(below.value().size(), 1u);
    EXPECT_EQ(below.value()[0].owner_core, 0u);
}

TEST(RangeDirectoryTest, TheFirstAndLastIdsOfTheSpaceResolve) {
    const std::vector<RangeTarget> ranges = ThreeRanges();

    auto first = ResolveRanges(ranges, PkSpan::Equality(0));
    ASSERT_TRUE(first.ok()) << first.status().message();
    ASSERT_EQ(first.value().size(), 1u);
    EXPECT_EQ(first.value()[0].owner_core, 0u);

    // kMaxKeystoneId is spellable, and `hi` being exclusive is what makes
    // it fall inside the last range rather than past it.
    auto last = ResolveRanges(ranges, PkSpan::Equality(kMaxKeystoneId));
    ASSERT_TRUE(last.ok()) << last.status().message();
    ASSERT_EQ(last.value().size(), 1u);
    EXPECT_EQ(last.value()[0].owner_core, 2u);
}

TEST(RangeDirectoryTest, ASpanStraddlingABoundaryNamesBothRanges) {
    const std::vector<RangeTarget> ranges = ThreeRanges();

    // The case §8 test 9 calls required rather than optional: a boundary
    // no predicate crosses is a boundary nothing checks.
    auto both = ResolveRanges(ranges, PkSpan{4000, 4200});
    ASSERT_TRUE(both.ok()) << both.status().message();
    ASSERT_EQ(both.value().size(), 2u);
    EXPECT_EQ(both.value()[0].owner_core, 0u);
    EXPECT_EQ(both.value()[1].owner_core, 1u);
}

TEST(RangeDirectoryTest, ASpanInsideOneRangeNamesOnlyIt) {
    const std::vector<RangeTarget> ranges = ThreeRanges();

    auto inside = ResolveRanges(ranges, PkSpan{4100, 4200});
    ASSERT_TRUE(inside.ok()) << inside.status().message();
    ASSERT_EQ(inside.value().size(), 1u);
    EXPECT_EQ(inside.value()[0].owner_core, 1u);
}

TEST(RangeDirectoryTest, ANonPkPredicateNamesEveryRangeInAscendingOrder) {
    const std::vector<RangeTarget> ranges = ThreeRanges();

    // §2a: a non-pk read predicate names no range, so the default is every
    // range - and the order is the one RD7 concatenates in.
    auto all = ResolveRanges(ranges, PkSpan::Whole());
    ASSERT_TRUE(all.ok()) << all.status().message();
    ASSERT_EQ(all.value().size(), 3u);
    EXPECT_EQ(all.value()[0].owner_core, 0u);
    EXPECT_EQ(all.value()[1].owner_core, 1u);
    EXPECT_EQ(all.value()[2].owner_core, 2u);
}

TEST(RangeDirectoryTest, TheAnswerIsASpanIntoTheCallersOwnStorage) {
    // §2c's shape (`range_directory.hpp`), pinned as the aliasing property
    // it is: a later change to a returned `std::vector` would compile
    // everywhere and turn a broken caller's use-after-free back into a
    // silently stale answer.
    const std::vector<RangeTarget> ranges = ThreeRanges();
    auto all = ResolveRanges(ranges, PkSpan::Whole());
    ASSERT_TRUE(all.ok()) << all.status().message();
    EXPECT_EQ(all.value().data(), ranges.data());
}

TEST(RangeDirectoryTest, AnEmptyOrInvertedSpanIsRefused) {
    const std::vector<RangeTarget> ranges = ThreeRanges();

    for (const PkSpan span : {PkSpan{100, 100}, PkSpan{200, 100}}) {
        auto refused = ResolveRanges(ranges, span);
        ASSERT_FALSE(refused.ok()) << "span [" << span.lo << ", " << span.hi << ")";
        EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
    }
}

TEST(RangeDirectoryTest, ASpanAboveTheIdSpaceIsRefused) {
    const std::vector<RangeTarget> ranges = ThreeRanges();

    // The ranges partition `[0, kIdSpaceEnd)` and no id falls outside it,
    // so a span that ends above the space is a caller that computed a
    // bound rather than one this could answer.
    auto refused = ResolveRanges(ranges, PkSpan{0, kIdSpaceEnd + 1});
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);

    // The boundary itself is in range: `Whole()` is exactly this span.
    auto whole = ResolveRanges(ranges, PkSpan{0, kIdSpaceEnd});
    EXPECT_TRUE(whole.ok()) << whole.status().message();
}

TEST(RangeDirectoryTest, AOneRowDirectoryStillResolvesRatherThanReadingAsUnsplit) {
    // A single `lo = 0` row is not the same fact as no rows at all: CC10's
    // migration writes one, and its `owner_core`/`entry_page` can then
    // differ from `sys.tables`. So "has rows" and not "has more than one
    // row" is the test the router branches on, and this pins it.
    std::vector<SysRangeRow> rows(1);
    rows[0].rel_oid = 4000;
    rows[0].lo = 0;
    rows[0].owner_core = 3;
    rows[0].entry_page = 700;

    const std::vector<RangeTarget> ranges = RangeTargetsFrom(rows);
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].hi, kIdSpaceEnd);

    auto one = ResolveRanges(ranges, PkSpan::Equality(999999));
    ASSERT_TRUE(one.ok()) << one.status().message();
    ASSERT_EQ(one.value().size(), 1u);
    EXPECT_EQ(one.value()[0].owner_core, 3u);
    EXPECT_EQ(one.value()[0].entry_page, 700u);
}

// ---- RD6: which chain a row belongs in ------------------------------

TEST(HeapChainForTest, AnUnsplitRelationAnswersTheTwoFieldsItAlwaysDid) {
    TableAccess access;
    access.oid = 4000;
    access.desc_page_id = 300;
    access.heap_tail_hint = 301;

    auto chain = access.HeapChainFor(12345);
    ASSERT_TRUE(chain.ok()) << chain.status().message();
    EXPECT_EQ(chain.value().head, 300u);
    // The *same* hint, by address: an unsplit insert must write its
    // landing page back where every insert before ranges existed wrote it,
    // or the O(1) tail search silently becomes a walk from the head.
    EXPECT_EQ(chain.value().tail_hint, &access.heap_tail_hint);
}

TEST(HeapChainForTest, ASplitRelationAnswersTheRangeTheIdBelongsTo) {
    TableAccess access;
    access.oid = 4000;
    access.desc_page_id = 300;
    std::vector<SysRangeRow> rows(2);
    rows[0].rel_oid = 4000;
    rows[0].lo = 0;
    rows[0].entry_page = 300;
    rows[1].rel_oid = 4000;
    rows[1].lo = 4096;
    rows[1].entry_page = 900;
    access.ranges = RangeTargetsFrom(rows);

    // **The defect, from the other side.** Before RD6 every insert used
    // `desc_page_id` - page 300 - for both of these, so the second row
    // landed in the lower range's chain, was accepted (its id clears that
    // tail's `min_key`), and then read as zero rows because the pk routed
    // the reader to page 900.
    auto below = access.HeapChainFor(4095);
    ASSERT_TRUE(below.ok()) << below.status().message();
    EXPECT_EQ(below.value().head, 300u);

    auto above = access.HeapChainFor(4096);
    ASSERT_TRUE(above.ok()) << above.status().message();
    EXPECT_EQ(above.value().head, 900u);
}

TEST(HeapChainForTest, EachRangeGetsItsOwnTailHint) {
    TableAccess access;
    access.oid = 4000;
    access.desc_page_id = 300;
    std::vector<SysRangeRow> rows(2);
    rows[0].rel_oid = 4000;
    rows[0].lo = 0;
    rows[0].entry_page = 300;
    rows[1].rel_oid = 4000;
    rows[1].lo = 4096;
    rows[1].entry_page = 900;
    access.ranges = RangeTargetsFrom(rows);

    // One hint per chain, which is what `heap_chain.hpp`'s safety argument
    // is stated over: a hint from another chain is a logic error that
    // layer cannot detect, and one hint shared across two chains would be
    // exactly that error, handed in on every other insert.
    auto lower = access.HeapChainFor(10);
    auto upper = access.HeapChainFor(5000);
    ASSERT_TRUE(lower.ok());
    ASSERT_TRUE(upper.ok());
    EXPECT_NE(lower.value().tail_hint, upper.value().tail_hint);
    EXPECT_NE(lower.value().tail_hint, &access.heap_tail_hint)
        << "a split relation wrote its landing page into the relation-wide hint";

    // Written through a const access, which is the borrow every write path
    // holds - the `mutable` on `RangeTarget::tail_hint` is what makes the
    // hint usable at all, and this is where that would fail to compile.
    const TableAccess& borrowed = access;
    auto again = borrowed.HeapChainFor(5000);
    ASSERT_TRUE(again.ok());
    *again.value().tail_hint = 901;
    EXPECT_EQ(access.ranges[1].tail_hint, 901u);
}

TEST(HeapChainForTest, AnIdOutsideTheSpaceIsRefusedRatherThanPlaced) {
    TableAccess access;
    access.oid = 4000;
    access.desc_page_id = 300;
    std::vector<SysRangeRow> rows(2);
    rows[0].rel_oid = 4000;
    rows[0].lo = 0;
    rows[0].entry_page = 300;
    rows[1].rel_oid = 4000;
    rows[1].lo = 4096;
    rows[1].entry_page = 900;
    access.ranges = RangeTargetsFrom(rows);

    // `ResolveRanges`' refusals cross unchanged. An id above the 40-bit
    // space names no range, and placing it somewhere would be the wrong
    // chain by definition.
    auto refused = access.HeapChainFor(kIdSpaceEnd);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::catalog
