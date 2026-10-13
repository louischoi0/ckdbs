#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

#include "kds/base/contention.hpp"

// The latch AR0's revised G1 admits beside the ring indices
// (`instructions/v3.0.0/ar0-architecture-revision.md` §3), and the shape
// that keeps G2 - `cores = 1` zero overhead - a property of the code
// rather than of a build flag: a structure that *can* be shared carries a
// `Latch*` that is **null** where it is not, and the guard is then two
// predictable branches and no atomic at all (G2's
// accepted cost class).
//
// **Unarmed at `cores = 1`**, each by a constructor argument or the core
// count and each with its call-site census in
// `instructions/v3.0.0/workorder-bp-per-core-residue.md` §7: the lock
// table's partitions (AO-R9), the instance read view's window latch
// (BP-H2), the Cabin store's latches (BP-H3), the assertion registry's, and
// the statement epochs' fences (BP-H1, fences rather than a latch).
//
// **A `std::mutex`, not a spin latch, because of what the sections do**:
// a WAL segment roll still syncs the segment it leaves under the stream
// latch (`wal/file_log_device.cpp`; BA-S12 moved the rest of the I/O
// out). Against a holder blocked in `fsync`, `sched_yield` returns at
// once on an otherwise idle CPU, so pinned reactor threads would spin at
// 100%; a futex sleeps, and uncontended it is the same single atomic
// operation a spin would be (AR0-M2: the spin primitive was deleted at
// `7839a29`).
//
// Not a reader/writer lock and not recursive: no ownership check, so a
// second acquisition on one thread hangs. A subsystem that takes this
// documents its acquisition order at the top of its own file
// (`docs/rules/rules.md` §3).
//
// **`HoldsLatch` is the debug half of that sentence** (AM-S3): a hang
// names nothing, so debug builds track which latches this thread holds,
// and a structure that guards its public readers asserts it is not
// already inside one.
//
// Debug only, and the tracking is a thread-local vector of pointers: at
// the depths this engine reaches (two, and only inside the store) a linear
// scan beats anything with an allocation in it.

#ifndef NDEBUG
#include <algorithm>
#include <vector>
#endif

namespace kds {

// **A `std::mutex` that knows which row of `rules.md` §3 it is** (BA-S2).
// `lock` tries first; only a failed try reads the clock and records the wait
// under the latch's kind (`contention.hpp`), so an uncontended acquisition is
// the same single atomic operation it was. BasicLockable and Lockable, so
// `std::unique_lock<Latch>` and `std::condition_variable_any` work over it.
class Latch {
public:
    constexpr Latch() noexcept = default;
    constexpr explicit Latch(LatchKind kind) noexcept : kind_(kind) {}
    Latch(const Latch&) = delete;
    Latch& operator=(const Latch&) = delete;

    void lock() { (void)LockReportingWait(); }
    // `lock`, returning the nanoseconds it waited: 0 when uncontended, and
    // charged to the kind exactly as `lock` charges it (BQ-S1).
    std::uint64_t LockReportingWait() {
        if (mutex_.try_lock()) return 0;
        const auto start = std::chrono::steady_clock::now();
        mutex_.lock();
        const std::uint64_t ns = NsSince(start);
        Contention::LatchWait(kind_, ns);
        return ns;
    }
    bool try_lock() noexcept { return mutex_.try_lock(); }
    void unlock() noexcept { mutex_.unlock(); }

private:
    std::mutex mutex_;
    LatchKind kind_ = LatchKind::kOther;
};

#ifndef NDEBUG
namespace detail {
// The latches this thread holds right now, innermost last. `inline` so the
// header needs no translation unit of its own; one instance per thread
// across every TU that includes this.
inline thread_local std::vector<const Latch*> held_latches;
}  // namespace detail

// Whether this thread already holds `latch`. Null is never held, so a
// caller may pass an unarmed structure's null latch without testing it.
inline bool HoldsLatch(const Latch* latch) noexcept {
    if (latch == nullptr) return false;
    return std::find(detail::held_latches.begin(), detail::held_latches.end(), latch) !=
           detail::held_latches.end();
}
#else
inline bool HoldsLatch(const Latch*) noexcept { return false; }
#endif

// RAII over an optional latch. `nullptr` means "not shared".
class LatchGuard {
public:
    // `waited_ns`, when given, receives what the acquisition waited: 0 for a
    // null latch or an uncontended one (BQ-S1).
    explicit LatchGuard(Latch* latch, std::uint64_t* waited_ns = nullptr) noexcept
        : latch_(latch) {
        std::uint64_t waited = 0;
        if (latch_ != nullptr) {
            waited = latch_->LockReportingWait();
#ifndef NDEBUG
            detail::held_latches.push_back(latch_);
#endif
        }
        if (waited_ns != nullptr) *waited_ns = waited;
    }
    ~LatchGuard() {
        if (latch_ != nullptr) {
#ifndef NDEBUG
            // Popped before the unlock, so a thread that is about to release
            // is never recorded as still holding. Erasing the *last* match
            // rather than the first keeps nesting honest if one latch were
            // ever taken twice by different guards on one thread - which is
            // a hang, and this is not the place that would report it.
            auto it = std::find(detail::held_latches.rbegin(), detail::held_latches.rend(), latch_);
            if (it != detail::held_latches.rend()) {
                detail::held_latches.erase(std::next(it).base());
            }
#endif
            latch_->unlock();
        }
    }
    LatchGuard(const LatchGuard&) = delete;
    LatchGuard& operator=(const LatchGuard&) = delete;

private:
    Latch* latch_;
};

}  // namespace kds
