#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "kds/base/latch.hpp"

// **A memory-resident `sys` view's rows, one ring per core** - the shape
// `sys.lock_waits` (`stats/lock_wait_log.hpp`, BO-S3) and `sys.checkpoints`
// (`stats/checkpoint_log.hpp`, BO-S7) share
// (`instructions/v3.0.0/workorder-bo-sys-statistics-show-sugar.md` §3).
//
// ---- The latch protocol ----------------------------------------------------
//
// One part per core, each a ring of `Capacity` rows under a latch of its own
// (`LatchKind::kStatsRing`). The core a row describes is the only writer of
// its part, so its latch is uncontended but for a reader; a reader on any
// core takes each part's latch in turn and copies its rows, so no core reads
// another's words unguarded and no cross-core write is added to what a row
// describes. A part allocates its ring on its first row: a core that never
// writes one costs nothing.
//
// **A write never fails, delays or reorders what it describes.** A full ring
// overwrites its oldest row and counts it; a ring that cannot be allocated
// drops the row and counts that. Both counts are the owner's `sys.meta`
// fields (SV3).
//
// `Row` carries `std::uint64_t seq`, which `Record` stamps: the part's own
// count of rows, from 1.

namespace kds::stats {

// The parts every memory-resident statistics store keeps, one per core an
// instance can run: `kMaxCores` (superblock.hpp), which a view's builder
// asserts against.
inline constexpr std::uint32_t kStatsParts = 64;

template <class Row, std::size_t Capacity>
class PerCoreRing {
public:
    static constexpr std::uint32_t kParts = kStatsParts;

    struct Totals {
        std::uint64_t recorded = 0;     // rows written, overwritten ones included
        std::uint64_t overwritten = 0;  // rows a full ring dropped for a newer one
        std::uint64_t dropped = 0;      // rows no ring could take
    };

    // Appends `row` to `core`'s part, stamping its `seq`.
    void Record(std::uint32_t core, Row row) noexcept {
        Part& part = parts_[core % kParts];
        const LatchGuard held(&part.latch);
        row.seq = ++part.recorded;
        if (part.ring.size() < Capacity) {
            // The one allocation, on this part's first row; a failure drops
            // the row rather than what it describes (the header's rule).
            try {
                if (part.ring.capacity() == 0) part.ring.reserve(Capacity);
                part.ring.push_back(row);
            } catch (const std::bad_alloc&) {
                --part.recorded;
                ++part.dropped;
            }
            return;
        }
        part.ring[part.next] = row;
        part.next = (part.next + 1) % Capacity;
        ++part.overwritten;
    }

    // Every part's rows, each copied under its part's latch: by core, then
    // oldest first.
    std::vector<Row> Snapshot() {
        std::vector<Row> out;
        for (Part& part : parts_) {
            const LatchGuard held(&part.latch);
            // Oldest first: once the ring has wrapped, `next` is the oldest slot.
            const std::size_t n = part.ring.size();
            const std::size_t first = n == Capacity ? part.next : 0;
            for (std::size_t i = 0; i < n; ++i) out.push_back(part.ring[(first + i) % n]);
        }
        return out;
    }

    Totals Read() noexcept {
        Totals t;
        for (Part& part : parts_) {
            const LatchGuard held(&part.latch);
            t.recorded += part.recorded;
            t.overwritten += part.overwritten;
            t.dropped += part.dropped;
        }
        return t;
    }

    // Empties every part; a test's alone.
    void Reset() noexcept {
        for (Part& part : parts_) {
            const LatchGuard held(&part.latch);
            part.ring.clear();
            part.next = 0;
            part.recorded = 0;
            part.overwritten = 0;
            part.dropped = 0;
        }
    }

private:
    struct Part {
        Latch latch{LatchKind::kStatsRing};
        std::vector<Row> ring;  // grows to `Capacity`, then wraps
        std::size_t next = 0;   // the slot the next row takes once full
        std::uint64_t recorded = 0;
        std::uint64_t overwritten = 0;
        std::uint64_t dropped = 0;
    };
    std::array<Part, kParts> parts_;
};

}  // namespace kds::stats
