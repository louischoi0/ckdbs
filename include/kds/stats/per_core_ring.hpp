#pragma once

#include <algorithm>
#include <array>
#include <atomic>
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
// its part; a reader on any core copies each part's rows in turn, so no core
// reads another's rows unguarded and no cross-core write is added to what a
// row describes. A part allocates its ring on its first row: a core that
// never writes one costs nothing.
//
// **A write never fails, delays or reorders what it describes**
// (`docs/rules/rules.md` §3a; DL-S2). **A record takes its part's latch by
// the try form and never waits for it**: the one other taker is a reader's
// copy, and a row that meets it is dropped and counted (`busy`). A full ring
// overwrites its oldest row and counts it; a ring that cannot be allocated
// drops the row and counts that. **A reader's hold is a copy and nothing
// else**: the copy's room is reserved before the latch is taken. The four
// counts are atomics a reader loads with no latch, and are the owner's
// `sys.meta` fields (SV3).
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
        std::uint64_t busy = 0;         // rows that met a reader's copy of their part
    };

    // Appends `row` to `core`'s part, stamping its `seq`; a row that meets a
    // reader's copy of the part is counted and not written.
    void Record(std::uint32_t core, Row row) noexcept {
        Part& part = parts_[core % kParts];
        const LatchGuard held(&part.latch, std::try_to_lock);
        if (!held.owns()) {
            part.busy.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint64_t seq = part.recorded.load(std::memory_order_relaxed) + 1;
        row.seq = seq;
        if (part.ring.size() < Capacity) {
            // The one allocation, on this part's first row; a failure drops
            // the row rather than what it describes (the header's rule).
            try {
                if (part.ring.capacity() == 0) part.ring.reserve(Capacity);
                part.ring.push_back(row);
            } catch (const std::bad_alloc&) {
                part.dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            part.recorded.store(seq, std::memory_order_relaxed);
            return;
        }
        part.ring[part.next] = row;
        part.next = (part.next + 1) % Capacity;
        part.recorded.store(seq, std::memory_order_relaxed);
        part.overwritten.fetch_add(1, std::memory_order_relaxed);
    }

    // Every part's rows, each copied under its part's latch into room made
    // before the hold: by core, then oldest first.
    std::vector<Row> Snapshot() {
        std::vector<Row> out;
        for (Part& part : parts_) {
            // A part that never recorded a row: no latch taken.
            if (part.recorded.load(std::memory_order_relaxed) == 0) continue;
            // Grown geometrically: an exact reserve per part would copy the
            // rows already taken once per part after them.
            if (out.capacity() - out.size() < Capacity) {
                out.reserve(std::max(2 * out.capacity(), out.size() + Capacity));
            }
            const LatchGuard held(&part.latch);
            // Oldest first: once the ring has wrapped, `next` is the oldest slot.
            const std::size_t n = part.ring.size();
            const std::size_t first = n == Capacity ? part.next : 0;
            for (std::size_t i = 0; i < n; ++i) out.push_back(part.ring[(first + i) % n]);
        }
        return out;
    }

    // The counts, as they stand; no latch.
    Totals Read() const noexcept {
        Totals t;
        for (const Part& part : parts_) {
            t.recorded += part.recorded.load(std::memory_order_relaxed);
            t.overwritten += part.overwritten.load(std::memory_order_relaxed);
            t.dropped += part.dropped.load(std::memory_order_relaxed);
            t.busy += part.busy.load(std::memory_order_relaxed);
        }
        return t;
    }

    // `core`'s part's latch, for a test that holds it as a reader's copy does.
    Latch* PartLatchForTest(std::uint32_t core) noexcept { return &parts_[core % kParts].latch; }

    // Empties every part; a test's alone.
    void Reset() noexcept {
        for (Part& part : parts_) {
            const LatchGuard held(&part.latch);
            part.ring.clear();
            part.next = 0;
            part.recorded.store(0, std::memory_order_relaxed);
            part.overwritten.store(0, std::memory_order_relaxed);
            part.dropped.store(0, std::memory_order_relaxed);
            part.busy.store(0, std::memory_order_relaxed);
        }
    }

private:
    struct Part {
        Latch latch{LatchKind::kStatsRing};
        std::vector<Row> ring;  // grows to `Capacity`, then wraps; under `latch`
        std::size_t next = 0;   // the slot the next row takes once full; under `latch`
        // Stored under the latch by the part's core, loaded by any thread.
        std::atomic<std::uint64_t> recorded{0};
        std::atomic<std::uint64_t> overwritten{0};
        std::atomic<std::uint64_t> dropped{0};
        std::atomic<std::uint64_t> busy{0};
    };
    std::array<Part, kParts> parts_;
};

}  // namespace kds::stats
