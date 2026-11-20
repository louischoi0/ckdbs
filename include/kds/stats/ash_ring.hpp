#pragma once

#include <atomic>
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
// The owning core is the only writer: its sampler, a phase of its reactor's
// loop. A query on any core reads through `Snapshot`, which copies the
// records under the ring's latch (`kStatsRing`), so no core reads another's
// words unguarded. The buffer is allocated once, on the first record, so a
// core that never samples costs nothing; a failed allocation drops the
// record and counts it, and never fails what is sampled.
//
// **A sample never waits** (`docs/rules/rules.md` §3a; DL-S2): the sampler
// runs on a reactor, so a wait of its would stop every statement on its
// core. `Record` takes the latch by the try form; the one other taker is a
// reader's copy, and a sample that meets it is dropped and counted (`busy`).
// **A reader's hold is a copy of the raw records and nothing else**: the
// copy's room is made before the latch is taken and the records are decoded
// after it is let go. The counts are atomics a reader loads with no latch.

namespace kds::stats {

class AshRing {
public:
    static constexpr std::size_t kDefaultBytes = 4u << 20;

    // `bytes` rounds down to whole records, with room for at least one.
    explicit AshRing(std::size_t bytes = kDefaultBytes) noexcept
        : capacity_(bytes / kAshRecordBytes == 0 ? 1 : bytes / kAshRecordBytes) {}

    std::size_t capacity() const noexcept { return capacity_; }

    void Record(const AshRecord& r) noexcept {
        const StatisticsRecordScope recording("sys.ash");
        const LatchGuard held(&latch_, std::try_to_lock);
        if (!held.owns()) {
            busy_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (slots_.empty()) {
            try {
                slots_.resize(capacity_);
            } catch (const std::bad_alloc&) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        slots_[next_] = EncodeAsh(r);
        next_ = (next_ + 1) % capacity_;
        recorded_.store(recorded_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    // The records now held, oldest first, decoded.
    std::vector<AshRecord> Snapshot() const {
        std::vector<AshBytes> raw;
        if (recorded_.load(std::memory_order_relaxed) == 0) return {};
        raw.reserve(capacity_);
        {
            const LatchGuard held(&latch_);
            const std::uint64_t recorded = recorded_.load(std::memory_order_relaxed);
            const std::size_t n =
                recorded < capacity_ ? static_cast<std::size_t>(recorded) : capacity_;
            const std::size_t first = recorded < capacity_ ? 0 : next_;
            for (std::size_t i = 0; i < n; ++i) raw.push_back(slots_[(first + i) % capacity_]);
        }
        std::vector<AshRecord> out;
        out.reserve(raw.size());
        for (const AshBytes& bytes : raw) out.push_back(DecodeAsh(bytes));
        return out;
    }

    std::uint64_t recorded() const noexcept { return recorded_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    // Samples that met a reader's copy and were not taken (DL-R2).
    std::uint64_t busy() const noexcept { return busy_.load(std::memory_order_relaxed); }

    // The ring's latch, for a test that holds it as a reader's copy does.
    Latch* LatchForTest() const noexcept { return &latch_; }

private:
    mutable Latch latch_{LatchKind::kStatsRing};
    const std::size_t capacity_;
    std::vector<AshBytes> slots_;  // allocated whole on the first record; under `latch_`
    std::size_t next_ = 0;         // the slot the next record takes; under `latch_`
    // Stored under the latch by the owning core, loaded by any thread.
    std::atomic<std::uint64_t> recorded_{0};  // records written, overwritten ones included
    std::atomic<std::uint64_t> dropped_{0};   // records no allocation could take
    std::atomic<std::uint64_t> busy_{0};
};

}  // namespace kds::stats
