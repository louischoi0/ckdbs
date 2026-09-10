#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kds/base/current_core.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/btree/btree_page.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/memory_page_device.hpp"

// **The clustered B+ tree with more than one core in it** (AT-S5c, D3).
//
// `btree_test.cpp` is this file's sibling and asks every structural
// question one thread can ask. This one asks the question AT-S5 created:
// **a write descent cannot hold what it found.** The page latch is never
// upgraded (AM-S1), so `DescendTo` releases the leaf's shared hold before
// asking for it exclusive, and between those two lines the leaf is held by
// nobody. Until AT-S5 nothing could enter that window - a relation's writes
// all ran on its owner core and the reactor serialised them - and the code
// said so in as many words: *"Nothing can evict between the release and the
// re-fetch on a single-threaded core."* AT-S5 made a write run where the
// session is.
//
// **And the gap is wider than that re-fetch**, which these cells are how
// the stage found out. There is no latch coupling in the descent: an
// internal node is read, a child chosen, and the node released before the
// child is asked for - so the child's key range can shrink between the
// parent read and the leaf's arrival, before this descent has touched the
// leaf at all. A first draft of the fix compared the leaf's right link
// across the re-fetch alone; it closed the narrow window, and
// `TheLeafChainStillAscendsAfterConcurrentSplits` below still failed. What
// the descent checks now is coverage itself, asked of the chain as it
// stands (`LeafStillCoversKey`).
//
// ---- The three cells, and what each is about ---------------------------
//
//   - **The chain ascends** and every tuple sits inside its own leaf's
//     interval. This is the structural damage, and it is H9's failure
//     reached from a new direction: a leaf spliced in front of one whose
//     keys are lower.
//   - **A scan returns every row in order.** The same damage stated where a
//     user meets it: on a btree relation the chain *is* the ordered read.
//   - **A lookup does not miss a row a divide moved**, which is the read
//     path's half of the same gap and the one quiet answer of the three.
//
// **What is deliberately not asserted here.** The first draft's opening
// cell claimed a descent could not *find* a row the write path had
// misplaced, and it passed under the mutation six runs out of six: a
// splitter promotes the separator that routes to its new page, so the tree
// half stays self-consistent and it is the chain that breaks. The cell was
// replaced rather than kept, because a cell that survives its own mutation
// tests nothing.
//
// **Why this shape produces the interleaving rather than hoping for it.**
// Both threads enter the same leaf at the same instant - `Rendezvous` below
// says what that is worth and what it was measured against - so one holds
// the leaf's share while the other is queued on its exclusive latch. The
// release hands the leaf to the queued writer, which splits it, and the
// first writer's own re-fetch is handed a leaf that has given the key away.
//
// **Mutations**: drop the `LeafStillCoversKey` arm from `DescendTo`, and
// separately from `BtreeLookup`. Each was run ten times against these
// cells; each was killed ten times.

namespace kds::btree {
namespace {

// One tuple per leaf, so every insert into a populated leaf restructures
// it. `btree_test.cpp` derives the number; it is repeated rather than
// shared because a test that imports another test's constants breaks in
// two places at once.
inline constexpr std::size_t kOnePerLeafFiller = 5000;

std::vector<std::byte> MakeTuple(std::uint64_t id) {
    auto word = Keystone::Encode(id, 0, 0);
    EXPECT_TRUE(word.ok()) << word.status().message();
    std::vector<std::byte> out(kKeystoneWordSize + kOnePerLeafFiller, std::byte{0xAB});
    std::uint64_t v = word.ok() ? word.value() : 0;
    for (std::size_t i = 0; i < kKeystoneWordSize; ++i) {
        out[i] = static_cast<std::byte>(v & 0xFF);
        v >>= 8;
    }
    return out;
}

std::unique_ptr<storage::DevicePageStore> ArmedStore(
    std::unique_ptr<storage::MemoryPageDevice>& device) {
    auto made = storage::MemoryPageDevice::Create(/*extent_pages=*/512, /*initial_pages=*/0);
    EXPECT_TRUE(made.ok()) << made.status().message();
    device = std::move(made.value());
    auto store = storage::DevicePageStore::Open(*device, /*first_new_page_id=*/16);
    EXPECT_TRUE(store.ok()) << store.status().message();
    // Armed, for `free_map_race_test.cpp`'s reason: the question exists only
    // where the store is shared, and a store is only shared where it is
    // armed. Here it is also what makes the window reachable at all - an
    // unarmed latch never queues the second writer.
    store.value()->SetLatchArmed(true, /*concurrent_pinners=*/16);
    return std::move(store.value());
}

// The leaves this workload walks, and the ids that reach them. The ids are
// a thousand apart so every insert below lands *inside* the chain rather
// than past its end, which is the splice the window cares about.
// **Sized against the root, not against the race.** Every insert below
// splits a leaf, so the run adds `kLeaves * kRounds * kThreads` separators
// to a root that holds 678 (`btree_page.hpp`'s `kInternalMaxEntries`).
// Past that the root grows a level, which republishes a pointer both
// threads hold a copy of - a different race, and one this cell excludes by
// assertion rather than by hope. 20 x 6 x 2 + 20 = 260 leaves it a wide
// margin, and the rendezvous rather than the volume is what makes the
// window reliable: measured at 20/20 mutant kills over ten runs of both
// cells, where the same workload without it killed three in six.
inline constexpr std::uint64_t kLeaves = 20;
inline constexpr std::uint64_t kStride = 1000;
inline constexpr int kRounds = 6;
inline constexpr int kThreads = 2;

// **The rendezvous, and why the cell is worth nothing without it.**
// Measured: with the threads merely started together and left to walk the
// chain, the mutant below survived **three runs in six** - the two drift
// apart after a few inserts, and once they are on different leaves neither
// is queued on the other's latch and the window never opens. What produces
// it is both threads entering `BtreeInsert` for the *same* leaf at the same
// instant: one takes the share, the other queues on the exclusive, and the
// release hands the leaf straight to a splitter while the first is still
// between its two fetches. So they are re-synchronised before every insert
// rather than only at the start.
//
// A generation counter rather than a count alone: a thread that leaves the
// barrier and arrives at the next one before its partner has left the first
// would otherwise be counted into the wrong round.
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

// ~1 KB payloads: several tuples per leaf, which is what a *divide* needs.
// The one-per-leaf filler above can only ever append - `SplitLeafAndInsert`
// refuses a page with fewer than two live tuples - and a divide is the only
// split that *moves* a key out from under a reader.
inline constexpr std::size_t kSmallFiller = 56;

std::vector<std::byte> MakeSmallTuple(std::uint64_t id) {
    auto word = Keystone::Encode(id, 0, 0);
    EXPECT_TRUE(word.ok()) << word.status().message();
    std::vector<std::byte> out(kKeystoneWordSize + kSmallFiller, std::byte{0xCD});
    std::uint64_t v = word.ok() ? word.value() : 0;
    for (std::size_t i = 0; i < kKeystoneWordSize; ++i) {
        out[i] = static_cast<std::byte>(v & 0xFF);
        v >>= 8;
    }
    return out;
}

TEST(BtreeRaceTest, ALookupDoesNotMissARowADivideMovedUnderIt) {
    // **The read half of the same gap.** A read descent takes no re-fetch
    // window - it holds what it finds - but it releases the parent before it
    // asks for the child, exactly as the write descent does. A **divide** on
    // another core moves the upper half of that child's keys to a new page
    // in between, and a lookup routed to the old leaf then finds nothing.
    //
    // `NotFound` is the whole damage and it is a quiet one: `btree.hpp` says
    // a descent is authoritative, so callers do not fall back to a scan. The
    // row is simply reported absent.
    //
    // **Mutation**: drop the `LeafStillCoversKey` arm from `BtreeLookup` and
    // the misses below appear.
    std::unique_ptr<storage::MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);

    auto created = store->CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    PageId current_root = created.value().first;
    ASSERT_TRUE(FormatRoot(created.value().second.bytes(), /*owner_oid=*/0).ok());
    created.value().second.Release();

    // Dense enough to fill leaves, and spaced by ten so the divider below
    // has somewhere to land *inside* one. **Sized against the root like the
    // other two**: 600 rows at ~1 KB is well under an internal node's 678
    // separators even after the divider doubles the leaf count, and
    // `root_moved` below is what says so rather than this arithmetic.
    constexpr std::uint64_t kRows = 600;
    constexpr int kProbesPerRound = 8;
    std::vector<std::uint64_t> present;
    for (std::uint64_t n = 1; n <= kRows; ++n) {
        const std::uint64_t id = n * 10;
        auto r = BtreeInsert(*store, current_root, id, MakeSmallTuple(id), /*trx_id=*/1,
                             /*owner_oid=*/0);
        ASSERT_TRUE(r.ok()) << "prefill " << id << ": " << r.status().message();
        if (r.value().new_root != kInvalidPageId) current_root = r.value().new_root;
        present.push_back(id);
    }

    Rendezvous gate(kThreads);
    std::atomic<bool> root_moved{false};
    std::vector<std::string> misses;
    std::mutex misses_latch;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    // Thread 0 divides; thread 1 reads. They meet on the same leaf at each
    // rendezvous, which is what puts the reader's descent on the far side of
    // the divider's promotion.
    threads.emplace_back([&] {
        SetCurrentCore(0);
        for (std::uint64_t n = 1; n <= kRows; ++n) {
            gate.Wait();
            const std::uint64_t id = n * 10 + 5;  // sorts *inside* a full leaf
            auto r = BtreeInsert(*store, current_root, id, MakeSmallTuple(id), /*trx_id=*/1,
                                 /*owner_oid=*/0);
            if (r.ok() && r.value().new_root != kInvalidPageId) {
                root_moved.store(true, std::memory_order_release);
            }
        }
    });
    threads.emplace_back([&] {
        SetCurrentCore(1);
        for (std::uint64_t n = 1; n <= kRows; ++n) {
            gate.Wait();
            // **Sampled across the divider's whole split, not once per
            // round.** A divide is a page's worth of work - copy out,
            // create, refill both halves, promote - and the reader's
            // descent is a few fetches, so one lookup per rendezvous lands
            // inside the gap about half the time. Measured: 5 kills in 10
            // runs at one, and 10 in 10 at this width. The keys sweep the
            // leaf around the inserted one, because which rows travel
            // depends on where the median falls.
            for (int probe = 0; probe < kProbesPerRound; ++probe) {
                const std::uint64_t id =
                    (n + static_cast<std::uint64_t>(probe % 4)) * 10;
                if (id > kRows * 10) continue;
                auto found = BtreeLookup(*store, current_root, id);
                if (!found.ok() && found.status().code() == StatusCode::kNotFound) {
                    const std::lock_guard<std::mutex> held(misses_latch);
                    misses.push_back(std::to_string(id));
                }
            }
        }
    });
    for (std::thread& thread : threads) thread.join();
    SetCurrentCore(0);

    ASSERT_FALSE(root_moved.load(std::memory_order_acquire))
        << "the root grew a level during the concurrent phase; this cell no longer isolates "
           "the descent's gap";

    std::string named;
    for (std::size_t i = 0; i < misses.size() && i < 8; ++i) named += " " + misses[i];
    EXPECT_TRUE(misses.empty())
        << misses.size() << " lookup(s) answered NotFound for a row that was never deleted:"
        << named;

    // And the rows are all still there when nothing is moving, which is what
    // says the misses above would have been about the race and not about a
    // prefill that never landed.
    for (std::uint64_t id : present) {
        auto found = BtreeLookup(*store, current_root, id);
        EXPECT_TRUE(found.ok()) << "id " << id << " is gone after the run: "
                                << found.status().message();
    }
}

TEST(BtreeRaceTest, AScanOfTheChainStillReturnsEveryRowInOrder) {
    // **What the damage looks like from outside the storage layer.** On a
    // btree relation the chain *is* the ordered scan: `ORDER BY <pk> ASC`
    // is served by walking `next_page_id` and reading each leaf, with no
    // sort, because the leaves ascend. A tuple written into a leaf that had
    // already handed its key range away leaves that ordering false, so the
    // scan answers rows out of order and every caller that reads the walk
    // as sorted - the top-N under `LIMIT`, the range scan, the
    // page-wise duplicate check - is answering from a premise the pages no
    // longer support.
    std::unique_ptr<storage::MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);

    auto created = store->CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    PageId current_root = created.value().first;
    ASSERT_TRUE(FormatRoot(created.value().second.bytes(), /*owner_oid=*/0).ok());
    created.value().second.Release();

    for (std::uint64_t k = 1; k <= kLeaves; ++k) {
        auto r = BtreeInsert(*store, current_root, k * kStride, MakeTuple(k * kStride),
                             /*trx_id=*/1, /*owner_oid=*/0);
        ASSERT_TRUE(r.ok()) << "prefill " << k * kStride << ": " << r.status().message();
        if (r.value().new_root != kInvalidPageId) current_root = r.value().new_root;
    }

    // **The root may not move during the concurrent phase, and the cell
    // says so rather than hoping.** A level growth republishes the root,
    // which every thread holds a copy of - a different race, and not this
    // one. Twenty leaves plus the inserts below stay far under an internal
    // node's 678 separators, so the growth cannot happen; the flag is here
    // so that a future change to the numbers fails loudly instead of
    // quietly testing something else.
    std::atomic<bool> root_moved{false};
    Rendezvous gate(kThreads);

    std::vector<std::vector<std::uint64_t>> landed(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            // **Distinct cores, or the cell tests nothing**: the page latch
            // is re-entrant for the core that holds it, so two threads
            // claiming core 0 would both be admitted to a leaf one of them
            // holds exclusive.
            SetCurrentCore(static_cast<std::uint32_t>(t));

            for (int round = 0; round < kRounds; ++round) {
                for (std::uint64_t k = 1; k <= kLeaves; ++k) {
                    // Both threads enter the same leaf together, with
                    // different ids in it.
                    gate.Wait();
                    const std::uint64_t id =
                        k * kStride + static_cast<std::uint64_t>(round) * 100 +
                        static_cast<std::uint64_t>(t) * 10 + 1;
                    auto r = BtreeInsert(*store, current_root, id, MakeTuple(id),
                                         /*trx_id=*/1, /*owner_oid=*/0);
                    if (r.ok()) {
                        if (r.value().new_root != kInvalidPageId) {
                            root_moved.store(true, std::memory_order_release);
                        }
                        landed[static_cast<std::size_t>(t)].push_back(id);
                    }
                    // A refusal is not a failure of this cell. `TxnConflict`
                    // is the descent giving up after `kMaxDescentRestarts`,
                    // which is a legal answer under this much churn; what
                    // the cell asserts is about the rows that *did* land.
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    SetCurrentCore(0);

    ASSERT_FALSE(root_moved.load(std::memory_order_acquire))
        << "the root grew a level during the concurrent phase; this cell no longer isolates "
           "the descent's window";

    std::vector<std::uint64_t> expected;
    for (const std::vector<std::uint64_t>& ids : landed) {
        expected.insert(expected.end(), ids.begin(), ids.end());
    }
    for (std::uint64_t k = 1; k <= kLeaves; ++k) expected.push_back(k * kStride);
    // Not a threshold on the race, a threshold on the *workload*: if the
    // inserts were nearly all refused there is nothing left to scan and the
    // assertions below would pass vacuously.
    ASSERT_GT(expected.size(), static_cast<std::size_t>(kLeaves * (kRounds + 1)))
        << "too few inserts landed; the cell would be asserting over almost nothing";
    std::sort(expected.begin(), expected.end());

    // The scan: the chain from its leftmost leaf, in the order the pages
    // link, exactly as an ordered read of a btree relation walks it.
    auto leftmost = BtreeLeftmostLeaf(*store, current_root);
    ASSERT_TRUE(leftmost.ok()) << leftmost.status().message();

    std::vector<std::uint64_t> scanned;
    PageId page = leftmost.value();
    std::size_t leaves_walked = 0;
    while (page != kInvalidPageId) {
        auto bytes = store->GetForRead(page);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        heap::PageView leaf(bytes.value().bytes());
        const std::uint16_t n = leaf.slot_count();
        // Within a leaf the slots are not ordered, which the ordered read
        // handles by sorting the page - the chain is what carries the order
        // *between* pages, and that is what this cell is about.
        std::vector<std::uint64_t> in_page;
        for (std::uint16_t slot = 0; slot < n; ++slot) {
            auto payload = leaf.PayloadAt(slot, n);
            if (payload.status().code() == StatusCode::kNotFound) continue;
            ASSERT_TRUE(payload.ok()) << payload.status().message();
            auto id = KeystoneIdOfPayload(payload.value());
            ASSERT_TRUE(id.ok()) << id.status().message();
            in_page.push_back(id.value());
        }
        std::sort(in_page.begin(), in_page.end());
        scanned.insert(scanned.end(), in_page.begin(), in_page.end());
        page = leaf.next_page_id();
        ++leaves_walked;
        ASSERT_LT(leaves_walked, std::size_t{4096}) << "the leaf chain does not terminate";
    }

    // **In order.** Reported before completeness, because it is the
    // assertion that names the defect: a walk that is not ascending is a
    // sorted answer that is not sorted.
    for (std::size_t i = 1; i < scanned.size(); ++i) {
        ASSERT_GT(scanned[i], scanned[i - 1])
            << "the chain walk returned " << scanned[i] << " after " << scanned[i - 1]
            << " at position " << i << " of " << scanned.size()
            << "; an ordered read of this relation is not ordered";
    }
    // **And complete**, which is the other half of what a scan owes.
    EXPECT_EQ(scanned, expected) << "the chain walk did not return exactly the rows that landed";
}

TEST(BtreeRaceTest, TheLeafChainStillAscendsAfterConcurrentSplits) {
    // The other half of the same damage, and the one H9 named: a leaf whose
    // tuples were written past its own range leaves the *chain* descending
    // where `heap-and-tuple.md` §3.1b requires it to ascend. Asserted over
    // the whole chain rather than over the rows, so it holds even for a
    // tuple no lookup asked about.
    std::unique_ptr<storage::MemoryPageDevice> device;
    auto store = ArmedStore(device);
    ASSERT_NE(store, nullptr);

    auto created = store->CreateNew();
    ASSERT_TRUE(created.ok()) << created.status().message();
    PageId current_root = created.value().first;
    ASSERT_TRUE(FormatRoot(created.value().second.bytes(), /*owner_oid=*/0).ok());
    created.value().second.Release();

    for (std::uint64_t k = 1; k <= kLeaves; ++k) {
        auto r = BtreeInsert(*store, current_root, k * kStride, MakeTuple(k * kStride),
                             /*trx_id=*/1, /*owner_oid=*/0);
        ASSERT_TRUE(r.ok()) << r.status().message();
        if (r.value().new_root != kInvalidPageId) current_root = r.value().new_root;
    }

    Rendezvous gate(kThreads);
    std::atomic<bool> root_moved{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            SetCurrentCore(static_cast<std::uint32_t>(t));
            for (int round = 0; round < kRounds; ++round) {
                for (std::uint64_t k = 1; k <= kLeaves; ++k) {
                    gate.Wait();
                    const std::uint64_t id =
                        k * kStride + static_cast<std::uint64_t>(round) * 100 +
                        static_cast<std::uint64_t>(t) * 10 + 1;
                    auto r = BtreeInsert(*store, current_root, id, MakeTuple(id),
                                         /*trx_id=*/1, /*owner_oid=*/0);
                    if (r.ok() && r.value().new_root != kInvalidPageId) {
                        root_moved.store(true, std::memory_order_release);
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) thread.join();
    SetCurrentCore(0);

    ASSERT_FALSE(root_moved.load(std::memory_order_acquire))
        << "the root grew a level during the concurrent phase; this cell no longer isolates "
           "the descent's window";

    // Walk the chain from its leftmost leaf. Each leaf's every live id must
    // sit at or above its own `min_key` and strictly below the next leaf's,
    // which is the interval a descent promises and the ordering a scan
    // depends on.
    auto leftmost = BtreeLeftmostLeaf(*store, current_root);
    ASSERT_TRUE(leftmost.ok()) << leftmost.status().message();

    PageId page = leftmost.value();
    std::uint64_t previous_min = 0;
    bool first = true;
    std::size_t leaves_walked = 0;
    while (page != kInvalidPageId) {
        auto bytes = store->GetForRead(page);
        ASSERT_TRUE(bytes.ok()) << bytes.status().message();
        heap::PageView leaf(bytes.value().bytes());
        const std::uint64_t min_key = leaf.min_key();
        if (!first) {
            EXPECT_GT(min_key, previous_min)
                << "leaf " << page << "'s min_key " << min_key
                << " does not exceed its predecessor's " << previous_min
                << "; the chain descends";
        }
        const PageId next = leaf.next_page_id();

        // The upper bound needs the next leaf's min_key, so it is read
        // before this page's tuples are judged. `min_key` is immutable
        // (invariant 2), so the two reads cannot disagree with each other.
        std::uint64_t next_min = 0;
        bool bounded = false;
        if (next != kInvalidPageId) {
            auto next_bytes = store->GetForRead(next);
            ASSERT_TRUE(next_bytes.ok()) << next_bytes.status().message();
            next_min = heap::PageView(next_bytes.value().bytes()).min_key();
            bounded = true;
        }

        const std::uint16_t n = leaf.slot_count();
        for (std::uint16_t slot = 0; slot < n; ++slot) {
            auto payload = leaf.PayloadAt(slot, n);
            if (payload.status().code() == StatusCode::kNotFound) continue;
            ASSERT_TRUE(payload.ok()) << payload.status().message();
            auto id = KeystoneIdOfPayload(payload.value());
            ASSERT_TRUE(id.ok()) << id.status().message();
            EXPECT_GE(id.value(), min_key)
                << "id " << id.value() << " is below leaf " << page << "'s min_key " << min_key;
            if (bounded) {
                EXPECT_LT(id.value(), next_min)
                    << "id " << id.value() << " sits in leaf " << page
                    << " but belongs past its right sibling's min_key " << next_min
                    << "; a descent for it will look on the other page";
            }
        }

        previous_min = min_key;
        first = false;
        page = next;
        ++leaves_walked;
        ASSERT_LT(leaves_walked, std::size_t{4096}) << "the leaf chain does not terminate";
    }
    EXPECT_GT(leaves_walked, static_cast<std::size_t>(kLeaves))
        << "the concurrent phase split nothing; the cell asserts over a tree it did not stress";
}

}  // namespace
}  // namespace kds::btree
