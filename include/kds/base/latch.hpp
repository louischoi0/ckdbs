#pragma once

#include <mutex>

// The latch AR0's revised G1 admits beside the ring indices
// (`instructions/v3.0.0/ar0-architecture-revision.md` §3), and the shape
// that keeps G2 - `cores = 1` zero overhead - a property of the code
// rather than of a build flag: a structure that *can* be shared carries a
// `Latch*` that is **null** where it is not, and the guard is then two
// predictable branches and no atomic at all (`docs/spec/sched.md` §5's
// accepted cost class, "phase 3 costs one null test").
//
// **A `std::mutex`, not a spin latch, and the reason is what the sections
// actually do.** The first draft of this header was a spinning
// `exchange`/`yield` pair, justified as guarding "a critical section
// measured in nanoseconds". Three of the WAL stream's four sections are
// not that: the flush holds it across a `pwrite`, and the segment roll
// holds it across `posix_fallocate`, a 64 MiB zero-filling prewrite and
// two `fsync`s (`wal/file_log_device.cpp`'s `CreateSegment`). Against a
// holder blocked in `fsync`, `sched_yield` on Linux returns immediately
// when no other thread is runnable on that CPU, so N-1 pinned reactor
// threads would burn at 100% for the length of a segment creation. A
// futex sleep is the right answer for a wait of that length, and
// uncontended it is the same single atomic operation the spin took.
//
// Not a reader/writer lock and not recursive: no ownership check, so a
// second acquisition on one thread hangs. A subsystem that takes this
// documents its acquisition order at the top of its own file
// (`docs/rules/rules.md` §3).
//
// **`HoldsLatch` is the debug half of that sentence** (AM-S3). "A second
// acquisition on one thread hangs" is a fact a reader has to keep in their
// head across every call an accessor makes; a hang is also the worst
// possible way to be told, because it names nothing. So debug builds track
// which latches this thread holds, and a structure that guards its public
// readers asserts it is not already inside one. The census AM-S3 needed -
// which of `DevicePageStore`'s free-map readers are reached from under the
// hold - is that assertion plus the suite, which is how AM-S1 found the
// two S-then-X sites for the page latch.
//
// Debug only, and the tracking is a thread-local vector of pointers: at
// the depths this engine reaches (two, and only inside the store) a linear
// scan beats anything with an allocation in it.

#ifndef NDEBUG
#include <algorithm>
#include <vector>
#endif

namespace kds {

using Latch = std::mutex;

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
    explicit LatchGuard(Latch* latch) noexcept : latch_(latch) {
        if (latch_ != nullptr) {
            latch_->lock();
#ifndef NDEBUG
            detail::held_latches.push_back(latch_);
#endif
        }
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
