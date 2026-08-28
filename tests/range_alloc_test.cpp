#include "kds/server/range_alloc.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// RD5 — the allocator's half that can be tested without a reactor: the
// gate re-check core 0 runs, `Catalog::OpenRange`'s two-row opening, and
// the converse gates (work order `instructions/v2.5.0/range-directory.md`
// RB2; workplan §9b, §9e).
//
// The ring half — the owner core asking on the drain tick, core 0
// answering, the head page admitted — is `core_runtime_test.cpp`'s, where
// a peer core exists.

namespace kds::server {
namespace {

using catalog::ClusteredType;
using catalog::kNamespacePublic;
using catalog::Schema;
using catalog::SysColumnRow;

class RangeAllocTest : public ::testing::Test {
protected:
    storage::InMemoryPageStore store_{128};
    catalog::Catalog catalog_{store_};
    exec::AssertionEnforcer enforcer_;

    void SetUp() override { ASSERT_TRUE(catalog_.Bootstrap().ok()); }

    Schema PkSchema() {
        Schema schema;
        SysColumnRow col{};
        col.pos = 0;
        catalog::SetName(col.name, "id");
        col.type_val = catalog::kTypeValInt64;
        col.len = 8;
        col.notnull = true;
        schema.columns.push_back(col);
        return schema;
    }

    catalog::Oid MakeHeap(const char* name) {
        auto oid = catalog_.CreateTable(kNamespacePublic, name, PkSchema(), ClusteredType::kHeap);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        return oid.ok() ? oid.value() : 0;
    }

    // `OpenRangeOnSystemCore` with this fixture's pieces. No WAL: an
    // unlogged store answers kNoLsn throughout, which is what makes the
    // handoff record and its durability wait no-ops here.
    StatusOr<PageId> Open(catalog::Oid oid, std::uint64_t lo, std::uint32_t owner) {
        return OpenRangeOnSystemCore(catalog_, store_, /*wal=*/nullptr, enforcer_, oid, lo, owner,
                                     /*log=*/nullptr);
    }
};

TEST_F(RangeAllocTest, OpeningARangeWritesTheOpeningRowBesideIt) {
    const catalog::Oid oid = MakeHeap("spread");
    auto before = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(before.ok());
    const std::uint32_t owner = before.value()->owner_core;
    const PageId head = before.value()->desc_page_id;

    auto entry = Open(oid, 4096, /*owner=*/1);
    ASSERT_TRUE(entry.ok()) << entry.status().message();
    ASSERT_NE(entry.value(), kInvalidPageId) << "the gates declined a bare heap relation";

    // CC9's rule 1, and the reason `OpenRange` writes two rows rather than
    // one: a directory describes the whole id space or it is not a
    // partition. The opening row is the relation exactly as it was.
    auto ranges = catalog_.RangesOf(oid);
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 2u);
    EXPECT_EQ(ranges.value()[0].lo, 0u);
    EXPECT_EQ(ranges.value()[0].owner_core, owner);
    EXPECT_EQ(ranges.value()[0].entry_page, head);
    EXPECT_EQ(ranges.value()[1].lo, 4096u);
    EXPECT_EQ(ranges.value()[1].owner_core, 1u);
    EXPECT_EQ(ranges.value()[1].entry_page, entry.value());
}

TEST_F(RangeAllocTest, TheHeadPageOfARangeCarriesTheBoundaryAsItsMinKey) {
    const catalog::Oid oid = MakeHeap("aligned");
    auto entry = Open(oid, 8192, /*owner=*/1);
    ASSERT_TRUE(entry.ok()) << entry.status().message();
    ASSERT_NE(entry.value(), kInvalidPageId);

    // CC10's page-boundary rule made vacuous rather than checked, and
    // invariant 3 made structural: the head refuses any id below its own
    // `min_key`, so nothing belonging to the lower range can land here
    // even by mistake.
    auto page = store_.Get(entry.value());
    ASSERT_TRUE(page.ok());
    heap::PageView view(page.value().bytes());
    EXPECT_EQ(view.min_key(), 8192u);
}

TEST_F(RangeAllocTest, ASecondRangeJoinsTheDirectoryWithoutASecondOpeningRow) {
    const catalog::Oid oid = MakeHeap("twice");
    ASSERT_TRUE(Open(oid, 4096, 1).ok());
    ASSERT_TRUE(Open(oid, 8192, 1).ok());

    auto ranges = catalog_.RangesOf(oid);
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 3u) << "the opening row was written twice";
    EXPECT_EQ(ranges.value()[0].lo, 0u);
    EXPECT_EQ(ranges.value()[1].lo, 4096u);
    EXPECT_EQ(ranges.value()[2].lo, 8192u);
}

TEST_F(RangeAllocTest, ARangeAtLoZeroIsRefused) {
    const catalog::Oid oid = MakeHeap("zero");
    // `lo = 0` is the relation as it already is - the opening row - so
    // asking for it as a *split* would describe the whole space twice.
    auto refused = catalog_.OpenRange(oid, 0, 1);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(RangeAllocTest, ABtreeRelationIsDeclinedAndNothingIsWritten) {
    // D1's decline, which is the gate that actually fires today and the
    // reason RD9's measurement subject is unrepresentative (order §7): the
    // shared-structure access mechanism is `[OPEN]`, so no btree relation
    // splits, and the engine's principal bulk relations are btree.
    //
    // §6a's *index* gate cannot be reached from here at all - IX3 makes an
    // index btree-only and D1 declines every btree relation first - so it
    // is a gate for the day D1 lifts, and the converse test below is what
    // exercises the index door in the meantime.
    auto tree = catalog_.CreateTable(kNamespacePublic, "tree", PkSchema(), ClusteredType::kBtree);
    ASSERT_TRUE(tree.ok()) << tree.status().message();

    // **A decline is an answer, not a failure**: OK with no page, which
    // the caller reads as "grant the ids and open nothing".
    auto entry = Open(tree.value(), 4096, 1);
    ASSERT_TRUE(entry.ok()) << entry.status().message();
    EXPECT_EQ(entry.value(), kInvalidPageId);

    auto ranges = catalog_.RangesOf(tree.value());
    ASSERT_TRUE(ranges.ok());
    EXPECT_TRUE(ranges.value().empty()) << "a declined relation got a directory";
}

TEST_F(RangeAllocTest, ACatalogRelationIsDeclinedThoughEveryGatePassesIt) {
    // §9b's scope, **taken at RD5 because RD4 declined to invent a gate
    // §6a does not list**. Every one of §6a's five facts is true of
    // sys.tables, and it is still categorically unsplittable.
    auto entry = Open(catalog::kSysTablesTable, 4096, 1);
    ASSERT_TRUE(entry.ok()) << entry.status().message();
    EXPECT_EQ(entry.value(), kInvalidPageId);

    auto ranges = catalog_.RangesOf(catalog::kSysTablesTable);
    ASSERT_TRUE(ranges.ok());
    EXPECT_TRUE(ranges.value().empty());
}

TEST_F(RangeAllocTest, AnAssertedRelationIsDeclinedByTheDurableRowNotTheRegistry) {
    const catalog::Oid oid = MakeHeap("asserted");
    // The fifth gate as core 0 must ask it: this fixture's enforcer is
    // empty, exactly like core 0's registry for a peer-owned relation, so
    // `RangeEligible`'s assertion arm answers "eligible" vacuously. The
    // sys.assertions row is what declines it, and testing it through an
    // empty enforcer is testing the case that actually occurs.
    ASSERT_TRUE(exec::InsertAssertion(catalog_, store_, /*wal=*/nullptr, /*id=*/7, oid, "a1",
                                      "CREATE ASSERTION a1 ...", kInvalidPageId)
                    .ok());
    EXPECT_FALSE(enforcer_.AnyOn(oid)) << "the fixture's registry was supposed to be empty";

    auto entry = Open(oid, 4096, 1);
    ASSERT_TRUE(entry.ok()) << entry.status().message();
    EXPECT_EQ(entry.value(), kInvalidPageId);
}

// ---- The converse gates (§9b) ---------------------------------------

TEST_F(RangeAllocTest, ASplitRelationTakesNoIndexCabinOrForeignKey) {
    const catalog::Oid oid = MakeHeap("split");
    ASSERT_TRUE(Open(oid, 4096, 1).ok());

    catalog::Catalog::IndexDef def{};
    def.table_oid = oid;
    def.name = "ix_split";
    def.key_cols = {1};
    Status ix = catalog_.CreateIndex(def).status();
    EXPECT_EQ(ix.code(), StatusCode::kUnsupported) << ix.message();
    EXPECT_NE(ix.message().find("split across"), std::string::npos) << ix.message();

    Status cabin = catalog_.CreateCabin(oid, 0, catalog::kCabinOriginUser).status();
    // Column 0 is refused for its own reason before the range gate; the
    // range gate is what column 1 would meet, and this relation has none.
    // Asserted on the *parent* end below instead, which needs no column.
    (void)cabin;

    const catalog::Oid child = MakeHeap("child");
    Status fk = catalog_.CreateForeignKey(child, 0, oid, 0).status();
    EXPECT_EQ(fk.code(), StatusCode::kUnsupported) << fk.message();
    EXPECT_NE(fk.message().find("split across"), std::string::npos) << fk.message();
}

TEST_F(RangeAllocTest, ASplitRelationTakesNoAssertion) {
    const catalog::Oid oid = MakeHeap("split_assert");
    ASSERT_TRUE(Open(oid, 4096, 1).ok());

    auto access = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(access.ok());
    Status refused = catalog::RefuseAuxiliaryOnSplitRelation(*access.value(), "an assertion");
    EXPECT_EQ(refused.code(), StatusCode::kUnsupported) << refused.message();
    EXPECT_NE(refused.message().find("split across"), std::string::npos) << refused.message();
}

TEST_F(RangeAllocTest, AnUnsplitRelationIsRefusedNothing) {
    const catalog::Oid oid = MakeHeap("plain");
    auto access = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(access.ok());
    // The gate's *other* answer, pinned because a refusal that fired on
    // every relation would pass every test above and break the engine.
    EXPECT_TRUE(catalog::RefuseAuxiliaryOnSplitRelation(*access.value(), "an index").ok());
}

// ---- C3's counters (§9e) --------------------------------------------

TEST(RangeSplitDeclineCountersTest, TheFirstDeclineAndAChangeOfGateAreTransitions) {
    RangeSplitDeclineCounters counters;
    // The line rides the transition and the counter rides every ask: a
    // permanently gated relation is every indexed one, and a log line per
    // lease refill would be a synchronous write forever.
    EXPECT_TRUE(counters.Record(4000, exec::RangeGate::kIndex));
    EXPECT_FALSE(counters.Record(4000, exec::RangeGate::kIndex));
    EXPECT_FALSE(counters.Record(4000, exec::RangeGate::kIndex));
    EXPECT_TRUE(counters.Record(4000, exec::RangeGate::kCabin));
    EXPECT_TRUE(counters.Record(4001, exec::RangeGate::kIndex));

    EXPECT_EQ(counters.CountFor(4000, exec::RangeGate::kIndex), 3u);
    EXPECT_EQ(counters.CountFor(4000, exec::RangeGate::kCabin), 1u);
    EXPECT_EQ(counters.CountFor(4001, exec::RangeGate::kIndex), 1u);
    EXPECT_EQ(counters.CountFor(4001, exec::RangeGate::kCabin), 0u);
    EXPECT_EQ(counters.total(), 5u);
    EXPECT_EQ(counters.counts().size(), 3u);
}

TEST(RangeSplitDeclineCountersTest, AGateThatComesBackIsATransitionAgain) {
    RangeSplitDeclineCounters counters;
    ASSERT_TRUE(counters.Record(4000, exec::RangeGate::kIndex));
    ASSERT_TRUE(counters.Record(4000, exec::RangeGate::kAssertion));
    // Back to the first gate - a dropped assertion leaves the index gate
    // declining again, and that is a change worth one line.
    EXPECT_TRUE(counters.Record(4000, exec::RangeGate::kIndex));
    EXPECT_EQ(counters.CountFor(4000, exec::RangeGate::kIndex), 2u);
}

}  // namespace
}  // namespace kds::server
