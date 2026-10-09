#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "kds/base/current_core.hpp"

// **BA's census instrument** (`instructions/v3.0.0/workorder-ba-parallelism.md`
// BA-R0, BA-S2): where one core waited for another, how often, and for how
// long. `SHOW META`'s `contention` block prints it.
//
// **Counted only on the contended branch.** A `Latch` tries first and reads
// the clock only when the try failed (`latch.hpp`); the other tallies sit on
// paths that already waited, refused or ran a carve or a checkpoint. An
// uncontended acquisition costs what it cost before.
//
// **Per core, summed when read.** Each slot is one cache-line-aligned block
// written by its own core's thread, so the instrument adds no line two cores
// write. A thread that never declared a core (the WAL writer, a test's
// driver) writes slot 0 beside core 0, which is why the writes are relaxed
// read-modify-writes rather than plain stores. `Read` sums the counts and
// takes the maximum of the longest-run fields; it is a statistic, read
// without a fence against the writers, and two reads bracket a delta.
//
// **Process-wide, like the page latch's word and `CurrentCore`.** One engine
// runs per process in production. A test reads a delta across the cell it
// drives rather than an absolute value, since the suite runs many instances
// in one process.

namespace kds {

// One per `Latch` row of `rules.md` §3, plus the raw mutexes BA-R0 names.
// `kOther` is a latch no row describes.
enum class LatchKind : std::uint8_t {
    kOther,
    kFrameTable,          // DevicePageStore's structure latch
    kFreeMap,             // DevicePageStore's free-map latch
    kFreeMapFlush,        // DevicePageStore's free-map flush latch, held across device I/O
    kWindow,              // InstanceVisibility's window latch
    kLockPartition,       // a lock-table partition
    kLockWaitFor,         // the lock table's wait-for graph latch
    kWalStream,           // WalStream's stream latch
    kWalSyncGate,         // WalStream's sync gate (`sync_mutex_`)
    kAssertionDirectory,  // AssertionEnforcer's directory latch and chain latches
    kOptimizer,           // the optimizer collector's latch and the Cabin view latch
    kSuperblock,          // the superblock latch
    kCabinPartition,      // a CabinStore partition
    kCabinStats,          // CabinStore's stats latch
    kHandoff,             // a connection-handoff inbox (D19's fallback)
    kCount,
};

inline constexpr std::size_t kLatchKindCount = static_cast<std::size_t>(LatchKind::kCount);

// The `SHOW META` spelling of each kind, in enum order.
inline constexpr std::array<const char*, kLatchKindCount> kLatchKindNames = {
    "other",          "frame_table",   "free_map",      "free_map_flush", "window",
    "lock_partition", "lock_wait_for", "wal_stream",    "wal_sync_gate",  "assertion_dir",
    "optimizer",      "superblock",    "cabin_partition", "cabin_stats",  "handoff",
};
static_assert(kLatchKindNames.back() != nullptr, "a LatchKind without a name");

// Counts BA-R0 asks for beside the latches.
enum class Tally : std::uint8_t {
    kPageLatchWaits,      // a page-latch acquisition that spun at least once
    kPageLatchSpinTurns,  // the turns those acquisitions spun
    kSyncsInline,         // a WAL device sync run inline on the owning core
    kSyncsWriter,         // a WAL device sync run by the writer thread
    kDrainPassesPending,  // a drain pass taken with a group commit staged
    kCeilingWaits,        // a statement that waited for the snapshot ceiling (BA-S1c)
    kCeilingWaitNs,       // the nanoseconds those waits took
    kCarves,              // a transaction-id carve
    kCheckpointRuns,      // a checkpoint run that was not gated away
    // The seven sites a stale descent is refused `TxnConflict` at.
    kRefusalBtreeDescend,
    kRefusalBtreeParent,
    kRefusalBtreeSecure,
    kRefusalBtreeLookup,
    kRefusalIndexDescend,
    kRefusalIndexParent,
    kRefusalIndexSecure,
    kCount,
};

inline constexpr std::size_t kTallyCount = static_cast<std::size_t>(Tally::kCount);

inline constexpr std::array<const char*, kTallyCount> kTallyNames = {
    "page_latch_waits",       "page_latch_spin_turns",  "syncs_inline",
    "syncs_writer",           "drain_passes_pending",   "ceiling_waits",
    "ceiling_wait_us",        "carves",                 "checkpoint_runs",
    "refused_btree_descend",  "refused_btree_parent",   "refused_btree_secure",
    "refused_btree_lookup",   "refused_index_descend",  "refused_index_parent",
    "refused_index_secure",
};
static_assert(kTallyNames.back() != nullptr, "a Tally without a name");

// The longest single run of a periodic stall, in nanoseconds.
enum class Longest : std::uint8_t {
    kCarveNs,
    kCheckpointNs,
    kCount,
};

inline constexpr std::size_t kLongestCount = static_cast<std::size_t>(Longest::kCount);

inline constexpr std::array<const char*, kLongestCount> kLongestNames = {
    "carve_longest_us",
    "checkpoint_longest_us",
};
static_assert(kLongestNames.back() != nullptr, "a Longest without a name");

// Nanoseconds since `start`, as the instrument stores them.
inline std::uint64_t NsSince(std::chrono::steady_clock::time_point start) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                             start)
            .count());
}

namespace detail {
// One cache-line-aligned block per core (see the header comment).
struct alignas(64) ContentionSlot {
    std::array<std::atomic<std::uint64_t>, kLatchKindCount> waits{};
    std::array<std::atomic<std::uint64_t>, kLatchKindCount> wait_ns{};
    std::array<std::atomic<std::uint64_t>, kTallyCount> tallies{};
    std::array<std::atomic<std::uint64_t>, kLongestCount> longest_ns{};
};
// `kMaxWalCores` (superblock.hpp): no instance has more cores than that.
inline constexpr std::uint32_t kContentionSlots = 64;
inline std::array<ContentionSlot, kContentionSlots> g_contention{};
}  // namespace detail

class Contention {
public:
    struct Snapshot {
        std::array<std::uint64_t, kLatchKindCount> waits{};
        std::array<std::uint64_t, kLatchKindCount> wait_ns{};
        std::array<std::uint64_t, kTallyCount> tallies{};
        std::array<std::uint64_t, kLongestCount> longest_ns{};

        std::uint64_t waits_of(LatchKind k) const noexcept {
            return waits[static_cast<std::size_t>(k)];
        }
        std::uint64_t wait_ns_of(LatchKind k) const noexcept {
            return wait_ns[static_cast<std::size_t>(k)];
        }
        std::uint64_t of(Tally t) const noexcept { return tallies[static_cast<std::size_t>(t)]; }
        std::uint64_t of(Longest l) const noexcept {
            return longest_ns[static_cast<std::size_t>(l)];
        }
    };

    // A contended acquisition of a `kind` latch that waited `ns`.
    static void LatchWait(LatchKind kind, std::uint64_t ns) noexcept {
        Slot& s = Mine();
        const auto i = static_cast<std::size_t>(kind);
        s.waits[i].fetch_add(1, std::memory_order_relaxed);
        s.wait_ns[i].fetch_add(ns, std::memory_order_relaxed);
    }

    static void Add(Tally t, std::uint64_t n = 1) noexcept {
        Mine().tallies[static_cast<std::size_t>(t)].fetch_add(n, std::memory_order_relaxed);
    }

    static void Max(Longest l, std::uint64_t ns) noexcept {
        auto& cell = Mine().longest_ns[static_cast<std::size_t>(l)];
        std::uint64_t seen = cell.load(std::memory_order_relaxed);
        while (seen < ns && !cell.compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {
        }
    }

    static Snapshot Read() noexcept {
        Snapshot out;
        for (const Slot& s : detail::g_contention) {
            for (std::size_t i = 0; i < kLatchKindCount; ++i) {
                out.waits[i] += s.waits[i].load(std::memory_order_relaxed);
                out.wait_ns[i] += s.wait_ns[i].load(std::memory_order_relaxed);
            }
            for (std::size_t i = 0; i < kTallyCount; ++i) {
                out.tallies[i] += s.tallies[i].load(std::memory_order_relaxed);
            }
            for (std::size_t i = 0; i < kLongestCount; ++i) {
                const std::uint64_t v = s.longest_ns[i].load(std::memory_order_relaxed);
                if (v > out.longest_ns[i]) out.longest_ns[i] = v;
            }
        }
        return out;
    }

private:
    using Slot = detail::ContentionSlot;
    static Slot& Mine() noexcept {
        return detail::g_contention[CurrentCore() % detail::kContentionSlots];
    }
};

// One run of a periodic stall: counted under `count`, and its length kept
// if it is the longest yet under `longest`.
class StallTimer {
public:
    StallTimer(Tally count, Longest longest) noexcept
        : count_(count), longest_(longest), start_(std::chrono::steady_clock::now()) {}
    ~StallTimer() {
        Contention::Add(count_);
        Contention::Max(longest_, NsSince(start_));
    }
    StallTimer(const StallTimer&) = delete;
    StallTimer& operator=(const StallTimer&) = delete;

private:
    Tally count_;
    Longest longest_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace kds
