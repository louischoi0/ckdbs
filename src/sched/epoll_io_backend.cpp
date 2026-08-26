#include "kds/sched/epoll_io_backend.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

namespace kds::sched {

namespace {

std::uint32_t ToEpollEvents(IoInterest interest) {
    std::uint32_t events = 0;
    if (static_cast<std::uint8_t>(interest) & static_cast<std::uint8_t>(IoInterest::kReadable)) {
        events |= EPOLLIN;
    }
    if (static_cast<std::uint8_t>(interest) & static_cast<std::uint8_t>(IoInterest::kWritable)) {
        events |= EPOLLOUT;
    }
    return events;
}

}  // namespace

StatusOr<EpollIoBackend> EpollIoBackend::Create() {
    int fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        return Status::IoError(std::string("epoll_create1() failed: ") + std::strerror(errno));
    }

    // Non-blocking so a saturated counter fails the write instead of
    // blocking the *sending* core - see Wake(), which treats that as a wake
    // already pending.
    int wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) {
        Status s = Status::IoError(std::string("eventfd() failed: ") + std::strerror(errno));
        ::close(fd);
        return s;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wake_fd;
    if (::epoll_ctl(fd, EPOLL_CTL_ADD, wake_fd, &ev) < 0) {
        Status s = Status::IoError(std::string("epoll_ctl(ADD, wake fd) failed: ") +
                                   std::strerror(errno));
        ::close(wake_fd);
        ::close(fd);
        return s;
    }
    return EpollIoBackend(fd, wake_fd);
}

EpollIoBackend::EpollIoBackend(EpollIoBackend&& other) noexcept
    : epoll_fd_(other.epoll_fd_), wake_fd_(other.wake_fd_) {
    other.epoll_fd_ = -1;
    other.wake_fd_ = -1;
}

EpollIoBackend& EpollIoBackend::operator=(EpollIoBackend&& other) noexcept {
    if (this != &other) {
        CloseIfOpen();
        epoll_fd_ = other.epoll_fd_;
        wake_fd_ = other.wake_fd_;
        other.epoll_fd_ = -1;
        other.wake_fd_ = -1;
    }
    return *this;
}

EpollIoBackend::~EpollIoBackend() { CloseIfOpen(); }

void EpollIoBackend::CloseIfOpen() noexcept {
    // The wake fd first: while the epoll fd is still open, a concurrent
    // Wake() on a closed-and-reused descriptor would at worst write to
    // something else. Neither order is safe against a peer that wakes a
    // destroyed backend - the reactor is torn down after its peers stop,
    // and that is the contract, not a rescue here.
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

void EpollIoBackend::Wake() noexcept {
    if (wake_fd_ < 0) return;
    const std::uint64_t one = 1;
    // EAGAIN is the counter at its ceiling, which means a wake is already
    // pending and undrained: the block this one would have ended is already
    // going to end. Every other error is unreachable for a valid eventfd,
    // and there is no caller to report one to - Wake() is called from a
    // peer's send path, which has no business failing over a wakeup.
    while (::write(wake_fd_, &one, sizeof(one)) < 0) {
        if (errno == EINTR) continue;
        break;
    }
}

Status EpollIoBackend::Register(IoHandle handle, IoInterest interest) {
    epoll_event ev{};
    ev.events = ToEpollEvents(interest);
    ev.data.fd = handle;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, handle, &ev) < 0) {
        return Status::IoError(std::string("epoll_ctl(ADD) failed: ") + std::strerror(errno));
    }
    return Status::OK();
}

Status EpollIoBackend::Modify(IoHandle handle, IoInterest interest) {
    epoll_event ev{};
    ev.events = ToEpollEvents(interest);
    ev.data.fd = handle;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, handle, &ev) < 0) {
        return Status::IoError(std::string("epoll_ctl(MOD) failed: ") + std::strerror(errno));
    }
    return Status::OK();
}

Status EpollIoBackend::Unregister(IoHandle handle) {
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, handle, nullptr) < 0) {
        return Status::IoError(std::string("epoll_ctl(DEL) failed: ") + std::strerror(errno));
    }
    return Status::OK();
}

Status EpollIoBackend::PollReady(int timeout_ms, std::vector<IoEvent>& out) {
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return Status::OK();
        return Status::IoError(std::string("epoll_wait() failed: ") + std::strerror(errno));
    }

    for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == wake_fd_) {
            // The reactor's own plumbing, never an IoEvent: nobody
            // registered a handler for it and the block it just ended *is*
            // the delivery. Drained in one read - the counter holds however
            // many wakes coalesced into this block, and leaving it set
            // would make the next PollReady() return instantly forever.
            std::uint64_t drained = 0;
            while (::read(wake_fd_, &drained, sizeof(drained)) < 0 && errno == EINTR) {
            }
            continue;
        }
        IoEvent ev;
        ev.handle = events[i].data.fd;
        ev.readable = (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
        ev.writable = (events[i].events & EPOLLOUT) != 0;
        out.push_back(ev);
    }
    return Status::OK();
}

}  // namespace kds::sched
