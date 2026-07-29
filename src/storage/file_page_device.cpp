#include "kds/storage/file_page_device.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace kds::storage {
namespace {

Status ErrnoStatus(const std::string& what, int err) {
    const std::string detail = what + ": " + std::strerror(err);
    switch (err) {
        case ENOSPC:
        case EDQUOT:
            return Status::OutOfSpace(detail);
        case EINVAL:
            return Status::InvalidArgument(detail);
        default:
            return Status::IoError(detail);
    }
}

// posix_fallocate reports failure by return value, not errno, and leaves
// errno untouched - a difference from every other call in this file that
// is easy to get wrong.
constexpr bool FallocateUnsupported(int err) {
    return err == EOPNOTSUPP || err == ENOSYS || err == EINVAL;
}

std::uint64_t PageOffset(PageId page_id) {
    return static_cast<std::uint64_t>(page_id) * kPageSize;
}

}  // namespace

FilePageDevice::FilePageDevice(FileDescriptor fd, std::string path, std::uint32_t extent_pages,
                               std::uint32_t page_capacity) noexcept
    : fd_(std::move(fd)),
      path_(std::move(path)),
      extent_pages_(extent_pages),
      page_capacity_(page_capacity) {}

StatusOr<std::unique_ptr<FilePageDevice>> FilePageDevice::Open(const std::string& path,
                                                               std::uint32_t extent_pages) {
    if (extent_pages == 0) {
        return Status::InvalidArgument("FilePageDevice: extent_pages must be non-zero");
    }

    int raw_fd = -1;
    do {
        raw_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    } while (raw_fd < 0 && errno == EINTR);
    if (raw_fd < 0) {
        return ErrnoStatus("FilePageDevice: open " + path, errno);
    }
    FileDescriptor fd(raw_fd);

    struct ::stat st {};
    if (::fstat(fd.get(), &st) != 0) {
        return ErrnoStatus("FilePageDevice: fstat " + path, errno);
    }

    const auto size = static_cast<std::uint64_t>(st.st_size);
    if (size % kPageSize != 0) {
        return Status::Corruption("FilePageDevice: " + path + " is " + std::to_string(size) +
                                  " bytes, not a whole number of " + std::to_string(kPageSize) +
                                  "-byte pages");
    }
    if (size > kMaxFileBytes) {
        return Status::Corruption("FilePageDevice: " + path + " is " + std::to_string(size) +
                                  " bytes, beyond the " + std::to_string(kMaxFileBytes) +
                                  "-byte design ceiling");
    }

    const auto capacity = static_cast<std::uint32_t>(size / kPageSize);
    // Constructors must not fail (rules.md section 1), which is why every
    // check above happens here in the factory.
    return std::unique_ptr<FilePageDevice>(
        new FilePageDevice(std::move(fd), path, extent_pages, capacity));
}

Status FilePageDevice::ReadAt(PageId first_page_id, std::uint32_t nr_pages, std::byte* buffer) {
    Status status = CheckPageRunRange(first_page_id, nr_pages, page_capacity_);
    if (!status.ok()) {
        return status;
    }

    std::size_t remaining = static_cast<std::size_t>(nr_pages) * kPageSize;
    std::uint64_t offset = PageOffset(first_page_id);
    while (remaining > 0) {
        const ::ssize_t n = ::pread(fd_.get(), buffer, remaining, static_cast<::off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("FilePageDevice: pread at offset " + std::to_string(offset), errno);
        }
        if (n == 0) {
            // Inside page_capacity() but the file ends here. The capacity
            // is derived from the file size and only ever grows, so this
            // means the file shrank underneath us - never a legal state.
            return Status::Corruption("FilePageDevice: unexpected EOF at offset " +
                                      std::to_string(offset) + " within capacity " +
                                      std::to_string(page_capacity_));
        }
        buffer += n;
        offset += static_cast<std::uint64_t>(n);
        remaining -= static_cast<std::size_t>(n);
    }
    return Status::OK();
}

Status FilePageDevice::WriteAt(PageId first_page_id, std::uint32_t nr_pages,
                               const std::byte* buffer) {
    Status status = CheckPageRunRange(first_page_id, nr_pages, page_capacity_);
    if (!status.ok()) {
        return status;
    }

    std::size_t remaining = static_cast<std::size_t>(nr_pages) * kPageSize;
    std::uint64_t offset = PageOffset(first_page_id);
    while (remaining > 0) {
        const ::ssize_t n = ::pwrite(fd_.get(), buffer, remaining, static_cast<::off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("FilePageDevice: pwrite at offset " + std::to_string(offset), errno);
        }
        buffer += n;
        offset += static_cast<std::uint64_t>(n);
        remaining -= static_cast<std::size_t>(n);
    }
    return Status::OK();
}

Status FilePageDevice::ReadPage(PageId page_id, std::span<std::byte, kPageSize> out) {
    return ReadAt(page_id, 1, out.data());
}

Status FilePageDevice::WritePage(PageId page_id, std::span<const std::byte, kPageSize> in) {
    return WriteAt(page_id, 1, in.data());
}

Status FilePageDevice::ReadPageRun(PageId first_page_id, std::uint32_t nr_pages,
                                   std::span<std::byte> out) {
    const Status status = CheckPageRunBuffer(nr_pages, out.size());
    if (!status.ok()) {
        return status;
    }
    return ReadAt(first_page_id, nr_pages, out.data());
}

Status FilePageDevice::WritePageRun(PageId first_page_id, std::uint32_t nr_pages,
                                    std::span<const std::byte> in) {
    const Status status = CheckPageRunBuffer(nr_pages, in.size());
    if (!status.ok()) {
        return status;
    }
    return WriteAt(first_page_id, nr_pages, in.data());
}

Status FilePageDevice::EnsureCapacity(std::uint32_t nr_pages) {
    if (nr_pages > kMaxPageCount) {
        return Status::InvalidArgument("FilePageDevice: requested capacity " +
                                       std::to_string(nr_pages) + " pages exceeds the " +
                                       std::to_string(kMaxPageCount) + "-page design ceiling");
    }
    if (nr_pages <= page_capacity_) {
        // Never shrinks: truncation is a recorded v1 non-goal (page.md
        // section 14). This is also what makes replay of ALLOC records
        // idempotent.
        return Status::OK();
    }

    // Round up to a whole extent, clamped so rounding cannot push a
    // near-ceiling request past the ceiling.
    std::uint64_t target = (static_cast<std::uint64_t>(nr_pages) + extent_pages_ - 1) /
                           extent_pages_ * extent_pages_;
    if (target > kMaxPageCount) {
        target = kMaxPageCount;
    }

    const std::uint64_t old_bytes = PageOffset(page_capacity_);
    const std::uint64_t new_bytes = target * kPageSize;

    // posix_fallocate, not ftruncate: page.md section 14 wants real block
    // reservation, so that a write issued after its ALLOC record was logged
    // cannot then fail with ENOSPC.
    const int rc = ::posix_fallocate(fd_.get(), static_cast<::off_t>(old_bytes),
                                     static_cast<::off_t>(new_bytes - old_bytes));
    if (rc != 0) {
        if (!FallocateUnsupported(rc)) {
            return ErrnoStatus("FilePageDevice: posix_fallocate on " + path_, rc);
        }
        // Filesystems that cannot preallocate (some tmpfs/network mounts)
        // still get a correctly sized sparse file; the ENOSPC-at-write risk
        // this call exists to remove comes back, and that is the best the
        // filesystem offers.
        if (::ftruncate(fd_.get(), static_cast<::off_t>(new_bytes)) != 0) {
            return ErrnoStatus("FilePageDevice: ftruncate on " + path_, errno);
        }
    }

    page_capacity_ = static_cast<std::uint32_t>(target);
    return Status::OK();
}

Status FilePageDevice::Sync() {
    while (::fsync(fd_.get()) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return ErrnoStatus("FilePageDevice: fsync " + path_, errno);
    }
    return Status::OK();
}

}  // namespace kds::storage
