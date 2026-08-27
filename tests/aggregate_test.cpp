#include "kds/exec/aggregate.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "alloc_counter.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/row_codec.hpp"

// AG03 - the fold itself (docs/spec/aggregate.md §5,
// docs/workplan-aggregate.md).
//
// Driven by a hand-built `ChainFrame` rather than by executing a chain,
// which is the right level for these questions: a fold's arithmetic, its
// NULL rules and its allocation behaviour are properties of the fold, and
// routing them through storage would test the executor's ability to produce
// rows instead. The end-to-end path is AG06's and the contract suite's.
//
// The allocation test is the one with teeth. Spec §5 requires **zero
// allocations per row** for a row that lands in a group that already
// exists: the key is encoded into a reused buffer, probed heterogeneously,
// and the item states are folded in place. Getting that wrong turns an
// O(1)-allocation scan into an O(rows) one, which is exactly how the
// dispatcher's trail collector regressed a point join by 18% before it was
// hoisted - the same failure mode, one layer up.

namespace kds::exec {
namespace {

// ---- An allocation counter ----------------------------------------------
//
// Shared with the row codec's own zero-allocation test - one counter,
// because it replaces the global operators and there can only be one set of
// those in a binary. See tests/alloc_counter.hpp.
using test_support::CountAllocations;

parser::AstValue IntVal(std::int64_t v) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = v;
    return out;
}

parser::AstValue StrVal(std::string v) {
    parser::AstValue out;
    out.type = parser::ValueType::kStr;
    out.str_val = std::move(v);
    return out;
}

parser::AstValue NullVal() { return parser::AstValue{}; }

// A `uint64` above INT64_MAX, decoded the way row_codec does it: `int_val`
// carries the wrapped bits and `raw_int_text` carries the true value,
// because a signed reading cannot represent it.
parser::AstValue BigUintVal(std::uint64_t v) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = static_cast<std::int64_t>(v);
    out.raw_int_text = std::to_string(v);
    return out;
}

AggregateItem Agg(parser::AggFunc func, std::uint16_t col_pos,
                  std::uint32_t type_val = catalog::kTypeValInt64) {
    AggregateItem item;
    item.is_aggregate = true;
    item.func = func;
    item.ref = ColumnRef{0, 0, col_pos};
    item.type_val = type_val;
    return item;
}

AggregateItem CountStar() {
    AggregateItem item;
    item.is_aggregate = true;
    item.func = parser::AggFunc::kCount;
    item.star_arg = true;
    return item;
}

AggregateItem KeyItem(std::uint16_t col_pos) {
    AggregateItem item;
    item.is_aggregate = false;
    item.ref = ColumnRef{0, 0, col_pos};
    item.type_val = catalog::kTypeValInt64;
    return item;
}

// One relation of `columns` columns, which is all a ChainFrame needs to
// size itself.
class Fold {
public:
    explicit Fold(std::size_t columns) {
        schema_.columns.resize(columns);
        schemas_.push_back(&schema_);
        frame_.Open(schemas_, nullptr);
    }

    // Writes one row's values into the frame and folds it.
    Status Row(Aggregator& agg, const std::vector<parser::AstValue>& values) {
        std::span<parser::AstValue> slots = frame_.SlotsFor(0);
        for (std::size_t i = 0; i < values.size() && i < slots.size(); ++i) {
            slots[i] = values[i];
        }
        return agg.Accumulate(frame_);
    }

    // Writes the row without folding, for the allocation test - so the
    // measured region is the fold and not the frame write.
    void Write(const std::vector<parser::AstValue>& values) {
        std::span<parser::AstValue> slots = frame_.SlotsFor(0);
        for (std::size_t i = 0; i < values.size() && i < slots.size(); ++i) {
            slots[i] = values[i];
        }
    }

    const ChainFrame& frame() const { return frame_; }

private:
    catalog::Schema schema_;
    std::vector<const catalog::Schema*> schemas_;
    ChainFrame frame_;
};

// Renders a fold's output the way a comparison can read it.
std::vector<std::vector<std::string>> Collect(Aggregator& agg) {
    std::vector<std::vector<std::string>> rows;
    Status s = agg.Finish([&](std::span<const parser::AstValue> row) {
        std::vector<std::string> out;
        for (const parser::AstValue& v : row) {
            out.push_back(v.type == parser::ValueType::kNull ? "NULL" : FormatValue(/*type_val=*/0, v));
        }
        rows.push_back(std::move(out));
        return Status::OK();
    });
    EXPECT_TRUE(s.ok()) << s.message();
    return rows;
}

std::vector<std::string> kNoLabels;

Aggregator Make(const AggregateSpec& spec, AggregateLimits limits = {}) {
    auto agg = Aggregator::Create(spec, kNoLabels, limits);
    EXPECT_TRUE(agg.ok()) << agg.status().message();
    return std::move(agg.value());
}

// ---- Zero allocations per row -------------------------------------------

TEST(AggregateTest, FoldingIntoAnExistingGroupAllocatesNothing) {
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kSum, 1));
    spec.items.push_back(Agg(parser::AggFunc::kMin, 1));

    Aggregator agg = Make(spec);
    Fold fold(2);

    // Found the group and let every reused buffer reach its capacity. The
    // scratch key allocates on the first row of a statement and never
    // again, which is what makes the measurement below meaningful rather
    // than merely lucky.
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(fold.Row(agg, {IntVal(7), IntVal(i)}).ok());
    }
    ASSERT_EQ(agg.group_count(), 1u);

    fold.Write({IntVal(7), IntVal(11)});
    {
        CountAllocations counter;
        for (int i = 0; i < 100; ++i) {
            ASSERT_TRUE(agg.Accumulate(fold.frame()).ok());
        }
        EXPECT_EQ(counter.count(), 0u)
            << "folding a row into an existing group must not allocate";
    }
}

TEST(AggregateTest, TheGlobalFormAllocatesNothingPerRowAtAll) {
    // No key is encoded and no map is probed, so the global form does not
    // even reach the scratch buffer.
    AggregateSpec spec;
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kSum, 1));

    Aggregator agg = Make(spec);
    Fold fold(2);
    fold.Write({IntVal(1), IntVal(2)});
    ASSERT_TRUE(agg.Accumulate(fold.frame()).ok());

    CountAllocations counter;
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(agg.Accumulate(fold.frame()).ok());
    }
    EXPECT_EQ(counter.count(), 0u);
}

// ---- §3.1 NULL semantics -------------------------------------------------

TEST(AggregateTest, TheGlobalFormEmitsOneRowOverEmptyInput) {
    // COUNT 0, SUM/MIN/MAX NULL. The standard's answer, and a different
    // shape from the grouped form rather than a degenerate one.
    AggregateSpec spec;
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kCount, 1));
    spec.items.push_back(Agg(parser::AggFunc::kSum, 1));
    spec.items.push_back(Agg(parser::AggFunc::kMin, 1));
    spec.items.push_back(Agg(parser::AggFunc::kMax, 1));

    Aggregator agg = Make(spec);
    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"0", "0", "NULL", "NULL", "NULL"}));
}

TEST(AggregateTest, TheGroupedFormEmitsZeroRowsOverEmptyInput) {
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(CountStar());

    Aggregator agg = Make(spec);
    EXPECT_TRUE(Collect(agg).empty());
}

TEST(AggregateTest, CountStarCountsRowsAndCountColumnCountsValues) {
    AggregateSpec spec;
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kCount, 1));

    Aggregator agg = Make(spec);
    Fold fold(2);
    ASSERT_TRUE(fold.Row(agg, {IntVal(1), IntVal(10)}).ok());
    ASSERT_TRUE(fold.Row(agg, {IntVal(1), NullVal()}).ok());
    ASSERT_TRUE(fold.Row(agg, {IntVal(1), IntVal(30)}).ok());

    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"3", "2"}));
}

TEST(AggregateTest, AGroupWithNoNonNullArgumentAnswersNull) {
    AggregateSpec spec;
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kSum, 1));
    spec.items.push_back(Agg(parser::AggFunc::kMin, 1));
    spec.items.push_back(Agg(parser::AggFunc::kMax, 1));

    Aggregator agg = Make(spec);
    Fold fold(2);
    ASSERT_TRUE(fold.Row(agg, {IntVal(1), NullVal()}).ok());
    ASSERT_TRUE(fold.Row(agg, {IntVal(1), NullVal()}).ok());

    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    // Two rows counted, and no value to fold - which is not the same
    // answer as a group whose values summed to zero.
    EXPECT_EQ(rows[0], (std::vector<std::string>{"2", "NULL", "NULL", "NULL"}));
}

TEST(AggregateTest, NullGroupingKeysFormOneGroup) {
    // Not because NULL equals NULL - `CompareValues` still says it does
    // not - but because the key encoding is the same bytes. Predicates
    // compare; grouping encodes identity.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(CountStar());

    Aggregator agg = Make(spec);
    Fold fold(2);
    ASSERT_TRUE(fold.Row(agg, {NullVal(), IntVal(1)}).ok());
    ASSERT_TRUE(fold.Row(agg, {NullVal(), IntVal(2)}).ok());
    ASSERT_TRUE(fold.Row(agg, {IntVal(5), IntVal(3)}).ok());

    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"NULL", "2"}));
    EXPECT_EQ(rows[1], (std::vector<std::string>{"5", "1"}));
}

TEST(AggregateTest, AnEmptyStringAndANullKeyAreDifferentGroups) {
    // The tag byte is what distinguishes them, which is the same reason
    // the tagged cell carries one on disk.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(CountStar());

    Aggregator agg = Make(spec);
    Fold fold(1);
    ASSERT_TRUE(fold.Row(agg, {NullVal()}).ok());
    ASSERT_TRUE(fold.Row(agg, {StrVal("")}).ok());
    EXPECT_EQ(agg.group_count(), 2u);
}

TEST(AggregateTest, StringKeysAreLengthPrefixedSoTheyCannotCollide) {
    // ('a','bc') and ('ab','c') concatenate to the same bytes without a
    // length prefix, and would read as one group.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.group_keys.push_back(ColumnRef{0, 0, 1});
    spec.items.push_back(CountStar());

    Aggregator agg = Make(spec);
    Fold fold(2);
    ASSERT_TRUE(fold.Row(agg, {StrVal("a"), StrVal("bc")}).ok());
    ASSERT_TRUE(fold.Row(agg, {StrVal("ab"), StrVal("c")}).ok());
    EXPECT_EQ(agg.group_count(), 2u);
}

// ---- §3.3 SUM arithmetic -------------------------------------------------

TEST(AggregateTest, SumCrossingInt64MaxFailsAndEmitsNothing) {
    AggregateSpec spec;
    spec.items.push_back(Agg(parser::AggFunc::kSum, 0));

    Aggregator agg = Make(spec);
    Fold fold(1);
    ASSERT_TRUE(fold.Row(agg, {IntVal(INT64_MAX)}).ok());

    const Status overflowed = fold.Row(agg, {IntVal(1)});
    ASSERT_FALSE(overflowed.ok());
    EXPECT_EQ(overflowed.code(), StatusCode::kOutOfRange);
    EXPECT_NE(overflowed.message().find("SUM overflow"), std::string::npos)
        << overflowed.message();
}

TEST(AggregateTest, SumCrossingInt64MinFailsToo) {
    AggregateSpec spec;
    spec.items.push_back(Agg(parser::AggFunc::kSum, 0));

    Aggregator agg = Make(spec);
    Fold fold(1);
    ASSERT_TRUE(fold.Row(agg, {IntVal(INT64_MIN)}).ok());
    EXPECT_FALSE(fold.Row(agg, {IntVal(-1)}).ok());
}

TEST(AggregateTest, AnOverflowErrorNamesTheAggregateAndTheGroup) {
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(Agg(parser::AggFunc::kSum, 1));

    const std::vector<std::string> labels = {"tier", "sum(qty)"};
    auto created = Aggregator::Create(spec, labels);
    ASSERT_TRUE(created.ok());
    Aggregator agg = std::move(created.value());

    Fold fold(2);
    ASSERT_TRUE(fold.Row(agg, {IntVal(42), IntVal(INT64_MAX)}).ok());
    const Status overflowed = fold.Row(agg, {IntVal(42), IntVal(1)});
    ASSERT_FALSE(overflowed.ok());
    EXPECT_NE(overflowed.message().find("sum(qty)"), std::string::npos)
        << overflowed.message();
    EXPECT_NE(overflowed.message().find("42"), std::string::npos) << overflowed.message();
}

TEST(AggregateTest, MinAndMaxOverUint64AboveInt64MaxAreExact) {
    // The comparison goes through `CompareValues` with the item's own
    // `type_val`, which reads a uint64 through its digit text - a signed
    // reading would order these two backwards.
    AggregateSpec spec;
    spec.items.push_back(Agg(parser::AggFunc::kMin, 0, catalog::kTypeValUint64));
    spec.items.push_back(Agg(parser::AggFunc::kMax, 0, catalog::kTypeValUint64));

    Aggregator agg = Make(spec);
    Fold fold(1);
    ASSERT_TRUE(fold.Row(agg, {BigUintVal(18446744073709551615ULL)}).ok());
    ASSERT_TRUE(fold.Row(agg, {BigUintVal(9223372036854775809ULL)}).ok());

    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0], "9223372036854775809");
    EXPECT_EQ(rows[0][1], "18446744073709551615");
}

// ---- MIN / MAX -----------------------------------------------------------

TEST(AggregateTest, MinAndMaxFoldOverSignedValues) {
    AggregateSpec spec;
    spec.items.push_back(Agg(parser::AggFunc::kMin, 0));
    spec.items.push_back(Agg(parser::AggFunc::kMax, 0));

    Aggregator agg = Make(spec);
    Fold fold(1);
    for (std::int64_t v : {5, -3, 40, 0}) {
        ASSERT_TRUE(fold.Row(agg, {IntVal(v)}).ok());
    }
    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"-3", "40"}));
}

TEST(AggregateTest, MinAndMaxFoldOverStrings) {
    AggregateSpec spec;
    spec.items.push_back(Agg(parser::AggFunc::kMin, 0, catalog::kTypeValVarchar));
    spec.items.push_back(Agg(parser::AggFunc::kMax, 0, catalog::kTypeValVarchar));

    Aggregator agg = Make(spec);
    Fold fold(1);
    for (const char* v : {"pear", "apple", "quince"}) {
        ASSERT_TRUE(fold.Row(agg, {StrVal(v)}).ok());
    }
    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"apple", "quince"}));
}

// ---- §9.7 Determinism ----------------------------------------------------

TEST(AggregateTest, GroupsAreEmittedInFirstSeenOrder) {
    // AG6. Not sorted - the groups live in a vector in the order the row
    // stream founded them, so the order is by construction. Hash-iteration
    // order would vary by seed and growth history.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(CountStar());

    Aggregator agg = Make(spec);
    Fold fold(1);
    for (std::int64_t v : {30, 10, 20, 10, 30}) {
        ASSERT_TRUE(fold.Row(agg, {IntVal(v)}).ok());
    }

    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"30", "2"}));
    EXPECT_EQ(rows[1], (std::vector<std::string>{"10", "2"}));
    EXPECT_EQ(rows[2], (std::vector<std::string>{"20", "1"}));
}

TEST(AggregateTest, TwoExecutionsOverTheSameRowsEmitIdenticalOutput) {
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kSum, 1));
    spec.items.push_back(Agg(parser::AggFunc::kMin, 1));

    const std::vector<std::vector<parser::AstValue>> input = {
        {IntVal(3), IntVal(1)}, {IntVal(1), IntVal(9)}, {IntVal(3), IntVal(4)},
        {IntVal(2), IntVal(2)}, {IntVal(1), IntVal(6)},
    };

    std::vector<std::vector<std::vector<std::string>>> runs;
    for (int run = 0; run < 2; ++run) {
        Aggregator agg = Make(spec);
        Fold fold(2);
        for (const auto& row : input) ASSERT_TRUE(fold.Row(agg, row).ok());
        runs.push_back(Collect(agg));
    }
    EXPECT_EQ(runs[0], runs[1]);
}

// ---- §6 Bounds -----------------------------------------------------------

TEST(AggregateTest, ExceedingMaxGroupsFailsTheStatementByName) {
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(CountStar());

    AggregateLimits limits;
    limits.max_groups = 3;
    Aggregator agg = Make(spec, limits);

    Fold fold(1);
    for (std::int64_t v = 0; v < 3; ++v) {
        ASSERT_TRUE(fold.Row(agg, {IntVal(v)}).ok()) << v;
    }
    // A fourth group is refused - it is never truncated, and no partial
    // answer is emitted.
    const Status refused = fold.Row(agg, {IntVal(99)});
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kResourceExhausted);
    EXPECT_NE(refused.message().find("aggregate_max_groups"), std::string::npos)
        << refused.message();
}

TEST(AggregateTest, ARowThatLandsInAnExistingGroupIsUnaffectedByTheCap) {
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(CountStar());

    AggregateLimits limits;
    limits.max_groups = 1;
    Aggregator agg = Make(spec, limits);

    Fold fold(1);
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(fold.Row(agg, {IntVal(7)}).ok());
    }
    EXPECT_EQ(agg.group_count(), 1u);
}

// ---- §3.2 DISTINCT (AG04) ------------------------------------------------

TEST(AggregateTest, CountDistinctAndSumDistinctFoldEachValueOnce) {
    AggregateSpec spec;
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kCount, 0));
    AggregateItem count_distinct = Agg(parser::AggFunc::kCount, 0);
    count_distinct.distinct = true;
    spec.items.push_back(count_distinct);
    AggregateItem sum_distinct = Agg(parser::AggFunc::kSum, 0);
    sum_distinct.distinct = true;
    spec.items.push_back(sum_distinct);
    spec.items.push_back(Agg(parser::AggFunc::kSum, 0));

    Aggregator agg = Make(spec);
    Fold fold(1);
    for (std::int64_t v : {10, 20, 10, 30, 20, 10}) {
        ASSERT_TRUE(fold.Row(agg, {IntVal(v)}).ok());
    }

    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    // 6 rows, 6 non-NULL values, 3 distinct, 60 distinct-summed, 100 summed.
    EXPECT_EQ(rows[0], (std::vector<std::string>{"6", "6", "3", "60", "100"}));
}

TEST(AggregateTest, DistinctIsPerGroupNotPerStatement) {
    // The set lives on the `(group, item)` pair: the same value in two
    // groups is distinct within each.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    AggregateItem distinct = Agg(parser::AggFunc::kCount, 1);
    distinct.distinct = true;
    spec.items.push_back(distinct);

    Aggregator agg = Make(spec);
    Fold fold(2);
    ASSERT_TRUE(fold.Row(agg, {IntVal(1), IntVal(5)}).ok());
    ASSERT_TRUE(fold.Row(agg, {IntVal(2), IntVal(5)}).ok());
    ASSERT_TRUE(fold.Row(agg, {IntVal(1), IntVal(5)}).ok());

    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"1", "1"}));
    EXPECT_EQ(rows[1], (std::vector<std::string>{"2", "1"}));
}

TEST(AggregateTest, CountDistinctSkipsNullsLikeEveryOtherAggregate) {
    AggregateSpec spec;
    AggregateItem distinct = Agg(parser::AggFunc::kCount, 0);
    distinct.distinct = true;
    spec.items.push_back(distinct);

    Aggregator agg = Make(spec);
    Fold fold(1);
    ASSERT_TRUE(fold.Row(agg, {NullVal()}).ok());
    ASSERT_TRUE(fold.Row(agg, {NullVal()}).ok());
    ASSERT_TRUE(fold.Row(agg, {IntVal(4)}).ok());

    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    // NULL is never a distinct value, because it is skipped before the set
    // is reached at all.
    EXPECT_EQ(rows[0], (std::vector<std::string>{"1"}));
}

TEST(AggregateTest, DistinctUsesTheSameEncodingTheGroupKeyUses) {
    // So an empty string and a NULL are told apart in a distinct set
    // exactly as they are told apart as group keys.
    AggregateSpec spec;
    AggregateItem distinct = Agg(parser::AggFunc::kCount, 0, catalog::kTypeValVarchar);
    distinct.distinct = true;
    spec.items.push_back(distinct);

    Aggregator agg = Make(spec);
    Fold fold(1);
    ASSERT_TRUE(fold.Row(agg, {StrVal("")}).ok());
    ASSERT_TRUE(fold.Row(agg, {NullVal()}).ok());
    ASSERT_TRUE(fold.Row(agg, {StrVal("")}).ok());
    ASSERT_TRUE(fold.Row(agg, {StrVal("x")}).ok());

    const auto rows = Collect(agg);
    // '' and 'x' are the two distinct values; the NULL was skipped.
    EXPECT_EQ(rows[0], (std::vector<std::string>{"2"}));
}

TEST(AggregateTest, MinDistinctEqualsMin) {
    // §3.2's accept-as-no-op: an extreme of a set equals the extreme of its
    // support, so the word changes nothing - and no set is built for it.
    AggregateSpec plain;
    plain.items.push_back(Agg(parser::AggFunc::kMin, 0));
    plain.items.push_back(Agg(parser::AggFunc::kMax, 0));

    AggregateSpec worded;
    AggregateItem min_distinct = Agg(parser::AggFunc::kMin, 0);
    min_distinct.distinct = true;
    AggregateItem max_distinct = Agg(parser::AggFunc::kMax, 0);
    max_distinct.distinct = true;
    worded.items.push_back(min_distinct);
    worded.items.push_back(max_distinct);

    std::vector<std::vector<std::string>> results[2];
    const AggregateSpec* specs[2] = {&plain, &worded};
    for (int i = 0; i < 2; ++i) {
        Aggregator agg = Make(*specs[i]);
        Fold fold(1);
        for (std::int64_t v : {7, 2, 7, 9, 2}) {
            ASSERT_TRUE(fold.Row(agg, {IntVal(v)}).ok());
        }
        results[i] = Collect(agg);
    }
    EXPECT_EQ(results[0], results[1]);
    EXPECT_EQ(results[0][0], (std::vector<std::string>{"2", "9"}));
}

TEST(AggregateTest, AStatementWithoutDistinctAllocatesNoSet) {
    // "Pays nothing for the feature" measured rather than asserted: the
    // group founded below allocates its key and state vectors and no set.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(Agg(parser::AggFunc::kCount, 1));

    Aggregator with_sets_off = Make(spec);
    Fold fold(2);
    fold.Write({IntVal(1), IntVal(2)});
    std::size_t without = 0;
    {
        CountAllocations counter;
        ASSERT_TRUE(with_sets_off.Accumulate(fold.frame()).ok());
        without = counter.count();
    }

    AggregateSpec with = spec;
    with.items[1].distinct = true;
    Aggregator with_sets_on = Make(with);
    Fold fold2(2);
    fold2.Write({IntVal(1), IntVal(2)});
    std::size_t withd = 0;
    {
        CountAllocations counter;
        ASSERT_TRUE(with_sets_on.Accumulate(fold2.frame()).ok());
        withd = counter.count();
    }
    EXPECT_LT(without, withd) << "the DISTINCT set should be the difference";
}

TEST(AggregateTest, ARepeatedDistinctValueAllocatesNothing) {
    AggregateSpec spec;
    AggregateItem distinct = Agg(parser::AggFunc::kCount, 0);
    distinct.distinct = true;
    spec.items.push_back(distinct);

    Aggregator agg = Make(spec);
    Fold fold(1);
    ASSERT_TRUE(fold.Row(agg, {IntVal(3)}).ok());

    fold.Write({IntVal(3)});
    CountAllocations counter;
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(agg.Accumulate(fold.frame()).ok());
    }
    EXPECT_EQ(counter.count(), 0u) << "a value already in the set must not allocate";
}

TEST(AggregateTest, ExceedingMaxDistinctFailsTheStatementByName) {
    AggregateSpec spec;
    AggregateItem distinct = Agg(parser::AggFunc::kCount, 0);
    distinct.distinct = true;
    spec.items.push_back(distinct);

    AggregateLimits limits;
    limits.max_distinct = 4;
    Aggregator agg = Make(spec, limits);

    Fold fold(1);
    for (std::int64_t v = 0; v < 4; ++v) {
        ASSERT_TRUE(fold.Row(agg, {IntVal(v)}).ok()) << v;
    }
    const Status refused = fold.Row(agg, {IntVal(99)});
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kResourceExhausted);
    EXPECT_NE(refused.message().find("aggregate_max_distinct"), std::string::npos)
        << refused.message();
}

TEST(AggregateTest, MaxDistinctIsSummedOverEverySetInTheStatement) {
    // The resource being bounded is the statement's memory, and a hundred
    // small sets cost what one large one does.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    AggregateItem distinct = Agg(parser::AggFunc::kCount, 1);
    distinct.distinct = true;
    spec.items.push_back(distinct);

    AggregateLimits limits;
    limits.max_distinct = 3;
    Aggregator agg = Make(spec, limits);

    Fold fold(2);
    // Three groups, one distinct value each: three entries in total, in
    // three separate sets.
    for (std::int64_t g = 0; g < 3; ++g) {
        ASSERT_TRUE(fold.Row(agg, {IntVal(g), IntVal(g)}).ok()) << g;
    }
    EXPECT_FALSE(fold.Row(agg, {IntVal(9), IntVal(9)}).ok());
}

// ---- AG-M: Merge (AG05) --------------------------------------------------

// Folds `rows` in one pass.
std::vector<std::vector<std::string>> FoldOnePass(
    const AggregateSpec& spec, std::size_t columns,
    const std::vector<std::vector<parser::AstValue>>& rows) {
    Aggregator agg = Make(spec);
    Fold fold(columns);
    for (const auto& row : rows) {
        EXPECT_TRUE(fold.Row(agg, row).ok());
    }
    return Collect(agg);
}

// Folds `rows` in two partitions decided by `in_left`, then merges.
std::vector<std::vector<std::string>> FoldPartitioned(
    const AggregateSpec& spec, std::size_t columns,
    const std::vector<std::vector<parser::AstValue>>& rows,
    const std::vector<bool>& in_left) {
    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold lfold(columns);
    Fold rfold(columns);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (in_left[i]) {
            EXPECT_TRUE(lfold.Row(left, rows[i]).ok());
        } else {
            EXPECT_TRUE(rfold.Row(right, rows[i]).ok());
        }
    }
    EXPECT_TRUE(left.Merge(std::move(right)).ok());
    return Collect(left);
}

// Sorted by the first output column, so two runs can be compared as sets
// of groups rather than as sequences.
std::vector<std::vector<std::string>> Sorted(std::vector<std::vector<std::string>> rows) {
    std::sort(rows.begin(), rows.end());
    return rows;
}

TEST(AggregateTest, PartitionFoldMergeEqualsOnePassFold) {
    // AG-M stated as a test rather than as a promise, and stated with the
    // precision the invariant has.
    //
    // **Two claims, not one.** The aggregate *values* are equal for any
    // partition whatsoever - that is what "mergeable" means and it is the
    // half a cross-core pipeline's correctness rests on. The group *order*
    // is equal only for a partition with a defined order, which spec §1
    // says in as many words: merge preserves the left's order and appends
    // the right's unseen groups in their own order. An arbitrary
    // interleaving has no order to preserve - a group founded second
    // overall can be the first one its partition sees - so the ordered
    // claim is tested against a **contiguous split**, which is also the
    // only kind a partitioned execution actually produces.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kCount, 1));
    spec.items.push_back(Agg(parser::AggFunc::kSum, 1));
    spec.items.push_back(Agg(parser::AggFunc::kMin, 1));
    spec.items.push_back(Agg(parser::AggFunc::kMax, 1));
    AggregateItem count_distinct = Agg(parser::AggFunc::kCount, 1);
    count_distinct.distinct = true;
    spec.items.push_back(count_distinct);
    AggregateItem sum_distinct = Agg(parser::AggFunc::kSum, 1);
    sum_distinct.distinct = true;
    spec.items.push_back(sum_distinct);

    std::vector<std::vector<parser::AstValue>> rows;
    std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
    auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    for (int i = 0; i < 200; ++i) {
        const std::int64_t key = static_cast<std::int64_t>(next() % 7);
        // Every fourth value NULL, so the "no non-NULL argument" path is
        // merged too and not just the arithmetic.
        rows.push_back({IntVal(key), (i % 4 == 3) ? NullVal()
                                                  : IntVal(static_cast<std::int64_t>(next() % 11))});
    }

    const auto one_pass = FoldOnePass(spec, 2, rows);

    // (1) Any partition at all: the same groups with the same values.
    for (int trial = 0; trial < 8; ++trial) {
        std::vector<bool> in_left(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) in_left[i] = (next() & 1) != 0;
        EXPECT_EQ(Sorted(FoldPartitioned(spec, 2, rows, in_left)), Sorted(one_pass))
            << "randomized trial " << trial;
    }

    // (2) A contiguous split - the shape a partitioned execution produces:
    // byte for byte, order included.
    for (std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{37},
                            rows.size() / 2, rows.size() - 1, rows.size()}) {
        std::vector<bool> in_left(rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) in_left[i] = i < cut;
        EXPECT_EQ(FoldPartitioned(spec, 2, rows, in_left), one_pass) << "cut at " << cut;
    }
}

TEST(AggregateTest, MergePreservesTheLeftGroupOrderAndAppendsTheRest) {
    // The order rule AG6 depends on across a merge: this side's groups stay
    // where they are, and the far side's newcomers arrive behind them in
    // the order that side founded them.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(CountStar());

    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold lfold(1);
    Fold rfold(1);
    for (std::int64_t v : {50, 10}) ASSERT_TRUE(lfold.Row(left, {IntVal(v)}).ok());
    for (std::int64_t v : {10, 90, 20}) ASSERT_TRUE(rfold.Row(right, {IntVal(v)}).ok());

    ASSERT_TRUE(left.Merge(std::move(right)).ok());
    const auto rows = Collect(left);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"50", "1"}));
    EXPECT_EQ(rows[1], (std::vector<std::string>{"10", "2"}));  // seen by both
    EXPECT_EQ(rows[2], (std::vector<std::string>{"90", "1"}));
    EXPECT_EQ(rows[3], (std::vector<std::string>{"20", "1"}));
}

TEST(AggregateTest, MergingTheGlobalFormAddsTheTwoStates) {
    AggregateSpec spec;
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kSum, 0));
    spec.items.push_back(Agg(parser::AggFunc::kMin, 0));

    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold lfold(1);
    Fold rfold(1);
    for (std::int64_t v : {5, 9}) ASSERT_TRUE(lfold.Row(left, {IntVal(v)}).ok());
    for (std::int64_t v : {2, 8}) ASSERT_TRUE(rfold.Row(right, {IntVal(v)}).ok());

    ASSERT_TRUE(left.Merge(std::move(right)).ok());
    const auto rows = Collect(left);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"4", "24", "2"}));
}

TEST(AggregateTest, MergingAnEmptyRightSideChangesNothing) {
    AggregateSpec spec;
    spec.items.push_back(CountStar());
    spec.items.push_back(Agg(parser::AggFunc::kMin, 0));

    Aggregator left = Make(spec);
    Fold lfold(1);
    ASSERT_TRUE(lfold.Row(left, {IntVal(7)}).ok());

    ASSERT_TRUE(left.Merge(Make(spec)).ok());
    const auto rows = Collect(left);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"1", "7"}));
}

TEST(AggregateTest, MergingIntoAnEmptyLeftSideTakesTheRightWhole) {
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(0));
    spec.items.push_back(Agg(parser::AggFunc::kMax, 0));

    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold rfold(1);
    for (std::int64_t v : {4, 6}) ASSERT_TRUE(rfold.Row(right, {IntVal(v)}).ok());

    ASSERT_TRUE(left.Merge(std::move(right)).ok());
    const auto rows = Collect(left);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"4", "4"}));
    EXPECT_EQ(rows[1], (std::vector<std::string>{"6", "6"}));
}

TEST(AggregateTest, ADistinctValueInBothPartitionsIsCountedOnce) {
    // The case adding the counters would get wrong: each side counted the
    // value once, so the union has to decide rather than the sum.
    AggregateSpec spec;
    AggregateItem count_distinct = Agg(parser::AggFunc::kCount, 0);
    count_distinct.distinct = true;
    AggregateItem sum_distinct = Agg(parser::AggFunc::kSum, 0);
    sum_distinct.distinct = true;
    spec.items.push_back(count_distinct);
    spec.items.push_back(sum_distinct);

    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold lfold(1);
    Fold rfold(1);
    for (std::int64_t v : {1, 2, 1}) ASSERT_TRUE(lfold.Row(left, {IntVal(v)}).ok());
    for (std::int64_t v : {2, 3, 2}) ASSERT_TRUE(rfold.Row(right, {IntVal(v)}).ok());

    ASSERT_TRUE(left.Merge(std::move(right)).ok());
    const auto rows = Collect(left);
    ASSERT_EQ(rows.size(), 1u);
    // {1, 2, 3}: three distinct values summing to six.
    EXPECT_EQ(rows[0], (std::vector<std::string>{"3", "6"}));
}

TEST(AggregateTest, AMergedAggregatorIsLeftEmptyRatherThanDuplicable) {
    AggregateSpec spec;
    spec.items.push_back(CountStar());

    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold rfold(1);
    ASSERT_TRUE(rfold.Row(right, {IntVal(1)}).ok());

    ASSERT_TRUE(left.Merge(std::move(right)).ok());
    // Deliberately reading the moved-from object: it must be empty rather
    // than holding a second copy of what was just folded elsewhere.
    EXPECT_EQ(right.group_count(), 0u);
    EXPECT_TRUE(Collect(right).empty());
}

TEST(AggregateTest, MergingFoldsOfDifferentShapesIsRefused) {
    AggregateSpec one;
    one.items.push_back(CountStar());
    AggregateSpec two;
    two.items.push_back(CountStar());
    two.items.push_back(Agg(parser::AggFunc::kSum, 0));

    Aggregator left = Make(one);
    Aggregator right = Make(two);
    const Status refused = left.Merge(std::move(right));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument);
}

TEST(AggregateTest, AMergeThatOverflowsSumFails) {
    AggregateSpec spec;
    spec.items.push_back(Agg(parser::AggFunc::kSum, 0));

    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold lfold(1);
    Fold rfold(1);
    ASSERT_TRUE(lfold.Row(left, {IntVal(INT64_MAX)}).ok());
    ASSERT_TRUE(rfold.Row(right, {IntVal(1)}).ok());

    const Status overflowed = left.Merge(std::move(right));
    ASSERT_FALSE(overflowed.ok());
    EXPECT_EQ(overflowed.code(), StatusCode::kOutOfRange);
}

// ---- AVG (aggregate.md §3.4, decided 2026-08-07) --------------------
//
// One principle, three consequences: AVG answers at exactly the scale the
// schema declared, rounding half to even. These tests pin the divide - the
// one place the pair state stops being (sum, count) and becomes a value.

parser::AstValue DecVal(std::int64_t unscaled, std::uint8_t scale) {
    parser::AstValue out;
    out.type = parser::ValueType::kDecimal;
    out.int_val = unscaled;
    out.scale = scale;
    return out;
}

AggregateItem AvgItem(std::uint16_t col_pos, std::uint8_t scale, bool distinct = false) {
    AggregateItem item = Agg(parser::AggFunc::kAvg, col_pos, catalog::kTypeValDecimal);
    item.scale = scale;
    item.distinct = distinct;
    return item;
}

TEST(AggregateTest, AvgAnswersAtTheColumnsDeclaredScale) {
    AggregateSpec spec;
    spec.items.push_back(AvgItem(0, 2));

    Aggregator agg = Make(spec);
    Fold fold(1);
    // 100.25 + 200.75 + 10.00 = 311.00; /3 = 103.666... -> 103.67. Not a
    // tie - the ordinary round-up case, at the column's own scale.
    for (std::int64_t v : {10025, 20075, 1000}) {
        ASSERT_TRUE(fold.Row(agg, {DecVal(v, 2)}).ok());
    }
    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"103.67"}));
}

TEST(AggregateTest, AvgTiesRoundHalfToEven) {
    // The pinned rounding rule, at its only interesting points. Each pair
    // averages to an exact .5 at the result scale, and the answer goes to
    // the even neighbor - in both signs, which half-up would get wrong on
    // one side.
    struct Case {
        std::vector<std::int64_t> unscaled;
        const char* expect;
    };
    const Case cases[] = {
        {{1, 2}, "0.02"},    // 1.5 -> 2 (1 is odd)
        {{2, 3}, "0.02"},    // 2.5 -> 2 (2 is even)
        {{-1, -2}, "-0.02"}, // -1.5 -> -2
        {{-2, -3}, "-0.02"}, // -2.5 -> -2
    };
    for (const Case& c : cases) {
        AggregateSpec spec;
        spec.items.push_back(AvgItem(0, 2));
        Aggregator agg = Make(spec);
        Fold fold(1);
        for (std::int64_t v : c.unscaled) ASSERT_TRUE(fold.Row(agg, {DecVal(v, 2)}).ok());
        const auto rows = Collect(agg);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0][0], c.expect);
    }
}

TEST(AggregateTest, AvgOverADeclaredScaleZeroRoundsToWholeUnits) {
    // DECIMAL(p, 0) averages - that scale was *declared*, which is the
    // whole line the integer-column refusal draws. avg(1, 2) = 1.5 -> 2.
    AggregateSpec spec;
    spec.items.push_back(AvgItem(0, 0));

    Aggregator agg = Make(spec);
    Fold fold(1);
    for (std::int64_t v : {1, 2}) ASSERT_TRUE(fold.Row(agg, {DecVal(v, 0)}).ok());
    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"2"}));
}

TEST(AggregateTest, AvgOverEmptyInputIsNullLikeSum) {
    // The global form emits one row over no rows, and an average of
    // nothing is an absence, not a zero - and never a divide by zero,
    // because the divide only runs when a value was seen.
    AggregateSpec spec;
    spec.items.push_back(AvgItem(0, 2));

    Aggregator agg = Make(spec);
    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"NULL"}));
}

TEST(AggregateTest, AvgDistinctDividesByTheDistinctCount) {
    // AVG(DISTINCT) is SUM(DISTINCT)/COUNT(DISTINCT) over one set: the
    // repeated 10.00 contributes once to the sum *and* once to the count,
    // or the two halves would disagree about which set they averaged.
    AggregateSpec spec;
    spec.items.push_back(AvgItem(0, 2, /*distinct=*/true));
    spec.items.push_back(AvgItem(0, 2));

    Aggregator agg = Make(spec);
    Fold fold(1);
    for (std::int64_t v : {1000, 1000, 2000}) {
        ASSERT_TRUE(fold.Row(agg, {DecVal(v, 2)}).ok());
    }
    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    // Distinct: (10.00 + 20.00) / 2 = 15.00. Plain: 40.00 / 3 = 13.33.
    EXPECT_EQ(rows[0], (std::vector<std::string>{"15.00", "13.33"}));
}

TEST(AggregateTest, AvgMergesAsSumAndCountPairsNotAsQuotients) {
    // AG-M's reason made concrete: the partitions' true averages are 1.00
    // and 3.00, whose naive mean is 2.00 - the pair state answers 2.33,
    // which is the average of the *rows*. Merging quotients would be
    // unrecoverable rounding; merging pairs is exact until the one divide.
    AggregateSpec spec;
    spec.items.push_back(AvgItem(0, 2));

    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold lfold(1);
    Fold rfold(1);
    ASSERT_TRUE(lfold.Row(left, {DecVal(100, 2)}).ok());
    for (std::int64_t v : {200, 400}) ASSERT_TRUE(rfold.Row(right, {DecVal(v, 2)}).ok());

    ASSERT_TRUE(left.Merge(std::move(right)).ok());
    const auto rows = Collect(left);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"2.33"}));
}

TEST(AggregateTest, AvgDistinctMergeCountsAValueInBothPartitionsOnce) {
    AggregateSpec spec;
    spec.items.push_back(AvgItem(0, 2, /*distinct=*/true));

    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold lfold(1);
    Fold rfold(1);
    for (std::int64_t v : {1000, 2000}) ASSERT_TRUE(lfold.Row(left, {DecVal(v, 2)}).ok());
    for (std::int64_t v : {1000, 3000}) ASSERT_TRUE(rfold.Row(right, {DecVal(v, 2)}).ok());

    ASSERT_TRUE(left.Merge(std::move(right)).ok());
    const auto rows = Collect(left);
    ASSERT_EQ(rows.size(), 1u);
    // The union is {10.00, 20.00, 30.00}: sum 60.00, count 3 - the shared
    // 10.00 counted once in both halves of the pair.
    EXPECT_EQ(rows[0], (std::vector<std::string>{"20.00"}));
}

// ---- The wide decimal in the fold (types.md TY2, 2026-08-07) --------

parser::AstValue DecWideVal(std::int64_t hi, std::int64_t lo, std::uint8_t scale) {
    parser::AstValue out;
    out.type = parser::ValueType::kDecimalWide;
    out.dec_hi = hi;
    out.int_val = lo;
    out.scale = scale;
    return out;
}

AggregateItem WideItem(parser::AggFunc func, std::uint16_t col_pos, std::uint8_t scale,
                       bool distinct = false) {
    AggregateItem item = Agg(func, col_pos, catalog::kTypeValDecimalWide);
    item.scale = scale;
    item.distinct = distinct;
    return item;
}

TEST(AggregateTest, WideSumFoldsThroughTheInt128Accumulator) {
    AggregateSpec spec;
    spec.items.push_back(WideItem(parser::AggFunc::kSum, 0, 0));

    Aggregator agg = Make(spec);
    Fold fold(1);
    // Each addend is 2^64 - beyond any int64 - so the sum being 2^65 means
    // the wide register carried it.
    ASSERT_TRUE(fold.Row(agg, {DecWideVal(1, 0, 0)}).ok());
    ASSERT_TRUE(fold.Row(agg, {DecWideVal(1, 0, 0)}).ok());
    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"36893488147419103232"}));
}

TEST(AggregateTest, WideSumOverflowFailsTheStatementLikeTheNarrowOne) {
    AggregateSpec spec;
    spec.items.push_back(WideItem(parser::AggFunc::kSum, 0, 0));

    Aggregator agg = Make(spec);
    Fold fold(1);
    // int128 max is (hi = INT64_MAX, lo = all ones); twice exceeds it.
    ASSERT_TRUE(fold.Row(agg, {DecWideVal(INT64_MAX, -1, 0)}).ok());
    const Status overflowed = fold.Row(agg, {DecWideVal(INT64_MAX, -1, 0)});
    ASSERT_FALSE(overflowed.ok());
    EXPECT_EQ(overflowed.code(), StatusCode::kOutOfRange);
    EXPECT_NE(overflowed.message().find("int128"), std::string::npos) << overflowed.message();
}

TEST(AggregateTest, WideAvgTiesRoundHalfToEvenBeyondInt64) {
    // sum = 2^65 + 1, count 2: the exact quotient is 2^64 + 0.5, a tie
    // whose even neighbor is 2^64 itself - only reachable through the wide
    // divide, since every number involved exceeds int64.
    AggregateSpec spec;
    spec.items.push_back(WideItem(parser::AggFunc::kAvg, 0, 0));

    Aggregator agg = Make(spec);
    Fold fold(1);
    ASSERT_TRUE(fold.Row(agg, {DecWideVal(1, 0, 0)}).ok());
    ASSERT_TRUE(fold.Row(agg, {DecWideVal(1, 1, 0)}).ok());
    const auto rows = Collect(agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0], (std::vector<std::string>{"18446744073709551616"}));
}

TEST(AggregateTest, WideDistinctMergeCountsAValueInBothPartitionsOnce) {
    // The union rule over 17-byte entries: the shared 2^64 contributes
    // once, so AVG(DISTINCT) divides 2^64 + 2^65 by 2 - and both the
    // decode and the accumulator have to be wide for that to come out.
    AggregateSpec spec;
    spec.items.push_back(WideItem(parser::AggFunc::kAvg, 0, 0, /*distinct=*/true));

    Aggregator left = Make(spec);
    Aggregator right = Make(spec);
    Fold lfold(1);
    Fold rfold(1);
    ASSERT_TRUE(lfold.Row(left, {DecWideVal(1, 0, 0)}).ok());
    ASSERT_TRUE(rfold.Row(right, {DecWideVal(1, 0, 0)}).ok());
    ASSERT_TRUE(rfold.Row(right, {DecWideVal(2, 0, 0)}).ok());

    ASSERT_TRUE(left.Merge(std::move(right)).ok());
    const auto rows = Collect(left);
    ASSERT_EQ(rows.size(), 1u);
    // (2^64 + 2^65) / 2 = 3 * 2^63 = 27670116110564327424.
    EXPECT_EQ(rows[0], (std::vector<std::string>{"27670116110564327424"}));
}

// ---- Malformed specs -----------------------------------------------------

TEST(AggregateTest, ASpecSelectingANonGroupingColumnIsRefused) {
    // Unreachable through the compiler, which enforces AG5 - and checked
    // anyway, because a spec can be built by something other than a
    // compile and a bound only one producer enforces is not a bound.
    AggregateSpec spec;
    spec.group_keys.push_back(ColumnRef{0, 0, 0});
    spec.items.push_back(KeyItem(1));

    auto agg = Aggregator::Create(spec, kNoLabels);
    ASSERT_FALSE(agg.ok());
    EXPECT_EQ(agg.status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::exec
