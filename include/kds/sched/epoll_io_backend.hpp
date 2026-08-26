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
// per-thread contract (rules.md #3) - an EpollIoBackend is core-local.

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

private:
    explicit EpollIoBackend(int epoll_fd) noexcept : epoll_fd_(epoll_fd) {}
    void CloseIfOpen() noexcept;

    int epoll_fd_;
};

}  // namespace kds::sched
