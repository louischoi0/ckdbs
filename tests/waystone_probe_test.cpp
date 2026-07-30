#include "kds/stats/waystone_probe.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>

#include <gtest/gtest.h>

#include "kds/stats/waystone_dir.hpp"
#include "kds/stats/waystone_hooks.hpp"
#include "kds/storage/in_memory_page_store.hpp"

// The probe (waystone-concpets.md sections 3.1 and 9, spec test 12-5).
//
// Almost every test here is about a *miss*. That is the point: the probe's
// value is that it is fast, but its correctness is entirely in refusing to
// answer when it should not, because a trusted-but-wrong location is the
// one failure mode that turns an advisory structure into a wrong row.

namespace kds::stats {
namespace {

// An epoch source a test can move, standing in for the [OPEN] decision
// about where a real one lives.
class FakeEpochs final : public EpochProvider {
public:
    std::uint32_t EpochOf(PageId page_id) const noexcept override {
        auto it = epochs_.find(page_id);
        return it == epochs_.end() ? 0u : it->second;
    }
    void BumpFor(PageId page_id) noexcept override { ++epochs_[page_id]; }

private:
    mutable std::unordered_map<PageId, std::uint32_t> epochs_;
};

// Stands in for "read the Keystone id of the tuple at (page, slot)". The
// real one goes through PageView; a table keeps this test independent of
// the heap format.
struct FakeHeap {
    std::unordered_map<std::uint64_t, std::uint64_t> id_at;  // (page<<32|slot) -> pk

    static std::uint64_t Key(PageId page_id, std::uint16_t slot) {
        return (static_cast<std::uint64_t>(page_id) << 32) | slot;
    }
    void Put(PageId page_id, std::uint16_t slot, std::uint64_t pk) {
        id_at[Key(page_id, slot)] = pk;
    }
};

std::optional<std::uint64_t> ReadIdFromFakeHeap(storage::PageStore&, PageId page_id,
                                                std::uint16_t slot, void* ctx) {
    auto* heap = static_cast<FakeHeap*>(ctx);
    auto it = heap->id_at.find(FakeHeap::Key(page_id, slot));
    if (it == heap->id_at.end()) return std::nullopt;
    return it->second;
}

class WaystoneProbeTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto root = CreateDirPage(store_);
        ASSERT_TRUE(root.ok()) << root.status().message();
        ws_.dir_root = root.value();
        ws_.depth = 1;
    }

    // Insert a tuple into both the fake heap and Waystone, consistently.
    void Insert(std::uint64_t pk, PageId page_id, std::uint16_t slot) {
        heap_.Put(page_id, slot, pk);
        ASSERT_TRUE(OnInsert(store_, ws_, pk, page_id, slot, epochs_.EpochOf(page_id)).ok());
    }

    storage::InMemoryPageStore store_{1};
    WaystoneRef ws_;
    FakeEpochs epochs_;
    FakeHeap heap_;
};

// ---- The hit ------------------------------------------------------------

TEST_F(WaystoneProbeTest, AFreshlyInsertedTupleProbesToWhereItWent) {
    Insert(/*pk=*/42, /*page_id=*/900, /*slot=*/7);

    auto result = Probe(store_, ws_, epochs_, 42);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_TRUE(result.value().trusted);
    EXPECT_EQ(result.value().page_id, 900u);
    EXPECT_EQ(result.value().slot, 7);
}

TEST_F(WaystoneProbeTest, ProbingIsIndependentOfHowManyTuplesExist) {
    // The property the whole exercise is for: a scan is O(rows), this is
    // not. Asserted structurally - the last pk costs the same page touches
    // as the first - since a unit test cannot time it meaningfully.
    for (std::uint64_t pk = 1; pk <= 2000; ++pk) {
        Insert(pk, static_cast<PageId>(900 + pk / 200), static_cast<std::uint16_t>(pk % 200));
    }
    const std::size_t settled = store_.page_count();

    for (const std::uint64_t pk : {std::uint64_t{1}, std::uint64_t{1000}, std::uint64_t{2000}}) {
        auto result = Probe(store_, ws_, epochs_, pk);
        ASSERT_TRUE(result.ok());
        EXPECT_TRUE(result.value().trusted) << "pk " << pk;
    }
    EXPECT_EQ(store_.page_count(), settled) << "a probe must never allocate";
}

// ---- The four ways to miss ----------------------------------------------

TEST_F(WaystoneProbeTest, APkWithNoEntryPageIsAMissNotAnError) {
    auto result = Probe(store_, ws_, epochs_, 12345);
    ASSERT_TRUE(result.ok()) << "a miss is an answer, not a failure";
    EXPECT_FALSE(result.value().trusted);
}

TEST_F(WaystoneProbeTest, APkWithAZeroedEntryOnAnExistingPageIsAMiss) {
    Insert(1, 900, 0);  // allocates the entry page pk 2 would share
    auto result = Probe(store_, ws_, epochs_, 2);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().trusted);
}

TEST_F(WaystoneProbeTest, ADeletedTupleIsAMiss) {
    Insert(5, 900, 1);
    ASSERT_TRUE(OnDelete(store_, ws_, 5).ok());

    auto result = Probe(store_, ws_, epochs_, 5);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().trusted)
        << "a cleared kEntryLive must not license a read of the location";
}

TEST_F(WaystoneProbeTest, AnEpochBumpMakesTheLocationUntrusted) {
    Insert(9, 900, 3);
    ASSERT_TRUE(Probe(store_, ws_, epochs_, 9).value().trusted);

    epochs_.BumpFor(900);  // something rearranged that page

    auto result = Probe(store_, ws_, epochs_, 9);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().trusted);
}

TEST_F(WaystoneProbeTest, HeatSurvivesAnEpochBump) {
    // Location and heat are separate facts (spec section 5): the tuple is
    // still as hot as it was, we merely no longer know where it is.
    Insert(9, 900, 3);
    {
        auto leaf = LookupOrCreateEntryPage(store_, ws_.dir_root, ws_.depth, 9);
        ASSERT_TRUE(leaf.ok());
        auto bytes = store_.Get(leaf.value());
        ASSERT_TRUE(bytes.ok());
        auto entry = ReadEntry(std::span<const std::byte, kPageSize>(bytes.value()),
                               EntrySlotOf(9));
        ASSERT_TRUE(entry.ok());
        WaystoneEntry hot = entry.value();
        hot.use_count = 77;
        ASSERT_TRUE(WriteEntry(bytes.value(), EntrySlotOf(9), hot).ok());
    }

    epochs_.BumpFor(900);
    auto result = Probe(store_, ws_, epochs_, 9);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().trusted);
    EXPECT_EQ(result.value().use_count, 77u) << "an untrusted location still carries its heat";
}

// ---- Re-observation -----------------------------------------------------

TEST_F(WaystoneProbeTest, ReObservingRestoresTrustAndKeepsHeat) {
    Insert(9, 900, 3);
    epochs_.BumpFor(900);
    ASSERT_FALSE(Probe(store_, ws_, epochs_, 9).value().trusted);

    // The authoritative path found it - somewhere else, as it happens.
    ASSERT_TRUE(Observe(store_, ws_, 9, /*page_id=*/901, /*slot=*/12,
                        epochs_.EpochOf(901))
                    .ok());

    auto result = Probe(store_, ws_, epochs_, 9);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().trusted);
    EXPECT_EQ(result.value().page_id, 901u);
    EXPECT_EQ(result.value().slot, 12);
}

TEST_F(WaystoneProbeTest, ObservingDoesNotResurrectADeletedEntry) {
    Insert(5, 900, 1);
    ASSERT_TRUE(OnDelete(store_, ws_, 5).ok());

    ASSERT_TRUE(Observe(store_, ws_, 5, 901, 2, 0).ok());
    EXPECT_FALSE(Probe(store_, ws_, epochs_, 5).value().trusted)
        << "re-observation must not undo a delete-mark";
}

TEST_F(WaystoneProbeTest, ObservingAnUncoveredPkIsAHarmlessNoOp) {
    EXPECT_TRUE(Observe(store_, ws_, 4321, 900, 0, 0).ok());
    EXPECT_EQ(store_.page_count(), 1u) << "observation must not allocate coverage";
}

// ---- The Keystone-id check ----------------------------------------------

TEST_F(WaystoneProbeTest, VerifyPassesWhenTheTupleThereReallyIsThePkAskedFor) {
    Insert(42, 900, 7);
    auto result = ProbeAndVerify(store_, ws_, epochs_, 42, ReadIdFromFakeHeap, &heap_);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(result.value().trusted);
    EXPECT_EQ(result.value().page_id, 900u);
}

TEST_F(WaystoneProbeTest, AStaleEntryPointingAtADifferentTupleIsDemotedToAMiss) {
    // The failure the amendment's rule 2 exists for, and the case spec
    // section 12-3 asks to be tested by name: an entry that passes the
    // epoch check and names a slot now holding somebody else. Without the
    // id check this returns the wrong row - not a slow answer, a wrong one.
    Insert(42, 900, 7);
    heap_.Put(900, 7, /*pk=*/999);  // the tuple moved on; nothing bumped the epoch

    auto result = ProbeAndVerify(store_, ws_, epochs_, 42, ReadIdFromFakeHeap, &heap_);
    ASSERT_TRUE(result.ok()) << "a stale entry is a miss, never a failure";
    EXPECT_FALSE(result.value().trusted);
}

TEST_F(WaystoneProbeTest, AnEntryPointingAtAnEmptySlotIsDemotedToAMiss) {
    Insert(42, 900, 7);
    heap_.id_at.clear();  // slot retired underneath us

    auto result = ProbeAndVerify(store_, ws_, epochs_, 42, ReadIdFromFakeHeap, &heap_);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().trusted);
}

TEST_F(WaystoneProbeTest, VerifyWithoutAnIdReaderIsRefusedRatherThanSkipped) {
    // Failing closed: the one thing that must never happen is a trusted
    // location handed back with the check quietly not performed.
    Insert(42, 900, 7);
    auto result = ProbeAndVerify(store_, ws_, epochs_, 42, nullptr, nullptr);
    EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(WaystoneProbeTest, AMissSkipsTheIdReaderEntirely) {
    // No location, nothing to verify - and no reason to touch the heap.
    auto result = ProbeAndVerify(store_, ws_, epochs_, 7777, nullptr, nullptr);
    ASSERT_TRUE(result.ok()) << "a miss must not reach the null-reader check";
    EXPECT_FALSE(result.value().trusted);
}

// ---- Caller errors are still errors -------------------------------------

TEST_F(WaystoneProbeTest, AnUnusableDirectoryIsAnErrorNotAMiss) {
    const WaystoneRef none;
    EXPECT_EQ(Probe(store_, none, epochs_, 1).status().code(), StatusCode::kInvalidArgument);
}

TEST_F(WaystoneProbeTest, APkPastTheDirectoryDepthIsOutOfRange) {
    EXPECT_EQ(Probe(store_, ws_, epochs_, DirCoverageAtDepth(1)).status().code(),
              StatusCode::kOutOfRange);
}

// ---- The advisory contract ----------------------------------------------

TEST_F(WaystoneProbeTest, EveryProbeMissesAfterTheEntryPagesAreDropped) {
    // Spec section 12-3: deleting the structure wholesale costs
    // performance and changes no result. The probe half of that is that
    // every lookup degrades to a miss - never to a wrong answer, and never
    // to an error a query would have to surface.
    for (std::uint64_t pk = 1; pk <= 300; ++pk) {
        Insert(pk, 900, static_cast<std::uint16_t>(pk));
    }

    // Drop the directory by pointing the relation at a fresh empty root -
    // the same observable state as having deleted its pages.
    auto empty_root = CreateDirPage(store_);
    ASSERT_TRUE(empty_root.ok());
    ws_.dir_root = empty_root.value();

    for (std::uint64_t pk = 1; pk <= 300; ++pk) {
        auto result = Probe(store_, ws_, epochs_, pk);
        ASSERT_TRUE(result.ok()) << "pk " << pk;
        EXPECT_FALSE(result.value().trusted);
    }
}

}  // namespace
}  // namespace kds::stats
