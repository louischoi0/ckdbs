#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
// Concurrency: one device per stream, and the stream is either one core's
// or the instance's (`wal/stream.hpp`, `wal/log_device.hpp`'s contract).
// `CreateSegment` and `DetachBelow` change `segments_` under
// `segments_mutex_`, while `Sync` copies the descriptors under the same lock
// and does its `fdatasync`s outside it. The unlocked reads in
// `WriteAt`/`ReadAt` are safe because the contract serializes every one of
// them against both table changes - the stream's latch, or its one thread.
// **A flush's `WriteAt` runs outside the latch since BA-S12**, and stays
// serialized all the same: the stream claims it under the latch, and a roll
// or a detach waits a claimed write out before it changes the table
// (`WalStream::WaitOutFlushInFlight`). The preparer thread reads the table
// under `segments_mutex_`.
// Concurrent `pwrite` and `fdatasync` on one descriptor need no lock at all
// (the note beside the mutex below).
//
// **A descriptor outlives every `Sync` that copied it** (BC-R4). The table
// holds each one by `shared_ptr`, and so does a `Sync`'s copy, so a segment
// detached while a sync is in flight is closed when that sync lets go of
// it - never under it, where its number could already name another file.
// The cost moves with it: the filesystem frees an unlinked segment's blocks
// at the last `close`, which can then be a syncing thread's.

namespace kds::wal {

class FileLogDevice final : public LogDevice {
public:
    // Opens `dir` (creating it if absent) and adopts the segments for
    // `core_id` from `first_needed` up, which is how recovery finds the
    // stream. `first_needed` is the segment holding the mount anchor's redo
    // start (BC-R3), 0 when nothing has been recycled past:
    //
    //   - a segment below it is **neither opened nor deleted**. It is a
    //     leftover of a removal a crash interrupted, gaps among them are
    //     expected, and it is queued for the next `ReclaimDetached` - after
    //     the mount has succeeded, so a refused mount leaves its log whole;
    //   - every segment from it to the highest present must exist, and a gap
    //     is Corruption, since segments are created in order;
    //   - none at or above it, when it is above 0, is Corruption: the anchor
    //     names a log that is not there.
    //
    // A segment file whose size is not exactly `segment_size` is Corruption
    // rather than silently accepted.
    static StatusOr<std::unique_ptr<FileLogDevice>> Open(
        const std::string& dir, std::uint32_t core_id = 0,
        std::uint64_t segment_size = kDefaultSegmentSize, std::uint64_t first_needed = 0);

    std::uint64_t segment_size() const noexcept override { return segment_size_; }
    std::uint64_t first_segment() const noexcept override {
        return first_.load(std::memory_order_acquire);
    }
    // Unlocked, under the contract's serialization: only the stream's own
    // calls read the run's end, and they never run beside a table change.
    std::uint64_t end_segment() const noexcept override {
        return first_segment() + segments_.size();
    }

    std::uint32_t core_id() const noexcept { return core_id_; }
    const std::string& dir() const noexcept { return dir_; }
    std::string SegmentPath(std::uint64_t segment_no) const;

    Status CreateSegment(std::uint64_t segment_no,
                         std::span<const std::byte> header = {}) override;
    // Hands the number to this device's preparer thread, which builds the
    // body under the temporary name `Open` never adopts - the reservation,
    // the zeroing and their fsync. `CreateSegment` of the same number then
    // writes the header, syncs it, and renames.
    void PrepareSegment(std::uint64_t segment_no) override;
    ~FileLogDevice() override;
    // The number of the segment built ahead, or none: what BA-S12's cells
    // wait on before a creation that should adopt it.
    std::uint64_t PreparedSegmentForTest() {
        std::lock_guard<std::mutex> guard(prepare_mutex_);
        return prepared_no_;
    }
    Status WriteAt(std::uint64_t segment_no, std::uint64_t offset,
                   std::span<const std::byte> in) override;
    Status ReadAt(std::uint64_t segment_no, std::uint64_t offset,
                  std::span<std::byte> out) override;

    // fdatasync of every segment in the live run - data only, because a
    // segment's size and extents were made durable at creation. Directory
    // metadata (the segment files' existence) is synced when a segment is
    // created or reclaimed, not here, so a crash right after CreateSegment
    // cannot leave a nameless file.
    Status Sync() override;

    Status DetachBelow(std::uint64_t segment_no) override;

    // Unlinks every detached segment and every leftover `Open` skipped -
    // a name already gone counts as removed - then fsyncs the directory
    // once. A failed unlink or a failed directory sync requeues what it
    // covered, so the next call repeats it.
    Status ReclaimDetached() override;

    std::uint64_t segments_removed() const noexcept override;

private:
    FileLogDevice(std::string dir, std::uint32_t core_id, std::uint64_t segment_size,
                  FileDescriptor dir_fd) noexcept;

    // **One at a time, and a failure is sticky** (fsync's fail-stop): a roll
    // and a reclaim both sync the directory, from two threads, and two
    // overlapping fsyncs can hand one the error and the other OK - a roll
    // told OK over a name that never reached the disk. After one failure
    // every later directory sync fails, so the next roll fails and stops
    // the log (`wal/stream.hpp`).
    //
    // Over `dir_fd_`, which `Open` opened once and keeps: a roll at the
    // descriptor limit then fails at the segment's own open, before any
    // file exists, rather than at the directory's after one does - which
    // stranded a full-size headerless segment until BC-S2 (the bug entry,
    // deleted with the fix, is at `19e1dc1f`).
    Status SyncDirectory();

    std::string dir_;
    std::uint32_t core_id_;
    std::uint64_t segment_size_;
    FileDescriptor dir_fd_;

    // **The one segment built ahead** (`PrepareSegment`), its number and
    // its descriptor on the temporary name. `prepare_mutex_` serializes a
    // preparation against the creation that adopts it; it is held across
    // the preparation's I/O, so a roll that reaches the same number while
    // the writer is still building it waits that out - no longer than the
    // creation would have taken itself. Taken before `segments_mutex_`,
    // never under it.
    std::mutex prepare_mutex_;
    std::uint64_t prepared_no_ = std::numeric_limits<std::uint64_t>::max();  // under prepare_mutex_
    FileDescriptor prepared_fd_;                                             // under prepare_mutex_
    // The building itself, on a thread of its own rather than the WAL
    // writer's: a 64 MiB zeroing on the writer would hold every sync a
    // committer is parked on behind it. Started at the first request, so a
    // device nobody rolls owns no thread. A failed build is dropped - the
    // creation then builds the segment itself, as it did before BA-S12.
    void BuildAhead(std::uint64_t segment_no);
    // A segment body under the temporary name `temp`: the leftover removed,
    // the reservation, the zeroing and their fsync. The header is the
    // creation's (`CreateSegment`).
    StatusOr<FileDescriptor> BuildBody(const std::string& temp);
    void RunPreparer();
    std::mutex request_mutex_;
    std::condition_variable request_cv_;
    std::uint64_t requested_no_ = std::numeric_limits<std::uint64_t>::max();  // under request_mutex_
    bool stopping_ = false;                                                   // under request_mutex_
    std::thread preparer_;
    // `SyncDirectory`'s serialization and its sticky failure. Innermost.
    std::mutex dir_sync_mutex_;
    bool dir_sync_failed_ = false;  // under dir_sync_mutex_; never cleared

    // ---- The one lock in the log device (rules.md §3's justification) ---
    //
    // **What it protects:** `segments_` and `first_` - the live run - and
    // `detached_` and `removed_`, the reclaim queue and its count.
    // **Acquisition order:** innermost; nothing is taken
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
    // Segment `first_ + i` is `segments_[i]`. A deque, because a detach
    // takes from the front and a roll adds at the back.
    std::deque<std::shared_ptr<const FileDescriptor>> segments_;
    // Atomic so `first_segment()` may be read from any thread (`SHOW META`);
    // written only under the mutex.
    std::atomic<std::uint64_t> first_{0};
    // Segment numbers to unlink: detached, or skipped below `first_needed`.
    std::vector<std::uint64_t> detached_;
    std::uint64_t removed_ = 0;
};

}  // namespace kds::wal
