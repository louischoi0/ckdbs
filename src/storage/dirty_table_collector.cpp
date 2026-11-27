#include "kds/storage/dirty_table_collector.hpp"

#include <algorithm>
#include <utility>

#include "kds/storage/device_page_store.hpp"

namespace kds::storage {

namespace {

std::size_t Low(PageId id) noexcept { return id & 0xFFFFu; }
std::size_t High(PageId id) noexcept { return id >> 16; }

// Counts into next write positions, in digit order.
void ToOffsets(std::vector<std::uint32_t>& counts) noexcept {
    std::uint32_t sum = 0;
    for (std::uint32_t& c : counts) {
        const std::uint32_t n = c;
        c = sum;
        sum += n;
    }
}

}  // namespace

DirtyTableCollector DirtyTableCollector::ForEntriesForTest(
    std::vector<wal::CheckpointDirtyPage> entries) {
    DirtyTableCollector c;
    c.entries_ = std::move(entries);
    c.BeginSort();
    return c;
}

void DirtyTableCollector::BeginSort() {
    scratch_ = std::make_unique_for_overwrite<wal::CheckpointDirtyPage[]>(entries_.size());
    low_.assign(kDigits, 0);
    high_.assign(kDigits, 0);
    at_ = 0;
    phase_ = Phase::kCount;
}

bool DirtyTableCollector::Advance() {
    if (phase_ == Phase::kWalk) {
        // The store's capacity bounds the table: reserved once, so no walk
        // slice pays a reallocation of everything collected before it.
        if (next_slot_ == 0) entries_.reserve(store_->configured_capacity());
        if (!store_->WalkDirtySlice(&next_slot_, chunks_per_slice_, &entries_)) return false;
        BeginSort();
    }
    // The sort's passes share one slice's budget, so a small table is whole
    // in the call that ends its walk.
    std::size_t budget = kEntriesPerSlice;
    while (phase_ != Phase::kDone && budget != 0) budget -= Pass(budget);
    return phase_ == Phase::kDone;
}

std::size_t DirtyTableCollector::Pass(std::size_t budget) {
    const std::size_t n = entries_.size();
    const std::size_t begin = at_;
    const std::size_t end = std::min(n, at_ + budget);
    switch (phase_) {
        case Phase::kWalk:
        case Phase::kDone:
            return 0;
        case Phase::kCount:
            for (std::size_t i = begin; i < end; ++i) {
                ++low_[Low(entries_[i].page_id)];
                ++high_[High(entries_[i].page_id)];
            }
            break;
        case Phase::kScatterLow:
            // Stable within a digit, which the second pass needs: entries
            // that share a high digit leave it in low-digit order.
            for (std::size_t i = begin; i < end; ++i) {
                scratch_[low_[Low(entries_[i].page_id)]++] = entries_[i];
            }
            break;
        case Phase::kScatterHigh:
            for (std::size_t i = begin; i < end; ++i) {
                entries_[high_[High(scratch_[i].page_id)]++] = scratch_[i];
            }
            break;
        case Phase::kMerge:
            // One entry a page, its **lowest non-zero** recLSN: a 0 is
            // "nothing to replay" (`RedoStartFrom` skips it) and must not
            // hide a real one met beside it (`MergeDirtyTable`'s rule).
            for (std::size_t i = begin; i < end; ++i) {
                const wal::CheckpointDirtyPage e = entries_[i];
                if (out_ != 0 && entries_[out_ - 1].page_id == e.page_id) {
                    wal::Lsn& kept = entries_[out_ - 1].rec_lsn;
                    if (kept == wal::kNoLsn || (e.rec_lsn != wal::kNoLsn && e.rec_lsn < kept)) {
                        kept = e.rec_lsn;
                    }
                } else {
                    entries_[out_++] = e;
                }
            }
            break;
    }
    at_ = end;
    if (at_ == n) {
        at_ = 0;
        switch (phase_) {
            case Phase::kCount:
                ToOffsets(low_);
                ToOffsets(high_);
                phase_ = Phase::kScatterLow;
                break;
            case Phase::kScatterLow:
                phase_ = Phase::kScatterHigh;
                break;
            case Phase::kScatterHigh:
                out_ = 0;
                phase_ = Phase::kMerge;
                break;
            case Phase::kMerge:
                entries_.resize(out_);
                scratch_.reset();
                std::vector<std::uint32_t>().swap(low_);
                std::vector<std::uint32_t>().swap(high_);
                phase_ = Phase::kDone;
                break;
            case Phase::kWalk:
            case Phase::kDone:
                break;
        }
    }
    return end - begin;
}

}  // namespace kds::storage
