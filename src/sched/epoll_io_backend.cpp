#include "kds/sched/epoll_io_backend.hpp"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
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
    int fd = ::epoll_create1(0);
    if (fd < 0) {
        return Status::IoError(std::string("epoll_create1() failed: ") + std::strerror(errno));
    }
    return EpollIoBackend(fd);
}

EpollIoBackend::EpollIoBackend(EpollIoBackend&& other) noexcept : epoll_fd_(other.epoll_fd_) {
    other.epoll_fd_ = -1;
}

EpollIoBackend& EpollIoBackend::operator=(EpollIoBackend&& other) noexcept {
    if (this != &other) {
        CloseIfOpen();
        epoll_fd_ = other.epoll_fd_;
        other.epoll_fd_ = -1;
    }
    return *this;
}

EpollIoBackend::~EpollIoBackend() { CloseIfOpen(); }

void EpollIoBackend::CloseIfOpen() noexcept {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
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
        IoEvent ev;
        ev.handle = events[i].data.fd;
        ev.readable = (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
        ev.writable = (events[i].events & EPOLLOUT) != 0;
        out.push_back(ev);
    }
    return Status::OK();
}

}  // namespace kds::sched
