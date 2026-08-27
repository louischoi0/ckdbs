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

TEST(RangeEligibleTest, AnIndexedRelationDeclines) {
    const AssertionEnforcer enforcer;
    catalog::TableAccess access = BareHeapRelation();
    catalog::TableAccess::IndexRef index{};
    index.index_oid = 7001;
    access.indexes.push_back(index);
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kIndex);
}

TEST(RangeEligibleTest, ALiveCabinIdDeclinesEvenWhereTheMaskWouldNot) {
    // The wrong-test pin (command_dispatcher.cpp's comment, kept here as a
    // contract): the gate reads live ids, not `cabin_mask` - a Cabin on a
    // column past 64 folds into no mask bit, so the mask stays 0 while the
    // id is live, and the mask test would admit the split.
    const AssertionEnforcer enforcer;
    catalog::TableAccess access = BareHeapRelation();
    access.cabin_ids[1].id = 41;
    ASSERT_EQ(access.cabin_mask, 0u);
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kCabin);
}

TEST(RangeEligibleTest, ZeroIdCabinEntriesDoNotDecline) {
    // The other wrong test: `cabin_ids` is column-parallel and non-empty
    // on every relation the cache fills, so emptiness would gate every
    // relation in the instance. All-zero ids are "no Cabin anywhere".
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
    // fixed order, so the decline's one reason is deterministic.
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
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kBtree);

    access.clustered_type = catalog::ClusteredType::kHeap;
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kIndex);

    access.indexes.clear();
    EXPECT_EQ(RangeEligible(access, enforcer), RangeGate::kAssertion);
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
