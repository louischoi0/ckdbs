#pragma once

#include <cstdint>
#include <vector>

#include "kds/base/status.hpp"

// Injectable readiness-polling interface consumed by the reactor's "drain
// I/O completions" phase (docs/spec/sched.md phase 1). rules.md #4 requires
// file/disk I/O to be injectable so engine logic can run under
// deterministic simulation; this is the socket-readiness analog of that
// rule, scoped to what TcpServer needs today (level-triggered readiness,
// not a completion queue). A real disk I/O backend (O_DIRECT vs io_uring)
// is a separate, still-open decision (CLAUDE.md) and is not this.

namespace kds::sched {

using IoHandle = int;  // a POSIX fd on the real backend

enum class IoInterest : std::uint8_t {
    kReadable = 1,
    kWritable = 2,
};

struct IoEvent {
    IoHandle handle;
    bool readable = false;
    bool writable = false;
};

class IoBackend {
public:
    virtual ~IoBackend() = default;

    virtual Status Register(IoHandle handle, IoInterest interest) = 0;
    virtual Status Modify(IoHandle handle, IoInterest interest) = 0;
    virtual Status Unregister(IoHandle handle) = 0;

    // Waits up to timeout_ms for at least one event (0 = return
    // immediately without blocking, negative = block indefinitely),
    // appending ready events to *out*. *out* is not cleared by this call -
    // callers own that so they can control scratch-buffer reuse.
    //
    // A negative timeout is accepted by the interface and **forbidden by
    // the reactor** (docs/spec/sched.md §7, the v2.3.0 order's D4): a lost
    // wake must degrade to a bounded latency, never to a hang.
    virtual Status PollReady(int timeout_ms, std::vector<IoEvent>& out) = 0;

    // Ends a concurrent PollReady() block, or makes the next one return
    // immediately if none is running yet.
    //
    // **This is the only method on this interface that another core's
    // thread may call**, and the only one that may be called while
    // PollReady() is running on the owning thread. Every other method keeps
    // the core-local contract above.
    //
    // Two properties the caller is entitled to, and which an implementation
    // that can block must provide (docs/spec/sched.md §7):
    //
    //  - **A wake is never lost to a race with the block.** A Wake() that
    //    lands after the decision to block but before the sleeper is inside
    //    the syscall must still return that block immediately - so the
    //    readiness it posts has to be *sticky* (an eventfd counter, not an
    //    edge) until the next PollReady() consumes it.
    //  - **Wakes coalesce.** N wakes with no intervening block return one
    //    block; nothing counts them.
    //
    // No Status: the only failure a correct implementation can hit is "a
    // wake is already pending", which is success by another name.
    virtual void Wake() noexcept = 0;
};

// Trivial backend that never registers anything and never reports events.
// Used by scheduler unit tests (and anything else) that only care about
// task/group behavior and don't want a real fd or epoll instance.
class NullIoBackend final : public IoBackend {
public:
    Status Register(IoHandle, IoInterest) override { return Status::OK(); }
    Status Modify(IoHandle, IoInterest) override { return Status::OK(); }
    Status Unregister(IoHandle) override { return Status::OK(); }
    Status PollReady(int, std::vector<IoEvent>&) override { return Status::OK(); }

    // Nothing to end: PollReady() above never blocks, so every wake is
    // already delivered by the time it is asked for.
    void Wake() noexcept override {}
};

}  // namespace kds::sched
