#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/stats/ash_record.hpp"

// **One core's ASH ring** (`instructions/v3.2.0/workorder-cj-wait-events-ash.md`
// §2.7, CJ's WE06; decisions WD7, WD8): a fixed circular buffer of 64-byte
// records, sized in bytes at construction (`kds.ash.ring_bytes_per_core`, 4 MiB
// by default) and overwriting its oldest record when full. Memory only.
//
// ---- The latch protocol ----------------------------------------------------
//
// The owning core is the only writer. A query on any core reads through
// `Snapshot`, which copies the records under the ring's latch, the same shape
// as `PerCoreRing`'s (`kStatsRing`): the latch is uncontended but for a reader,
// and no core reads another's words unguarded. The buffer is allocated once, on
// the first record, so a core that never samples costs nothing; a failed
// allocation drops the record and counts it, and never fails what is sampled.

namespace kds::stats {

class AshRing {
public:
    static constexpr std::size_t kDefaultBytes = 4u << 20;

    // `bytes` rounds down to whole records, with room for at least one.
    explicit AshRing(std::size_t bytes = kDefaultBytes) noexcept
        : capacity_(bytes / kAshRecordBytes == 0 ? 1 : bytes / kAshRecordBytes) {}

    std::size_t capacity() const noexcept { return capacity_; }

    void Record(const AshRecord& r) noexcept {
        const LatchGuard held(&latch_);
        if (slots_.empty()) {
            try {
                slots_.resize(capacity_);
            } catch (const std::bad_alloc&) {
                ++dropped_;
                return;
            }
        }
        slots_[next_] = EncodeAsh(r);
        next_ = (next_ + 1) % capacity_;
        ++recorded_;
    }

    // The records now held, oldest first, decoded.
    std::vector<AshRecord> Snapshot() const {
        std::vector<AshRecord> out;
        const LatchGuard held(&latch_);
        const std::size_t n = recorded_ < capacity_ ? static_cast<std::size_t>(recorded_) : capacity_;
        const std::size_t first = recorded_ < capacity_ ? 0 : next_;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) out.push_back(DecodeAsh(slots_[(first + i) % capacity_]));
        return out;
    }

    std::uint64_t recorded() const noexcept {
        const LatchGuard held(&latch_);
        return recorded_;
    }
    std::uint64_t dropped() const noexcept {
        const LatchGuard held(&latch_);
        return dropped_;
    }

private:
    mutable Latch latch_{LatchKind::kStatsRing};
    const std::size_t capacity_;
    std::vector<AshBytes> slots_;  // allocated whole on the first record
    std::size_t next_ = 0;         // the slot the next record takes
    std::uint64_t recorded_ = 0;   // records written, overwritten ones included
    std::uint64_t dropped_ = 0;    // records no allocation could take
};

}  // namespace kds::stats
