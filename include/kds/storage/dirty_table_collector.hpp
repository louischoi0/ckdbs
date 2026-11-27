#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "kds/wal/payload.hpp"

// **A checkpoint run's dirty table, taken in slices** (DZ-S1,
// `instructions/v3.2.0/workorder-dz-checkpoint-reactor-residue.md`).
//
// DZ-S0 measured the run's start - one call of `DirtyPagesWithRecLsn` and
// its projection - holding its core's reactor 28-73 ms at 1 Mi dirty frames,
// and the publish's collection about 10 ms. This is the same walk and the
// same result - sorted by page, one entry a page with its lowest non-zero
// recLSN (`DevicePageStore::MergeDirtyTable`'s) - in pieces of bounded work
// the run task yields between:
//
//   walk     `kChunksPerSlice` slot chunks a slice (`WalkDirtySlice`)
//   count    both radix digits' histograms, `kEntriesPerSlice` a slice
//   scatter  the low 16 bits into the scratch, then the high 16 back,
//            `kEntriesPerSlice` a slice each - an LSD radix sort by page id,
//            which a slice can stop and resume where a comparison sort
//            cannot
//   merge    one entry a page, its lowest non-zero recLSN
//
// **Why a yield between slices is sound** is DZ-S0's re-read of DO's §5a
// (the order's §7): every window a yield opens is a page dirtied after its
// slot was visited, which the publish of the same run writes before its
// anchor lands. What the yield does break is the begin record's
// active-transaction table, which the checkpointer therefore takes after
// the collection (DZ-R1).
//
// Holds nothing between slices: no latch, no pin, only its own vectors.
// One thread drives it.

namespace kds::storage {

class DevicePageStore;

class DirtyTableCollector {
public:
    // **The slice sizes, derived from DZ-R5's bar** (DZ-Q3 (a)). The bar is
    // no reactor hold above 10 ms; a slice is sized at about a tenth of it,
    // so the run's longest poll stays under the bar on a host that runs a
    // slice several times slower than DZ-S0's did. DZ-S0's walk cost 20-40 us
    // a 1,024-slot chunk at 1 Mi frames under three readers (41.7 ms for the
    // start's walk, projection and sort included): 16 chunks is 0.3-0.7 ms.
    // A scatter touches two random cache lines an entry, about 5-10 ns: 64 Ki
    // entries is 0.3-0.7 ms; the count and the merge are cheaper.
    static constexpr std::size_t kChunksPerSlice = 16;
    static constexpr std::size_t kEntriesPerSlice = 64 * 1024;

    // `chunks_per_slice` is `kChunksPerSlice` but in a cell that needs a
    // store of a few chunks to take several slices.
    explicit DirtyTableCollector(const DevicePageStore& store,
                                 std::size_t chunks_per_slice = kChunksPerSlice) noexcept
        : store_(&store), chunks_per_slice_(chunks_per_slice) {}

    // A collector over `entries` already walked: the sort and the merge
    // alone. Its test's door.
    static DirtyTableCollector ForEntriesForTest(std::vector<wal::CheckpointDirtyPage> entries);

    // One slice of the work: a walk slice, or up to `kEntriesPerSlice`
    // entries of the sort's passes, which share the budget (the call that
    // ends the walk spends one too, so a small table is whole in one call).
    // True once `table()` is whole; further calls return true and do nothing.
    bool Advance();

    // Whole once `Advance` returned true.
    std::vector<wal::CheckpointDirtyPage>& table() noexcept { return entries_; }

private:
    DirtyTableCollector() noexcept = default;

    enum class Phase : std::uint8_t { kWalk, kCount, kScatterLow, kScatterHigh, kMerge, kDone };
    static constexpr std::size_t kDigits = std::size_t{1} << 16;

    // Ends the walk: the scratch sized, the histograms cleared.
    void BeginSort();
    // Up to `budget` entries of the current pass, moving to the next pass at
    // its end; the entries done.
    std::size_t Pass(std::size_t budget);

    const DevicePageStore* store_ = nullptr;
    std::size_t chunks_per_slice_ = kChunksPerSlice;
    Phase phase_ = Phase::kWalk;
    std::size_t next_slot_ = 0;
    std::size_t at_ = 0;   // the next entry of the current pass
    std::size_t out_ = 0;  // the merge's write position
    std::vector<wal::CheckpointDirtyPage> entries_;
    // The scatter's other buffer, never zero-filled (the entry is trivial):
    // its pages fault in across the low pass's slices, not in one poll.
    std::unique_ptr<wal::CheckpointDirtyPage[]> scratch_;
    // Each digit's count, then, after the count, its next write position.
    std::vector<std::uint32_t> low_;
    std::vector<std::uint32_t> high_;
};

}  // namespace kds::storage
