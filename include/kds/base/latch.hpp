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

namespace kds {

using Latch = std::mutex;

// RAII over an optional latch. `nullptr` means "not shared".
class LatchGuard {
public:
    explicit LatchGuard(Latch* latch) noexcept : latch_(latch) {
        if (latch_ != nullptr) latch_->lock();
    }
    ~LatchGuard() {
        if (latch_ != nullptr) latch_->unlock();
    }
    LatchGuard(const LatchGuard&) = delete;
    LatchGuard& operator=(const LatchGuard&) = delete;

private:
    Latch* latch_;
};

}  // namespace kds
