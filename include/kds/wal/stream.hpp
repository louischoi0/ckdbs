#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/record.hpp"

// One core's WAL stream (wal.md sections 3, 4.1, 6): the layer that turns
// "append this record" into bytes at a place, and the only thing that knows
// how an LSN maps onto a segment file.
//
// Concurrency: core-local, like the device under it (rules.md section 3).
// One stream, one core, no internal synchronization - the whole point of
// per-core streams is that the append path has no shared tail pointer, no
// lock, and no atomic contention.
//
// ---- LSN arithmetic ------------------------------------------------------
//
// An LSN is a stream-local byte offset (wal.md section 3), counted across
// segments including their headers:
//
//     segment_no = lsn / segment_size
//     offset     = lsn % segment_size
//
// Offset 0 of every segment is its 4 KiB header block, so a valid record
// LSN always has `offset >= kSegmentHeaderSize` and the first record of
// segment N sits at `N * segment_size + kSegmentHeaderSize`. LSN 0 is the
// first segment's header, which is why no record ever has LSN 0 and why
// page_lsn 0 keeps meaning "never logged" (record.hpp).
//
// Plain division rather than shift/mask: the segment size is [OPEN]
// (wal.md section 15), and a power-of-two requirement here would quietly
// decide it. One divide per append is nothing next to the memcpy.
//
// ---- The ring ------------------------------------------------------------
//
// The core-local ring of wal.md section 6 is, today, a preallocated linear
// staging buffer: append is a memcpy plus a cursor bump, and Flush() hands
// the whole staged range to the device in one write and resets the cursor.
// It is not circular, and deliberately so - every I/O path in the engine is
// still synchronous, so there is no in-flight write for an appender to run
// ahead of, and a circular buffer would buy nothing but the inability to
// encode a record that straddles the wrap point. When an asynchronous
// backend lands (the [OPEN] I/O decision), this becomes a real ring; the
// interface above it does not change.
//
// Backpressure is a Status, not a suspension: a full ring fails the append
// with OutOfSpace and the caller drains it. wal.md section 6-4 wants the
// appending task suspended instead, which needs the reactor - when that
// exists, it is the reactor that turns this Status into a suspension.
//
// ---- What this layer does not do ----------------------------------------
//
// Durability classes, group commit, and the commit-ack wait (wal.md
// sections 1 and 6-3) are transaction-layer policy on top of Sync() and
// durable_lsn(); checkpointing and recovery are their own subsystems. This
// file only guarantees that a record handed to Append() is placed at the
// LSN it returns, and that durable_lsn() never claims more than the device
// has actually synced.

namespace kds::wal {

// Proposed ring capacity - [OPEN] in wal.md section 15, so it is a
// parameter with a default, never a constant anything may depend on. Must
// be at least kMinRingCapacity so a record of any legal size can be staged.
inline constexpr std::size_t kDefaultRingCapacity = 1024 * 1024;

// Smallest ring a stream will open with. A record has to fit the ring
// whole - oversized payloads are a design error, not a spanning case
// (wal.md section 4.1) - and the biggest record the catalog defines is a
// FULL_PAGE_IMAGE at kRecordHeaderSize + 8192 bytes, so anything below
// this would reject records the format considers ordinary.
inline constexpr std::size_t kMinRingCapacity = 64 * 1024;

class WalStream {
public:
    // Opens the stream on `device`, which must outlive it.
    //
    // An empty device is a fresh stream: segment 0 is created and its
    // header written. Otherwise the last segment is scanned for the durable
    // end and appending resumes there - the same forward walk recovery
    // does, stopping at the first record that does not decode (wal.md
    // section 4.2). A segment header that does not match this stream
    // (magic, format version, core_id, segment_no, start_lsn) is Corruption
    // rather than something to append past.
    static StatusOr<std::unique_ptr<WalStream>> Open(
        LogDevice* device, std::uint32_t core_id = 0,
        std::size_t ring_capacity = kDefaultRingCapacity);

    std::uint32_t core_id() const noexcept { return core_id_; }
    std::uint64_t segment_size() const noexcept { return segment_size_; }

    // Bytes of a segment a record can actually occupy: everything after the
    // header block. No record may exceed this, since records never span
    // segments.
    std::uint64_t usable_segment_bytes() const noexcept {
        return segment_size_ - kSegmentHeaderSize;
    }

    // LSN the next appended record will be stamped with - unless the
    // current segment is sealed, in which case the next append rolls and
    // the LSN jumps to the next segment's first record. `sealed()` says
    // which.
    Lsn append_lsn() const noexcept { return append_lsn_; }

    // Everything below this has been handed to the device; everything below
    // durable_lsn() has been synced by it. The gap between them is exactly
    // what a crash would lose.
    Lsn flushed_lsn() const noexcept { return append_lsn_ - ring_used_; }
    Lsn durable_lsn() const noexcept { return durable_lsn_; }

    bool sealed() const noexcept { return sealed_; }
    std::size_t ring_used() const noexcept { return ring_used_; }
    std::size_t ring_capacity() const noexcept { return ring_.size(); }

    // The device under this stream, for the one caller that has to reach it
    // past the stream: the WAL writer thread syncs the device while this
    // stream keeps staging and writing on the reactor (wal/writer.hpp). It
    // is deliberately not an owning handle - the device outlives both.
    LogDevice* device() const noexcept { return device_; }
    std::size_t ring_free() const noexcept { return ring_.size() - ring_used_; }

    // Stages one record and returns the LSN it was placed at.
    //
    // Rolls to a new segment first if the record does not fit the current
    // one: the tail is sealed with a PAD marker, the ring is drained, and
    // the next segment is created and headered. That roll is the one path
    // on which Append does I/O.
    //
    // Fails with InvalidArgument for a record larger than the ring or than
    // a whole segment's usable space, and with OutOfSpace when the ring is
    // full - the caller drains and retries.
    StatusOr<Lsn> Append(const RecordSpec& spec, std::span<const std::byte> payload = {});

    // Writes the staged bytes to the device. Not durable until Sync().
    Status Flush();

    // Flush() plus a device sync; advances durable_lsn() to flushed_lsn()
    // only if the device reports success, so a failed sync never moves the
    // durable point.
    Status Sync();

    // Seals the current segment with a PAD marker and drains the ring, so
    // nothing more is ever written to it. The next Append rolls. Sealing an
    // already-sealed stream is a no-op.
    Status Seal();

private:
    WalStream(LogDevice* device, std::uint32_t core_id, std::size_t ring_capacity);

    std::uint64_t SegmentOf(Lsn lsn) const noexcept { return lsn / segment_size_; }
    std::uint64_t OffsetOf(Lsn lsn) const noexcept { return lsn % segment_size_; }

    // Bytes left in the current segment after append_lsn_.
    //
    // An offset of exactly 0 is **one past the end of a segment an append
    // just filled**, never a position inside one - every segment opens
    // with its header block, so no record ever sits at offset 0. The
    // unguarded form answered a full segment there, which is the wedge
    // the bulk-insert bench found (bench/results-bulk-insert.md): the
    // roll was skipped, the next record was placed at the start of a
    // segment that had never been created, and every later Flush refused
    // the boundary-spanning range - permanently, at ~300K logged rows.
    std::uint64_t SegmentRemaining() const noexcept {
        const std::uint64_t offset = OffsetOf(append_lsn_);
        return offset == 0 ? 0 : segment_size_ - offset;
    }

    Status StartSegment(std::uint64_t segment_no);
    Status Roll();
    // Walks segment `segment_no` forward and leaves append_lsn_ at its
    // durable end, or seals it if a PAD marker is found.
    Status ScanTail(std::uint64_t segment_no);

    LogDevice* device_;
    std::uint32_t core_id_;
    std::uint64_t segment_size_ = 0;

    std::vector<std::byte> ring_;
    std::size_t ring_used_ = 0;

    Lsn append_lsn_ = 0;
    Lsn durable_lsn_ = 0;
    bool sealed_ = false;

    // Preallocated so writing a segment header never allocates.
    std::vector<std::byte> header_block_;
};

}  // namespace kds::wal
