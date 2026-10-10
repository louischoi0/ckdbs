#include "kds/wal/file_log_device.hpp"

#include <fcntl.h>
#include <stdio.h>
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
// A segment under construction (`CreateSegment`): not a name `Open` adopts.
constexpr const char* kTempSuffix = ".tmp";

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

// Full-size reservation, or a sized sparse file where the filesystem cannot
// preallocate - the prewrite that follows then allocates the blocks for real,
// which restores the no-ENOSPC-at-append promise by another route.
Status Reserve(int fd, std::uint64_t size, const std::string& path) {
    const int rc = ::posix_fallocate(fd, 0, static_cast<::off_t>(size));
    if (rc == 0) return Status::OK();
    if (!FallocateUnsupported(rc)) {
        return ErrnoStatus("FileLogDevice: posix_fallocate on " + path, rc);
    }
    if (::ftruncate(fd, static_cast<::off_t>(size)) != 0) {
        return ErrnoStatus("FileLogDevice: ftruncate on " + path, errno);
    }
    return Status::OK();
}

// A segment's header, written over its zeroed first block and made durable
// before the rename: `fdatasync`, since the file's size and extents reached
// the disk with the body's `fsync` (`Prewrite`).
Status WriteHeaderAndSync(int fd, std::span<const std::byte> header) {
    for (std::size_t done = 0; done < header.size();) {
        const ::ssize_t n =
            ::pwrite(fd, header.data() + done, header.size() - done, static_cast<::off_t>(done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return ErrnoStatus("FileLogDevice: header write", errno);
        }
        done += static_cast<std::size_t>(n);
    }
    while (::fdatasync(fd) != 0) {
        if (errno == EINTR) continue;
        return ErrnoStatus("FileLogDevice: fdatasync after the header", errno);
    }
    return Status::OK();
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

FileLogDevice::FileLogDevice(std::string dir, std::uint32_t core_id, std::uint64_t segment_size,
                             FileDescriptor dir_fd) noexcept
    : dir_(std::move(dir)),
      core_id_(core_id),
      segment_size_(segment_size),
      dir_fd_(std::move(dir_fd)) {}

std::string FileLogDevice::SegmentPath(std::uint64_t segment_no) const {
    return dir_ + "/" + NamePrefix(core_id_) + std::to_string(segment_no) + kNameSuffix;
}

StatusOr<std::unique_ptr<FileLogDevice>> FileLogDevice::Open(const std::string& dir,
                                                             std::uint32_t core_id,
                                                             std::uint64_t segment_size,
                                                             std::uint64_t first_needed) {
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

    int raw_dir_fd = -1;
    do {
        raw_dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (raw_dir_fd < 0 && errno == EINTR);
    if (raw_dir_fd < 0) {
        return ErrnoStatus("FileLogDevice: open directory " + dir, errno);
    }

    auto device = std::unique_ptr<FileLogDevice>(
        new FileLogDevice(dir, core_id, segment_size, FileDescriptor(raw_dir_fd)));
    device->first_.store(first_needed, std::memory_order_relaxed);

    // BC-R3: below the anchor's segment is a leftover, not opened and not
    // deleted here; at and above it, the live run, whole.
    if (first_needed > 0 && (found.empty() || found.rbegin()->first < first_needed)) {
        return Status::Corruption("FileLogDevice: the anchor needs segment " +
                                  std::to_string(first_needed) + ", and " + dir +
                                  " holds no segment at or above it");
    }
    std::uint64_t expected = first_needed;
    for (const auto& [segment_no, path] : found) {
        if (segment_no < first_needed) {
            device->detached_.push_back(segment_no);
            continue;
        }
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
        device->segments_.push_back(std::make_shared<const FileDescriptor>(std::move(fd.value())));
        ++expected;
    }

    return device;
}

void FileLogDevice::PrepareSegment(std::uint64_t segment_no) {
    std::lock_guard<std::mutex> guard(request_mutex_);
    if (stopping_) return;
    requested_no_ = segment_no;
    if (!preparer_.joinable()) preparer_ = std::thread([this] { RunPreparer(); });
    request_cv_.notify_one();
}

FileLogDevice::~FileLogDevice() {
    {
        std::lock_guard<std::mutex> guard(request_mutex_);
        stopping_ = true;
    }
    request_cv_.notify_one();
    if (preparer_.joinable()) preparer_.join();
}

void FileLogDevice::RunPreparer() {
    for (;;) {
        std::uint64_t want = 0;
        {
            std::unique_lock<std::mutex> guard(request_mutex_);
            request_cv_.wait(guard, [this] {
                return stopping_ || requested_no_ != std::numeric_limits<std::uint64_t>::max();
            });
            if (stopping_) return;
            want = std::exchange(requested_no_, std::numeric_limits<std::uint64_t>::max());
        }
        BuildAhead(want);
    }
}

StatusOr<FileDescriptor> FileLogDevice::BuildBody(const std::string& temp) {
    if (::unlink(temp.c_str()) != 0 && errno != ENOENT) {
        return ErrnoStatus("FileLogDevice: remove leftover " + temp, errno);
    }
    auto fd = OpenSegmentFile(temp, /*create_exclusive=*/true);
    if (!fd.ok()) return fd.status();
    const auto abandon = [&temp](Status s) {
        std::error_code remove_ec;
        std::filesystem::remove(temp, remove_ec);
        return s;
    };
    // Full-size reservation up front: an append issued after its record was
    // already accepted into the ring must not be able to fail for space
    // (file_log_device.hpp).
    if (Status s = Reserve(fd.value().get(), segment_size_, temp); !s.ok()) return abandon(s);
    if (Status s = Prewrite(fd.value().get(), segment_size_); !s.ok()) return abandon(s);
    return fd;
}

void FileLogDevice::BuildAhead(std::uint64_t segment_no) {
    std::lock_guard<std::mutex> guard(prepare_mutex_);
    std::uint64_t end = 0;
    {
        // Under the table's lock: this thread is outside the stream's
        // serialization, and a detach may change the table beside it. A
        // creation cannot - it holds `prepare_mutex_`.
        std::lock_guard<std::mutex> table(segments_mutex_);
        end = end_segment();
    }
    if (segment_no != end || prepared_no_ == segment_no) return;
    // A failed build is dropped: the creation then builds the body itself.
    auto fd = BuildBody(SegmentPath(segment_no) + kTempSuffix);
    if (!fd.ok()) return;
    prepared_fd_ = std::move(fd.value());
    prepared_no_ = segment_no;
}

Status FileLogDevice::CreateSegment(std::uint64_t segment_no, std::span<const std::byte> header) {
    if (segment_no != end_segment()) {
        return Status::InvalidArgument("FileLogDevice: segments are created in order (expected " +
                                       std::to_string(end_segment()) + ", got " +
                                       std::to_string(segment_no) + ")");
    }
    std::lock_guard<std::mutex> prepared(prepare_mutex_);

    // **Built under a name `Open` never adopts, and renamed when whole.** A
    // power cut inside a creation - during the prewrite, before its fsync -
    // can still make the new file's name and size durable through any other
    // journal commit, and a full-size segment with a zeroed header under the
    // final name is one the mount refuses. Under the temporary name it is
    // nothing: `Open` adopts only `<prefix><n>.log`, and the next creation
    // of the same number removes the leftover first. The rename happens only
    // after the fsync that made the body and the header durable, and it
    // never replaces a file already there.
    const std::string path = SegmentPath(segment_no);
    const std::string temp = path + kTempSuffix;
    const std::string temp_name = std::filesystem::path(temp).filename().string();
    const std::string final_name = std::filesystem::path(path).filename().string();
    const auto abandon = [&temp](Status s) {
        std::error_code remove_ec;
        std::filesystem::remove(temp, remove_ec);
        return s;
    };
    // **The body built ahead or here, then the header** (BA-S12): either way
    // the body is reserved, zeroed and synced first, and the header is
    // written over its first block and synced before the rename - so a
    // crash never leaves a full-size segment with a zeroed header under the
    // final name. Ahead (`PrepareSegment`) the body costs this call nothing.
    StatusOr<FileDescriptor> fd = FileDescriptor();
    if (prepared_no_ == segment_no) {
        fd = std::move(prepared_fd_);
        prepared_no_ = std::numeric_limits<std::uint64_t>::max();
    } else {
        fd = BuildBody(temp);
        if (!fd.ok()) return fd.status();
    }
    if (Status s = WriteHeaderAndSync(fd.value().get(), header); !s.ok()) return abandon(s);

    // The final name, refused if a file already holds it: segments are
    // created once. A filesystem that refuses `RENAME_NOREPLACE` (`EINVAL`:
    // some NFS and FUSE mounts) gets the same no-replace move as a hard link
    // to the final name and the temporary name's removal.
    int moved = ::renameat2(dir_fd_.get(), temp_name.c_str(), dir_fd_.get(), final_name.c_str(),
                            RENAME_NOREPLACE);
    if (moved != 0 && errno == EINVAL) {
        moved = ::linkat(dir_fd_.get(), temp_name.c_str(), dir_fd_.get(), final_name.c_str(), 0);
        if (moved == 0) (void)::unlinkat(dir_fd_.get(), temp_name.c_str(), 0);
    }
    if (moved != 0) {
        const int err = errno;
        if (err == EEXIST) {
            return abandon(Status::AlreadyExists("FileLogDevice: segment file " + path +
                                                 " already exists but was not adopted at open"));
        }
        return abandon(ErrnoStatus("FileLogDevice: rename " + temp + " to " + path, err));
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
        segments_.push_back(std::make_shared<const FileDescriptor>(std::move(fd.value())));
    }
    return Status::OK();
}

Status FileLogDevice::WriteAt(std::uint64_t segment_no, std::uint64_t offset,
                              std::span<const std::byte> in) {
    if (Status s = CheckSegmentRange(segment_no, offset, in.size(), first_segment(), end_segment(),
                                     segment_size_);
        !s.ok()) {
        return s;
    }

    const int fd = segments_[segment_no - first_segment()]->get();
    const std::byte* buffer = in.data();
    std::size_t remaining = in.size();
    std::uint64_t at = offset;
    while (remaining > 0) {
        const ::ssize_t n =
            ::pwrite(fd, buffer, remaining, static_cast<::off_t>(at));
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
    if (Status s = CheckSegmentRange(segment_no, offset, out.size(), first_segment(), end_segment(),
                                     segment_size_);
        !s.ok()) {
        return s;
    }

    const int fd = segments_[segment_no - first_segment()]->get();
    std::byte* buffer = out.data();
    std::size_t remaining = out.size();
    std::uint64_t at = offset;
    while (remaining > 0) {
        const ::ssize_t n =
            ::pread(fd, buffer, remaining, static_cast<::off_t>(at));
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
    // Every segment, not just the tail. Two reasons, and the second is a
    // correctness dependency another file rests on:
    //
    //   - a torn-page-style partial write to an earlier segment (recovery
    //     rewriting a record, an FPI landing late) is still pending until
    //     its own fd is synced;
    //   - **`WalStream::Sync` captures its flushed watermark, releases the
    //     stream latch, and only then calls this** (`wal/stream.hpp`), so
    //     another core can roll to a new segment in between. Narrowing
    //     this to the newest fd would silently publish a watermark whose
    //     bytes live in the previous segment and were never synced.
    //     Do not "optimise" it to the tail.
    //
    // **The descriptors are copied out under the lock and synced without
    // it** (see the header's justification). This runs on the WAL writer
    // thread while the reactor may be rolling to a new segment, and holding
    // the lock across an fsync would put the reactor behind the device -
    // which is the thing this whole design exists to stop. A segment created
    // after the copy is simply not covered by *this* sync, which is what the
    // writer's snapshot rule already assumes.
    //
    // **Copied as owners, not as numbers** (BC-R4): a segment detached after
    // the copy stays open until this sync lets go of it, so no `fdatasync`
    // below can land on a closed or reused descriptor.
    std::vector<std::shared_ptr<const FileDescriptor>> held;
    std::uint64_t first = 0;
    {
        std::lock_guard<std::mutex> guard(segments_mutex_);
        held.assign(segments_.begin(), segments_.end());
        first = first_segment();
    }

    // fdatasync, not fsync: a segment is prewritten at creation, so its
    // size and extents are already durable and the only thing a commit-path
    // sync has left to flush is data. fsync would add a timestamp-metadata
    // journal commit to every durability point for nothing a recovery could
    // ever read.
    for (std::size_t i = 0; i < held.size(); ++i) {
        while (::fdatasync(held[i]->get()) != 0) {
            if (errno == EINTR) {
                continue;
            }
            return ErrnoStatus("FileLogDevice: fdatasync segment " + std::to_string(first + i),
                               errno);
        }
    }
    return Status::OK();
}

Status FileLogDevice::DetachBelow(std::uint64_t segment_no) {
    std::lock_guard<std::mutex> guard(segments_mutex_);
    if (Status s = CheckDetachBound(segment_no, first_segment(), end_segment()); !s.ok()) {
        return s;
    }
    // The table lets go of its reference; a `Sync` still holding one keeps
    // the file open until it returns (BC-R4). No I/O here.
    for (std::uint64_t s = first_segment(); s < segment_no; ++s) {
        segments_.pop_front();
        detached_.push_back(s);
    }
    first_.store(segment_no, std::memory_order_release);
    return Status::OK();
}

Status FileLogDevice::ReclaimDetached() {
    std::vector<std::uint64_t> pending;
    {
        std::lock_guard<std::mutex> guard(segments_mutex_);
        pending.swap(detached_);
    }
    if (pending.empty()) {
        return Status::OK();
    }

    Status first_failure = Status::OK();
    std::vector<std::uint64_t> unlinked;
    std::vector<std::uint64_t> requeue;
    for (const std::uint64_t segment_no : pending) {
        const std::string path = SegmentPath(segment_no);
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
            const int err = errno;
            if (first_failure.ok()) {
                first_failure = ErrnoStatus("FileLogDevice: unlink " + path, err);
            }
            requeue.push_back(segment_no);
            continue;
        }
        unlinked.push_back(segment_no);
    }
    // One directory sync for the batch: until it returns, a crash may bring
    // back any of these names, which the next `Open` skips (BC-R3).
    if (Status s = SyncDirectory(); !s.ok()) {
        if (first_failure.ok()) first_failure = s;
        requeue.insert(requeue.end(), unlinked.begin(), unlinked.end());
        unlinked.clear();
    }

    std::lock_guard<std::mutex> guard(segments_mutex_);
    detached_.insert(detached_.end(), requeue.begin(), requeue.end());
    removed_ += unlinked.size();
    return first_failure;
}

std::uint64_t FileLogDevice::segments_removed() const noexcept {
    std::lock_guard<std::mutex> guard(segments_mutex_);
    return removed_;
}

Status FileLogDevice::SyncDirectory() {
    std::lock_guard<std::mutex> guard(dir_sync_mutex_);
    if (dir_sync_failed_) {
        return Status::IoError("FileLogDevice: an earlier fsync of directory " + dir_ +
                               " failed; no later one is trusted");
    }
    while (::fsync(dir_fd_.get()) != 0) {
        if (errno == EINTR) {
            continue;
        }
        const int err = errno;
        dir_sync_failed_ = true;
        return ErrnoStatus("FileLogDevice: fsync directory " + dir_, err);
    }
    return Status::OK();
}

}  // namespace kds::wal
