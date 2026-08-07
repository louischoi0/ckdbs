#include <cstdint>
#include <map>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kds/bootstrap/bootstrap.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/varheap.hpp"
#include "kds/storage/visit.hpp"

// The fixed-length rule, end to end (docs/rule-fixed-length-tuple.md
// section 8).
//
// The test that names the feature is TupleAddressesSurviveARandomizedUpdate
// Workload. Everything else in this file supports it: the point of fixing a
// tuple's length was never the length, it was that **an UPDATE can never
// migrate a tuple**, so combined with the immutable min_key a row's address
// is stable for life until relayout moves it on purpose. That is what a
// Waystone trail entry is recorded against, and an engine that quietly
// relocated rows on update would burn those entries through epoch churn.
//
// So the assertion is not "the update succeeded" - it is "every row is at
// the same (page_id, slot) it was before, and nothing moved".

namespace kds::server {
namespace {

class FixedLengthTupleTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto boot = bootstrap::BootstrapDatabase(store_, 1000);
        ASSERT_TRUE(boot.ok()) << boot.status().message();
        boot_.emplace(std::move(boot.value()));
    }

    std::string Run(const std::string& sql) {
        CommandDispatcher d(boot_->superblock, boot_->catalog, store_);
        return d.Dispatch(sql).response;
    }

    const catalog::TableAccess* Access(const std::string& table) {
        auto oid = boot_->catalog.FindTableOidByName(table);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        auto access = boot_->catalog.InitTableAccess(oid.value());
        EXPECT_TRUE(access.ok()) << access.status().message();
        return access.ok() ? access.value() : nullptr;
    }

    // Walks whichever storage the relation uses. A btree leaf *is* a heap
    // page, so the visitor is the same either way - only the walk differs.
    //
    // The visitor is called once per **slot**, not once per page: the third
    // argument is the slot index (heap_chain.cpp), which is why nothing
    // below loops over it.
    Status Walk(const catalog::TableAccess& access,
                const std::function<StatusOr<storage::VisitControl>(
                    PageId, heap::PageView&, std::uint16_t)>& fn) {
        if (access.clustered_type == catalog::ClusteredType::kBtree) {
            return btree::BtreeVisit(store_, access.desc_page_id, storage::PageAccess::kRead, fn);
        }
        return heap::ChainVisit(store_, access.desc_page_id, storage::PageAccess::kRead, fn);
    }

    // Where every live row of `table` physically sits, keyed by pk. Read
    // straight off the pages rather than inferred from a reply, because
    // "did the row move?" is a question only the pages can answer.
    std::map<std::uint64_t, std::pair<PageId, std::uint16_t>> Locations(const std::string& table) {
        std::map<std::uint64_t, std::pair<PageId, std::uint16_t>> out;
        const catalog::TableAccess* access = Access(table);
        if (access == nullptr) return out;

        Status walked = Walk(*access, [&](PageId page_id, heap::PageView& page,
                                          std::uint16_t slot)
                                          -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (tuple.ok() && !tuple.value().deleted) {
                auto id = exec::RowKeystoneId(tuple.value().payload);
                if (id.ok()) out[id.value()] = {page_id, slot};
            }
            return storage::VisitControl::kContinue;
        });
        EXPECT_TRUE(walked.ok()) << walked.message();
        return out;
    }

    // Every live row's payload length, which under invariant 13 must be
    // the relation's constant and nothing else.
    std::vector<std::size_t> PayloadSizes(const std::string& table) {
        std::vector<std::size_t> out;
        const catalog::TableAccess* access = Access(table);
        if (access == nullptr) return out;

        Status walked = Walk(*access, [&](PageId, heap::PageView& page, std::uint16_t slot)
                                          -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (tuple.ok() && !tuple.value().deleted) {
                out.push_back(tuple.value().payload.size());
            }
            return storage::VisitControl::kContinue;
        });
        EXPECT_TRUE(walked.ok()) << walked.message();
        return out;
    }

    std::uint32_t RowSizeOf(const std::string& table) {
        const catalog::TableAccess* access = Access(table);
        return access == nullptr ? 0 : access->layout.row_size;
    }

    storage::InMemoryPageStore store_{kFirstUserPageId};
    std::optional<bootstrap::BootstrapResult> boot_;
};

// ---- The property the feature is named for -------------------------------

TEST_F(FixedLengthTupleTest, TupleAddressesSurviveARandomizedUpdateWorkload) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, s varchar, n int64)").substr(0, 7), "CREATED");

    constexpr int kRows = 60;
    for (int i = 0; i < kRows; ++i) {
        ASSERT_EQ(Run("INSERT INTO t VALUES ('seed', " + std::to_string(i) + ")").substr(0, 8),
                  "INSERTED");
    }

    const auto before = Locations("t");
    ASSERT_EQ(before.size(), static_cast<std::size_t>(kRows));

    // Values that oscillate across the whole inline range, including both
    // ends of it: the empty string and the last length that fits. Before
    // the fixed-length rule this is exactly the workload that grew a row
    // past its slot reservation and failed with OutOfSpace.
    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    std::mt19937 rng(20260801);
    std::uniform_int_distribution<std::uint32_t> length(0, capacity);

    for (int round = 0; round < 4; ++round) {
        for (int i = 1; i <= kRows; ++i) {
            const std::string value(length(rng), 'v');
            const std::string reply =
                Run("UPDATE t SET s = '" + value + "' WHERE id = " + std::to_string(i));
            ASSERT_EQ(reply, "UPDATED 1") << reply << " (round " << round << ", id " << i << ")";
        }
    }

    // Zero moves. Not "few", not "the same page" - the same slot.
    EXPECT_EQ(Locations("t"), before);
}

TEST_F(FixedLengthTupleTest, EveryStoredRowIsTheSchemaConstant) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, s varchar)").substr(0, 7), "CREATED");
    ASSERT_EQ(Run("INSERT INTO t VALUES ('')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("INSERT INTO t VALUES ('short')").substr(0, 8), "INSERTED");

    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    ASSERT_EQ(Run("INSERT INTO t VALUES ('" + std::string(capacity, 'x') + "')").substr(0, 8),
              "INSERTED");

    // Three values of wildly different lengths, three identical rows: the
    // size is a property of the relation, not of what is in the row.
    const std::uint32_t row_size = RowSizeOf("t");
    EXPECT_EQ(row_size, 8u + storage::kDefaultInlineCellWidth);
    for (std::size_t size : PayloadSizes("t")) {
        EXPECT_EQ(size, row_size);
    }
}

// ---- Values round-trip, whatever their length ----------------------------

TEST_F(FixedLengthTupleTest, ValuesRoundTripAcrossTheWholeInlineRange) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, s varchar)").substr(0, 7), "CREATED");

    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    const std::vector<std::string> values = {"", "a", "hello world",
                                             std::string(capacity - 1, 'p'),
                                             std::string(capacity, 'q')};
    for (const std::string& value : values) {
        ASSERT_EQ(Run("INSERT INTO t VALUES ('" + value + "')").substr(0, 8), "INSERTED") << value;
    }

    const std::string selected = Run("SELECT * FROM t");
    for (const std::string& value : values) {
        if (value.empty()) continue;  // an empty cell renders as an empty field
        EXPECT_NE(selected.find(value), std::string::npos) << "missing a value of length "
                                                            << value.size();
    }
}

TEST_F(FixedLengthTupleTest, AShortUpdateOverALongValueLeavesNoStaleTail) {
    // The page-level counterpart of the tagged cell's padding test: a
    // reader must see the new value and nothing of the old one behind it.
    ASSERT_EQ(Run("CREATE TABLE t (id int64, s varchar)").substr(0, 7), "CREATED");

    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    const std::string longv(capacity, 'L');
    ASSERT_EQ(Run("INSERT INTO t VALUES ('" + longv + "')").substr(0, 8), "INSERTED");
    ASSERT_EQ(Run("UPDATE t SET s = 'ab' WHERE id = 1"), "UPDATED 1");

    const std::string selected = Run("SELECT * FROM t");
    EXPECT_NE(selected.find("ab"), std::string::npos) << selected;
    EXPECT_EQ(selected.find('L'), std::string::npos) << selected;
}

// ---- Crossing the spill boundary -----------------------------------------

TEST_F(FixedLengthTupleTest, TupleAddressesSurviveUpdatesAcrossTheSpillBoundary) {
    // The phase-1 property re-run over the boundary the var-heap adds. A
    // value oscillating between inline and spilled changes the cell's
    // *tag*, never the tuple's size - so the row still cannot move, and a
    // Waystone trail entry recorded against it still cannot be burned.
    ASSERT_EQ(Run("CREATE TABLE t (id int64, s varchar)").substr(0, 7), "CREATED");

    constexpr int kRows = 30;
    for (int i = 0; i < kRows; ++i) {
        ASSERT_EQ(Run("INSERT INTO t VALUES ('seed')").substr(0, 8), "INSERTED");
    }

    const auto before = Locations("t");
    ASSERT_EQ(before.size(), static_cast<std::size_t>(kRows));

    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    std::mt19937 rng(20260802);
    // Deliberately straddling: half the draws fit inline, half must spill.
    std::uniform_int_distribution<std::uint32_t> length(0, capacity * 2);

    for (int round = 0; round < 4; ++round) {
        for (int i = 1; i <= kRows; ++i) {
            const std::string value(length(rng), 'v');
            const std::string reply =
                Run("UPDATE t SET s = '" + value + "' WHERE id = " + std::to_string(i));
            ASSERT_EQ(reply, "UPDATED 1") << reply << " (round " << round << ", id " << i << ")";
        }
    }

    EXPECT_EQ(Locations("t"), before);
    for (std::size_t size : PayloadSizes("t")) {
        EXPECT_EQ(size, RowSizeOf("t"));
    }
}

TEST_F(FixedLengthTupleTest, AValueSpilledThenShortenedReadsBackAsTheShortOne) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, s varchar)").substr(0, 7), "CREATED");

    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    const std::string spilled(capacity * 3, 'L');
    ASSERT_EQ(Run("INSERT INTO t VALUES ('" + spilled + "')").substr(0, 8), "INSERTED");
    EXPECT_NE(Run("SELECT * FROM t").find(spilled), std::string::npos);

    // Back under the boundary: the cell's tag flips to inline and the old
    // var-heap value is simply abandoned (reclamation rides on purge, which
    // does not exist yet - varheap.hpp).
    ASSERT_EQ(Run("UPDATE t SET s = 'tiny' WHERE id = 1"), "UPDATED 1");
    const std::string selected = Run("SELECT * FROM t");
    EXPECT_NE(selected.find("tiny"), std::string::npos) << selected;
    EXPECT_EQ(selected.find('L'), std::string::npos) << selected;
}

TEST_F(FixedLengthTupleTest, ARelationWithNothingSpillableGetsNoVarHeapChain) {
    // One page per relation that *could* spill is the price of an
    // immutable, cacheable root; a relation of plain integers must not pay
    // it (catalog.cpp's CreateTable).
    ASSERT_EQ(Run("CREATE TABLE nums (id int64, a int64, b int32)").substr(0, 7), "CREATED");
    EXPECT_EQ(Access("nums")->varheap_page_id, kInvalidPageId);

    ASSERT_EQ(Run("CREATE TABLE strs (id int64, s varchar)").substr(0, 7), "CREATED");
    EXPECT_NE(Access("strs")->varheap_page_id, kInvalidPageId);
}

TEST_F(FixedLengthTupleTest, ManySpilledValuesGrowTheVarHeapChain) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, s varchar)").substr(0, 7), "CREATED");

    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    const std::string big(capacity * 10, 'b');
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(Run("INSERT INTO t VALUES ('" + big + "')").substr(0, 8), "INSERTED");
    }

    auto length = varheap::ChainLength(store_, Access("t")->varheap_page_id);
    ASSERT_TRUE(length.ok()) << length.status().message();
    EXPECT_GT(length.value(), 1u) << "the var-heap never grew; the test proves nothing";

    // Every row still reads back, which is what makes the growth boring.
    const std::string selected = Run("SELECT * FROM t");
    EXPECT_NE(selected.find(big), std::string::npos);
}

// ---- Refusals a client can see -------------------------------------------

TEST_F(FixedLengthTupleTest, AValueTooLongToInlineSpillsAndStillRoundTrips) {
    ASSERT_EQ(Run("CREATE TABLE t (id int64, s varchar)").substr(0, 7), "CREATED");

    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    const std::string spilled(capacity * 4, 'x');
    ASSERT_EQ(Run("INSERT INTO t VALUES ('" + spilled + "')").substr(0, 8), "INSERTED");

    // Storage is invisible above this layer: a spilled value reads back the
    // same as an inline one would (spec section 6).
    EXPECT_NE(Run("SELECT * FROM t").find(spilled), std::string::npos);

    // And the tuple is still the relation's constant - the cell holds a
    // pointer, not the text.
    for (std::size_t size : PayloadSizes("t")) {
        EXPECT_EQ(size, RowSizeOf("t"));
    }
}

TEST_F(FixedLengthTupleTest, AFloatColumnIsRefusedAtCreateTable) {
    // Refused at definition time rather than at the first INSERT, and now
    // on the merits rather than for want of a width: IEEE semantics
    // conflict with this engine's exactness discipline (spec-types.md TY1).
    const std::string reply = Run("CREATE TABLE bad_float (id int64, x float)");
    EXPECT_EQ(reply.substr(0, 3), "ERR") << reply;
    EXPECT_NE(reply.find("float"), std::string::npos) << reply;
}

TEST_F(FixedLengthTupleTest, ABareDecimalColumnIsRefusedUntilItCanCarryScale) {
    // `decimal` has a width now (TY2's scaled int64), so the refusal moved
    // from RowLayout to the column-type check - and its *reason* moved with
    // it. A bare `decimal` says nothing about scale, and a default scale is
    // a silent decision about someone's money (spec-types.md §2).
    const std::string reply = Run("CREATE TABLE bad_dec (id int64, x decimal)");
    EXPECT_EQ(reply.substr(0, 3), "ERR") << reply;
    EXPECT_NE(reply.find("precision"), std::string::npos) << reply;
}

// ---- The same, on a clustered B+ tree ------------------------------------

TEST_F(FixedLengthTupleTest, ABtreeRelationHoldsTheSameConstantAndMovesNoRows) {
    // A btree leaf *is* a heap page (btree.hpp), so the rule has to hold
    // there too - and it holds for the same reason, not by a second
    // mechanism.
    ASSERT_EQ(Run("CREATE TABLE bt (id int64, s varchar) BTREE").substr(0, 7), "CREATED");
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(Run("INSERT INTO bt VALUES ('seed')").substr(0, 8), "INSERTED");
    }

    const std::uint32_t capacity = storage::InlineCapacity(storage::kDefaultInlineCellWidth);
    for (int i = 1; i <= 20; ++i) {
        const std::string value(static_cast<std::size_t>(i) * capacity / 20, 'w');
        ASSERT_EQ(Run("UPDATE bt SET s = '" + value + "' WHERE id = " + std::to_string(i)),
                  "UPDATED 1");
    }

    const auto before = Locations("bt");
    ASSERT_EQ(before.size(), 20u);
    for (std::size_t size : PayloadSizes("bt")) {
        EXPECT_EQ(size, RowSizeOf("bt"));
    }
    EXPECT_EQ(Locations("bt"), before);
}

}  // namespace
}  // namespace kds::server
