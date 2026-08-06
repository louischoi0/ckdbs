#include "kds/exec/aggregate.hpp"

#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/row_codec.hpp"

// AG03 - the fold itself (docs/feat-aggregate.md §5,
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
// Replaces the global operators for the whole test binary, and counts only
// while `counting` is set - so every other test in the binary pays one
// predicate per allocation and nothing else.
std::size_t g_allocations = 0;
bool g_counting = false;

struct CountAllocations {
    CountAllocations() {
        g_allocations = 0;
        g_counting = true;
    }
    ~CountAllocations() { g_counting = false; }
    std::size_t count() const { return g_allocations; }
};

}  // namespace
}  // namespace kds::exec

void* operator new(std::size_t size) {
    if (kds::exec::g_counting) ++kds::exec::g_allocations;
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace kds::exec {
namespace {

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
            out.push_back(v.type == parser::ValueType::kNull ? "NULL" : FormatValue(v));
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
