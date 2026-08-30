#include "kds/stats/pattern_defs.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/catalog_spills.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/extent_lease.hpp"
#include "kds/storage/in_memory_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/exec/varheap_sweep.hpp"
#include "kds/storage/varheap.hpp"

// sys.pattern_defs, the one catalog relation stored in ordinary user tuple
// format. What is worth pinning here is not that a row round-trips - the row
// codec's own tests cover that - but the three things that are true of this
// relation and of no other catalog one: it exists at bootstrap with a
// var-heap, a body longer than the inline cell survives the spill, and a
// deleted definition is *gone* rather than delete-marked.

namespace kds::stats {
namespace {

class PatternDefsTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(catalog_.Bootstrap().ok()); }

    // 128 = kds::server::kFirstUserPageId, so pages allocated by CreateNew()
    // (this relation's var-heap chain among them) never collide with the
    // fixed catalog pages.
    storage::InMemoryPageStore store_{128};
    catalog::Catalog catalog_{store_};
};

TEST_F(PatternDefsTest, BootstrapCreatesTheRelationWithAVarHeap) {
    auto access = catalog_.InitTableAccess(catalog::kSysPatternDefsTable);
    ASSERT_TRUE(access.ok()) << access.status().message();

    // Five columns: the Keystone pk the spec's list omits (invariant 11),
    // then pattern_id, the materialized arity, and the two text columns.
    ASSERT_EQ(access.value()->schema.columns.size(), 5u);
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[0].name), "id");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[1].name), "pattern_id");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[2].name), "param_count");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[3].name), "name");
    EXPECT_EQ(catalog::NameView(access.value()->schema.columns[4].name), "source_text");

    // It can spill, so it has a chain. A relation of plain integers would
    // get kInvalidPageId and no page at all.
    EXPECT_NE(access.value()->varheap_page_id, kInvalidPageId);
    EXPECT_EQ(access.value()->desc_page_id, catalog::kCatalogPagePatternDefs);
}

TEST_F(PatternDefsTest, ADefinitionRoundTripsByNameAndByPatternId) {
    // The **whole declaration**, not just the body: it is the canon a
    // fingerprint version bump re-registers from, and the declared types and
    // WITH options are recoverable from nothing else.
    const std::string decl =
        "CREATE PATTERN acct($flag bool) WITH (pinned = on) "
        "OF SELECT id FROM account AS a WHERE a.flag = $flag";
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 0xDEADBEEFCAFEF00Dull, "acct", decl, 1).ok());

    auto by_name = FindPatternDefByName(catalog_, store_, "acct");
    ASSERT_TRUE(by_name.ok());
    ASSERT_TRUE(by_name.value().has_value());
    EXPECT_EQ(by_name.value()->pattern_id, 0xDEADBEEFCAFEF00Dull)
        << "a uint64 pattern_id must survive the upper half of the range";
    // Verbatim, sigils included: a normalized copy would re-register the
    // pattern under an id that no longer matches the traffic it was written
    // for.
    EXPECT_EQ(by_name.value()->source_text, decl);
    // The materialized arity, stored rather than rederived so it cannot come
    // to disagree with what an older build hashed.
    EXPECT_EQ(by_name.value()->param_count, 1u);

    auto by_id = FindPatternDefByPatternId(catalog_, store_, 0xDEADBEEFCAFEF00Dull);
    ASSERT_TRUE(by_id.ok());
    ASSERT_TRUE(by_id.value().has_value());
    EXPECT_EQ(by_id.value()->name, "acct");
}

TEST_F(PatternDefsTest, NameLookupIsCaseInsensitive) {
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 1, "AcctTrades", "SELECT * FROM t", 0).ok());

    auto found = FindPatternDefByName(catalog_, store_, "accttrades");
    ASSERT_TRUE(found.ok());
    EXPECT_TRUE(found.value().has_value())
        << "every other identifier in this engine folds; a DROP that missed "
           "because of case would strand the declaration";

    auto missing = FindPatternDefByName(catalog_, store_, "other");
    ASSERT_TRUE(missing.ok());
    EXPECT_FALSE(missing.value().has_value()) << "an absence is not an error";
}

TEST_F(PatternDefsTest, ABodyLongerThanAnInlineCellSpillsAndComesBackWhole) {
    // Well past kds.inline_cell_width, so both text columns take the
    // var-heap path and the decode has to resolve two pending spills - the
    // ordering I15's R1 forces.
    const std::string body(4000, 'x');
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 7, "big", body, 0).ok());

    auto found = FindPatternDefByPatternId(catalog_, store_, 7);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    EXPECT_EQ(found.value()->source_text.size(), body.size());
    EXPECT_EQ(found.value()->source_text, body);
}

TEST_F(PatternDefsTest, ABodyLargerThanOneVarHeapPageIsRefusedRatherThanChained) {
    // The spilled-value size cap is an open decision and this does not
    // settle it: one var-heap page is what fits without a multi-page
    // representation, and anything larger is Unsupported.
    const std::string too_long(varheap::kMaxValueSize + 1, 'x');
    Status s = InsertPatternDef(catalog_, store_, nullptr, 9, "huge", too_long, 0);
    EXPECT_EQ(s.code(), StatusCode::kUnsupported);
    EXPECT_NE(s.message().find(std::to_string(varheap::kMaxValueSize)), std::string::npos)
        << "the message has to name the limit; the client wrote the body";
}

TEST_F(PatternDefsTest, DeleteRemovesTheRowRatherThanMarkingIt) {
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 11, "gone", "SELECT * FROM t", 0).ok());
    ASSERT_TRUE(DeletePatternDef(catalog_, store_, nullptr, 11).ok());

    auto found = FindPatternDefByName(catalog_, store_, "gone");
    ASSERT_TRUE(found.ok());
    EXPECT_FALSE(found.value().has_value());

    // The point of retiring rather than delete-marking: catalog reads have
    // no snapshot to filter a mark against, so re-declaring the same name
    // has to succeed.
    EXPECT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 12, "gone", "SELECT * FROM u", 0).ok());
    auto again = FindPatternDefByName(catalog_, store_, "gone");
    ASSERT_TRUE(again.ok());
    ASSERT_TRUE(again.value().has_value());
    EXPECT_EQ(again.value()->pattern_id, 12u);

    EXPECT_EQ(DeletePatternDef(catalog_, store_, nullptr, 999).code(), StatusCode::kNotFound);
}

TEST_F(PatternDefsTest, ListReturnsEveryDefinitionInChainOrder) {
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 1, "one", "SELECT * FROM a", 0).ok());
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 2, "two", "SELECT * FROM b", 0).ok());

    auto all = ListPatternDefs(catalog_, store_);
    ASSERT_TRUE(all.ok());
    ASSERT_EQ(all.value().size(), 2u);
    EXPECT_EQ(all.value()[0].name, "one");
    EXPECT_EQ(all.value()[1].name, "two");
    // Keystone ids come from the relation's own persistent sequence, so
    // they are distinct and increasing.
    EXPECT_LT(all.value()[0].id, all.value()[1].id);
}

// ---- H7: the sweep that collects a spill nothing points at ---------------
//
// `exec::LogChainInsert` logs its spills under `kNoTxnId`, so a spill made
// on this path has no transaction to chain an undo record to and no
// compensation. Every *other* spill has been released by the ordinary
// rollback path since 2026-08-28, which is what makes this the remaining
// hole rather than the general case - and `DeletePatternDef` retires the
// row outright, so the value it pointed at is orphaned the moment the
// definition goes.
TEST_F(PatternDefsTest, TheMountSweepCollectsASpillNoRowPointsAt) {
    const std::string body(4000, 'x');
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 21, "doomed", body, 0).ok());

    auto access = catalog_.InitTableAccess(catalog::kSysPatternDefsTable);
    ASSERT_TRUE(access.ok()) << access.status().message();
    const PageId root = access.value()->varheap_page_id;
    ASSERT_NE(root, kInvalidPageId);
    {
        auto page = store_.GetForRead(root);
        ASSERT_TRUE(page.ok());
        ASSERT_GT(varheap::PageLiveSlots(page.value().bytes()), 0u)
            << "the body did not spill, so there is nothing for the sweep to be about";
    }

    // A sweep now must collect **nothing**: the row is live and points at
    // its value. This is the assertion that keeps the sweep from being a
    // collector of things in use.
    auto before = exec::SweepUnownedSpills(catalog_, store_, /*wal=*/nullptr);
    ASSERT_TRUE(before.ok()) << before.status().message();
    EXPECT_GT(before.value().retained, 0u) << "a live spill was not retained";
    const std::uint64_t live_slots_after_first_sweep = [&] {
        auto page = store_.GetForRead(root);
        EXPECT_TRUE(page.ok());
        return static_cast<std::uint64_t>(varheap::PageLiveSlots(page.value().bytes()));
    }();
    EXPECT_GT(live_slots_after_first_sweep, 0u)
        << "the sweep collected a value the relation still points at";

    // Now orphan it: the row is retired outright, so nothing references the
    // value and nothing released it.
    ASSERT_TRUE(DeletePatternDef(catalog_, store_, nullptr, 21).ok());
    {
        auto page = store_.GetForRead(root);
        ASSERT_TRUE(page.ok());
        EXPECT_GT(varheap::PageLiveSlots(page.value().bytes()), 0u)
            << "something already released the spill, and this row has no leak to close";
    }

    auto after = exec::SweepUnownedSpills(catalog_, store_, /*wal=*/nullptr);
    ASSERT_TRUE(after.ok()) << after.status().message();
    EXPECT_GT(after.value().released, 0u) << "the orphaned spill was not collected";
    {
        auto page = store_.GetForRead(root);
        ASSERT_TRUE(page.ok());
        EXPECT_EQ(varheap::PageLiveSlots(page.value().bytes()), 0u)
            << "the orphaned value is still live after the sweep";
    }

    // Idempotent, which is what makes it safe to run at every mount and to
    // replay after a crash mid-sweep: a second pass changes nothing about
    // what is live.
    auto again = exec::SweepUnownedSpills(catalog_, store_, /*wal=*/nullptr);
    ASSERT_TRUE(again.ok()) << again.status().message();
    auto page = store_.GetForRead(root);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(varheap::PageLiveSlots(page.value().bytes()), 0u);
}

TEST_F(PatternDefsTest, TheMountSweepKeepsEveryValueALiveRowPointsAt) {
    // The direction the sweep must never err in. Three definitions, all
    // live, all spilled - a sweep that collected any of them would turn a
    // leak into a wrong answer, and the body comes back whole afterwards.
    const std::string a(3000, 'a');
    const std::string b(3000, 'b');
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 31, "keep_a", a, 0).ok());
    ASSERT_TRUE(InsertPatternDef(catalog_, store_, nullptr, 32, "keep_b", b, 0).ok());

    auto swept = exec::SweepUnownedSpills(catalog_, store_, /*wal=*/nullptr);
    ASSERT_TRUE(swept.ok()) << swept.status().message();
    EXPECT_EQ(swept.value().released, 0u) << "a live spill was collected";

    auto found = FindPatternDefByPatternId(catalog_, store_, 31);
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value());
    EXPECT_EQ(found.value()->source_text, a) << "the sweep damaged a value it should have kept";
    auto second = FindPatternDefByPatternId(catalog_, store_, 32);
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(second.value().has_value());
    EXPECT_EQ(second.value()->source_text, b);
}

// ---- CR1/CB0: what a peer hits when a definition spilled ----------------
//
// A catalog relation's *root* page is reserved so that bootstrap can find
// it without a catalog read; its **var-heap is not** - it comes from the
// general supply through `CreateNew()` and is recorded in `sys.tables`
// (`crosscore.md` CC12/CR1). So a peer, whose store refuses pages at or
// above `system_page_limit_` unless a lease, a grant or a stamp says
// otherwise, can read every `sys.pattern_defs` *row* and not the bodies
// those rows point at.
//
// `sys.assertions` has the same shape and a peer reader, so its mount
// grants itself the pages its rows name. `sys.pattern_defs` has the shape
// and no peer reader, which is why this is the relation CR1 gets exercised
// on first.
class PatternDefsPeerReadTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto made = storage::MemoryPageDevice::Create(/*extent_pages=*/64, /*initial_pages=*/0);
        ASSERT_TRUE(made.ok()) << made.status().message();
        device_ = std::move(made.value());

        // Core 0: no lease, so it reaches everything and writes the
        // catalog. 128 = kds::server::kFirstUserPageId, the reserved
        // range's end.
        auto opened = storage::DevicePageStore::Open(*device_, 128);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        core0_ = std::move(opened.value());
        core0_catalog_.emplace(*core0_);
        ASSERT_TRUE(core0_catalog_->Bootstrap().ok());

        // Long enough to spill: `kDefaultInlineCellWidth` is far below this,
        // so `source_text` cannot sit in the row and the body goes to the
        // var-heap page the relation's `varheap_page_id` roots.
        ASSERT_TRUE(InsertPatternDef(*core0_catalog_, *core0_, /*wal=*/nullptr, 77, "wide",
                                     body_, 1)
                        .ok());
        // The peer reads the device, never core 0's frames.
        ASSERT_TRUE(core0_->Sync().ok());
    }

    // A second store over the same device, arranged as a peer's: a lease of
    // its own ids, and the reserved range as the system limit.
    void OpenPeer() {
        auto opened = storage::DevicePageStore::Open(*device_, 128);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        peer_ = std::move(opened.value());
        peer_->SetCoreOwnership(/*core_id=*/1, &peer_lease_, /*system_page_limit=*/128);
        peer_catalog_.emplace(*peer_, storage::kDefaultInlineCellWidth, /*core_count=*/2,
                              /*core_id=*/1);
    }

    const std::string body_ = std::string(3000, 'p');
    std::unique_ptr<storage::MemoryPageDevice> device_;
    std::unique_ptr<storage::DevicePageStore> core0_;
    std::optional<catalog::Catalog> core0_catalog_;
    storage::LeasedIdSource peer_lease_{storage::Extent{4096, 8}};
    std::unique_ptr<storage::DevicePageStore> peer_;
    std::optional<catalog::Catalog> peer_catalog_;
};

TEST_F(PatternDefsPeerReadTest, WithoutAGrantTheRowsReadAndTheBodyDoesNot) {
    OpenPeer();

    // The rows themselves are reachable: they live on page 10, inside the
    // reserved range every core reads (CC11).
    auto access = peer_catalog_->InitTableAccess(catalog::kSysPatternDefsTable);
    ASSERT_TRUE(access.ok()) << access.status().message();
    ASSERT_NE(access.value()->varheap_page_id, kInvalidPageId);
    EXPECT_LT(access.value()->desc_page_id, 128u) << "the root page is reserved (CR1)";
    EXPECT_GE(access.value()->varheap_page_id, 128u)
        << "the var-heap root is not reserved (CR1) - that is the whole subject";

    auto defs = ListPatternDefs(*peer_catalog_, *peer_);
#ifndef NDEBUG
    // The refusal, read rather than assumed: `DevicePageStore` checks
    // `MayFault` on the fault path and answers `InvalidArgument`, naming the
    // page.
    ASSERT_FALSE(defs.ok()) << "a peer resolved a spill it was granted nothing for";
    EXPECT_EQ(defs.status().code(), StatusCode::kInvalidArgument) << defs.status().message();
    EXPECT_EQ(defs.status().message(),
              "DevicePageStore: core 1 may not fault page " +
                  std::to_string(access.value()->varheap_page_id) +
                  "; it belongs to another core")
        << "the refusal a peer meets, recorded verbatim (CB0)";
#else
    // **And the divergence, stated rather than left to be discovered.** The
    // `MayFault` gate on the *read* path is `#ifndef NDEBUG`
    // (`device_page_store.cpp`, the guideline-1 check); only the write half
    // is enforced in every build. So in a release build the peer faults core
    // 0's page and answers correctly, and the shared-nothing violation is
    // invisible - which is what makes this a latent defect rather than a
    // visible one, and why the grant is built rather than argued about.
    ASSERT_TRUE(defs.ok()) << defs.status().message();
#endif
}

TEST_F(PatternDefsPeerReadTest, TheGrantTheMountTakesMakesTheBodyReadable) {
    OpenPeer();

    // What `CoreRuntime::Open` does for a peer, in the one line it does it:
    // read the ids the rows name - without fetching any of them, which is
    // the property that lets this run before the rights exist - and grant
    // exactly those pages, one at a time.
    auto pages = exec::CatalogSpillPages(*peer_catalog_, *peer_, exec::kVarHeapCatalogRelations);
    ASSERT_TRUE(pages.ok()) << pages.status().message();
    ASSERT_FALSE(pages.value().empty()) << "the definition spilled, so a page must be named";
    for (const PageId page : pages.value()) {
        EXPECT_GE(page, 128u) << "a reserved-range page needs no grant";
        peer_->GrantFaultPages(storage::Extent{page, 1});
    }

    // And now the whole body, byte for byte, on the core that did not write
    // it. A truncated or empty answer here is the spill resolution silently
    // not happening, which is the failure this relation's own round-trip
    // test guards on core 0.
    auto def = FindPatternDefByName(*peer_catalog_, *peer_, "wide");
    ASSERT_TRUE(def.ok()) << def.status().message();
    ASSERT_TRUE(def.value().has_value());
    EXPECT_EQ(def.value()->source_text, body_);
    EXPECT_EQ(def.value()->pattern_id, 77u);
}

TEST_F(PatternDefsPeerReadTest, TheGrantIsExactlyThePagesTheRowsNameAndNoExtent) {
    OpenPeer();

    auto pages = exec::CatalogSpillPages(*peer_catalog_, *peer_, exec::kVarHeapCatalogRelations);
    ASSERT_TRUE(pages.ok()) << pages.status().message();
    for (const PageId page : pages.value()) peer_->GrantFaultPages(storage::Extent{page, 1});

    // The extent around a named page stays unreachable. This is the half
    // that is not a convenience: a page answering `MayFault` from a grant
    // never reaches `TryClaimByStamp`, so an extent grant would cost a
    // restarted owner the write rights its stamp restores
    // (`catalog_spills.hpp`; the measured failure was
    // `APeersOwnPagesSurviveARestartByTheirStamp`).
    const PageId neighbour = pages.value().back() + 1;
    EXPECT_FALSE(peer_->MayFault(neighbour))
        << "the grant widened to the extent around the pages the rows name";

    // A grant conveys read rights and nothing else, which is what makes it
    // sound for a page core 0 owns: a var-heap value is immutable per
    // version (invariant 14), so reading one needs no coherence protocol,
    // and writing one is refused in every build.
    EXPECT_FALSE(peer_->MayWrite(pages.value().front()));
}

}  // namespace
}  // namespace kds::stats
