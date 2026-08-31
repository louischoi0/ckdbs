#include "kds/server/range_coalesce.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/server/range_alloc.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/memory_page_device.hpp"
#include "kds/wal/manager.hpp"
#include "kds/wal/memory_log_device.hpp"

// AX — the halves of a coalesce that need no reactor: the census and the
// absorber choice (AX1), the concatenation and its idempotence (AX2), the
// directory contraction (AX3), and the walk bound that makes a
// part-merged relation read exactly as the split one did
// (`docs/spec/crosscore.md` §6c; build order
// `instructions/v2.7.0/ax-coalesce-on-auxiliary-ddl.md`).
//
// The ring halves - the quiesce leg on a departing owner, the absorb leg
// on the absorber - are `core_runtime_test.cpp`'s, where a peer core
// exists.
//
// **One store, and every page writable**, which is core 0's arrangement:
// it holds no lease, so `MayWrite` admits everything and `LinkSegments`'
// grant check passes without a grant having to be plumbed in. That is the
// production shape for a relation core 0 absorbs; a peer absorber's grant
// is the leg the runtime test covers.

namespace kds::server {
namespace {

using catalog::ClusteredType;
using catalog::kNamespacePublic;
using catalog::Schema;
using catalog::SysColumnRow;

class RangeCoalesceTest : public ::testing::Test {
protected:
    std::unique_ptr<storage::MemoryPageDevice> device_ =
        std::move(storage::MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0)
                      .value());
    std::unique_ptr<storage::DevicePageStore> store_holder_ =
        std::move(storage::DevicePageStore::Open(*device_, /*first_new_page_id=*/128).value());
    storage::DevicePageStore& store_ = *store_holder_;
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

    PageId OpenRange(catalog::Oid oid, std::uint64_t lo, std::uint32_t owner) {
        auto entry = OpenRangeOnSystemCore(catalog_, store_, /*wal=*/nullptr, enforcer_, oid, lo,
                                           owner, /*log=*/nullptr);
        EXPECT_TRUE(entry.ok()) << entry.status().message();
        EXPECT_NE(entry.ok() ? entry.value() : kInvalidPageId, kInvalidPageId)
            << "the gates declined a bare heap relation";
        return entry.ok() ? entry.value() : kInvalidPageId;
    }

    // A row whose Keystone id is `id`. The payload only has to be long
    // enough to hold the word and to match it - `ChainInsert` checks both
    // and nothing here reads the value column.
    std::vector<std::byte> Row(std::uint64_t id, std::size_t filler = 8) {
        auto word = Keystone::Encode(id, 0, 0);
        EXPECT_TRUE(word.ok()) << word.status().message();
        std::vector<std::byte> out(kKeystoneWordSize + filler, std::byte{0xAB});
        std::uint64_t v = word.ok() ? word.value() : 0;
        for (std::size_t i = 0; i < kKeystoneWordSize; ++i) {
            out[i] = static_cast<std::byte>(v & 0xFF);
            v >>= 8;
        }
        return out;
    }

    // Places `id` in the chain headed at `head`, which is the range's own
    // chain - `HeapChainFor`'s answer, taken directly because this fixture
    // has no dispatcher to route through.
    void Place(PageId head, std::uint64_t id, catalog::Oid oid) {
        const std::vector<std::byte> payload = Row(id);
        auto placed = heap::ChainInsert(store_, head, id, payload, /*trx_id=*/1, oid);
        ASSERT_TRUE(placed.ok()) << placed.status().message();
    }

    // Every live id the relation answers, walking each range bounded by
    // its own `hi` - the read path's rule (§6c), reproduced here because
    // that is exactly the property a merge must not change.
    std::vector<std::uint64_t> ReadThroughDirectory(catalog::Oid oid) {
        std::vector<std::uint64_t> ids;
        auto access = catalog_.InitTableAccess(oid);
        EXPECT_TRUE(access.ok());
        if (!access.ok()) return ids;
        const auto visit = [&](PageId, heap::PageView& page,
                               std::uint16_t slot) -> StatusOr<storage::VisitControl> {
            auto tuple = page.ReadTuple(slot);
            if (tuple.ok() && !tuple.value().deleted) {
                auto id = KeystoneIdOfPayload(tuple.value().payload);
                EXPECT_TRUE(id.ok());
                if (id.ok()) ids.push_back(id.value());
            }
            return storage::VisitControl::kContinue;
        };
        // `WalkHeadsFor` minus its ownership filter, deliberately: this
        // fixture has one store and no cores, so what it is checking is
        // which **rows** the directory reaches, not which core may fault
        // them. The bound is the same one the read path applies.
        const auto walk = [&](PageId head, std::uint64_t stop) {
            EXPECT_TRUE(
                heap::ChainVisit(store_, head, storage::PageAccess::kRead, visit, nullptr, stop)
                    .ok());
        };
        if (access.value()->ranges.empty()) {
            walk(access.value()->desc_page_id, catalog::kIdSpaceEnd);
        } else {
            for (const catalog::RangeTarget& range : access.value()->ranges) {
                walk(range.entry_page, range.hi);
            }
        }
        return ids;
    }
};

TEST_F(RangeCoalesceTest, ThePlanNamesEveryRangeInLoOrderAndCountsItsPages) {
    const catalog::Oid oid = MakeHeap("planned");
    auto before = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(before.ok());
    const PageId head = before.value()->desc_page_id;
    OpenRange(oid, 4096, /*owner=*/1);
    OpenRange(oid, 8192, /*owner=*/2);

    auto plan = PlanCoalesce(catalog_, store_, oid);
    ASSERT_TRUE(plan.ok()) << plan.status().message();
    ASSERT_EQ(plan.value().segments.size(), 3u);
    EXPECT_EQ(plan.value().segments[0].lo, 0u);
    EXPECT_EQ(plan.value().segments[0].entry_page, head);
    EXPECT_EQ(plan.value().segments[1].lo, 4096u);
    EXPECT_EQ(plan.value().segments[2].lo, 8192u);
    // Each range is one empty head page. The census names it anyway,
    // which a slot-visiting walk would not have - the reason
    // `CollectRangePages` is a page walk.
    EXPECT_EQ(plan.value().pages_total, 3u);
    for (const CoalesceSegment& segment : plan.value().segments) {
        ASSERT_EQ(segment.pages.size(), 1u);
        EXPECT_EQ(segment.tail, segment.entry_page);
    }
}

TEST_F(RangeCoalesceTest, TheAbsorberIsTheCoreHoldingTheMostPages) {
    const catalog::Oid oid = MakeHeap("absorbed");
    auto before = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(before.ok());
    const PageId head = before.value()->desc_page_id;
    const PageId mid = OpenRange(oid, 4096, /*owner=*/1);
    OpenRange(oid, 8192, /*owner=*/2);

    // Core 1's range gets a second page; core 0's and core 2's stay at
    // one each. AX-D3 is a page count and not a range count, which is what
    // this distinguishes: core 1 owns one range of two pages, and the
    // other two cores own one range of one page each.
    Place(mid, 4096, oid);
    // Fill the head so the next id has to open a page.
    for (std::uint64_t id = 4097; id < 4600; ++id) {
        auto placed = heap::ChainInsert(store_, mid, id, Row(id), /*trx_id=*/1, oid);
        if (!placed.ok()) break;
        auto len = heap::ChainLength(store_, mid);
        ASSERT_TRUE(len.ok());
        if (len.value() >= 2) break;
    }
    auto grew = heap::ChainLength(store_, mid);
    ASSERT_TRUE(grew.ok());
    ASSERT_GE(grew.value(), 2u) << "the fixture never grew core 1's chain";

    auto plan = PlanCoalesce(catalog_, store_, oid);
    ASSERT_TRUE(plan.ok()) << plan.status().message();
    EXPECT_EQ(plan.value().absorber, 1u);
    EXPECT_EQ(plan.value().pages_to_move, plan.value().pages_total - grew.value());
    // Unchanged by the plan: it reads and writes nothing.
    EXPECT_EQ(plan.value().segments[0].entry_page, head);
}

TEST_F(RangeCoalesceTest, ATieGoesToTheLowestCore) {
    const catalog::Oid oid = MakeHeap("tied");
    OpenRange(oid, 4096, /*owner=*/2);
    OpenRange(oid, 8192, /*owner=*/1);

    // Three ranges, one page each, on cores 0, 2 and 1. Every count is 1,
    // so the tie rule alone decides - and it is determinism the rule is
    // for, which is what this asserts rather than any property of core 0.
    auto plan = PlanCoalesce(catalog_, store_, oid);
    ASSERT_TRUE(plan.ok()) << plan.status().message();
    EXPECT_EQ(plan.value().absorber, 0u);
}

TEST_F(RangeCoalesceTest, LinkingConcatenatesTheChainsInLoOrder) {
    const catalog::Oid oid = MakeHeap("linked");
    auto before = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(before.ok());
    const PageId head = before.value()->desc_page_id;
    const PageId mid = OpenRange(oid, 4096, /*owner=*/1);
    const PageId top = OpenRange(oid, 8192, /*owner=*/2);

    Place(head, 1, oid);
    Place(mid, 4096, oid);
    Place(top, 8192, oid);

    auto plan = PlanCoalesce(catalog_, store_, oid);
    ASSERT_TRUE(plan.ok()) << plan.status().message();
    ASSERT_TRUE(LinkSegments(store_, /*wal=*/nullptr, plan.value(), /*core_id=*/0).ok());

    // One chain now, headed by `desc_page_id` - which is what a merged
    // relation resolves through once the directory is gone.
    auto length = heap::ChainLength(store_, head);
    ASSERT_TRUE(length.ok()) << length.status().message();
    EXPECT_EQ(length.value(), 3u);
    auto tail = heap::ChainTail(store_, head);
    ASSERT_TRUE(tail.ok());
    EXPECT_EQ(tail.value(), top);
}

TEST_F(RangeCoalesceTest, TheBoundKeepsAPartMergedRelationReadingAsItDid) {
    const catalog::Oid oid = MakeHeap("bounded");
    auto before = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(before.ok());
    const PageId head = before.value()->desc_page_id;
    const PageId mid = OpenRange(oid, 4096, /*owner=*/1);
    const PageId top = OpenRange(oid, 8192, /*owner=*/2);
    Place(head, 1, oid);
    Place(mid, 4096, oid);
    Place(top, 8192, oid);

    const std::vector<std::uint64_t> split_read = ReadThroughDirectory(oid);
    ASSERT_EQ(split_read.size(), 3u);

    // **The crash state §6c makes safe**: chains linked, directory intact.
    // Without the bound each range's walk would run into its successors'
    // pages and then the successors would be walked again from their own
    // heads - 1, 4096, 8192, 4096, 8192, 8192.
    auto plan = PlanCoalesce(catalog_, store_, oid);
    ASSERT_TRUE(plan.ok());
    ASSERT_TRUE(LinkSegments(store_, /*wal=*/nullptr, plan.value(), /*core_id=*/0).ok());

    EXPECT_EQ(ReadThroughDirectory(oid), split_read)
        << "a linked-but-uncontracted relation must read exactly as the split one did";

    // And after the contraction it is one range, read through the one
    // chain, with the same rows again.
    ASSERT_TRUE(catalog_.ContractRangeRows(oid, /*absorber=*/0).ok());
    auto after = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(after.ok());
    EXPECT_TRUE(after.value()->ranges.empty()) << "the merged directory is zero rows, not one";
    EXPECT_EQ(after.value()->desc_page_id, head);
    EXPECT_EQ(ReadThroughDirectory(oid), split_read);
}

TEST_F(RangeCoalesceTest, APartlyLinkedRelationReadsAsTheSplitOneToo) {
    // The crash prefix **inside** `LinkSegments`: each link is its own full
    // page image, so a crash between two of them leaves some chains
    // concatenated and some not. AX7's seed sweep is what would exercise
    // this through a real crash; the harness is single-core and opens no
    // range at all (see the AX report), so the *state* is built here
    // directly and the property AX7's oracle would assert is checked on it.
    const catalog::Oid oid = MakeHeap("partial");
    auto before = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(before.ok());
    const PageId head = before.value()->desc_page_id;
    const PageId mid = OpenRange(oid, 4096, /*owner=*/1);
    const PageId top = OpenRange(oid, 8192, /*owner=*/2);
    Place(head, 1, oid);
    Place(mid, 4096, oid);
    Place(top, 8192, oid);

    const std::vector<std::uint64_t> split_read = ReadThroughDirectory(oid);
    ASSERT_EQ(split_read.size(), 3u);

    // Only the first link, by hand: range 0's tail now points at range 1's
    // head and range 1's tail still ends its chain.
    {
        auto page = store_.Get(head);
        ASSERT_TRUE(page.ok());
        heap::PageView(page.value().bytes()).set_next_page_id(mid);
    }
    EXPECT_EQ(ReadThroughDirectory(oid), split_read)
        << "a half-linked relation must read exactly as the split one did";

    // And the merge finishes from there rather than being repaired: the
    // link already written is left alone, the one that was not is made.
    auto plan = PlanCoalesce(catalog_, store_, oid);
    ASSERT_TRUE(plan.ok()) << plan.status().message();
    ASSERT_TRUE(LinkSegments(store_, /*wal=*/nullptr, plan.value(), plan.value().absorber).ok());
    ASSERT_TRUE(catalog_.ContractRangeRows(oid, plan.value().absorber).ok());
    EXPECT_EQ(ReadThroughDirectory(oid), split_read);
    auto length = heap::ChainLength(store_, head);
    ASSERT_TRUE(length.ok());
    EXPECT_EQ(length.value(), 3u);
}

TEST_F(RangeCoalesceTest, LinkingTwiceIsTheSameAsLinkingOnce) {
    const catalog::Oid oid = MakeHeap("idempotent");
    auto before = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(before.ok());
    const PageId head = before.value()->desc_page_id;
    const PageId mid = OpenRange(oid, 4096, /*owner=*/1);
    Place(head, 1, oid);
    Place(mid, 4096, oid);

    auto plan = PlanCoalesce(catalog_, store_, oid);
    ASSERT_TRUE(plan.ok());
    ASSERT_TRUE(LinkSegments(store_, /*wal=*/nullptr, plan.value(), /*core_id=*/0).ok());
    // The re-run §6c makes the repair for a crash mid-merge. It must not
    // refuse, and it must not write.
    ASSERT_TRUE(LinkSegments(store_, /*wal=*/nullptr, plan.value(), /*core_id=*/0).ok());

    auto length = heap::ChainLength(store_, head);
    ASSERT_TRUE(length.ok());
    EXPECT_EQ(length.value(), 2u);
}

TEST_F(RangeCoalesceTest, ContractionSetsTheOwnerToTheAbsorber) {
    const catalog::Oid oid = MakeHeap("owned");
    OpenRange(oid, 4096, /*owner=*/1);

    ASSERT_TRUE(catalog_.ContractRangeRows(oid, /*absorber=*/1).ok());
    auto after = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(after.ok());
    EXPECT_TRUE(after.value()->ranges.empty());
    // AX-D3's second consequence: a merged relation is routed by
    // `sys.tables.owner_core`, so it has to name the core that holds the
    // pages.
    EXPECT_EQ(after.value()->owner_core, 1u);
    auto rows = catalog_.RangesOf(oid);
    ASSERT_TRUE(rows.ok());
    EXPECT_TRUE(rows.value().empty());
}

TEST_F(RangeCoalesceTest, PlanningAnUnsplitRelationIsRefused) {
    const catalog::Oid oid = MakeHeap("single");
    auto plan = PlanCoalesce(catalog_, store_, oid);
    // Refused rather than answered with an empty merge: a caller that got
    // here without asking has a bug, and doing nothing quietly is how it
    // would keep it.
    EXPECT_FALSE(plan.ok());
    EXPECT_EQ(plan.status().code(), StatusCode::kInvalidArgument);
}

// The restamp needs a stream to point at, so it gets its own fixture: an
// unlogged store answers `kNoLsn` to every append and `StampPageLsn`
// refuses zero, which is why the cases above exercise the linking and not
// the acquisition.
class RangeCoalesceWalTest : public RangeCoalesceTest {
protected:
    void SetUp() override {
        RangeCoalesceTest::SetUp();
        auto device = wal::MemoryLogDevice::Create(1u << 20);
        ASSERT_TRUE(device.ok()) << device.status().message();
        log_device_ = std::move(device.value());
        auto manager = wal::WalManager::Open(log_device_.get(), clock_, /*core_id=*/0);
        ASSERT_TRUE(manager.ok()) << manager.status().message();
        wal_ = std::move(manager.value());
    }

    sched::SystemClock clock_;
    std::unique_ptr<wal::LogDevice> log_device_;
    std::unique_ptr<wal::WalManager> wal_;
};

TEST_F(RangeCoalesceWalTest, TheAbsorberRestampsEveryPageItTakes) {
    // **The hole this closes is core 0's.** `AdmitWritePages` skips its
    // restamp when `MayWrite` is already true, which is sound where it was
    // written and unconditionally true on core 0 - so core 0 absorbing a
    // peer's ranges would take every page without restamping any, leaving
    // the pages saying one stream and `sys.tables` saying another. PL §9
    // rule 6 is the rule: no page leaves a stream without being restamped.
    const catalog::Oid oid = MakeHeap("stamped");
    auto before = catalog_.InitTableAccess(oid);
    ASSERT_TRUE(before.ok());
    const PageId head = before.value()->desc_page_id;
    const PageId mid = OpenRange(oid, 4096, /*owner=*/1);

    // The peer's stamp on the page it owned, which is what the absorber
    // has to overwrite. Written by hand: this fixture has no peer store.
    {
        auto page = store_.Get(mid);
        ASSERT_TRUE(page.ok());
        storage::SetPageStreamStamp(page.value().bytes(), storage::StreamStampFor(1));
    }
    ASSERT_EQ(storage::GetPageStreamStamp(store_.GetForRead(mid).value().bytes()),
              storage::StreamStampFor(1));

    auto plan = PlanCoalesce(catalog_, store_, oid);
    ASSERT_TRUE(plan.ok()) << plan.status().message();
    ASSERT_EQ(plan.value().absorber, 0u) << "a tie goes to the lowest core";
    ASSERT_TRUE(LinkSegments(store_, &*wal_, plan.value(), /*core_id=*/0).ok());

    auto page = store_.GetForRead(mid);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(storage::GetPageStreamStamp(page.value().bytes()), storage::StreamStampFor(0))
        << "the absorber must restamp a page it took, or the departed core re-claims it "
           "from the stamp at its next fault";
    // And `page_lsn` names the acquisition record, not zero: the WAL gate
    // refuses a page claiming a record that was never logged, which is what
    // forced the acquisition to be a record at all.
    EXPECT_NE(storage::GetPageLsn(page.value().bytes()), 0u);
    EXPECT_LT(storage::GetPageLsn(page.value().bytes()), wal_->appended_lsn());

    EXPECT_EQ(storage::GetPageStreamStamp(store_.GetForRead(head).value().bytes()),
              storage::StreamStampFor(0));

    // **A page that already carries this stream's stamp gets no second
    // acquisition record**, which is what a re-run after a crash is - and
    // a repeat implies an erase that a re-delivery cannot promise is
    // sound. Re-reading the head's stamp would not pin it: that stamp was
    // already core 0's before the first call, so the assertion would pass
    // whether or not a record had been written. Counting the stream does
    // pin it - every page is now stamped, so a second run must append
    // nothing at all.
    const wal::Lsn settled = wal_->appended_lsn();
    ASSERT_TRUE(LinkSegments(store_, &*wal_, plan.value(), /*core_id=*/0).ok());
    EXPECT_EQ(wal_->appended_lsn(), settled)
        << "a re-run wrote records for acquisitions and links that had already happened";
}

TEST_F(RangeCoalesceTest, ACoreThatIsNotTheAbsorberMayNotLink) {
    const catalog::Oid oid = MakeHeap("wrongcore");
    OpenRange(oid, 4096, /*owner=*/1);
    auto plan = PlanCoalesce(catalog_, store_, oid);
    ASSERT_TRUE(plan.ok());
    ASSERT_EQ(plan.value().absorber, 0u);

    Status linked = LinkSegments(store_, /*wal=*/nullptr, plan.value(), /*core_id=*/3);
    EXPECT_FALSE(linked.ok());
    EXPECT_EQ(linked.code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace kds::server
