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
    virtual Status PollReady(int timeout_ms, std::vector<IoEvent>& out) = 0;
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
};

}  // namespace kds::sched
