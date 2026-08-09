#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
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
// Segment files are created at full size - posix_fallocate for the space
// promise, then zero-filled and fsynced - so a later append cannot fail for
// lack of space after the record it belongs to was already accepted, and so
// the commit path never writes into an unwritten extent. That second half
// is a latency property: converting a reserved extent to a written one is a
// journal transaction, and paying it inside the fsync a commit waits for
// measured as the difference between a ~950us flush and a ~2,100us one
// (bench/results-scenario2-freight.md). Creation therefore costs one
// sequential segment-sized write, and every commit-path Sync() is
// data-only fdatasync.
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

    // fdatasync of every open segment - data only, because a segment's size
    // and extents were made durable at creation. Directory metadata (the
    // segment files' existence) is synced when a segment is created, not
    // here, so a crash right after CreateSegment cannot leave a nameless
    // file.
    Status Sync() override;

private:
    FileLogDevice(std::string dir, std::uint32_t core_id, std::uint64_t segment_size) noexcept;

    Status SyncDirectory();

    std::string dir_;
    std::uint32_t core_id_;
    std::uint64_t segment_size_;

    // ---- The one lock in the log device (rules.md §3's justification) ---
    //
    // **What it protects:** `segments_` - the open segment descriptors -
    // and nothing else. **Acquisition order:** innermost; nothing is taken
    // while it is held, and it is *never* held across an `fsync` or a
    // `pwrite`.
    //
    // It exists because the WAL writer thread (wal/writer.hpp) syncs while
    // the reactor may be rolling to a new segment, and a vector that grows
    // under an iterator is the one race here that corrupts rather than
    // merely delays. `Sync()` copies the descriptors under it, releases,
    // and does the I/O outside - so the reactor can roll a segment while a
    // sync is in flight, and the sync covers the segments that existed when
    // it started, which is exactly what the writer's snapshot rule wants.
    //
    // Concurrent `pwrite` and `fsync` on one descriptor need no lock: the
    // kernel serializes them, and whether the fsync includes a write racing
    // with it is precisely the question `WalWriter` answers by publishing
    // the watermark it was *asked* for rather than the current one.
    mutable std::mutex segments_mutex_;
    std::vector<FileDescriptor> segments_;
};

}  // namespace kds::wal
