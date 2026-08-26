#include "kds/sched/waker.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <sys/eventfd.h>
#include <unistd.h>

namespace kds::sched {

StatusOr<Waker> Waker::Create() {
    // Non-blocking, because both ends must be: `Wake()` runs on a sender's
    // thread and may never block on a full counter, and `Drain()` runs on
    // the reactor and may never block on an empty one. `EFD_CLOEXEC` for
    // the reason every fd in this codebase takes it.
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        return Status::IoError(std::string("waker: eventfd failed: ") + std::strerror(errno));
    }
    return Waker(fd);
}

Waker::~Waker() {
    if (fd_ >= 0) ::close(fd_);
}

void Waker::Wake() const noexcept {
    if (fd_ < 0) return;
    const std::uint64_t one = 1;
    // The only failure a non-blocking eventfd write has is EAGAIN at
    // `UINT64_MAX - 1` pending wakes, which means the reactor has not
    // drained in 2^64 wakes and is a bigger problem than this one. Counted
    // rather than reported: the caller is a sender with nowhere to return
    // it, and a lost wake costs a statement the idle block rather than
    // correctness.
    if (::write(fd_, &one, sizeof(one)) != static_cast<ssize_t>(sizeof(one))) {
        wake_failures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    wakes_.fetch_add(1, std::memory_order_relaxed);
}

void Waker::Drain() const noexcept {
    if (fd_ < 0) return;
    std::uint64_t sink = 0;
    // One read takes the whole counter, whatever it accumulated: N wakes
    // that arrived before the reactor looked are one wake, which is the
    // correct reading — the ring is the queue and this only says "look".
    (void)::read(fd_, &sink, sizeof(sink));
}

}  // namespace kds::sched
