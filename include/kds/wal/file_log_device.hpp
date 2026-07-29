#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kds/base/file_descriptor.hpp"
#include "kds/wal/log_device.hpp"

// The on-disk LogDevice: one file per segment, in a directory of their
// own, named
//
//   <dir>/wal-<core_id>-<segment_no>.log
//
// One file per segment rather than one growing file, because sealing,
// archiving, and recycling are all whole-segment operations (wal.md
// sections 4.1, 11, 13) - "archive a segment" is then a file copy and
// "recycle" is a rename or unlink, with no hole-punching.
//
// Segment files are created at full size via posix_fallocate, so a later
// append cannot fail for lack of space after the record it belongs to was
// already accepted. Sparse reads inside that space return zeroes.
//
// pwrite/pread rather than seek+write: no shared file offset means no
// hidden state between calls, matching FilePageDevice.
//
// O_DIRECT is not enabled, for the same reason as FilePageDevice: it is
// bound up with the still-open I/O backend decision (CLAUDE.md) and would
// impose alignment requirements on the ring buffer that nothing satisfies
// yet. Nothing here blocks it later.
//
// Concurrency: core-local, one device per stream.

namespace kds::wal {

class FileLogDevice final : public LogDevice {
public:
    // Opens `dir` (creating it if absent) and adopts whatever segments for
    // `core_id` are already there, which is how recovery finds the stream.
    // A segment file whose size is not exactly `segment_size` is reported
    // as Corruption rather than silently accepted, and a gap in the
    // numbering is Corruption too - segments are created in order.
    static StatusOr<std::unique_ptr<FileLogDevice>> Open(
        const std::string& dir, std::uint32_t core_id = 0,
        std::uint64_t segment_size = kDefaultSegmentSize);

    std::uint64_t segment_size() const noexcept override { return segment_size_; }
    std::uint64_t segment_count() const noexcept override { return segments_.size(); }

    std::uint32_t core_id() const noexcept { return core_id_; }
    const std::string& dir() const noexcept { return dir_; }
    std::string SegmentPath(std::uint64_t segment_no) const;

    Status CreateSegment(std::uint64_t segment_no) override;
    Status WriteAt(std::uint64_t segment_no, std::uint64_t offset,
                   std::span<const std::byte> in) override;
    Status ReadAt(std::uint64_t segment_no, std::uint64_t offset,
                  std::span<std::byte> out) override;

    // fsync of every open segment. Directory metadata (the segment files'
    // existence) is synced when a segment is created, not here, so a crash
    // right after CreateSegment cannot leave a nameless file.
    Status Sync() override;

private:
    FileLogDevice(std::string dir, std::uint32_t core_id, std::uint64_t segment_size) noexcept;

    Status SyncDirectory();

    std::string dir_;
    std::uint32_t core_id_;
    std::uint64_t segment_size_;
    std::vector<FileDescriptor> segments_;
};

}  // namespace kds::wal
