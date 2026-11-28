#include "kds/exec/step_vm.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/exec/step_compiler.hpp"
#include "kds/parser/parser.hpp"
#include "kds/server/superblock.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// BM-S2 (`instructions/v3.0.0/workorder-bm-scan-row-kernel.md`): the walk
// owns its btree slot loop (BM-R5), and what cannot change within a
// statement is checked once per step (BM-R6). Each cell pins a promise the
// rewrite could break without changing a row of an ordinary result: slot
// order across leaf boundaries, where a stop leaves the counters, the empty
// relation, the range stop, the per-row length check, and the first row a
// malformed chain is refused at - through a walk and through a lookup.

namespace kds::exec {
namespace {

class ScanRowWalkTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 4000);
        ASSERT_TRUE(boot.ok());
        boot_.emplace(std::move(boot.value()));
    }

    // `t (id int64, v int64, w int64)`, btree, rows (i, i, i) for i in
    // [1, rows].
    void Load(std::uint32_t rows) {
        catalog::Schema schema;
        const char* const names[] = {"id", "v", "w"};
        for (std::uint32_t pos = 0; pos < 3; ++pos) {
            auto type_row = boot_->catalog.ResolveTypeByName("int64");
            ASSERT_TRUE(type_row.ok());
            catalog::SysColumnRow row{};
            row.pos = pos;
            catalog::SetName(row.name, names[pos]);
            row.type_val = type_row.value().type_val;
            row.len = type_row.value().len;
            row.notnull = true;
            schema.columns.push_back(row);
        }
        auto created = boot_->catalog.CreateTable(catalog::kNamespacePublic, "t", schema);
        ASSERT_TRUE(created.ok()) << created.status().message();
        for (std::uint32_t i = 1; i <= rows; ++i) Insert(i);
    }

    void Insert(std::int64_t v) {
        auto oid = boot_->catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, "t");
        ASSERT_TRUE(oid.ok());
        auto access = boot_->catalog.InitTableAccess(oid.value());
        ASSERT_TRUE(access.ok());
        auto id = boot_->catalog.AllocateRowId(oid.value());
        ASSERT_TRUE(id.ok());
        auto payload =
            EncodeRow(access.value()->schema, access.value()->layout, id.value(), {Int(v), Int(v)});
        ASSERT_TRUE(payload.ok()) << payload.status().message();
        auto placed = btree::BtreeInsert(store_, access.value()->desc_page_id, id.value(),
                                         payload.value(), /*trx_id=*/1, access.value()->oid);
        ASSERT_TRUE(placed.ok()) << placed.status().message();
        if (placed.value().new_root != kInvalidPageId) {
            ASSERT_TRUE(boot_->catalog
                            .PublishRoot(oid.value(), /*index_oid=*/0, placed.value().new_root,
                                         access.value()->anchor_page_id)
                            .ok());
        }
    }

    PageId Root() {
        auto oid = boot_->catalog.FindTableOidByName(::kds::catalog::kNamespacePublic, "t");
        auto access = boot_->catalog.InitTableAccess(oid.value());
        return access.value()->desc_page_id;
    }

    static parser::AstValue Int(std::int64_t v) {
        parser::AstValue out;
        out.type = parser::ValueType::kInt;
        out.int_val = v;
        out.raw_int_text = std::to_string(v);
        return out;
    }

    StepChain CompileSql(const std::string& sql) {
        auto parsed = parser::Parse(sql);
        EXPECT_TRUE(parsed.ok()) << parsed.status().message();
        auto chain = Compile(boot_->catalog, std::get<parser::SelectStmt>(parsed.value()));
        EXPECT_TRUE(chain.ok()) << chain.status().message();
        return chain.ok() ? chain.value() : StepChain{};
    }

    struct Ran {
        Status status;
        std::vector<std::int64_t> ids;
        StepStats total;
    };

    // Runs `chain`, collecting each row's column 0; `stop_after` rows, then
    // the sink stops the statement.
    Ran RunChain(const StepChain& chain, std::size_t stop_after = SIZE_MAX,
                 const Budget& budget = Budget(), BytesFold* fold = nullptr) {
        Ran out;
        ExecStats stats;
        out.status = Execute(
            boot_->catalog, store_, chain,
            [&](const ChainFrame& frame) -> StatusOr<storage::VisitControl> {
                out.ids.push_back(frame.Get(chain.projection.front()).int_val);
                return out.ids.size() >= stop_after ? storage::VisitControl::kStop
                                                    : storage::VisitControl::kContinue;
            },
            &stats, budget, /*trail=*/nullptr, /*replay=*/nullptr, /*cabins=*/nullptr,
            /*snapshot=*/nullptr, /*indexes=*/true, fold);
        out.total = stats.Total();
        return out;
    }

    storage::InMemoryPageStore store_{server::kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// Enough rows for three leaves at least, so the walk crosses two leaf
// boundaries.
constexpr std::uint32_t kThreeLeaves = 900;

TEST_F(ScanRowWalkTest, AWalkOverThreeLeavesEmitsEverySlotInOrderAndCountsEachLeafOnce) {
    Load(kThreeLeaves);
    auto leaves = btree::BtreeLeafCount(store_, Root());
    ASSERT_TRUE(leaves.ok());
    ASSERT_GE(leaves.value(), 3u);

    Ran ran = RunChain(CompileSql("SELECT t.id FROM t"));
    ASSERT_TRUE(ran.status.ok()) << ran.status.message();
    ASSERT_EQ(ran.ids.size(), kThreeLeaves);
    for (std::size_t i = 0; i < ran.ids.size(); ++i) {
        ASSERT_EQ(ran.ids[i], static_cast<std::int64_t>(i + 1)) << "slot order broke at row " << i;
    }
    EXPECT_EQ(ran.total.pages_fetched, leaves.value());
    EXPECT_EQ(ran.total.rows_examined, kThreeLeaves);
}

TEST_F(ScanRowWalkTest, AStopMidLeafLeavesOnePageAndTheStoppingRowsOrdinal) {
    Load(kThreeLeaves);
    // The fifth row matching `v >= 3` is row 7: examined rows 1..7, one leaf.
    Ran ran = RunChain(CompileSql("SELECT t.id FROM t WHERE v >= 3"), /*stop_after=*/5);
    ASSERT_TRUE(ran.status.ok()) << ran.status.message();
    EXPECT_EQ(ran.ids, (std::vector<std::int64_t>{3, 4, 5, 6, 7}));
    EXPECT_EQ(ran.total.pages_fetched, 1u);
    EXPECT_EQ(ran.total.rows_examined, 7u);
    EXPECT_EQ(ran.total.rows_matched, 5u);
}

TEST_F(ScanRowWalkTest, AnEmptyRelationFetchesNoPageAndRefusesNoMalformedChain) {
    Load(0);
    StepChain chain = CompileSql("SELECT t.id FROM t WHERE v = 3");
    ASSERT_FALSE(chain.steps.empty());
    ASSERT_FALSE(chain.steps[0].residual.empty());
    chain.steps[0].residual[0].lhs.col_pos = 99;
    Ran ran = RunChain(chain);
    EXPECT_TRUE(ran.status.ok()) << ran.status.message();
    EXPECT_TRUE(ran.ids.empty());
    EXPECT_EQ(ran.total.pages_fetched, 0u);
}

TEST_F(ScanRowWalkTest, ARangeWalkStopsOnTheFirstLeafPastItsBound) {
    Load(kThreeLeaves);
    Ran ran = RunChain(CompileSql("SELECT t.id FROM t WHERE id BETWEEN 1 AND 3"));
    ASSERT_TRUE(ran.status.ok()) << ran.status.message();
    EXPECT_EQ(ran.ids, (std::vector<std::int64_t>{1, 2, 3}));
    EXPECT_EQ(ran.total.range_pages_pruned, 1u);
    EXPECT_EQ(ran.total.pages_fetched, 2u) << "the first leaf, and the one the bound stopped at";
}

TEST_F(ScanRowWalkTest, AMalformedConjunctIsRefusedAtTheFirstRowThroughAWalk) {
    Load(10);
    StepChain chain = CompileSql("SELECT t.id FROM t WHERE v = 3");
    chain.steps[0].residual[0].lhs.col_pos = 99;
    Ran ran = RunChain(chain);
    ASSERT_FALSE(ran.status.ok());
    EXPECT_EQ(ran.status.code(), StatusCode::kCorruption);
    // Today's message: column 99 is outside the step's frame span, so the
    // frame's own resolve check is the one that fires.
    EXPECT_NE(ran.status.message().find("cannot resolve; the chain is malformed"),
              std::string::npos)
        << ran.status.message();
    EXPECT_TRUE(ran.ids.empty());
}

TEST_F(ScanRowWalkTest, AMalformedConjunctIsRefusedThroughALookupStep) {
    Load(10);
    StepChain chain = CompileSql("SELECT t.id FROM t WHERE id = 4 AND v = 4");
    ASSERT_FALSE(chain.steps.empty());
    // An unbound `$param` on the v conjunct: a skipped check would compare
    // it false and answer empty, never read out of bounds.
    bool broke = false;
    for (StepPredicate& pred : chain.steps[0].residual) {
        if (pred.lhs.col_pos == 1) {
            pred.rhs.literal.type = parser::ValueType::kParam;
            pred.rhs.literal.str_val = "p";
            broke = true;
        }
    }
    ASSERT_TRUE(broke) << "the lookup step carries the v conjunct in its residual";
    Ran ran = RunChain(chain);
    ASSERT_FALSE(ran.status.ok());
    EXPECT_EQ(ran.status.code(), StatusCode::kCorruption);
    EXPECT_NE(ran.status.message().find("parameter '$p' has no bound value"), std::string::npos)
        << ran.status.message();
}

// BM-Q7: a later conjunct's malformation is refused even where an earlier
// conjunct false on every row used to mask it.
TEST_F(ScanRowWalkTest, AMalformedLaterConjunctIsRefusedThoughAnEarlierOneRejectsEveryRow) {
    Load(10);
    StepChain chain = CompileSql("SELECT t.id FROM t WHERE v = -1 AND w = 3");
    ASSERT_EQ(chain.steps[0].residual.size(), 2u);
    chain.steps[0].residual[1].lhs.col_pos = 99;
    Ran ran = RunChain(chain);
    ASSERT_FALSE(ran.status.ok());
    EXPECT_EQ(ran.status.code(), StatusCode::kCorruption);
}

TEST_F(ScanRowWalkTest, APayloadOfTheWrongLengthIsStillRefusedPerRow) {
    Load(10);
    // Shorten row 6's data_len by one byte: ReadTuple still accepts it (the
    // payload stays inside its tuple), and the decode's length check is the
    // one that must refuse it - on that row, after five good ones.
    auto leaf = btree::BtreeLeftmostLeaf(store_, Root());
    ASSERT_TRUE(leaf.ok());
    {
        auto page = store_.Get(leaf.value());
        ASSERT_TRUE(page.ok());
        std::byte* base = page.value().bytes().data();
        heap::PageView view(page.value().bytes());
        auto payload = view.PayloadAt(5, view.slot_count());
        ASSERT_TRUE(payload.ok());
        const std::size_t at = static_cast<std::size_t>(payload.value().data() - base) -
                               heap::kTupleHeaderOnDiskSize + heap::kTupleDataLenOffset;
        std::uint16_t data_len = 0;
        std::memcpy(&data_len, base + at, sizeof(data_len));
        --data_len;
        std::memcpy(base + at, &data_len, sizeof(data_len));
        page.value().MarkDirty();
    }

    Ran ran = RunChain(CompileSql("SELECT t.id FROM t WHERE v > 0"));
    ASSERT_FALSE(ran.status.ok());
    EXPECT_EQ(ran.status.code(), StatusCode::kCorruption);
    EXPECT_NE(ran.status.message().find("for a relation whose row size is"), std::string::npos)
        << ran.status.message();
    EXPECT_EQ(ran.ids, (std::vector<std::int64_t>{1, 2, 3, 4, 5}));
}

// A slot whose offset leaves the page is no row, as `ReadTuple` has always
// answered it (BF-R10): the walk skips it and the statement succeeds.
TEST_F(ScanRowWalkTest, ASlotWhoseOffsetLeavesThePageIsNoRow) {
    Load(10);
    auto leaf = btree::BtreeLeftmostLeaf(store_, Root());
    ASSERT_TRUE(leaf.ok());
    {
        auto page = store_.Get(leaf.value());
        ASSERT_TRUE(page.ok());
        std::byte* slot3 = page.value().bytes().data() + heap::kHeapHeaderOffset +
                           heap::kHeaderSize + 3 * heap::kSlotOnDiskSize;
        const std::uint16_t past = kPageSize - 4;
        std::memcpy(slot3 + heap::kSlotOffsetOffset, &past, sizeof(past));
        page.value().MarkDirty();
    }
    Ran ran = RunChain(CompileSql("SELECT t.id FROM t"));
    ASSERT_TRUE(ran.status.ok()) << ran.status.message();
    EXPECT_EQ(ran.ids, (std::vector<std::int64_t>{1, 2, 3, 5, 6, 7, 8, 9, 10}));
}

// The once-per-statement check refuses a `$param` with today's message.
// Unlike a column past the frame, skipping the check reads nothing out of
// bounds: the comparison answers false and the statement would succeed
// empty, so this cell kills a skipped check without relying on a fault.
TEST_F(ScanRowWalkTest, AnUnboundParameterIsRefusedAtTheFirstRow) {
    Load(10);
    StepChain chain = CompileSql("SELECT t.id FROM t WHERE v = 3");
    parser::AstValue param;
    param.type = parser::ValueType::kParam;
    param.str_val = "p";
    chain.steps[0].residual[0].rhs.literal = param;
    Ran ran = RunChain(chain);
    ASSERT_FALSE(ran.status.ok());
    EXPECT_EQ(ran.status.code(), StatusCode::kCorruption);
    EXPECT_NE(ran.status.message().find("parameter '$p' has no bound value"), std::string::npos)
        << ran.status.message();
}

// A true EXISTS stops its sub-chain's walk at the first match, mid-leaf,
// and the outer walk still answers every row the rest of it keeps.
TEST_F(ScanRowWalkTest, ATrueExistsStopsItsWalkMidLeafForEveryOuterRow) {
    Load(kThreeLeaves);
    Ran ran = RunChain(CompileSql(
        "SELECT t.id FROM t WHERE t.id <= 5 AND EXISTS (SELECT t.id FROM t WHERE t.v = 3)"));
    ASSERT_TRUE(ran.status.ok()) << ran.status.message();
    EXPECT_EQ(ran.ids, (std::vector<std::int64_t>{1, 2, 3, 4, 5}));
}

// BM-S3 (BM-R2): a row the scan kernel rejects is charged as the residual's
// rejection is, so the row-touch budget refuses on the same row with the
// kernel on and off - the rows emitted before the refusal say which.
TEST_F(ScanRowWalkTest, TheBudgetRefusesOnTheSameRowWithTheKernelOnAndOff) {
    Load(100);
    const StepChain chain = CompileSql("SELECT t.id FROM t WHERE v >= 20");
    Budget on(/*limit=*/40);
    Budget off(/*limit=*/40);
    off.set_scan_kernel(false);
    Ran with = RunChain(chain, SIZE_MAX, on);
    Ran without = RunChain(chain, SIZE_MAX, off);
    ASSERT_EQ(with.status.code(), StatusCode::kResourceExhausted) << with.status.message();
    EXPECT_EQ(with.status.message(), without.status.message());
    EXPECT_EQ(with.ids, without.ids);
    EXPECT_EQ(with.ids.size(), 21u) << "rows 20..40 match before the 41st row is refused";
    EXPECT_EQ(with.total.rows_examined, without.total.rows_examined);
}

// The control the equivalence cells need: without it, on and off could agree
// because the kernel never ran. Rows 2..19 of `v >= 20` are the kernel's
// (row 1, the step's first visible row, takes the full path that checks the
// step); with the switch off, none are.
TEST_F(ScanRowWalkTest, TheKernelRejectsEveryRowItHoldsAfterTheFirst) {
    Load(100);
    const StepChain chain = CompileSql("SELECT t.id FROM t WHERE v >= 20");
    Budget off;
    off.set_scan_kernel(false);
    Ran with = RunChain(chain);
    Ran without = RunChain(chain, SIZE_MAX, off);
    ASSERT_TRUE(with.status.ok()) << with.status.message();
    EXPECT_EQ(with.ids, without.ids);
    EXPECT_EQ(with.total.rows_kernel_rejected, 18u);
    EXPECT_EQ(without.total.rows_kernel_rejected, 0u);
    EXPECT_EQ(with.total.rows_examined, 100u);
    EXPECT_EQ(with.total.rows_matched, without.total.rows_matched);
}

// ---- EH-S1: a fold handed the visible row's bytes (`bytes_fold.hpp`) -------
//
// The executor's half of the contract, with a consumer of the test's own:
// which rows come to it under the leaf's hold, which stay on the row sink,
// and that a row is counted and charged the same either way. The fold's own
// half - that `Aggregator` answers the same from bytes as from the frame - is
// `bytes_fold_equivalence_test.cpp`'s.
struct SummingFold final : BytesFold {
    bool admit = true;
    std::uint64_t refuse_at = 0;  // the 1-based row to refuse; 0: none
    std::uint64_t prepared = 0;
    std::uint64_t rows = 0;
    std::int64_t sum_v = 0;
    const catalog::RowLayout* layout = nullptr;

    bool PrepareBytesFold(const catalog::Schema& schema, const catalog::RowLayout& l,
                          std::uint16_t rel_slot) override {
        ++prepared;
        EXPECT_EQ(schema.columns.size(), 3u);
        EXPECT_EQ(rel_slot, 0u);
        layout = &l;
        return admit;
    }
    Status AccumulateBytes(std::span<const std::byte> payload) override {
        EXPECT_EQ(payload.size(), layout->row_size);
        ++rows;
        if (rows == refuse_at) return Status::OutOfRange("the fold refused this row");
        std::int64_t v = 0;  // column 1, an int64 cell, where the layout puts it
        std::memcpy(&v, payload.data() + layout->offsets[1], sizeof v);
        sum_v += v;
        return Status::OK();
    }
};

// Every visible row of a step that keeps every row goes to the fold but the
// first, which takes the full path that checks the step (BM-R6) - and the
// rows are examined and matched as the sink's rows are.
TEST_F(ScanRowWalkTest, AStepThatKeepsEveryRowHandsItsFoldEveryRowButTheFirst) {
    Load(kThreeLeaves);
    SummingFold fold;
    Ran ran = RunChain(CompileSql("SELECT t.id FROM t"), SIZE_MAX, Budget(), &fold);
    ASSERT_TRUE(ran.status.ok()) << ran.status.message();
    EXPECT_EQ(fold.prepared, 1u);
    EXPECT_EQ(ran.ids, (std::vector<std::int64_t>{1})) << "the first row, through the sink";
    EXPECT_EQ(fold.rows, kThreeLeaves - 1u);
    EXPECT_EQ(fold.sum_v, std::int64_t{kThreeLeaves} * (kThreeLeaves + 1) / 2 - 1);
    EXPECT_EQ(ran.total.rows_folded_in_place, kThreeLeaves - 1u);
    EXPECT_EQ(ran.total.rows_examined, kThreeLeaves);
    EXPECT_EQ(ran.total.rows_matched, kThreeLeaves);

    // The same statement with no fold: the counters a statement reports do
    // not say which path a row took.
    Ran plain = RunChain(CompileSql("SELECT t.id FROM t"));
    EXPECT_EQ(plain.total.rows_examined, ran.total.rows_examined);
    EXPECT_EQ(plain.total.rows_matched, ran.total.rows_matched);
    EXPECT_EQ(plain.total.rows_folded_in_place, 0u);
}

// A step that can drop a row keeps the full path for every row, and the
// fold is not even asked: a conjunct, and a second step.
TEST_F(ScanRowWalkTest, AStepWithAConjunctOrAnotherStepNeverHandsARowToTheFold) {
    Load(100);
    SummingFold filtered;
    Ran ran = RunChain(CompileSql("SELECT t.id FROM t WHERE v >= 1"), SIZE_MAX, Budget(), &filtered);
    ASSERT_TRUE(ran.status.ok()) << ran.status.message();
    EXPECT_EQ(ran.ids.size(), 100u);
    EXPECT_EQ(filtered.prepared, 0u);
    EXPECT_EQ(filtered.rows, 0u);

    SummingFold joined;
    Ran join = RunChain(CompileSql("SELECT a.id FROM t AS a JOIN t AS b ON a.id = b.id"),
                        SIZE_MAX, Budget(), &joined);
    ASSERT_TRUE(join.status.ok()) << join.status.message();
    EXPECT_EQ(join.ids.size(), 100u);
    EXPECT_EQ(joined.prepared, 0u);
    EXPECT_EQ(joined.rows, 0u);
}

// A hoisted sub-chain leaves the step with no conjunct but makes it decode
// every column (`read_columns` is `kAllColumns`): the full path then reads,
// and may refuse, cells the fold would never read, so the fold is not asked
// (EH-R4).
TEST_F(ScanRowWalkTest, AStepThatDecodesEveryColumnNeverHandsARowToTheFold) {
    Load(100);
    const StepChain chain = CompileSql("SELECT t.id FROM t WHERE EXISTS (SELECT u.id FROM t AS u)");
    ASSERT_FALSE(chain.hoisted.empty());
    ASSERT_TRUE(chain.steps[0].residual.empty());
    ASSERT_EQ(chain.steps[0].read_columns, Step::kAllColumns);
    SummingFold fold;
    Ran ran = RunChain(chain, SIZE_MAX, Budget(), &fold);
    ASSERT_TRUE(ran.status.ok()) << ran.status.message();
    EXPECT_EQ(ran.ids.size(), 100u);
    EXPECT_EQ(fold.prepared, 0u);
    EXPECT_EQ(fold.rows, 0u);
}

// A fold that declines the relation leaves every row on the sink.
TEST_F(ScanRowWalkTest, AFoldThatDeclinesLeavesEveryRowOnTheSink) {
    Load(100);
    SummingFold fold;
    fold.admit = false;
    Ran ran = RunChain(CompileSql("SELECT t.id FROM t"), SIZE_MAX, Budget(), &fold);
    ASSERT_TRUE(ran.status.ok()) << ran.status.message();
    EXPECT_EQ(fold.prepared, 1u);
    EXPECT_EQ(fold.rows, 0u);
    EXPECT_EQ(ran.ids.size(), 100u);
    EXPECT_EQ(ran.total.rows_folded_in_place, 0u);
}

// EH-R4: the row-touch budget refuses on the same row with the fold and
// without, with the same words, and before the fold sees that row.
TEST_F(ScanRowWalkTest, TheBudgetRefusesOnTheSameRowWithTheFoldAndWithout) {
    Load(100);
    const StepChain chain = CompileSql("SELECT t.id FROM t");
    SummingFold fold;
    Ran with = RunChain(chain, SIZE_MAX, Budget(/*limit=*/40), &fold);
    Ran without = RunChain(chain, SIZE_MAX, Budget(/*limit=*/40));
    ASSERT_EQ(with.status.code(), StatusCode::kResourceExhausted) << with.status.message();
    EXPECT_EQ(with.status.message(), without.status.message());
    EXPECT_EQ(with.total.rows_examined, without.total.rows_examined);
    EXPECT_EQ(with.total.rows_matched, without.total.rows_matched);
    EXPECT_EQ(with.ids.size() + fold.rows, without.ids.size());
}

// The fold's own refusal ends the statement on its row, as a sink's does.
TEST_F(ScanRowWalkTest, AFoldsRefusalEndsTheStatementOnItsRow) {
    Load(100);
    SummingFold fold;
    fold.refuse_at = 10;
    Ran ran = RunChain(CompileSql("SELECT t.id FROM t"), SIZE_MAX, Budget(), &fold);
    EXPECT_EQ(ran.status.code(), StatusCode::kOutOfRange) << ran.status.message();
    EXPECT_EQ(fold.rows, 10u);
    EXPECT_EQ(ran.total.rows_examined, 11u) << "the first row and the ten the fold was handed";
}

}  // namespace
}  // namespace kds::exec
