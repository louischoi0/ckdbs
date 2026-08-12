#include "kds/server/stop_signal.hpp"

#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace kds::server {
namespace {

Status Errno(const char* what) {
    return Status::IoError(std::string("stop signal: ") + what + " failed: " +
                           std::strerror(errno));
}

}  // namespace

StatusOr<StopSignal> StopSignal::Install() {
    sigset_t mask;
    if (sigemptyset(&mask) != 0) {
        return Errno("sigemptyset");
    }
    if (sigaddset(&mask, SIGTERM) != 0 || sigaddset(&mask, SIGINT) != 0) {
        return Errno("sigaddset");
    }

    // Blocked before the fd exists, and before any thread does. An unblocked
    // signal races the default disposition and wins often enough to look
    // intermittent (the header says why this ordering is not a preference).
    if (int err = pthread_sigmask(SIG_BLOCK, &mask, nullptr); err != 0) {
        errno = err;
        return Errno("pthread_sigmask");
    }

    // SFD_CLOEXEC so the descriptor does not survive into anything this process
    // execs. Not non-blocking: the reactor only ever reads it after epoll has
    // said it is readable, and a blocking read of a ready signalfd returns
    // immediately.
    const int fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (fd < 0) {
        return Errno("signalfd");
    }
    return StopSignal(fd);
}

StopSignal& StopSignal::operator=(StopSignal&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

StopSignal::~StopSignal() {
    if (fd_ >= 0) ::close(fd_);
}

std::uint32_t StopSignal::Consume() noexcept {
    if (fd_ < 0) return 0;

    struct signalfd_siginfo info{};
    const ssize_t n = ::read(fd_, &info, sizeof(info));
    // A short or failed read is not worth a Status: the caller is shutting down
    // either way, and the only thing lost is which signal to name in the log.
    if (n != static_cast<ssize_t>(sizeof(info))) return 0;
    return info.ssi_signo;
}

}  // namespace kds::server
