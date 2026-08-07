#include "kds/wal/file_log_device.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace kds::wal {
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

// posix_fallocate reports failure by return value, not errno - the same
// trap FilePageDevice documents.
constexpr bool FallocateUnsupported(int err) {
    return err == EOPNOTSUPP || err == ENOSYS || err == EINVAL;
}

// Segment file names are `wal-<core_id>-<segment_no>.log` (file_log_device.hpp).
// The numbers are not zero-padded, so directory listings do not sort into
// stream order; every scan below parses the number instead of trusting the
// order readdir hands back.
constexpr const char* kNameSuffix = ".log";

std::string NamePrefix(std::uint32_t core_id) {
    return "wal-" + std::to_string(core_id) + "-";
}

// Returns the segment number encoded in `name`, or nullopt if the name does
// not belong to this core's stream. Rejects anything with a non-digit,
// leading zeroes, or an empty number, so one segment never has two spellings.
std::optional<std::uint64_t> ParseSegmentNo(const std::string& name, const std::string& prefix) {
    if (name.size() <= prefix.size() + std::strlen(kNameSuffix)) {
        return std::nullopt;
    }
    if (name.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    if (name.compare(name.size() - std::strlen(kNameSuffix), std::strlen(kNameSuffix),
                     kNameSuffix) != 0) {
        return std::nullopt;
    }

    const std::string digits =
        name.substr(prefix.size(), name.size() - prefix.size() - std::strlen(kNameSuffix));
    if (digits.empty() || (digits.size() > 1 && digits[0] == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        // A number this long is not a segment this build can address; treat
        // it as a foreign file rather than wrapping.
        if (value > (UINT64_MAX - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

// Zero-fills a freshly allocated segment so its extents are *written*, not
// merely reserved. posix_fallocate hands back unwritten extents, and the
// first write into each of those costs an extent-conversion journal
// transaction inside the very fsync a commit is waiting on - measured as
// the difference between a ~950us flush and a ~2,100us one on xfs over EBS
// (bench/results-scenario2-freight.md). Paying the whole conversion here,
// once per segment and off every commit path, is what PostgreSQL's
// wal_init_zero does and for the same reason.
Status Prewrite(int fd, std::uint64_t size) {
    // 1 MiB per write: large enough that a 64 MiB segment is 64 syscalls,
    // small enough not to be a resident buffer anyone notices.
    static constexpr std::size_t kChunk = std::size_t{1} << 20;
    const std::vector<std::byte> zeros(kChunk, std::byte{0});
    std::uint64_t at = 0;
    while (at < size) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, size - at));
        const ::ssize_t n = ::pwrite(fd, zeros.data(), want, static_cast<::off_t>(at));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("FileLogDevice: prewrite at offset " + std::to_string(at), errno);
        }
        at += static_cast<std::uint64_t>(n);
    }
    // fsync, not fdatasync: this is the one sync that must also persist the
    // file's metadata (its size, its now-written extents), so that every
    // later commit-path sync has only data to flush.
    while (::fsync(fd) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return ErrnoStatus("FileLogDevice: fsync after prewrite", errno);
    }
    return Status::OK();
}

StatusOr<FileDescriptor> OpenSegmentFile(const std::string& path, bool create_exclusive) {
    const int flags =
        O_RDWR | O_CLOEXEC | (create_exclusive ? (O_CREAT | O_EXCL) : 0);
    int raw_fd = -1;
    do {
        raw_fd = ::open(path.c_str(), flags, 0600);
    } while (raw_fd < 0 && errno == EINTR);
    if (raw_fd < 0) {
        if (create_exclusive && errno == EEXIST) {
            return Status::AlreadyExists("FileLogDevice: segment file " + path +
                                         " already exists but was not adopted at open");
        }
        return ErrnoStatus("FileLogDevice: open " + path, errno);
    }
    return FileDescriptor(raw_fd);
}

}  // namespace

FileLogDevice::FileLogDevice(std::string dir, std::uint32_t core_id,
                             std::uint64_t segment_size) noexcept
    : dir_(std::move(dir)), core_id_(core_id), segment_size_(segment_size) {}

std::string FileLogDevice::SegmentPath(std::uint64_t segment_no) const {
    return dir_ + "/" + NamePrefix(core_id_) + std::to_string(segment_no) + kNameSuffix;
}

StatusOr<std::unique_ptr<FileLogDevice>> FileLogDevice::Open(const std::string& dir,
                                                             std::uint32_t core_id,
                                                             std::uint64_t segment_size) {
    if (segment_size == 0) {
        return Status::InvalidArgument("FileLogDevice: segment_size must be non-zero");
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        // create_directories reports an error for a directory that already
        // existed on some implementations; only an actually-absent directory
        // is a failure.
        std::error_code exists_ec;
        if (!std::filesystem::is_directory(dir, exists_ec)) {
            return Status::IoError("FileLogDevice: cannot create directory " + dir + ": " +
                                   ec.message());
        }
        ec.clear();
    }

    // Collect this core's segments by number first: readdir order says
    // nothing, and the gap check below needs the whole set.
    // Every filesystem call here takes an error_code: the throwing overloads
    // are not usable in engine code (rules.md section 1), and a directory
    // that changes underneath the scan must come back as a Status.
    const std::string prefix = NamePrefix(core_id);
    std::map<std::uint64_t, std::string> found;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        return Status::IoError("FileLogDevice: cannot scan directory " + dir + ": " + ec.message());
    }
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            return Status::IoError("FileLogDevice: cannot scan directory " + dir + ": " +
                                   ec.message());
        }
        if (!it->is_regular_file(ec) || ec) {
            // A non-file, or something that vanished mid-scan: either way it
            // is not a segment this device owns.
            ec.clear();
            continue;
        }
        const std::string name = it->path().filename().string();
        const std::optional<std::uint64_t> segment_no = ParseSegmentNo(name, prefix);
        if (!segment_no.has_value()) {
            // Another core's stream, an archive, anything else - not ours.
            continue;
        }
        found.emplace(*segment_no, it->path().string());
    }

    auto device = std::unique_ptr<FileLogDevice>(new FileLogDevice(dir, core_id, segment_size));

    std::uint64_t expected = 0;
    for (const auto& [segment_no, path] : found) {
        if (segment_no != expected) {
            return Status::Corruption("FileLogDevice: segment " + std::to_string(expected) +
                                      " is missing from " + dir + " (next present is " +
                                      std::to_string(segment_no) + ")");
        }

        auto fd = OpenSegmentFile(path, /*create_exclusive=*/false);
        if (!fd.ok()) {
            return fd.status();
        }

        struct ::stat st {};
        if (::fstat(fd.value().get(), &st) != 0) {
            return ErrnoStatus("FileLogDevice: fstat " + path, errno);
        }
        // A short segment means the file was truncated or the device was
        // opened with a different segment_size than it was written with -
        // either way the LSN arithmetic that maps offsets onto this file no
        // longer holds, so it is Corruption, not something to grow back.
        if (static_cast<std::uint64_t>(st.st_size) != segment_size) {
            return Status::Corruption("FileLogDevice: segment file " + path + " is " +
                                      std::to_string(st.st_size) + " bytes, expected " +
                                      std::to_string(segment_size));
        }

        // Open() runs before any writer thread exists, so this one needs
        // no lock - stated rather than left as an inconsistency.
        device->segments_.push_back(std::move(fd.value()));
        ++expected;
    }

    return device;
}

Status FileLogDevice::CreateSegment(std::uint64_t segment_no) {
    if (segment_no != segments_.size()) {
        return Status::InvalidArgument("FileLogDevice: segments are created in order (expected " +
                                       std::to_string(segments_.size()) + ", got " +
                                       std::to_string(segment_no) + ")");
    }

    const std::string path = SegmentPath(segment_no);
    auto fd = OpenSegmentFile(path, /*create_exclusive=*/true);
    if (!fd.ok()) {
        return fd.status();
    }

    // Full-size reservation up front: an append issued after its record was
    // already accepted into the ring must not be able to fail for space
    // (file_log_device.hpp).
    const int rc = ::posix_fallocate(fd.value().get(), 0, static_cast<::off_t>(segment_size_));
    if (rc != 0) {
        if (!FallocateUnsupported(rc)) {
            std::error_code remove_ec;
            std::filesystem::remove(path, remove_ec);
            return ErrnoStatus("FileLogDevice: posix_fallocate on " + path, rc);
        }
        // Filesystems that cannot preallocate still get a correctly sized
        // sparse file; the prewrite below then allocates the blocks for
        // real, which restores the no-ENOSPC-at-append promise by another
        // route.
        if (::ftruncate(fd.value().get(), static_cast<::off_t>(segment_size_)) != 0) {
            const int err = errno;
            std::error_code remove_ec;
            std::filesystem::remove(path, remove_ec);
            return ErrnoStatus("FileLogDevice: ftruncate on " + path, err);
        }
    }

    if (Status s = Prewrite(fd.value().get(), segment_size_); !s.ok()) {
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
        return s;
    }

    // Directory metadata is synced here, not in Sync(): a crash right after
    // this call must not leave a segment whose name never reached the disk.
    if (Status s = SyncDirectory(); !s.ok()) {
        return s;
    }

    {
        // The only mutation of the table, and the only reason the lock
        // exists: a vector growing under the writer thread's iteration is
        // the one race here that corrupts rather than delays.
        std::lock_guard<std::mutex> guard(segments_mutex_);
        segments_.push_back(std::move(fd.value()));
    }
    return Status::OK();
}

Status FileLogDevice::WriteAt(std::uint64_t segment_no, std::uint64_t offset,
                              std::span<const std::byte> in) {
    if (Status s = CheckSegmentRange(segment_no, offset, in.size(), segments_.size(), segment_size_);
        !s.ok()) {
        return s;
    }

    const std::byte* buffer = in.data();
    std::size_t remaining = in.size();
    std::uint64_t at = offset;
    while (remaining > 0) {
        const ::ssize_t n =
            ::pwrite(segments_[segment_no].get(), buffer, remaining, static_cast<::off_t>(at));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("FileLogDevice: pwrite segment " + std::to_string(segment_no) +
                                   " at offset " + std::to_string(at),
                               errno);
        }
        buffer += n;
        at += static_cast<std::uint64_t>(n);
        remaining -= static_cast<std::size_t>(n);
    }
    return Status::OK();
}

Status FileLogDevice::ReadAt(std::uint64_t segment_no, std::uint64_t offset,
                             std::span<std::byte> out) {
    if (Status s =
            CheckSegmentRange(segment_no, offset, out.size(), segments_.size(), segment_size_);
        !s.ok()) {
        return s;
    }

    std::byte* buffer = out.data();
    std::size_t remaining = out.size();
    std::uint64_t at = offset;
    while (remaining > 0) {
        const ::ssize_t n =
            ::pread(segments_[segment_no].get(), buffer, remaining, static_cast<::off_t>(at));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("FileLogDevice: pread segment " + std::to_string(segment_no) +
                                   " at offset " + std::to_string(at),
                               errno);
        }
        if (n == 0) {
            // Inside the segment but the file ends here: it was created at
            // full size and never shrinks, so the file changed underneath us.
            return Status::Corruption("FileLogDevice: unexpected EOF in segment " +
                                      std::to_string(segment_no) + " at offset " +
                                      std::to_string(at));
        }
        buffer += n;
        at += static_cast<std::uint64_t>(n);
        remaining -= static_cast<std::size_t>(n);
    }
    return Status::OK();
}

Status FileLogDevice::Sync() {
    // Every segment, not just the tail: a torn-page-style partial write to an
    // earlier segment (recovery rewriting a record, an FPI landing late) is
    // still pending until its own fd is synced.
    //
    // **The descriptors are copied out under the lock and synced without
    // it** (see the header's justification). This runs on the WAL writer
    // thread while the reactor may be rolling to a new segment, and holding
    // the lock across an fsync would put the reactor behind the device -
    // which is the thing this whole design exists to stop. A segment created
    // after the copy is simply not covered by *this* sync, which is what the
    // writer's snapshot rule already assumes.
    std::vector<int> raw;
    {
        std::lock_guard<std::mutex> guard(segments_mutex_);
        raw.reserve(segments_.size());
        for (const FileDescriptor& fd : segments_) raw.push_back(fd.get());
    }

    // fdatasync, not fsync: a segment is prewritten at creation, so its
    // size and extents are already durable and the only thing a commit-path
    // sync has left to flush is data. fsync would add a timestamp-metadata
    // journal commit to every durability point for nothing a recovery could
    // ever read.
    for (std::size_t i = 0; i < raw.size(); ++i) {
        while (::fdatasync(raw[i]) != 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("FileLogDevice: fdatasync segment " + std::to_string(i), errno);
        }
    }
    return Status::OK();
}

Status FileLogDevice::SyncDirectory() {
    int raw_fd = -1;
    do {
        raw_fd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (raw_fd < 0 && errno == EINTR);
    if (raw_fd < 0) {
        return ErrnoStatus("FileLogDevice: open directory " + dir_, errno);
    }
    FileDescriptor fd(raw_fd);

    while (::fsync(fd.get()) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return ErrnoStatus("FileLogDevice: fsync directory " + dir_, errno);
    }
    return Status::OK();
}

}  // namespace kds::wal
