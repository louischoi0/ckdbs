#pragma once

#include <unistd.h>

#include <utility>

// Move-only RAII owner for a POSIX file descriptor (rules.md section 6:
// RAII for every resource, file descriptors named explicitly). Lives in
// base/ rather than next to a single user because more than one subsystem
// owns fds; the platform layer is the only place raw fds are legitimate at
// all (rules.md section 4).

namespace kds {

class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

    ~FileDescriptor() { Reset(); }

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            Reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

    // Closes any owned fd. The close() return value is deliberately
    // discarded: there is nothing a caller can do about it, and retrying is
    // unsafe on Linux (the descriptor is released even when close reports
    // an error). Durability is Sync()'s job, not close()'s.
    void Reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] int Release() noexcept { return std::exchange(fd_, -1); }

private:
    int fd_ = -1;
};

}  // namespace kds
