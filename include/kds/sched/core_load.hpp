#pragma once

#include <array>
#include <atomic>
#include <cstdint>

// **Each core's load, readable from every core** (BL-S1,
// `instructions/v3.0.0/workorder-bl-session-balancer-priority-affinity.md`).
//
// BL places a session's next transaction by load (BL-R4), so a core has to
// read another core's load. Everything a scheduler counts is reactor-thread
// state (`scheduler.hpp`), and `SHOW META`'s `sched_*` block describes only
// the asking session's core; this table is the part of it another core may
// read. `SHOW META` prints it as `load_c<k>_*`, beside each core's
// contention slot (`base/contention.hpp`), which says what the core waited
// on.
//
// ---- Protocol -------------------------------------------------------------
//
// One cache-line slot per core. **Every field has one writer**: the core's
// reactor thread, at its iteration end (`Scheduler::RecordIterationHold`) or
// in a listener that runs on that reactor (`TcpServer`). Any thread reads
// any slot with relaxed loads: each field is a statistic, read without a
// fence against its writer, and no decision taken from it is a correctness
// statement - a stale word places a transaction on a busier core, never on
// a wrong one. `rules.md` §3 lists the table.
//
// **Written only above one core** (BL-R9): no other core exists to read it,
// so a reactor with no wake registry writes nothing. A reactor that attaches
// to one as core `k` resets slot `k` first (`Reset`), and the instance
// declares how many cores it runs (`SetCores`), which together make the
// table describe the instance running now: it is process-wide, like
// `CheckpointCensus` and the contention slots, and the suite runs many
// instances in one process.

namespace kds::sched {

class CoreLoad {
public:
    static constexpr std::uint32_t kSlots = 64;  // `kMaxCores` (superblock.hpp), the most cores an instance runs

    // **The window the fractions are taken over.** A reactor publishes once
    // its iterations since the last publish span this much wall time, so a
    // fraction is never older than one window plus one iteration - and a
    // long hold publishes at its end, the first moment the reactor could.
    // 50 ms is longer than one statement and shorter than a session's think
    // time between transactions.
    static constexpr std::uint64_t kWindowNs = 50'000'000;

    struct alignas(64) Slot {
        // The fraction of the last window the reactor was not blocked idle,
        // in parts per million: a held reactor counts as busy.
        std::atomic<std::uint32_t> busy_ppm{0};
        // The fraction of the last window spent polling `foreground` tasks -
        // client statements - in parts per million.
        std::atomic<std::uint32_t> foreground_ppm{0};
        // `foreground` tasks queued at the last iteration's end, parked ones
        // included (a parked task stays in its queue, `sched.md` §7).
        std::atomic<std::uint32_t> queued_foreground{0};
        // Client connections this core's listeners hold.
        std::atomic<std::uint32_t> sessions{0};
        // Sessions that ran a statement in the last window - the busy
        // fraction's owners, which an idle connection is not - and the count
        // the current window is gathering (both this reactor's).
        std::atomic<std::uint32_t> active_sessions{0};
        std::atomic<std::uint32_t> active_now{0};
        // **Where a move is possible** (BL-R1): statements that began a
        // transaction - a session with none open asked for a dispatch - and
        // statements that ran inside an open one.
        std::atomic<std::uint64_t> heads{0};
        std::atomic<std::uint64_t> inside{0};
        // When the last window ended, on the instance's one clock: a core
        // whose word is old is inside a hold (`load_placer.hpp`).
        std::atomic<std::uint64_t> window_end_ns{0};
        // The window this core last let a session leave in (its
        // `window_end_ns` then), and the sessions it has let leave: the
        // balancer moves at most one session off a core per window.
        std::atomic<std::uint64_t> moved_out_in{0};
        std::atomic<std::uint64_t> moves_out{0};
        // Of `moves_out`, the moves the partition made (BL-R4): a session on
        // a core outside its level's partition, moved with no margin and no
        // dwell.
        std::atomic<std::uint64_t> partition_moves_out{0};
    };

    static Slot& Of(std::uint32_t core) noexcept { return slots()[core % kSlots]; }

    // The running instance's core count, 0 at one core (nothing is written
    // there). Set once by the instance before its reactors run.
    static void SetCores(std::uint32_t cores) noexcept {
        cores_word().store(cores, std::memory_order_relaxed);
    }
    static std::uint32_t Cores() noexcept { return cores_word().load(std::memory_order_relaxed); }

    // Zeroes `core`'s slot: the reactor about to run as that core owns it
    // from here on.
    static void Reset(std::uint32_t core) noexcept {
        Slot& s = Of(core);
        s.busy_ppm.store(0, std::memory_order_relaxed);
        s.foreground_ppm.store(0, std::memory_order_relaxed);
        s.queued_foreground.store(0, std::memory_order_relaxed);
        s.sessions.store(0, std::memory_order_relaxed);
        s.active_sessions.store(0, std::memory_order_relaxed);
        s.active_now.store(0, std::memory_order_relaxed);
        s.heads.store(0, std::memory_order_relaxed);
        s.inside.store(0, std::memory_order_relaxed);
        s.window_end_ns.store(0, std::memory_order_relaxed);
        s.moved_out_in.store(0, std::memory_order_relaxed);
        s.moves_out.store(0, std::memory_order_relaxed);
        s.partition_moves_out.store(0, std::memory_order_relaxed);
    }

    // **Live batch-level sessions on the instance** (BL-R2): the partition
    // is active while this is above zero. The table's one word with more
    // than one writer - changed by read-modify-write, never by `Bump`, at
    // every core count, and never reset with a slot (`rules.md` §3;
    // `server/priority.hpp`'s `PrioritySeat` is its only writer).
    static std::atomic<std::uint32_t>& BatchSessions() noexcept {
        static std::atomic<std::uint32_t> n{0};
        return n;
    }

    // Adds one to a single-writer count: a load and a store, not a
    // read-modify-write, since only the owning core writes it.
    static void Bump(std::atomic<std::uint64_t>& field) noexcept {
        field.store(field.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

private:
    static std::array<Slot, kSlots>& slots() noexcept {
        static std::array<Slot, kSlots> s{};
        return s;
    }
    static std::atomic<std::uint32_t>& cores_word() noexcept {
        static std::atomic<std::uint32_t> n{0};
        return n;
    }
};

}  // namespace kds::sched
