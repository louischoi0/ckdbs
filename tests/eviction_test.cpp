#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

// Buffer-pool frame reclamation (docs/inflight/in-progress/workplan-eviction.md EV01-EV02).
//
// The decision this file backs is EV1: reclamation is **pin refcount + clock
// second chance**, not epoch-based. What that buys is a guarantee that can be
// stated as a test rather than as a discipline - a frame somebody holds is
// never reclaimed - and this file is that statement.
//
// Two things worth knowing before editing it.
//
// **The sweep must be reclaiming something for the refusals to mean
// anything.** A test that asserts "the pinned frame survived" against a sweep
// that reclaimed nothing at all is a tautology, and would pass just as well
// if `EvictColdFrames` were `return 0;`. Every refusal test below therefore
// checks a *victim* fell in the same pass, so the sweep is provably doing its
// job while declining to touch the protected frame.
//
// **Nothing calls the sweep in production** (EV7 - eviction stays off until
// EV03 has migrated every caller off raw spans, because a raw span into a
// reclaimed frame is a use-after-free and `page.md` §3 says so). These tests
// call it directly. That is the point: the guarantee AST04 needs has to be
// true *now*, so the Bound Cabin can be built against it, without eviction
// being on anywhere.

namespace kds::storage {
namespace {

// Enough pages that a sweep has somewhere to go, few enough to enumerate.
class EvictionTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());

        auto store = DevicePageStore::Open(*device_, /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
    }

    // A fresh page, written, flushed and dropped from the pool - so a later
    // Get() faults it back in and the sweep has a *clean* frame to work with.
    // A frame that was only ever created is dirty, and a dirty frame is
    // deliberately not a victim (EV02).
    PageId MakeCleanResidentPage(std::byte fill) {
        auto created = store_->CreateNew();
        EXPECT_TRUE(created.ok()) << created.status().message();
        const PageId id = created.value().first;
        FormatPage(created.value().second.bytes(), PageType::kHeap);
        created.value().second.bytes()[kPageBodyOffset] = fill;
        EXPECT_TRUE(store_->Sync().ok());
        // Sync writes back but leaves the frame resident and clean.
        return id;
    }

    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
};

// ---- EV01: the handle ---------------------------------------------------

TEST_F(EvictionTest, APageRefPinsForItsLifetimeAndUnpinsExactlyOnce) {
    const PageId id = MakeCleanResidentPage(std::byte{1});
    EXPECT_EQ(store_->pinned_frames(), 0u);

    {
        auto ref = store_->PinnedGet(id);
        ASSERT_TRUE(ref.ok()) << ref.status().message();
        EXPECT_TRUE(ref.value().valid());
        EXPECT_EQ(ref.value().page_id(), id);
        EXPECT_EQ(store_->pinned_frames(), 1u);

        // A second handle to the same page is a second pin, not a shared
        // one: the count is what makes "somebody is still holding it" true
        // after the first is dropped.
        {
            auto second = store_->PinnedGetForRead(id);
            ASSERT_TRUE(second.ok());
            EXPECT_EQ(store_->pinned_frames(), 1u);  // one frame...
            EXPECT_EQ(store_->EvictColdFrames(8), 0u);
        }
        // ...and still pinned after the inner one dies.
        EXPECT_EQ(store_->pinned_frames(), 1u);
    }

    EXPECT_EQ(store_->pinned_frames(), 0u);
}

TEST_F(EvictionTest, MovingAHandleTransfersThePinRatherThanDuplicatingIt) {
    const PageId id = MakeCleanResidentPage(std::byte{2});

    auto ref = store_->PinnedGet(id);
    ASSERT_TRUE(ref.ok());
    DevicePageStore::PageRef moved = std::move(ref.value());

    EXPECT_TRUE(moved.valid());
    EXPECT_FALSE(ref.value().valid()) << "the source is emptied, so it drops nothing";
    EXPECT_EQ(store_->pinned_frames(), 1u);

    moved.Release();
    EXPECT_EQ(store_->pinned_frames(), 0u);

    // Release is idempotent - the destructor calls it again on the way out.
    moved.Release();
    EXPECT_EQ(store_->pinned_frames(), 0u);
}

TEST_F(EvictionTest, AHandleSeesAndCanDirtyItsOwnBytes) {
    const PageId id = MakeCleanResidentPage(std::byte{3});

    auto ref = store_->PinnedGetForRead(id);
    ASSERT_TRUE(ref.ok());
    EXPECT_EQ(ref.value().bytes()[kPageBodyOffset], std::byte{3});
    EXPECT_EQ(ref.value().bytes().size(), kPageSize);

    // Taken for read, then written: MarkDirty is what keeps the write from
    // being dropped by a pool that trusts the accessor the caller chose.
    ref.value().bytes()[kPageBodyOffset] = std::byte{9};
    ref.value().MarkDirty();
    ref.value().Release();

    ASSERT_TRUE(store_->Sync().ok());
    auto reread = store_->GetForRead(id);
    ASSERT_TRUE(reread.ok());
    EXPECT_EQ(reread.value().bytes()[kPageBodyOffset], std::byte{9});
}

// ---- EV02: the sweep ----------------------------------------------------

TEST_F(EvictionTest, TheSweepReclaimsAColdCleanFrameAndTheBytesComeBack) {
    const PageId id = MakeCleanResidentPage(std::byte{42});
    const std::size_t before = store_->resident_pages();

    // First pass clears the reference bit the creating fetch set; the second
    // collects it. That two-lap shape *is* the second chance.
    EXPECT_EQ(store_->EvictColdFrames(8), 1u);
    EXPECT_LT(store_->resident_pages(), before);

    // Reclaimed, not lost: the next fetch faults it back off the device and
    // the bytes are the ones that were written.
    auto back = store_->GetForRead(id);
    ASSERT_TRUE(back.ok()) << back.status().message();
    EXPECT_EQ(back.value().bytes()[kPageBodyOffset], std::byte{42});
}

// EV1's actual claim, and the reason it is a *counter* and not a reference
// bit: a page touched five times outlives one touched once. A bit cannot
// express that - both would be "referenced" and both would fall on the pass
// after next - so this is the test that would still pass against a bit only
// if the counter were not doing its job.
TEST_F(EvictionTest, AFrequentlyTouchedFrameOutlivesARarelyTouchedOne) {
    const PageId hot = MakeCleanResidentPage(std::byte{1});
    const PageId cold = MakeCleanResidentPage(std::byte{2});
    ASSERT_NE(hot, cold);

    // `hot` well past the cap, `cold` just created (usage 1 from its own
    // fetch). Saturation is what bounds how long a once-popular page can
    // hold a frame after falling out of use.
    for (int i = 0; i < 20; ++i) ASSERT_TRUE(store_->GetForRead(hot).ok());

    // One rotation at a time, one frame of budget: `cold`'s counter runs out
    // first, so it is the one reclaimed.
    std::size_t rotations_until_cold_fell = 0;
    while (store_->EvictColdFrames(1) == 0) {
        ++rotations_until_cold_fell;
        ASSERT_LT(rotations_until_cold_fell, 32u) << "the sweep made no progress";
    }
    EXPECT_TRUE(store_->IsAllocated(hot)) << "the hot page outlived the cold one";

    // And `hot` falls too, once its counter is walked down - saturating, so
    // 20 touches cost no more rotations than the cap allows.
    std::size_t more = 0;
    for (int pass = 0; pass < 8 && more == 0; ++pass) more = store_->EvictColdFrames(1);
    EXPECT_EQ(more, 1u) << "a saturating counter is bounded by the cap, not by touch count";
}

TEST_F(EvictionTest, ADirtyFrameIsSkippedRatherThanDropped) {
    // Created and not synced, so the frame is dirty and its write is only in
    // memory. Dropping it would lose that write, which is why EV02 refuses
    // dirty victims outright and leaves them to EV04's WAL-gated flush.
    auto created = store_->CreateNew();
    ASSERT_TRUE(created.ok());
    const PageId dirty = created.value().first;
    created.value().second.bytes()[kPageBodyOffset] = std::byte{7};
    // Setup done: the pin drops, so the sweep sees a cold dirty frame
    // rather than a pinned one - the lifetime discipline every caller now
    // follows (MG01). Without this the sweep's pin refusal fires first and
    // the dirty branch under test is never reached.
    created.value().second.Release();

    EXPECT_EQ(store_->EvictColdFrames(8), 0u);
    EXPECT_EQ(store_->EvictColdFrames(8), 0u) << "still refused on the second lap";

    // Queued rather than silently skipped: §3.2's fourth branch hands it to
    // §4's writeback, which is EVT03's. Draining the queue is what a
    // background writer will do; here it is the evidence the sweep saw the
    // frame and made the specified decision about it.
    std::vector<PageId> queued = store_->TakeDirtyEvictionQueue();
    EXPECT_NE(std::find(queued.begin(), queued.end(), dirty), queued.end())
        << "the sweep queued the dirty frame for writeback";
    EXPECT_TRUE(store_->TakeDirtyEvictionQueue().empty()) << "taking it clears it";

    // And the write is still there to be flushed.
    ASSERT_TRUE(store_->Sync().ok());
    auto back = store_->GetForRead(dirty);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value().bytes()[kPageBodyOffset], std::byte{7});
}

// ---- The guarantee AST04 rests on ---------------------------------------

TEST_F(EvictionTest, APinnedFrameIsNeverReclaimedWhileAVictimFallsBesideIt) {
    const PageId pinned = MakeCleanResidentPage(std::byte{1});
    const PageId victim = MakeCleanResidentPage(std::byte{2});
    ASSERT_NE(pinned, victim);

    // ForRead, so the frame stays *clean* and the only thing standing
    // between it and the sweep is the pin. Taking it with PinnedGet would
    // dirty it, and a dirty frame is refused for a different reason - which
    // would make this test pass without testing anything.
    auto ref = store_->PinnedGetForRead(pinned);
    ASSERT_TRUE(ref.ok());

    // Generous budget and repeated passes: the refusal is not a budget
    // artefact, and it does not decay.
    std::size_t reclaimed = 0;
    for (int pass = 0; pass < 5; ++pass) reclaimed += store_->EvictColdFrames(64);

    // The victim fell - so the sweep was working, and the assertion below is
    // not a tautology about a sweep that does nothing.
    EXPECT_GE(reclaimed, 1u);
    EXPECT_EQ(store_->pinned_frames(), 1u);

    // The handle's bytes are still the frame's, which is the property the
    // whole design exists for: a reclaimed frame would have freed them.
    EXPECT_EQ(ref.value().bytes()[kPageBodyOffset], std::byte{1});

    // And once released it becomes an ordinary candidate.
    ref.value().Release();
    EXPECT_EQ(store_->pinned_frames(), 0u);
    std::size_t after = 0;
    for (int pass = 0; pass < 2; ++pass) after += store_->EvictColdFrames(64);
    EXPECT_GE(after, 1u) << "unpinning makes it evictable, so the pin was the reason";
}

TEST_F(EvictionTest, APinnedClassFrameIsNeverReclaimedAndNeedsNoPin) {
    // EV3: residency is a property of the page's *class*. This is what
    // `docs/spec/assertion.md` §5's Bound Cabin will declare, and what
    // AST04's "exempt from eviction" acceptance criterion means.
    const PageId resident = MakeCleanResidentPage(std::byte{1});
    const PageId victim = MakeCleanResidentPage(std::byte{2});
    ASSERT_LT(resident, victim) << "CreateNew hands out ascending ids";

    store_->SetResidentLimit(victim);
    EXPECT_TRUE(store_->IsPinnedClass(resident));
    EXPECT_FALSE(store_->IsPinnedClass(victim));

    std::size_t reclaimed = 0;
    for (int pass = 0; pass < 5; ++pass) reclaimed += store_->EvictColdFrames(64);

    EXPECT_GE(reclaimed, 1u) << "the victim fell, so the sweep ran";
    EXPECT_EQ(store_->pinned_frames(), 0u) << "residency is not a pin";

    // Still resident, and still the bytes it had.
    auto still = store_->GetForRead(resident);
    ASSERT_TRUE(still.ok());
    EXPECT_EQ(still.value().bytes()[kPageBodyOffset], std::byte{1});
}

TEST_F(EvictionTest, ThePinnedClassRangeOnlyEverGrows) {
    // A structure that declared itself un-evictable and then found itself
    // evictable is exactly the failure the declaration exists to prevent, so
    // lowering the limit is silently refused rather than honoured.
    store_->SetResidentLimit(100);
    EXPECT_TRUE(store_->IsPinnedClass(50));

    store_->SetResidentLimit(10);
    EXPECT_TRUE(store_->IsPinnedClass(50)) << "the limit did not fall";

    store_->SetResidentLimit(200);
    EXPECT_TRUE(store_->IsPinnedClass(150));
}

// ---- The pre-existing eviction path learns about pins -------------------

TEST_F(EvictionTest, EvictCleanRefusesAPinnedPageAsItAlreadyRefusesADirtyOne) {
    const PageId id = MakeCleanResidentPage(std::byte{1});
    const PageId ids[] = {id};

    // ForRead for the reason above: EvictClean checks dirty *before* pinned,
    // so a PinnedGet here would report the dirty refusal and never reach the
    // one this test is about.
    auto ref = store_->PinnedGetForRead(id);
    ASSERT_TRUE(ref.ok());
    Status refused = store_->EvictClean(ids);
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(refused.message().find("pinned"), std::string::npos) << refused.message();

    ref.value().Release();
    EXPECT_TRUE(store_->EvictClean(ids).ok());
}

TEST_F(EvictionTest, AnEmptyOrZeroBudgetSweepIsAWellDefinedNoOp) {
    EXPECT_EQ(store_->EvictColdFrames(0), 0u);
    const PageId id = MakeCleanResidentPage(std::byte{1});
    EXPECT_EQ(store_->EvictColdFrames(0), 0u);
    EXPECT_TRUE(store_->IsAllocated(id));
}

// ---- EVT03: the writeback primitive, the drain, the watermark loop ------

// The flush-before-evict oracle, checked by construction: the gate flips a
// flag when asked, and the device refuses to count a write that arrived
// before it - so "no page write ever precedes its WAL durability point" is
// a counter that must stay zero, not an inspection.
class GateProbe final : public wal::WalDurability {
public:
    wal::Lsn durable_lsn() const noexcept override { return durable_; }
    Status EnsureDurable(wal::Lsn lsn) override {
        asked_ = true;
        if (lsn >= durable_) durable_ = lsn + 1;
        return Status::OK();
    }
    bool asked() const noexcept { return asked_; }

private:
    wal::Lsn durable_ = 1;  // a real record LSN is never 0
    bool asked_ = false;
};

class ProbedDevice final : public PageDevice {
public:
    ProbedDevice(PageDevice& inner, const GateProbe* gate) : inner_(inner), gate_(gate) {}

    std::uint32_t page_capacity() const noexcept override { return inner_.page_capacity(); }
    Status ReadPage(PageId id, std::span<std::byte, kPageSize> out) override {
        return inner_.ReadPage(id, out);
    }
    Status WritePage(PageId id, std::span<const std::byte, kPageSize> in) override {
        Note();
        ++page_writes_;
        return inner_.WritePage(id, in);
    }
    Status WritePageRun(PageId first, std::uint32_t nr, std::span<const std::byte> in) override {
        Note();
        ++run_writes_;
        return inner_.WritePageRun(first, nr, in);
    }
    Status EnsureCapacity(std::uint32_t nr_pages) override {
        return inner_.EnsureCapacity(nr_pages);
    }
    Status Sync() override { return inner_.Sync(); }

    std::size_t page_writes() const noexcept { return page_writes_; }
    std::size_t run_writes() const noexcept { return run_writes_; }
    std::size_t writes_before_gate() const noexcept { return writes_before_gate_; }

private:
    void Note() {
        if (gate_ != nullptr && !gate_->asked()) ++writes_before_gate_;
    }
    PageDevice& inner_;
    const GateProbe* gate_;
    std::size_t page_writes_ = 0;
    std::size_t run_writes_ = 0;
    std::size_t writes_before_gate_ = 0;
};

TEST_F(EvictionTest, TheDrainWritesQueuedDirtCleanAndTheNextSweepReclaims) {
    // Three dirty frames at usage zero: the sweep queues, never reclaims.
    std::vector<PageId> ids;
    for (int i = 0; i < 3; ++i) {
        auto created = store_->CreateNew();
        ASSERT_TRUE(created.ok());
        FormatPage(created.value().second.bytes(), PageType::kHeap);
        created.value().second.bytes()[kPageBodyOffset] = std::byte{static_cast<unsigned char>(10 + i)};
        ids.push_back(created.value().first);
    }
    EXPECT_EQ(store_->EvictColdFrames(8), 0u);

    // The drain is §4's steps in order, ending clean - and *only* clean:
    // the frames stay cached "until frames are actually needed".
    auto drained = store_->DrainDirtyEvictionQueue();
    ASSERT_TRUE(drained.ok()) << drained.status().message();
    EXPECT_GE(drained.value(), 3u);
    EXPECT_TRUE(store_->DirtyPageIds().empty());
    const std::size_t resident_after_drain = store_->resident_pages();

    // The sweep's next visit reclaims what the drain cleaned (§4), and the
    // bytes come back from the device intact.
    EXPECT_GE(store_->EvictColdFrames(8), 3u);
    EXPECT_LT(store_->resident_pages(), resident_after_drain);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto back = store_->GetForRead(ids[i]);
        ASSERT_TRUE(back.ok()) << back.status().message();
        EXPECT_EQ(back.value().bytes()[kPageBodyOffset],
                  std::byte{static_cast<unsigned char>(10 + i)});
    }

    // An empty queue drains to zero, not to an error.
    auto empty = store_->DrainDirtyEvictionQueue();
    ASSERT_TRUE(empty.ok());
    EXPECT_EQ(empty.value(), 0u);
}

TEST(EvictionWritebackTest, NoPageWritePrecedesItsWalDurabilityPoint) {
    auto device = MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok());
    GateProbe gate;
    ProbedDevice probed(*device.value(), &gate);
    auto opened = DevicePageStore::Open(probed, /*first_new_page_id=*/16);
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    store.SetWalGate(&gate);

    auto created = store.CreateNew();
    ASSERT_TRUE(created.ok());
    FormatPage(created.value().second.bytes(), PageType::kHeap);
    ASSERT_TRUE(store.StampPageLsn(created.value().first, /*lsn=*/77).ok());
    created.value().second.Release();  // setup done - the sweep must see it cold

    EXPECT_EQ(store.EvictColdFrames(8), 0u);  // dirty at zero: queued
    auto drained = store.DrainDirtyEvictionQueue();
    ASSERT_TRUE(drained.ok());
    EXPECT_GE(drained.value(), 1u);

    EXPECT_TRUE(gate.asked()) << "writeback never consulted the gate";
    EXPECT_EQ(probed.writes_before_gate(), 0u)
        << "a page write reached the device before its WAL durability point";
}

TEST(EvictionWritebackTest, ContiguousRunsCoalesceIntoOneDeviceCall) {
    auto device = MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
    ASSERT_TRUE(device.ok());
    ProbedDevice probed(*device.value(), nullptr);
    auto opened = DevicePageStore::Open(probed, /*first_new_page_id=*/16);
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();

    // Four consecutive ids (CreateNew hands them out in order), one run.
    std::vector<PageId> ids;
    for (int i = 0; i < 4; ++i) {
        auto created = store.CreateNew();
        ASSERT_TRUE(created.ok());
        FormatPage(created.value().second.bytes(), PageType::kHeap);
        created.value().second.bytes()[kPageBodyOffset] = std::byte{static_cast<unsigned char>(i)};
        ids.push_back(created.value().first);
    }
    ASSERT_EQ(ids[3], ids[0] + 3) << "the run premise does not hold";

    auto written = store.WriteBack(ids);
    ASSERT_TRUE(written.ok());
    EXPECT_EQ(written.value(), 4u);
    EXPECT_EQ(probed.run_writes(), 1u) << "four contiguous pages should be one device run";
    EXPECT_EQ(probed.page_writes(), 0u);

    // Best-effort, not lossy: every page reads back through the device.
    ASSERT_TRUE(store.EvictColdFrames(8) >= 4u);
    for (int i = 0; i < 4; ++i) {
        auto back = store.GetForRead(ids[i]);
        ASSERT_TRUE(back.ok());
        EXPECT_EQ(back.value().bytes()[kPageBodyOffset], std::byte{static_cast<unsigned char>(i)});
    }
}

TEST_F(EvictionTest, MaintainFreeReserveRestoresTheWatermarkThroughDirt) {
    // A dirty burst: four frames the sweep alone could never free.
    std::vector<PageId> ids;
    for (int i = 0; i < 4; ++i) {
        auto created = store_->CreateNew();
        ASSERT_TRUE(created.ok());
        FormatPage(created.value().second.bytes(), PageType::kHeap);
        created.value().second.bytes()[kPageBodyOffset] = std::byte{static_cast<unsigned char>(20 + i)};
        ids.push_back(created.value().first);
    }
    const std::size_t resident = store_->resident_pages();

    // A pool exactly as large as what is resident and a watermark of two:
    // the loop must sweep (queueing the dirt), drain (cleaning it), and
    // sweep again (reclaiming) until the reserve exists - §4's rotation.
    const std::size_t reclaimed = store_->MaintainFreeReserve(resident, /*watermark=*/2);
    EXPECT_GE(reclaimed, 2u);
    EXPECT_LE(store_->resident_pages(), resident - 2);

    // No write was lost to the reserve: every page reads back intact.
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto back = store_->GetForRead(ids[i]);
        ASSERT_TRUE(back.ok());
        EXPECT_EQ(back.value().bytes()[kPageBodyOffset],
                  std::byte{static_cast<unsigned char>(20 + i)});
    }

    // A satisfied watermark is a no-op, and an unsatisfiable one ends on
    // "a full rotation yielded nothing" rather than spinning.
    EXPECT_EQ(store_->MaintainFreeReserve(store_->resident_pages() + 8, 2), 0u);
    (void)store_->MaintainFreeReserve(0, 1'000'000);  // must terminate
}

// ---- EVT06: the scan ring -------------------------------------------------

TEST_F(EvictionTest, ARingBoundsAScansResidencyAndSparesTheWorkingSet) {
    // The pre-scan working set: three pages the foreground is using, their
    // usage counters raised by ordinary reads.
    std::vector<PageId> working;
    for (int i = 0; i < 3; ++i) {
        working.push_back(MakeCleanResidentPage(std::byte{static_cast<unsigned char>(i)}));
        for (int touch = 0; touch < 3; ++touch) {
            ASSERT_TRUE(store_->GetForRead(working.back()).ok());
        }
    }

    // Twelve scan pages, written out and reclaimed so the scan must fault
    // every one of them back in.
    std::vector<PageId> scanned;
    for (int i = 0; i < 12; ++i) {
        scanned.push_back(MakeCleanResidentPage(std::byte{static_cast<unsigned char>(50 + i)}));
    }
    while (store_->EvictColdFrames(16) > 0) {
    }
    for (const PageId id : working) {
        // Re-raise: the reclaim laps above walked the counters down.
        for (int touch = 0; touch < 3; ++touch) ASSERT_TRUE(store_->GetForRead(id).ok());
    }
    const std::size_t resident_before = store_->resident_pages();

    // The scan, through a four-slot ring: every page's bytes are right,
    // and residency grows by at most the ring's size - the whole point.
    auto ring = store_->OpenScanRing(/*frames=*/4);
    for (std::size_t i = 0; i < scanned.size(); ++i) {
        auto bytes = ring->Fetch(scanned[i]);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        EXPECT_EQ(bytes.value()[kPageBodyOffset],
                  std::byte{static_cast<unsigned char>(50 + i)});
    }
    EXPECT_LE(store_->resident_pages(), resident_before + 4)
        << "the scan flooded the pool instead of reusing its ring";

    // The working set is untouched: still resident, and still *hot* - a
    // sweep that reclaims the ring leftovers spares it, which is only true
    // if the scan never walked its usage down or dropped its frames.
    ASSERT_GT(store_->EvictColdFrames(16), 0u);
    for (const PageId id : working) {
        auto back = store_->GetForRead(id);
        ASSERT_TRUE(back.ok());
    }
    EXPECT_GE(store_->resident_pages(), working.size());
}

TEST_F(EvictionTest, RingFetchesNeverBumpUsage) {
    const PageId id = MakeCleanResidentPage(std::byte{9});
    ASSERT_GT(store_->EvictColdFrames(8), 0u);  // start absent

    // Fetched through the ring five times: if any fetch bumped usage, the
    // single sweep below could not reclaim it in one pass.
    auto ring = store_->OpenScanRing(/*frames=*/2);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(ring->Fetch(id).ok());
    }
    EXPECT_EQ(store_->EvictColdFrames(8), 1u)
        << "a scan's touches registered as heat (usage was bumped)";
}

TEST_F(EvictionTest, RotationSparesAPinnedPageAndDropsAColdOne) {
    std::vector<PageId> ids;
    for (int i = 0; i < 4; ++i) {
        ids.push_back(MakeCleanResidentPage(std::byte{static_cast<unsigned char>(30 + i)}));
    }
    while (store_->EvictColdFrames(16) > 0) {
    }
    ASSERT_EQ(store_->resident_pages(), 0u);

    auto ring = store_->OpenScanRing(/*frames=*/2);
    ASSERT_TRUE(ring->Fetch(ids[0]).ok());
    ASSERT_TRUE(ring->Fetch(ids[1]).ok());

    // The foreground pins the first ring page mid-scan.
    auto pinned = store_->PinnedGet(ids[0]);
    ASSERT_TRUE(pinned.ok());

    // Rotation reaches both slots: ids[0]'s is skipped (pinned - abandoned
    // to the table), ids[1]'s is dropped.
    ASSERT_TRUE(ring->Fetch(ids[2]).ok());
    ASSERT_TRUE(ring->Fetch(ids[3]).ok());

    EXPECT_EQ(pinned.value().bytes()[kPageBodyOffset], std::byte{30})
        << "the pinned page's bytes moved under the pin";
    // ids[0] survived with the pin; ids[1] was rotated out; the ring holds
    // ids[2] and ids[3]: three resident of the four.
    EXPECT_EQ(store_->resident_pages(), 3u);
    EXPECT_EQ(store_->pinned_frames(), 1u);
}

TEST_F(EvictionTest, TheRingNeverDropsADirtyFrameOrAResidentClassPage) {
    // Staged first, because MakeCleanResidentPage syncs the whole store
    // and would clean the page this test needs dirty.
    const PageId other = MakeCleanResidentPage(std::byte{78});
    while (store_->EvictColdFrames(8) > 0) {
    }

    // A page the foreground dirtied is not the ring's to discard, however
    // cold: the write must reach the device first (§5's dirty rule).
    auto created = store_->CreateNew();
    ASSERT_TRUE(created.ok());
    const PageId dirty = created.value().first;
    FormatPage(created.value().second.bytes(), PageType::kHeap);
    created.value().second.bytes()[kPageBodyOffset] = std::byte{77};

    auto ring = store_->OpenScanRing(/*frames=*/1);
    ASSERT_TRUE(ring->Fetch(dirty).ok());  // in place: already resident
    ASSERT_TRUE(ring->Fetch(other).ok());  // faults into the one slot
    ASSERT_TRUE(ring->Fetch(dirty).ok());  // in place again; slot keeps `other`

    ring.reset();  // scan over: drops `other`, must not drop `dirty`
    const std::vector<PageId> dirty_ids = store_->DirtyPageIds();
    EXPECT_NE(std::find(dirty_ids.begin(), dirty_ids.end(), dirty), dirty_ids.end())
        << "the dirty page lost its frame to the ring";
    ASSERT_TRUE(store_->Sync().ok());
    auto back = store_->GetForRead(dirty);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.value().bytes()[kPageBodyOffset], std::byte{77});
}

}  // namespace
}  // namespace kds::storage
