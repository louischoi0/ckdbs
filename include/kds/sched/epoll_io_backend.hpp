#pragma once

#include "kds/sched/io_backend.hpp"

// Real IoBackend implementation on Linux epoll, level-triggered (a
// listener socket that still has pending connections, or a client socket
// that still has unread bytes, keeps reporting readable on every
// PollReady() call until actually drained - this is what lets Scheduler's
// per-event handlers each do a single bounded read/accept and rely on the
// next reactor iteration to pick up anything left over, instead of
// draining in an EAGAIN loop here).
//
// Concurrency: not thread-safe, matching Scheduler's own single-reactor-
// per-thread contract (rules.md #3) - an EpollIoBackend is core-local,
// **with the single exception of Wake()**, which peers call from their own
// threads and which is what lets a message to a sleeping core arrive
// without waiting out its block (docs/spec/sched.md §7).
//
// ---- The wake fd, and why an eventfd ------------------------------------
//
// Wake() writes 1 to an `eventfd(2)` this backend owns and keeps registered
// in its own epoll set, so a wake is an ordinary readable event on the one
// descriptor the reactor already waits on. Two properties come from the
// eventfd itself rather than from any code here, and io_backend.hpp's
// contract is stated in terms of them:
//
//  - the counter is **sticky**: a wake that lands before the sleeper enters
//    epoll_wait() leaves the fd readable, so that block returns at once.
//    This is what makes the wake race-free without a mutex on either side;
//  - counting up **coalesces**: PollReady() drains the whole counter in one
//    read, so N wakes cost one wakeup and one 8-byte read.
//
// The wake fd is never reported as an IoEvent - it is the reactor's own
// plumbing, and no handler is registered for it.

namespace kds::sched {

class EpollIoBackend final : public IoBackend {
public:
    static StatusOr<EpollIoBackend> Create();

    EpollIoBackend(EpollIoBackend&& other) noexcept;
    EpollIoBackend& operator=(EpollIoBackend&& other) noexcept;
    EpollIoBackend(const EpollIoBackend&) = delete;
    EpollIoBackend& operator=(const EpollIoBackend&) = delete;
    ~EpollIoBackend() override;

    Status Register(IoHandle handle, IoInterest interest) override;
    Status Modify(IoHandle handle, IoInterest interest) override;
    Status Unregister(IoHandle handle) override;
    Status PollReady(int timeout_ms, std::vector<IoEvent>& out) override;
    void Wake() noexcept override;

private:
    EpollIoBackend(int epoll_fd, int wake_fd) noexcept
        : epoll_fd_(epoll_fd), wake_fd_(wake_fd) {}
    void CloseIfOpen() noexcept;

    int epoll_fd_;
    int wake_fd_;
};

}  // namespace kds::sched
