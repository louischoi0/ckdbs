#include "kds/server/range_alloc.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/stats/cabin_store.hpp"

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
    // A **device** store, not the in-memory one, and it is the flush that
    // forces it: `OpenRangeOnSystemCore` writes the head page out before
    // the handoff record because every core has its own store over one
    // shared device, and a fixture that cannot tell a written page from an
    // unwritten one would pass with that flush deleted.
    std::unique_ptr<storage::MemoryPageDevice> device_ =
        std::move(storage::MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0)
                      .value());
    std::unique_ptr<storage::DevicePageStore> store_holder_ =
        std::move(storage::DevicePageStore::Open(*device_, /*first_new_page_id=*/128).value());
    storage::DevicePageStore& store_ = *store_holder_;
    catalog::Catalog catalog_{store_};
    exec::AssertionEnforcer enforcer_;

    void SetUp() override { ASSERT_TRUE(catalog_.Bootstrap().ok()); }

    // Two columns, and the second is load-bearing: `CreateCabin` refuses
    // column 0 for its own reason (the pk's cabin is the clustered tree),
    // so a one-column relation cannot reach the range gate at that door at
    // all - which is what made the Cabin third of the converse test assert
    // nothing.
    Schema PkSchema() {
        Schema schema;
        SysColumnRow col{};
        col.pos = 0;
        catalog::SetName(col.name, "id");
        col.type_val = catalog::kTypeValInt64;
        col.len = 8;
        col.notnull = true;
        schema.columns.push_back(col);
        SysColumnRow val{};
        val.pos = 1;
        catalog::SetName(val.name, "v");
        val.type_val = catalog::kTypeValInt64;
        val.len = 8;
        val.notnull = true;
        schema.columns.push_back(val);
        return schema;
    }

    catalog::Oid MakeHeap(const char* name) {
        auto oid = catalog_.CreateTable(kNamespacePublic, name, PkSchema(), ClusteredType::kHeap);
        EXPECT_TRUE(oid.ok()) << oid.status().message();
        return oid.ok() ? oid.value() : 0;
    }

    // `OpenRangeOnSystemCore` with this fixture's pieces. No WAL: an
    // unlogged store answers kNoLsn throughout, which is what makes the
    // handoff record and its durability wait no-ops here. The Cabin store
    // and the discard counter are SB1's, passed on every call so the
    // pre-grant discard is exercised by every test in this file rather
    // than by the one that names it.
    StatusOr<PageId> Open(catalog::Oid oid, std::uint64_t lo, std::uint32_t owner) {
        return OpenRangeOnSystemCore(catalog_, store_, /*wal=*/nullptr, enforcer_, oid, lo, owner,
                                     /*log=*/nullptr, &cabins_, &discards_);
    }

    // An observed set for `cabin_id` on one value, which is what a probe
    // would have banked. Committed through the store's own door so the
    // per-cabin accounting is the accounting a real recording produces.
    void ObserveOneValue(std::uint64_t cabin_id, std::int64_t value, std::uint64_t pk) {
        parser::AstValue v{};
        v.type = parser::ValueType::kInt;
        v.int_val = value;
        auto key = stats::MakeCabinKey(cabin_id, v);
        ASSERT_TRUE(key.has_value());
        stats::CabinEntry entry;
        entry.pk = pk;
        ASSERT_TRUE(cabins_.Commit(*key, {entry}));
        ASSERT_NE(cabins_.Find(*key), nullptr);
    }

    stats::CabinStore cabins_;
    CabinSplitDiscardCounters discards_;
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

TEST_F(RangeAllocTest, TheHeadPageIsOnTheDeviceBeforeTheGrantLeaves) {
    const catalog::Oid oid = MakeHeap("granted");
    auto entry = Open(oid, 4096, /*owner=*/1);
    ASSERT_TRUE(entry.ok()) << entry.status().message();
    ASSERT_NE(entry.value(), kInvalidPageId);

    // CC7's flush-then-grant, from the receiving side. Core 0 formats the
    // head in **its own** frame; the owner has its own store over this one
    // device and faults the page from there. A second store over the same
    // device is that owner's view, and without the flush it reads the id
    // back as "allocated but was never written" - a granted head no core
    // can write.
    auto owner_view = storage::DevicePageStore::Open(*device_, /*first_new_page_id=*/128);
    ASSERT_TRUE(owner_view.ok()) << owner_view.status().message();
    EXPECT_TRUE(owner_view.value()->IsAllocated(entry.value()))
        << "the free map reached the device without the page it describes";
    auto faulted = owner_view.value()->GetForRead(entry.value());
    ASSERT_TRUE(faulted.ok()) << faulted.status().message();
    heap::PageView view(faulted.value().bytes());
    EXPECT_EQ(view.min_key(), 4096u);
}

TEST_F(RangeAllocTest, ASecondRangeJoinsTheDirectoryWithoutASecondOpeningRow) {
    const catalog::Oid oid = MakeHeap("twice");
    // Two **different** owners since R4/IS5: a second block for the core
    // that already owns the top range opens no boundary at all (the test
    // below), so this - the shape that still opens one - is the one the
    // opening row's once-only rule has to be checked against.
    ASSERT_TRUE(Open(oid, 4096, 1).ok());
    ASSERT_TRUE(Open(oid, 8192, 2).ok());

    auto ranges = catalog_.RangesOf(oid);
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    ASSERT_EQ(ranges.value().size(), 3u) << "the opening row was written twice";
    EXPECT_EQ(ranges.value()[0].lo, 0u);
    EXPECT_EQ(ranges.value()[1].lo, 4096u);
    EXPECT_EQ(ranges.value()[2].lo, 8192u);
}

// ---- R4/IS5 ------------------------------------------------------------
//
// The block lands in the top range either way - ids only ascend and that
// range runs to the end of the id space - so when the asking core already
// owns it, a boundary would cut that core's own chain in two and spend a
// fan-in stage for a partition with one owner on both sides.
TEST_F(RangeAllocTest, ASecondBlockForTheCoreOwningTheTopRangeOpensNoBoundary) {
    const catalog::Oid oid = MakeHeap("continues");
    ASSERT_TRUE(Open(oid, 4096, 1).ok());

    auto again = Open(oid, 8192, 1);
    ASSERT_TRUE(again.ok()) << again.status().message();
    EXPECT_EQ(again.value(), kInvalidPageId) << "a continuing block opened a range";

    auto ranges = catalog_.RangesOf(oid);
    ASSERT_TRUE(ranges.ok()) << ranges.status().message();
    EXPECT_EQ(ranges.value().size(), 2u)
        << "the same core's second block opened a second boundary";

    // And a **different** core's block still opens one, so the suppression
    // is about continuity and not about "one range per relation".
    auto other = Open(oid, 12288, 2);
    ASSERT_TRUE(other.ok()) << other.status().message();
    EXPECT_NE(other.value(), kInvalidPageId);
    EXPECT_EQ(catalog_.RangesOf(oid).value().size(), 3u);
}

// The relation's own owner asking for its first block: the empty directory
// says one range owned by `sys.tables.owner_core` (CC9), so this core
// already owns the top range and there is nothing to partition.
TEST_F(RangeAllocTest, TheRelationsOwnerAskingForItsOwnFirstBlockOpensNoBoundary) {
    const catalog::Oid oid = MakeHeap("selfowned");
    auto row = catalog_.GetSysTableRow(oid);
    ASSERT_TRUE(row.ok()) << row.status().message();

    auto opened = Open(oid, 4096, row.value().owner_core);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    EXPECT_EQ(opened.value(), kInvalidPageId);
    EXPECT_TRUE(catalog_.RangesOf(oid).value().empty())
        << "a relation gained a directory describing a partition with one owner";
}

TEST_F(RangeAllocTest, ARangeAtLoZeroIsRefusedByBothHalves) {
    const catalog::Oid oid = MakeHeap("zero");
    // `lo = 0` is the relation as it already is - the opening row - so
    // asking for it as a *split* would describe the whole space twice.
    // Refused at both halves, because either one reached alone would leave
    // the other's work orphaned: a head page with no boundary, or a
    // boundary with no head.
    auto page = catalog_.CreateRangeEntryPage(oid, 0);
    ASSERT_FALSE(page.ok());
    EXPECT_EQ(page.status().code(), StatusCode::kInvalidArgument);

    Status rows = catalog_.OpenRangeRows(oid, 0, 1, /*entry_page=*/500);
    EXPECT_EQ(rows.code(), StatusCode::kInvalidArgument) << rows.message();
}

TEST_F(RangeAllocTest, ABoundaryRowNeedsAnEntryPage) {
    const catalog::Oid oid = MakeHeap("headless");
    // CC8 makes a range its own sub-structure, so a row naming no entry
    // page would describe a partition with nowhere to put a row - and RD6
    // would read `kInvalidPageId` as the insert head.
    Status refused = catalog_.OpenRangeRows(oid, 4096, 1, kInvalidPageId);
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument) << refused.message();
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

TEST_F(RangeAllocTest, ASplitRelationTakesNoIndexOrForeignKeyButTakesACabin) {
    const catalog::Oid oid = MakeHeap("split");
    ASSERT_TRUE(Open(oid, 4096, 1).ok());

    catalog::Catalog::IndexDef def{};
    def.table_oid = oid;
    def.name = "ix_split";
    def.key_cols = {1};
    Status ix = catalog_.CreateIndex(def).status();
    EXPECT_EQ(ix.code(), StatusCode::kNotImplemented) << ix.message();
    EXPECT_NE(ix.message().find("split across"), std::string::npos) << ix.message();

    // **A Cabin is admitted since SB3**, and column 1 rather than 0
    // because the pk column is refused for its own reason before the
    // range question is reached. It is born correctly scoped rather than
    // born incomplete: an Observational set speaks for the ranges its
    // core owns (`docs/spec/cabin.md` §4b). The optimizer's auto path
    // comes through this same door and is admitted on the same terms.
    auto cabin = catalog_.CreateCabin(oid, 1, catalog::kCabinOriginUser);
    EXPECT_TRUE(cabin.ok()) << cabin.status().message();

    const catalog::Oid child = MakeHeap("child");
    Status fk = catalog_.CreateForeignKey(child, 0, oid, 0).status();
    EXPECT_EQ(fk.code(), StatusCode::kNotImplemented) << fk.message();
    EXPECT_NE(fk.message().find("split across"), std::string::npos) << fk.message();
}

TEST_F(RangeAllocTest, ASplitRelationTakesNoAssertion) {
    const catalog::Oid oid = MakeHeap("split_assert");
    ASSERT_TRUE(Open(oid, 4096, 1).ok());

    // Through `InsertAssertion` - the row's own door, the one the **peer**
    // path reaches after its park - rather than through the helper
    // directly: `PrepareAssertionDef`'s copy of this gate runs before the
    // build, so a range opened while that build was running meets only this
    // one. Asserting on the helper would pass with both doors unwired.
    Status refused = exec::InsertAssertion(catalog_, store_, /*wal=*/nullptr, /*id=*/11, oid,
                                           "a_split", "CREATE ASSERTION a_split ...",
                                           kInvalidPageId);
    EXPECT_EQ(refused.code(), StatusCode::kNotImplemented) << refused.message();
    EXPECT_NE(refused.message().find("split across"), std::string::npos) << refused.message();

    // And the row really is absent, not merely reported so.
    auto targets = exec::ListAssertionTargets(catalog_, store_);
    ASSERT_TRUE(targets.ok()) << targets.status().message();
    for (const exec::AssertionDef& def : targets.value()) {
        EXPECT_NE(def.target_oid, oid) << "a split relation took an assertion row anyway";
    }
}

TEST_F(RangeAllocTest, AnUnsplitRelationIsRefusedNothing) {
    const catalog::Oid oid = MakeHeap("plain");
    auto access = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(access.ok());
    // The gate's *other* answer, pinned because a refusal that fired on
    // every relation would pass every test above and break the engine.
    EXPECT_TRUE(catalog::RefuseAuxiliaryOnSplitRelation(*access.value(), "an index").ok());
}

// ---- RD8 / §8 test 13: the split gates, each by name ----------------
//
// "The allocator's admission check declines an indexed, cabined,
// spilling, or FK-linked relation, **names the gate**, and the relation
// stays one range." The naming half is `RangeGateName`, which is what the
// decline's log line and `SHOW META`'s `range_split_decline_detail` both
// print; the staying-one-range half is that `sys.ranges` is untouched.
//
// **Two of §8's four cannot be reached through the allocator at all, and
// saying so is part of the test.** An index is btree-only (IX3) and D1
// declines every btree relation first, so `kIndex` is a gate for the day
// D1 lifts - `ABtreeRelationIsDeclinedAndNothingIsWritten` above covers
// what actually fires. The other three are reachable on a heap relation
// and are here.

TEST_F(RangeAllocTest, ACabinedRelationSplitsAndItsSetsAreDiscardedFirst) {
    // SB1 + SB3, and the ordering is the assertion: the sets are gone by
    // the time the call that grants the range returns, because the grant
    // is the caller's reply to this call and a peer may write from it
    // onward (`crosscore.md` CC10's pre-grant window).
    //
    // `PkSchema` already carries the second column a Cabin needs -
    // `CreateCabin` refuses column 0 for its own reason.
    auto oid = catalog_.CreateTable(kNamespacePublic, "cabined", PkSchema(),
                                    ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto cabin = catalog_.CreateCabin(oid.value(), 1, catalog::kCabinOriginUser);
    ASSERT_TRUE(cabin.ok()) << cabin.status().message();
    const std::uint64_t cabin_id = cabin.value();

    ObserveOneValue(cabin_id, /*value=*/7, /*pk=*/1);
    ObserveOneValue(cabin_id, /*value=*/8, /*pk=*/2);
    ASSERT_EQ(cabins_.observed_value_count(), 2u);

    auto access = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    ASSERT_TRUE(access.value()->AnyCabin());
    EXPECT_EQ(exec::RangeEligible(*access.value(), enforcer_), exec::RangeGate::kNone);

    auto entry = Open(oid.value(), 4096, 1);
    ASSERT_TRUE(entry.ok()) << entry.status().message();
    EXPECT_NE(entry.value(), kInvalidPageId) << "a cabined relation was still gated";

    auto ranges = catalog_.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok());
    EXPECT_EQ(ranges.value().size(), 2u) << "a cabined relation did not split";

    // The discard, counted in value sets and keyed by the relation.
    EXPECT_EQ(cabins_.observed_value_count(), 0u) << "sets survived the grant";
    EXPECT_EQ(cabins_.InfoFor(cabin_id).values, 0u);
    EXPECT_EQ(cabins_.InfoFor(cabin_id).entries, 0u);
    EXPECT_EQ(discards_.CountFor(oid.value()), 2u);
    EXPECT_EQ(discards_.total(), 2u);
}

TEST_F(RangeAllocTest, ADeclinedSplitDiscardsNothing) {
    // The discard sits **after** the gates for a reason: a relation the
    // re-check declines is not splitting, so charging it re-observation
    // would price a boundary that never opened. The spilling gate is the
    // convenient one to prove it with - it survives SB3 untouched.
    Schema schema = PkSchema();
    catalog::SetName(schema.columns[1].name, "note");
    schema.columns[1].type_val = catalog::kTypeValVarchar;
    schema.columns[1].len = 0;
    auto oid = catalog_.CreateTable(kNamespacePublic, "gated_cabined", schema,
                                    ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();
    auto cabin = catalog_.CreateCabin(oid.value(), 1, catalog::kCabinOriginUser);
    ASSERT_TRUE(cabin.ok()) << cabin.status().message();
    ObserveOneValue(cabin.value(), /*value=*/7, /*pk=*/1);

    EXPECT_EQ(Open(oid.value(), 4096, 1).value(), kInvalidPageId);
    EXPECT_EQ(cabins_.observed_value_count(), 1u) << "a declined split discarded anyway";
    EXPECT_EQ(discards_.total(), 0u);
}

TEST_F(RangeAllocTest, AMigrationGrantDiscardsToo) {
    // SB4's second cell, as far as this level can carry it: a grant to a
    // core that owns nothing of this relation is what a migration's step 4
    // is, and the discard is keyed on the grant rather than on the word
    // "split". The mover (R5) does not exist, so what is asserted here is
    // the rule and not the mover: **every** boundary this function opens
    // drops the sets first, so the day a move grants through here it
    // inherits the discard rather than needing a second one.
    const catalog::Oid oid = MakeHeap("moved");
    auto cabin = catalog_.CreateCabin(oid, 1, catalog::kCabinOriginUser);
    ASSERT_TRUE(cabin.ok()) << cabin.status().message();
    ObserveOneValue(cabin.value(), /*value=*/9, /*pk=*/3);

    ASSERT_TRUE(Open(oid, 4096, /*owner=*/1).ok());
    EXPECT_EQ(cabins_.observed_value_count(), 0u);

    // And a second grant on the now-split relation discards **again**,
    // because the discard is per boundary and not per relation: whatever
    // was re-observed between the two grants was banked under the claim
    // the second grant is about to falsify. 1 + 1, so the counter
    // accumulates rather than latching.
    ObserveOneValue(cabin.value(), /*value=*/9, /*pk=*/3);
    ASSERT_TRUE(Open(oid, 8192, /*owner=*/2).ok());
    EXPECT_EQ(discards_.CountFor(oid), 2u);
}

TEST_F(RangeAllocTest, ASpillingRelationDeclinesNamingItsGate) {
    Schema schema = PkSchema();
    catalog::SetName(schema.columns[1].name, "note");
    schema.columns[1].type_val = catalog::kTypeValVarchar;
    schema.columns[1].len = 0;  // the instance width; a bare varchar can spill
    auto oid = catalog_.CreateTable(kNamespacePublic, "spilling", schema, ClusteredType::kHeap);
    ASSERT_TRUE(oid.ok()) << oid.status().message();

    auto access = catalog_.InitTableAccess(oid.value());
    ASSERT_TRUE(access.ok());
    EXPECT_EQ(exec::RangeEligible(*access.value(), enforcer_), exec::RangeGate::kSpill);
    EXPECT_EQ(exec::RangeGateName(exec::RangeGate::kSpill), "spilling-schema");

    EXPECT_EQ(Open(oid.value(), 4096, 1).value(), kInvalidPageId);
    auto ranges = catalog_.RangesOf(oid.value());
    ASSERT_TRUE(ranges.ok());
    EXPECT_TRUE(ranges.value().empty()) << "a spilling relation was split";
}

TEST_F(RangeAllocTest, AnFkChildDeclinesNamingItsGate) {
    // **The parent must be BTREE**, which the catalog refuses to do
    // without: a heap parent has no pk index, so every FK check would scan
    // it (`CreateForeignKey`'s own refusal). That has a consequence for
    // §8 test 13 worth stating rather than discovering: the FK gate's
    // *parent* half is unreachable through the allocator, because D1
    // declines every btree relation before `RangeEligible` looks at
    // `fkeys_in` - the same shape `kIndex` is in. What is reachable, and
    // is here, is the **child**: a heap relation carrying `fkeys_out`.
    Schema parent_schema = PkSchema();
    auto parent = catalog_.CreateTable(kNamespacePublic, "fk_parent", parent_schema,
                                       ClusteredType::kBtree);
    ASSERT_TRUE(parent.ok()) << parent.status().message();

    Schema child_schema = PkSchema();
    catalog::SetName(child_schema.columns[1].name, "parent_id");
    auto child = catalog_.CreateTable(kNamespacePublic, "fk_child", child_schema,
                                      ClusteredType::kHeap);
    ASSERT_TRUE(child.ok()) << child.status().message();
    auto fk = catalog_.CreateForeignKey(child.value(), 1, parent.value(), 0);
    ASSERT_TRUE(fk.ok()) << fk.status().message();

    auto access = catalog_.InitTableAccess(child.value());
    ASSERT_TRUE(access.ok());
    EXPECT_EQ(exec::RangeEligible(*access.value(), enforcer_), exec::RangeGate::kForeignKey);
    EXPECT_EQ(exec::RangeGateName(exec::RangeGate::kForeignKey), "fk-linked");

    EXPECT_EQ(Open(child.value(), 4096, 1).value(), kInvalidPageId);
    auto ranges = catalog_.RangesOf(child.value());
    ASSERT_TRUE(ranges.ok());
    EXPECT_TRUE(ranges.value().empty()) << "an FK-linked relation was split";

    // And the parent declines too, on D1 rather than on the FK gate -
    // pinned so the ordering is deliberate rather than incidental.
    auto parent_access = catalog_.InitTableAccess(parent.value());
    ASSERT_TRUE(parent_access.ok());
    EXPECT_EQ(exec::RangeEligible(*parent_access.value(), enforcer_), exec::RangeGate::kBtree);
}

TEST_F(RangeAllocTest, EveryGateHasADistinctNameForTheCounterToKeyOn) {
    // `range_split_decline_detail` prints `oid:gate=count`, so two gates
    // sharing a token would merge two decisions into one number - and the
    // reading the counter exists for is *which* gate declines how often.
    // Neither may carry ':' or ',' either, or the detail string stops
    // parsing.
    const exec::RangeGate gates[] = {
        exec::RangeGate::kNone,   exec::RangeGate::kBtree, exec::RangeGate::kIndex,
        exec::RangeGate::kCabin,  exec::RangeGate::kSpill, exec::RangeGate::kForeignKey,
        exec::RangeGate::kAssertion};
    std::set<std::string_view> seen;
    for (exec::RangeGate gate : gates) {
        const std::string_view name = exec::RangeGateName(gate);
        EXPECT_FALSE(name.empty());
        EXPECT_EQ(name.find(':'), std::string_view::npos) << name;
        EXPECT_EQ(name.find(','), std::string_view::npos) << name;
        EXPECT_TRUE(seen.insert(name).second) << "two gates share the token " << name;
    }
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

TEST(RangeSplitDeclineCountersTest, AGateThatComesBackIsNotLoggedTwice) {
    RangeSplitDeclineCounters counters;
    ASSERT_TRUE(counters.Record(4000, exec::RangeGate::kIndex));
    ASSERT_TRUE(counters.Record(4000, exec::RangeGate::kAssertion));
    // Back to the first gate - a dropped assertion leaves the index gate
    // declining again - and this is deliberately **not** a second line.
    // The bound is what the rule is for: first-seen caps the lines at
    // relations x gates for the process's life, where "the gate changed"
    // is unbounded under an index created and dropped in a loop. The
    // count still moves, which is where that volume is read.
    EXPECT_FALSE(counters.Record(4000, exec::RangeGate::kIndex));
    EXPECT_EQ(counters.CountFor(4000, exec::RangeGate::kIndex), 2u);
}

}  // namespace
}  // namespace kds::server
