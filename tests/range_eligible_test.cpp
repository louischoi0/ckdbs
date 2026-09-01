#include "kds/exec/range_eligible.hpp"

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string_view>

#include "kds/catalog/well_known.hpp"

// RD4's contract (docs/spec/crosscore.md §6a;
// docs/inflight/in-progress/workplan-range-directory.md §9): the six
// checks — D1's btree decline plus §6a's five gates — each tripped alone,
// the eligible shape, the wrong-test pins the cabin gate carries, and the
// fixed precedence. **This file is deliberately RangeEligible's only
// caller** (H2, range-foundation.md §4): the allocator that will ask is
// RD5's, and nothing on a statement path may call it before then.

namespace kds::exec {
namespace {

catalog::SysColumnRow Column(std::uint32_t pos, std::uint32_t type_val) {
    catalog::SysColumnRow col{};
    col.pos = pos;
    col.type_val = type_val;
    return col;
}

// The one shape §6a leaves splittable: heap-clustered, non-spilling,
// unindexed, un-cabined, FK-free — and, since C2, un-asserted.
catalog::TableAccess BareHeapRelation(catalog::Oid oid = 900) {
    catalog::TableAccess access{};
    access.oid = oid;
    access.namespace_oid = catalog::kNamespacePublic;
    access.clustered_type = catalog::ClusteredType::kHeap;
    access.desc_page_id = kInvalidPageId;
    access.schema.columns.push_back(Column(0, catalog::kTypeValInt64));
    access.schema.columns.push_back(Column(1, catalog::kTypeValInt32));
    // Column-parallel with every id 0, as CatalogCache fills it for a
    // relation with no Cabins - present but empty-of-ids, which is
    // exactly the case the emptiness test would misread.
    access.cabin_ids.resize(access.schema.columns.size());
    return access;
}

TEST(RangeEligibleTest, TheBareHeapShapeIsEligible) {
    const AssertionEnforcer enforcer;
    EXPECT_EQ(RangeEligible(BareHeapRelation(), enforcer), RangeGate::kNone);
    EXPECT_EQ(RangeGateName(RangeGate::kNone), "eligible");
}

TEST(RangeEligibleTest, BtreeDeclinesByD1) {
    const AssertionEnforcer enforcer;
    catalog::TableAccess access = BareHeapRelation();
    access.clustered_type = catalog::ClusteredType::kBtree;
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kBtree);
}

TEST(RangeEligibleTest, AnIndexedRelationNoLongerDeclines) {
    // SB3 dropped the arm as dead code: a secondary index is btree-only
    // (IX3), so `kBtree` declined every relation that could trip it
    // first, and this shape - heap-clustered *and* indexed - is one only
    // this fixture can build. The expectation flips back when **D1**
    // lifts, not at IX11; the enum value is kept for exactly that.
    const AssertionEnforcer enforcer;
    catalog::TableAccess access = BareHeapRelation();
    catalog::TableAccess::IndexRef index{};
    index.index_oid = 7001;
    access.indexes.push_back(index);
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kNone);
}

TEST(RangeEligibleTest, ALiveCabinIdNoLongerDeclines) {
    // SB3, and it was answered rather than deferred: an Observational
    // set is authoritative for (observed value x the ranges its core
    // owns), so a boundary narrows what a set speaks for instead of
    // falsifying it (`docs/spec/cabin.md` §4b). The wrong-test pin the
    // old assertion carried - live ids, never `cabin_mask` - survives in
    // `AnyCabin`'s own comment, which is where the rule lives; the field
    // is set here so a re-added arm reading the mask still fails.
    const AssertionEnforcer enforcer;
    catalog::TableAccess access = BareHeapRelation();
    access.cabin_ids[1].id = 41;
    ASSERT_EQ(access.cabin_mask, 0u);
    ASSERT_TRUE(access.AnyCabin());
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kNone);
}

TEST(RangeEligibleTest, ZeroIdCabinEntriesDoNotDecline) {
    // `cabin_ids` is column-parallel and non-empty on every relation the
    // cache fills. Kept after SB3 dropped the Cabin arm because it pins
    // the *shape* rather than the arm: all-zero ids are "no Cabin
    // anywhere", and a future gate reading emptiness would gate every
    // relation in the instance.
    const AssertionEnforcer enforcer;
    catalog::TableAccess access = BareHeapRelation();
    ASSERT_FALSE(access.cabin_ids.empty());
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kNone);
}

TEST(RangeEligibleTest, ASpillingSchemaDeclines) {
    const AssertionEnforcer enforcer;
    catalog::TableAccess access = BareHeapRelation();
    access.schema.columns.push_back(Column(2, catalog::kTypeValVarchar));
    access.cabin_ids.resize(access.schema.columns.size());
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kSpill);
}

TEST(RangeEligibleTest, AnFkChildDeclines) {
    const AssertionEnforcer enforcer;
    catalog::TableAccess access = BareHeapRelation();
    access.fkeys_out.push_back(catalog::ForeignKeyRef{.fk_id = 1, .rel_oid = 901});
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kForeignKey);
}

TEST(RangeEligibleTest, AnFkParentDeclines) {
    const AssertionEnforcer enforcer;
    catalog::TableAccess access = BareHeapRelation();
    access.fkeys_in.push_back(catalog::ForeignKeyRef{.fk_id = 2, .rel_oid = 902});
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kForeignKey);
}

TEST(RangeEligibleTest, ALiveAssertionDeclines) {
    AssertionEnforcer enforcer;
    LiveAssertion assertion;
    assertion.assertion_id = 501;
    assertion.target_oid = 900;
    enforcer.Adopt(std::move(assertion));
    EXPECT_EQ(RangeEligible(BareHeapRelation(900), enforcer), RangeGate::kAssertion);
}

TEST(RangeEligibleTest, AKnownUnenforceableAssertionDeclines) {
    // The pre-PW1c-6c shape: this core knows of the assertion and may not
    // write its cabin. It refuses the relation's writes; it must also
    // refuse its split.
    AssertionEnforcer enforcer;
    enforcer.NoteUnenforceable(900, 502);
    EXPECT_EQ(RangeEligible(BareHeapRelation(900), enforcer), RangeGate::kAssertion);
}

TEST(RangeEligibleTest, AnAssertionOnAnotherRelationDoesNotDecline) {
    AssertionEnforcer enforcer;
    LiveAssertion assertion;
    assertion.assertion_id = 503;
    assertion.target_oid = 901;
    enforcer.Adopt(std::move(assertion));
    EXPECT_EQ(RangeEligible(BareHeapRelation(900), enforcer), RangeGate::kNone);
}

TEST(RangeEligibleTest, PrecedenceIsTheDocumentedOrder) {
    // A relation tripping several gates names the first in the header's
    // fixed order, so the decline's one reason is deterministic. All of
    // them tripped at once, then cleared one per step: the walk pins the
    // whole order, not just its head — a reordering anywhere breaks a step.
    //
    // **The index and Cabin facts stay set for every step** after SB3
    // dropped their arms, which is the point: the walk now proves they
    // are *transparent*, so a re-added arm at IX11 fails this test
    // instead of silently changing which gate a relation names.
    AssertionEnforcer enforcer;
    LiveAssertion assertion;
    assertion.assertion_id = 504;
    assertion.target_oid = 900;
    enforcer.Adopt(std::move(assertion));

    catalog::TableAccess access = BareHeapRelation(900);
    access.clustered_type = catalog::ClusteredType::kBtree;
    catalog::TableAccess::IndexRef index{};
    index.index_oid = 7002;
    access.indexes.push_back(index);
    access.schema.columns.push_back(Column(2, catalog::kTypeValVarchar));
    // After the grow, so the id survives it - `resize` appends and leaves
    // the existing entries alone, which is why one assignment is enough.
    access.cabin_ids.resize(access.schema.columns.size());
    access.cabin_ids[1].id = 43;
    access.fkeys_out.push_back(catalog::ForeignKeyRef{.fk_id = 3, .rel_oid = 903});

    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kBtree);

    access.clustered_type = catalog::ClusteredType::kHeap;
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kSpill);

    access.schema.columns.pop_back();
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kForeignKey);

    access.fkeys_out.clear();
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kAssertion);

    // And with the assertion gone, an indexed *and* cabined relation is
    // eligible — the two facts SB3 made transparent, asserted as such
    // rather than left as an absence.
    enforcer.Evict(504);
    ASSERT_FALSE(access.indexes.empty());
    ASSERT_TRUE(access.AnyCabin());
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kNone);
}

TEST(RangeEligibleTest, ACatalogShapedRelationAnswersEligible) {
    // The §9b scope hole, pinned executable (workplan-range-directory.md
    // §9b): none of §6a's five facts is true of `sys.tables`, so every
    // gate passes it — yet a catalog relation is categorically
    // unsplittable, and §6a lists no such gate, so the function
    // deliberately does not invent one. Whichever of §6a or RD5 takes
    // the scope explicitly is the change that flips this expectation.
    const AssertionEnforcer enforcer;
    catalog::TableAccess access{};
    access.oid = catalog::kSysTablesTable;
    access.namespace_oid = catalog::kNamespaceSys;
    access.clustered_type = catalog::ClusteredType::kHeap;
    access.desc_page_id = kInvalidPageId;
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kNone);
}

TEST(RangeEligibleTest, GateNamesAreDistinctNonEmptyTokens) {
    const std::array<RangeGate, 7> gates = {
        RangeGate::kNone,       RangeGate::kBtree,      RangeGate::kIndex,
        RangeGate::kCabin,      RangeGate::kSpill,      RangeGate::kForeignKey,
        RangeGate::kAssertion,
    };
    std::set<std::string_view> names;
    for (const RangeGate gate : gates) {
        const std::string_view name = RangeGateName(gate);
        EXPECT_FALSE(name.empty());
        // A log token, so greppable: no spaces.
        EXPECT_EQ(name.find(' '), std::string_view::npos) << name;
        names.insert(name);
    }
    EXPECT_EQ(names.size(), gates.size());
}

}  // namespace
}  // namespace kds::exec
