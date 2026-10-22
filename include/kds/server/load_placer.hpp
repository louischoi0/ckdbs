#pragma once

#include <cstdint>
#include <span>

#include "kds/sched/core_load.hpp"
#include "kds/server/transaction_placement.hpp"

// **BL's balancer: a transaction placed on a less-busy core** (BL-R4,
// `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`).
//
// A decision each core takes at a transaction's head, on its own reactor -
// no balancer thread and no coordinator (W3). It reads each core's slot in
// `sched::CoreLoad`: the busy fraction (BL-Q3: of the candidates BL-S1
// printed, the one whose per-core order followed the per-core p99 best
// overall, tied with the `foreground` fraction where no statement wrote),
// the sessions active in the window, and when the core last published. Its
// three numbers are engine constants (W4: no session setting says how
// readily a session moves).
//
// A session moves from core `h` to the least-busy fresh core `l` only when
// all of:
// - `busy[h] - busy[l] > kMarginPpm` - the gap is worth a move;
// - the session's share, `busy[h]` over `h`'s active sessions, is under the
//   gap - so the move shrinks it (a gap `g` becomes `|g - 2 * share|`); a
//   lone session, whose share is its core's whole busy, never is;
// - `l` published within `kStaleNs` - a core inside a long hold publishes
//   nothing until it ends, and its last, low word would draw sessions into
//   the hold;
// - no session left `h` in this window - every head in one window reads the
//   same words and would leave for the same core;
// - the session has stayed `kDwellNs` since its last move.
//
// **The partition comes before load** (BL-R2, BL-R4, BL-S4). While a
// batch-level session lives on the instance (`priority.hpp`,
// `sched::CoreLoad::BatchSessions`), the cores split: `max(1, cores / 4)`
// batch cores - core 0 first, then the highest-numbered (ruling 2 marked
// (a), 2026-10-21: core 0's reactor runs the log's group-commit drain,
// which waits out every segment roll, and an interactive session sharing
// it waits with it) - and the rest interactive. A session on a core outside
// its level's side moves at its head to that side's least-busy core, fresh
// first, with no margin, no dwell and no per-window guard; inside its side
// the rules above apply, and a load move never leaves it. With no
// batch-level session every core is every level's, which is BL-S3's path.
// A partition move stamps the per-window guard like any move, so no load
// move leaves that core in the same window - conservative, never wrong.
//
// Stateless apart from the session's own move time, so every core may ask it
// at once (`transaction_placement.hpp`'s concurrency contract).

namespace kds::server {

class LoadPlacer final : public TransactionPlacer {
public:
    // **The margin**, in parts per million of a core's time: above the
    // widest gap BL-S1 read between two cores whose sessions were placed
    // evenly - 179,793 ppm at `group`, at most 92,132 at `relaxed` - and
    // below most skewed placements' (176,000 to 960,000), though the two
    // overlap at `group` (`bench/v3.0.0/results-bl-s1-*.md` §2). One
    // mid-span reading per run, so `[provisional]`.
    static constexpr std::uint32_t kMarginPpm = 200'000;
    // **The dwell**: ten windows (`sched::CoreLoad::kWindowNs`), so a
    // session that moved is not reconsidered before both cores' words have
    // been republished several times with it on its new core. A move costs
    // a statement 28-39 us at its p50 (the same file, §5), which this
    // amortises. `[provisional]`.
    static constexpr std::uint64_t kDwellNs = 500'000'000;
    // **A word older than this is a core inside a hold.** Four windows: an
    // idle reactor publishes once its blocks add up to a window, and each
    // block is at most `max_idle_block_ms` (10 ms by default) - so this
    // holds while that key stays under three windows.
    static constexpr std::uint64_t kStaleNs = 4 * sched::CoreLoad::kWindowNs;

    // One core's words, as `Place` reads them.
    struct CoreView {
        std::uint32_t busy_ppm = 0;
        std::uint32_t active_sessions = 0;
        std::uint64_t window_end_ns = 0;
    };

    // Which cores a session may run on: every core while the partition is
    // inactive, else its level's side.
    enum class Side : std::uint8_t { kAny, kInteractive, kBatch };

    std::uint32_t Place(std::uint32_t core, Session& session) override;

    // Whether core `k` of `cores` is a batch core while the partition is
    // active; none at one core, where there is no partition (BL-R9).
    static constexpr bool IsBatchCore(std::uint32_t k, std::uint32_t cores) noexcept {
        if (cores <= 1) return false;
        const std::uint32_t batch = cores / 4 > 1 ? cores / 4 : 1;
        return k == 0 || k + (batch - 1) >= cores;
    }
    // Whether core `k` of `cores` is on `side`.
    static constexpr bool OnSide(std::uint32_t k, std::uint32_t cores, Side side) noexcept {
        return side == Side::kAny || IsBatchCore(k, cores) == (side == Side::kBatch);
    }

    // The decision, over a snapshot of every core - the seam a cell drives
    // with fixed words rather than racing reactors for them. `now_ns` is the
    // asking core's latest window end, on the instance's one clock;
    // `left_this_window` says a session already left `core` in it; `side` is
    // the session's.
    static std::uint32_t Decide(std::span<const CoreView> cores, std::uint32_t core,
                                std::uint64_t now_ns, std::uint64_t moved_at_ns,
                                bool left_this_window, Side side = Side::kAny) noexcept;
};

}  // namespace kds::server
