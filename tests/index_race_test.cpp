#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/storage/memory_page_device.hpp"

// **The secondary index with more than one core in it** (AT-S15).
//
// `index_tree_test.cpp` puts another core's divide into each gap of an
// insert's descent by hand, on the fetch that ends the gap. This file asks
// the same question with real threads on an armed latch, the way
// `btree_race_test.cpp` asks it of the clustered tree: two cores inserting
// into one leaf at the same instant, so one's divide lands between the
// other's shared read and its exclusive re-fetch, or between its parent
// read and its leaf read.
//
// **What is asserted is the tree, not one key**: after the rounds, the
// whole leaf chain ascends by sort key, and every entry either thread
// placed is found by a probe from the root. An entry written into a leaf
// that had given its key to a new right sibling breaks both: the chain
// holds a key above its successor's, and the probe - routed to the
// sibling - walks right past it.
//
// **Excluded by assertion, not by hope**: a root that divides during the
// rounds. That is the walk back up (window 2, AT-S16's), and a separator
// promoted into a parent another core divided is a different failure this
// cell must not be able to report as its own. The layout keeps the root
// far from full: covered bytes make a *leaf* entry wide (eight to a page,
// so divides are constant) and leave a *separator* narrow (hundreds to a
// page).
//
// **Mutation**: drop the coverage arm from `DescendTo` - killed 20 runs in
// 20 (2026-09-26, each run six fresh trees). The first draft AT-S5c
// rejected, a comparison of the leaf's right link across the descent's own
// two fetches, is killed 20 in 20 as well: with real threads the divide
// lands before the leaf is read about as often as between its two fetches.
// Before the fix every run failed (60 in 60).

namespace kds::index {
namespace {

constexpr std::uint16_t kKeyWidth = 4;
constexpr std::uint16_t kCoveredWidth = 1000;  // eight entries to a leaf

IndexLayout Layout() { return IndexLayout{kKeyWidth, kCoveredWidth}; }

std::vector<std::byte> Key(std::uint32_t v) {
    std::vector<std::byte> out(kKeyWidth, std::byte{0});
    for (std::uint16_t i = 0; i < kKeyWidth; ++i) {
        out[kKeyWidth - 1 - i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
    return out;
}

std::uint32_t KeyOf(std::span<const std::byte> key) {
    std::uint32_t v = 0;
    for (std::byte b : key) v = (v << 8) | static_cast<std::uint32_t>(b);
    return v;
}

std::unique_ptr<storage::DevicePageStore> ArmedStore(
    std::unique_ptr<storage::MemoryPageDevice>& device) {
    auto made = storage::MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    EXPECT_TRUE(made.ok()) << made.status().message();
    device = std::move(made.value());
    auto store = storage::DevicePageStore::Open(*device, /*first_new_page_id=*/16);
    EXPECT_TRUE(store.ok()) << store.status().message();
    // Armed: an unarmed latch never queues the second writer, and the
    // window is only reachable where the store is shared.
    store.value()->SetLatchArmed(true, /*concurrent_pinners=*/16);
    return std::move(store.value());
}

// `btree_race_test.cpp`'s rendezvous, for its reason: two threads started
// together drift onto different leaves within a few inserts, and then
// neither is queued on the other's latch. Re-synchronised before every
// insert, they reach one leaf at one instant. A generation counter, so a
// thread that leaves and re-arrives before its partner has left is not
// counted into the wrong round.
class Rendezvous {
  public:
    explicit Rendezvous(int parties) noexcept : parties_(parties) {}

    void Wait() noexcept {
        const int generation = generation_.load(std::memory_order_acquire);
        if (waiting_.fetch_add(1, std::memory_order_acq_rel) + 1 == parties_) {
            waiting_.store(0, std::memory_order_release);
            generation_.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation) {
            std::this_thread::yield();
        }
    }

  private:
    const int parties_;
    std::atomic<int> waiting_{0};
    std::atomic<int> generation_{0};
};

// Regions a stride apart, each prefilled to one leaf's worth, so a round's
// two inserts land inside one existing leaf rather than past the chain's
// end. Per region per round, thread t inserts `region * kStride + 1 +
// 2 * round + t`: unique, and inside the region.
constexpr std::uint32_t kRegions = 16;
constexpr std::uint32_t kStride = 1000;
constexpr std::uint32_t kPrefillPerRegion = 8;
constexpr int kRoundsPerRegion = 12;
constexpr int kThreads = 2;
// Fresh trees: one run of the rounds is a sample, not a proof.
constexpr int kRepeats = 6;

TEST(IndexRaceTest, TwoCoresDividingOneIndexLeaveEveryEntryWhereAProbeFindsIt) {
    const IndexLayout layout = Layout();
    const std::vector<std::byte> covered(kCoveredWidth, std::byte{0x5A});
    int misplaced_runs = 0;
    std::string first_failure;
    std::uint64_t restructured = 0;

    for (int repeat = 0; repeat < kRepeats; ++repeat) {
        std::unique_ptr<storage::MemoryPageDevice> device;
        auto store = ArmedStore(device);
        ASSERT_NE(store, nullptr);
        auto created = store->CreateNew();
        ASSERT_TRUE(created.ok()) << created.status().message();
        PageId root = created.value().first;
        ASSERT_TRUE(FormatRoot(created.value().second.bytes(), layout, /*owner_oid=*/0).ok());
        created.value().second.Release();

        std::vector<std::pair<std::uint32_t, std::uint64_t>> placed;
        for (std::uint32_t region = 0; region < kRegions; ++region) {
            for (std::uint32_t j = 0; j < kPrefillPerRegion; ++j) {
                const std::uint32_t key = region * kStride + 100 * j;
                auto r = IndexInsert(*store, root, layout, Key(key), key, covered, 0);
                ASSERT_TRUE(r.ok()) << "prefill " << key << ": " << r.status().message();
                if (r.value().new_root != kInvalidPageId) root = r.value().new_root;
                placed.emplace_back(key, key);
            }
        }

        Rendezvous gate(kThreads);
        std::atomic<bool> root_moved{false};
        std::atomic<std::uint64_t> divides{0};
        std::vector<std::string> errors;
        std::mutex errors_latch;
        std::vector<std::vector<std::pair<std::uint32_t, std::uint64_t>>> mine(kThreads);

        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                SetCurrentCore(static_cast<std::uint32_t>(t));
                for (int round = 0; round < kRoundsPerRegion; ++round) {
                    for (std::uint32_t region = 0; region < kRegions; ++region) {
                        gate.Wait();
                        const std::uint32_t key = region * kStride + 1 +
                                                  2 * static_cast<std::uint32_t>(round) +
                                                  static_cast<std::uint32_t>(t);
                        const std::uint64_t pk = 1'000'000 + key;
                        auto r = IndexInsert(*store, root, layout, Key(key), pk, covered, 0);
                        if (!r.ok()) {
                            const std::lock_guard<std::mutex> held(errors_latch);
                            errors.push_back(std::to_string(key) + ": " + r.status().message());
                            continue;
                        }
                        if (r.value().new_root != kInvalidPageId) {
                            root_moved.store(true, std::memory_order_release);
                        }
                        if (r.value().restructured()) divides.fetch_add(1);
                        mine[static_cast<std::size_t>(t)].emplace_back(key, pk);
                    }
                }
            });
        }
        for (std::thread& thread : threads) thread.join();
        SetCurrentCore(0);

        ASSERT_FALSE(root_moved.load(std::memory_order_acquire))
            << "the root grew during the rounds; this cell no longer isolates the leaf window";
        ASSERT_TRUE(errors.empty()) << errors.size() << " insert(s) failed, first: " << errors[0];
        restructured += divides.load();
        for (const auto& list : mine) placed.insert(placed.end(), list.begin(), list.end());

        // The chain ascends by sort key, across every leaf boundary.
        std::string failure;
        std::vector<std::byte> previous;
        Status walked = IndexVisit(
            *store, root, layout, storage::PageAccess::kRead,
            [&](PageId page_id, IndexLeafView& leaf,
                std::uint16_t idx) -> StatusOr<storage::VisitControl> {
                auto key = leaf.SortKey(idx);
                if (!key.ok()) return key.status();
                if (failure.empty() && !previous.empty() &&
                    std::memcmp(previous.data(), key.value().data(), previous.size()) > 0) {
                    failure = "key " + std::to_string(KeyOf(key.value().subspan(0, kKeyWidth))) +
                              " at entry " + std::to_string(idx) + " of leaf " +
                              std::to_string(page_id) + " sorts below its predecessor";
                }
                previous.assign(key.value().begin(), key.value().end());
                return storage::VisitControl::kContinue;
            });
        ASSERT_TRUE(walked.ok()) << walked.message();

        // Every entry is found by a probe from the root.
        for (const auto& [key, pk] : placed) {
            if (!failure.empty()) break;
            std::vector<std::byte> sort_key(layout.sort_key_width(), std::byte{0});
            const auto k = Key(key);
            std::memcpy(sort_key.data(), k.data(), k.size());
            auto leaf = IndexSeekLeaf(*store, root, layout, sort_key);
            ASSERT_TRUE(leaf.ok()) << leaf.status().message();
            bool found = false;
            Status probed = IndexVisitFrom(
                *store, leaf.value(), layout, storage::PageAccess::kRead,
                [&](PageId, IndexLeafView& page,
                    std::uint16_t idx) -> StatusOr<storage::VisitControl> {
                    auto entry = page.Entry(idx);
                    if (!entry.ok()) return entry.status();
                    const std::uint32_t at = KeyOf(entry.value().subspan(0, kKeyWidth));
                    if (at < key) return storage::VisitControl::kContinue;
                    if (at > key) return storage::VisitControl::kStop;
                    if (GetIndexPk(entry.value().subspan(kKeyWidth)) == pk) found = true;
                    return storage::VisitControl::kContinue;
                });
            ASSERT_TRUE(probed.ok()) << probed.message();
            if (!found) failure = "a probe for key " + std::to_string(key) + " missed pk " +
                                  std::to_string(pk);
        }

        if (!failure.empty()) {
            ++misplaced_runs;
            if (first_failure.empty()) first_failure = failure;
        }
    }

    EXPECT_GT(restructured, 0u) << "no insert divided a leaf; the cell tested nothing";
    EXPECT_EQ(0, misplaced_runs) << misplaced_runs << " of " << kRepeats
                                 << " runs left an entry where a probe cannot find it; first: "
                                 << first_failure;
}

}  // namespace
}  // namespace kds::index
