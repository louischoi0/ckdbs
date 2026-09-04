#include "kds/storage/page_latch.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kds/storage/device_page_store.hpp"
#include "kds/storage/memory_page_device.hpp"

// AM-S1 (`instructions/v3.0.0/workorder-am-m1-shared-pool.md`): the page
// latch word - first the primitive alone, on raw words with no store, no
// frame and no pin; then what the store does with it (the arming, the
// modes the accessors pick, the sweep's and EvictClean's refusals) on
// `PageLatchStoreTest` below. The scan ring's refusal (ReleaseScanSlot)
// has no cell. The arming on the real assembly is `expeditor_test.cpp`'s.
//
// Two rules the cells pin because the header states them as rules rather
// than as behaviour that happens to hold: the word is never upgraded (an
// exclusive request against shared holders is busy, whoever holds the
// shares), and it is re-entrant for the owning core (the shape every chain
// growth and split path in the tree takes).

namespace kds::storage {
namespace {

TEST(PageLatch, TheWordStartsFreeAndDecodesAsSuch) {
    std::uint32_t word = 0;
    const PageLatchWord w = DecodePageLatch(word);
    EXPECT_FALSE(w.exclusive);
    EXPECT_EQ(w.count, 0u);
    EXPECT_FALSE(PageLatch::IsHeld(word));
    EXPECT_FALSE(PageLatch::HasSharedHolders(word));

    // The encode/decode pair is exact for both shapes of the word.
    const std::uint32_t x = EncodePageLatch(true, 5, 3);
    const PageLatchWord dx = DecodePageLatch(x);
    EXPECT_TRUE(dx.exclusive);
    EXPECT_EQ(dx.owner_core, 5u);
    EXPECT_EQ(dx.count, 3u);

    const std::uint32_t s = EncodePageLatch(false, 0, 7);
    const PageLatchWord ds = DecodePageLatch(s);
    EXPECT_FALSE(ds.exclusive);
    EXPECT_EQ(ds.count, 7u);

    // The largest core id the owner field can name round-trips; the field
    // holds core_id + 1, which is why the cap is one below the field's max.
    const std::uint32_t top = EncodePageLatch(true, kPageLatchMaxCoreId, 1);
    EXPECT_EQ(DecodePageLatch(top).owner_core, kPageLatchMaxCoreId);
}

TEST(PageLatch, ExclusiveIsReentrantForItsOwnerAndCountsEveryHold) {
    std::uint32_t word = 0;
    EXPECT_EQ(PageLatch::Acquire(word, PageLatchMode::kExclusive, /*core=*/2), 0u)
        << "a free word is taken on the first turn";
    EXPECT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kExclusive, 2),
              PageLatchOutcome::kAcquired)
        << "the owning core may hold a page twice";
    const PageLatchWord w = DecodePageLatch(PageLatch::Load(word));
    EXPECT_TRUE(w.exclusive);
    EXPECT_EQ(w.owner_core, 2u);
    EXPECT_EQ(w.count, 2u);
    EXPECT_TRUE(PageLatch::IsHeldExclusiveBy(word, 2));
    EXPECT_FALSE(PageLatch::IsHeldExclusiveBy(word, 3));

    PageLatch::Release(word, 2);
    EXPECT_TRUE(PageLatch::IsHeld(word)) << "one of two holds released";
    PageLatch::Release(word, 2);
    EXPECT_EQ(PageLatch::Load(word), 0u) << "the word returns to free at depth zero";
}

TEST(PageLatch, AnOwnerMayTakeSharedUnderItsOwnExclusive) {
    // The X-then-S shape the word admits. No production path takes it
    // today - LogFullPageImage re-fetches with Get(), so it is
    // exclusive-under-exclusive - and page_latch.hpp says why the word
    // admits it anyway.
    std::uint32_t word = 0;
    ASSERT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kExclusive, 3),
              PageLatchOutcome::kAcquired);
    EXPECT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kShared, 3),
              PageLatchOutcome::kAcquired);
    const PageLatchWord w = DecodePageLatch(PageLatch::Load(word));
    EXPECT_TRUE(w.exclusive) << "the shared hold under an exclusive one keeps the word exclusive";
    EXPECT_EQ(w.count, 2u);
    PageLatch::Release(word, 3);
    PageLatch::Release(word, 3);
    EXPECT_EQ(PageLatch::Load(word), 0u);
}

TEST(PageLatch, SharedHoldersCountAndAnExclusiveRequestWaitsForThem) {
    std::uint32_t word = 0;
    ASSERT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kShared, 0),
              PageLatchOutcome::kAcquired);
    ASSERT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kShared, 1),
              PageLatchOutcome::kAcquired);
    const PageLatchWord w = DecodePageLatch(PageLatch::Load(word));
    EXPECT_FALSE(w.exclusive);
    EXPECT_EQ(w.count, 2u);
    EXPECT_TRUE(PageLatch::HasSharedHolders(word));

    EXPECT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kExclusive, 5),
              PageLatchOutcome::kBusy)
        << "a foreign exclusive request waits for the readers";

    PageLatch::Release(word, 0);
    PageLatch::Release(word, 1);
    EXPECT_EQ(PageLatch::Load(word), 0u);
    EXPECT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kExclusive, 5),
              PageLatchOutcome::kAcquired)
        << "and takes the word once the last reader is gone";
    PageLatch::Release(word, 5);
}

TEST(PageLatch, AnUpgradeIsNeverGrantedByTheWord) {
    // The rule the store diagnoses as a self-deadlock: a core holding the
    // page shared asks for it exclusive. The word cannot know whose the
    // shares are, so it answers busy - never a grant, never a silent
    // upgrade. A release-build caller that spins here hangs, which is the
    // failure a recursive std::mutex gives and the reason the store checks
    // its own pins first.
    std::uint32_t word = 0;
    ASSERT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kShared, 4),
              PageLatchOutcome::kAcquired);
    EXPECT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kExclusive, 4),
              PageLatchOutcome::kBusy);
    EXPECT_TRUE(PageLatch::HasSharedHolders(word));
    PageLatch::Release(word, 4);
    EXPECT_EQ(PageLatch::Load(word), 0u);
}

TEST(PageLatch, AReleaseByTheWrongCoreOrOfAFreeWordChangesNothing) {
    std::uint32_t word = 0;
    PageLatch::Release(word, 1);  // nothing held: left alone, no underflow
    EXPECT_EQ(PageLatch::Load(word), 0u);

    ASSERT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kExclusive, 1),
              PageLatchOutcome::kAcquired);
    PageLatch::Release(word, 2);  // not the owner: left alone
    EXPECT_TRUE(PageLatch::IsHeldExclusiveBy(word, 1));
    PageLatch::Release(word, 1);
    EXPECT_EQ(PageLatch::Load(word), 0u);
}

TEST(PageLatch, AForeignCoreWaitsOutAnExclusiveHolder) {
    std::uint32_t word = 0;
    ASSERT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kExclusive, 1),
              PageLatchOutcome::kAcquired);
    EXPECT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kShared, 2), PageLatchOutcome::kBusy);
    EXPECT_EQ(PageLatch::TryAcquire(word, PageLatchMode::kExclusive, 2),
              PageLatchOutcome::kBusy);

    // The waiter publishes its first *refused* attempt, not its start, so
    // the release below is ordered after a wait that demonstrably happened
    // - a waiter descheduled between starting and trying could otherwise
    // find the word free on its first try and prove nothing.
    std::atomic<bool> refused_once{false};
    std::atomic<std::uint64_t> turns{0};
    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        while (PageLatch::TryAcquire(word, PageLatchMode::kShared, 2) ==
               PageLatchOutcome::kAcquired) {
            // Not expected: core 1 holds the word until refused_once is seen.
            PageLatch::Release(word, 2);
        }
        refused_once.store(true, std::memory_order_release);
        turns.store(PageLatch::Acquire(word, PageLatchMode::kShared, 2),
                    std::memory_order_release);
        acquired.store(true, std::memory_order_release);
        PageLatch::Release(word, 2);
    });
    while (!refused_once.load(std::memory_order_acquire)) {
    }
    EXPECT_FALSE(acquired.load(std::memory_order_acquire))
        << "the reader got in while core 1 held the page exclusive";
    PageLatch::Release(word, 1);
    waiter.join();
    EXPECT_TRUE(acquired.load(std::memory_order_acquire));
    EXPECT_EQ(PageLatch::Load(word), 0u);
}

TEST(PageLatch, EightThreadsNeverShareAFrameExclusivelyAndTheCountsBalance) {
    // AM-S1's "contention cell at 8 cores", on the primitive: the store's
    // frame table is not thread-safe and cannot host it, so the words
    // stand in for frames. Eight cores, sixty-four words, random shared
    // and exclusive holds, and a shadow census per word checked at entry:
    // an exclusive holder must find nobody, a shared holder must find no
    // writer. In the discipline of wal_shared_stream_test.cpp:136-139, the
    // cell also asserts that contention actually happened - a run that
    // serialized by accident would prove nothing.
    constexpr int kCores = 8;
    constexpr int kWords = 64;
    constexpr int kTurnsPerCore = 20000;

    std::array<std::uint32_t, kWords> words{};
    std::array<std::atomic<int>, kWords> readers{};
    std::array<std::atomic<int>, kWords> writers{};
    std::atomic<std::uint64_t> contended_turns{0};
    std::atomic<int> violations{0};

    std::vector<std::thread> cores;
    cores.reserve(kCores);
    for (int core = 0; core < kCores; ++core) {
        cores.emplace_back([&, core] {
            std::mt19937 rng(static_cast<std::uint32_t>(1000 + core));
            std::uniform_int_distribution<int> pick(0, kWords - 1);
            std::uniform_int_distribution<int> roll(0, 3);
            std::uint64_t spun = 0;
            for (int i = 0; i < kTurnsPerCore; ++i) {
                const int w = pick(rng);
                const bool exclusive = roll(rng) == 0;
                const PageLatchMode mode =
                    exclusive ? PageLatchMode::kExclusive : PageLatchMode::kShared;
                spun += PageLatch::Acquire(words[w], mode,
                                           static_cast<std::uint32_t>(core));
                if (exclusive) {
                    if (readers[w].load() != 0 || writers[w].load() != 0) ++violations;
                    writers[w].fetch_add(1);
                    // Re-entrancy under contention: the owner takes it again.
                    if (PageLatch::TryAcquire(words[w], PageLatchMode::kExclusive,
                                              static_cast<std::uint32_t>(core)) !=
                        PageLatchOutcome::kAcquired) {
                        ++violations;
                    }
                    PageLatch::Release(words[w], static_cast<std::uint32_t>(core));
                    writers[w].fetch_sub(1);
                } else {
                    if (writers[w].load() != 0) ++violations;
                    readers[w].fetch_add(1);
                    // Held across a second shared hold, so a writer that
                    // slipped in would find the reader census non-zero.
                    if (PageLatch::TryAcquire(words[w], PageLatchMode::kShared,
                                              static_cast<std::uint32_t>(core)) !=
                        PageLatchOutcome::kAcquired) {
                        ++violations;
                    }
                    if (writers[w].load() != 0) ++violations;
                    PageLatch::Release(words[w], static_cast<std::uint32_t>(core));
                    readers[w].fetch_sub(1);
                }
                PageLatch::Release(words[w], static_cast<std::uint32_t>(core));
            }
            contended_turns.fetch_add(spun);
        });
    }
    for (auto& t : cores) t.join();

    EXPECT_EQ(violations.load(), 0) << "two holders met on one word in conflicting modes";
    EXPECT_GT(contended_turns.load(), 0u) << "eight threads over sixty-four words never contended";
    for (int w = 0; w < kWords; ++w) {
        EXPECT_EQ(words[static_cast<std::size_t>(w)], 0u) << "word " << w << " did not return to free";
        EXPECT_EQ(readers[static_cast<std::size_t>(w)].load(), 0);
        EXPECT_EQ(writers[static_cast<std::size_t>(w)].load(), 0);
    }
}

// ---- The store: where the pin is taken, the latch is taken ---------------
//
// Modelled on eviction_test.cpp's fixture: a DevicePageStore over a
// MemoryPageDevice, hand-built, which is exactly the kind of store the
// assembly never arms - so the unarmed arm is the default and the armed
// one is a deliberate SetLatchArmed(true).

class PageLatchStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto device = MemoryPageDevice::Create(/*extent_pages=*/8, /*initial_pages=*/0);
        ASSERT_TRUE(device.ok()) << device.status().message();
        device_ = std::move(device.value());
        auto store = DevicePageStore::Open(*device_, /*first_new_page_id=*/16);
        ASSERT_TRUE(store.ok()) << store.status().message();
        store_ = std::move(store.value());
    }

    // A fresh page, written, flushed and left resident and clean - the
    // frame a sweep can reclaim (eviction_test.cpp's helper).
    PageId MakeCleanResidentPage(std::byte fill) {
        auto created = store_->CreateNew();
        EXPECT_TRUE(created.ok()) << created.status().message();
        const PageId id = created.value().first;
        FormatPage(created.value().second.bytes(), PageType::kHeap);
        created.value().second.bytes()[kPageBodyOffset] = fill;
        EXPECT_TRUE(store_->Sync().ok());
        return id;
    }

    std::uint32_t WordOf(PageId id) {
        auto word = store_->latch_word_for_test(id);
        EXPECT_TRUE(word.ok()) << word.status().message();
        return word.ok() ? word.value() : 0xFFFFFFFFu;
    }

    std::unique_ptr<MemoryPageDevice> device_;
    std::unique_ptr<DevicePageStore> store_;
};

TEST_F(PageLatchStoreTest, AnUnarmedStoreNeverTouchesTheWord) {
    // The cores = 1 arm, and every hand-built store's: the word stays 0
    // across creation, both accessors, a flush and a sweep. G2's zero
    // overhead, asserted. Under the census run (`KDS_TEST_PAGE_LATCH=1`)
    // every store is armed at Open() and stays armed whatever SetLatchArmed
    // is told, so the unarmed path this cell is about does not exist there:
    // skipped, not faked.
    if (std::getenv("KDS_TEST_PAGE_LATCH") != nullptr) {
        GTEST_SKIP() << "the census override arms every store; this cell is about the unarmed path";
    }
    ASSERT_FALSE(store_->latch_armed()) << "a hand-built store is unarmed by default";
    const PageId a = MakeCleanResidentPage(std::byte{0x11});
    const PageId b = MakeCleanResidentPage(std::byte{0x22});
    {
        auto x = store_->Get(a);
        ASSERT_TRUE(x.ok());
        auto s = store_->GetForRead(b);
        ASSERT_TRUE(s.ok());
        EXPECT_EQ(WordOf(a), 0u);
        EXPECT_EQ(WordOf(b), 0u);
    }
    EXPECT_TRUE(store_->Sync().ok());
    (void)store_->EvictColdFrames(1);
    // The sweep may have reclaimed either page; whichever is still resident
    // carries a zero word.
    for (const PageId id : {a, b}) {
        auto word = store_->latch_word_for_test(id);
        if (word.ok()) EXPECT_EQ(word.value(), 0u) << "page " << id;
    }
}

TEST_F(PageLatchStoreTest, AnArmedStoreLatchesExclusiveOnGetAndSharedOnGetForRead) {
    store_->SetLatchArmed(true);
    const PageId a = MakeCleanResidentPage(std::byte{0x33});
    EXPECT_EQ(WordOf(a), 0u) << "a handle that died released its hold";
    {
        auto x = store_->Get(a);
        ASSERT_TRUE(x.ok());
        const PageLatchWord w = DecodePageLatch(WordOf(a));
        EXPECT_TRUE(w.exclusive);
        EXPECT_EQ(w.owner_core, 0u) << "a hand-built store is core 0's";
        EXPECT_EQ(w.count, 1u);
    }
    EXPECT_EQ(WordOf(a), 0u);
    {
        auto s = store_->GetForRead(a);
        ASSERT_TRUE(s.ok());
        const PageLatchWord w = DecodePageLatch(WordOf(a));
        EXPECT_FALSE(w.exclusive);
        EXPECT_EQ(w.count, 1u);
    }
    EXPECT_EQ(WordOf(a), 0u);
}

TEST_F(PageLatchStoreTest, OneTaskMayHoldAPageTwiceAndTheWordReturnsToFree) {
    // The tree's own shapes: a second exclusive handle on a page already
    // held (chain growth, the btree rebuild, LogFullPageImage's Get under
    // its caller's), a shared read under an exclusive hold (no production
    // path yet - page_latch.hpp says why the word admits it), and the
    // reassign idiom `page = store.Get(tail)` where the new pin lands
    // before the old handle's release (assertion_build.cpp).
    store_->SetLatchArmed(true);
    const PageId a = MakeCleanResidentPage(std::byte{0x44});
    {
        auto first = store_->Get(a);
        ASSERT_TRUE(first.ok());
        auto second = store_->Get(a);
        ASSERT_TRUE(second.ok());
        EXPECT_EQ(DecodePageLatch(WordOf(a)).count, 2u);
        auto third = store_->GetForRead(a);
        ASSERT_TRUE(third.ok());
        const PageLatchWord w = DecodePageLatch(WordOf(a));
        EXPECT_TRUE(w.exclusive) << "a shared hold under the task's own exclusive keeps it exclusive";
        EXPECT_EQ(w.count, 3u);
    }
    EXPECT_EQ(WordOf(a), 0u);

    {
        auto page = store_->Get(a);
        ASSERT_TRUE(page.ok());
        page = store_->Get(a);  // the reassign idiom: pin, then release the old
        ASSERT_TRUE(page.ok());
        EXPECT_EQ(DecodePageLatch(WordOf(a)).count, 1u);
    }
    EXPECT_EQ(WordOf(a), 0u);
}

TEST_F(PageLatchStoreTest, TheSweepAndEvictCleanRefuseAFrameAnotherCoreHolds) {
    // The AM-S2 shape, exercised now through the test hook: a hold taken
    // by core 7 has no pin in this table, and only the word says the frame
    // is spoken for. In eviction_test.cpp's discipline the sweep must
    // reclaim a *victim* in the same pass, or the refusal proves nothing.
    store_->SetLatchArmed(true);
    const PageId held = MakeCleanResidentPage(std::byte{0x55});
    const PageId victim = MakeCleanResidentPage(std::byte{0x66});
    ASSERT_TRUE(store_->LatchFrameForTest(held, PinMode::kShared, /*core=*/7).ok());

    const std::size_t reclaimed = store_->EvictColdFrames(/*budget=*/2);
    EXPECT_EQ(reclaimed, 1u) << "the sweep reclaimed the victim and only the victim";
    EXPECT_TRUE(store_->latch_word_for_test(held).ok()) << "the held frame is still resident";
    EXPECT_FALSE(store_->latch_word_for_test(victim).ok()) << "the victim fell";

    const std::array<PageId, 1> ids{held};
    const Status refused = store_->EvictClean(ids);
    EXPECT_EQ(refused.code(), StatusCode::kInvalidArgument) << refused.message();

    ASSERT_TRUE(store_->UnlatchFrameForTest(held, /*core=*/7).ok());
    EXPECT_EQ(WordOf(held), 0u);
    EXPECT_TRUE(store_->EvictClean(ids).ok()) << "released, the frame may go";
}

}  // namespace
}  // namespace kds::storage
