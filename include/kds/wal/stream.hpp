#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "kds/base/spin_latch.hpp"
#include "kds/base/status.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/record.hpp"

// The WAL stream (wal.md sections 3, 4.1, 6): the layer that turns "append
// this record" into bytes at a place, and the only thing that knows how an
// LSN maps onto a segment file.
//
// ---- Concurrency: one core's stream, or the instance's -------------------
//
// AR0 M0 (`instructions/v3.0.0/workorder-al-m0-single-wal.md`, AL-R1/AL-R2)
// makes this the one stream every core appends to. Opened **unshared** -
// the `cores = 1` path and every in-process test - it is core-local like
// the device under it (rules.md section 3): one thread, no latch taken, and
// the gauges' relaxed atomics compile to the plain moves they replaced.
// Opened **shared**, one spin latch guards the staging state:
//
//   - `Append`, from any thread: latch; size checks; the roll if the
//     segment is full; encode into the staging buffer; bump the cursor;
//     unlatch. The LSN is fixed under the latch, so a record's address is
//     known before its bytes are visible to anyone else - `StampPageLsn`
//     and the `recLSN` discipline (page.md section 8) are unchanged.
//   - `Flush` and `Seal`, from any thread: the device write happens
//     **under the latch**. Accepted for M0's first cut and named here: a
//     flush is a page-cache copy of what was staged since the last one,
//     microseconds against the 1 ms drain cadence, and a roll's segment
//     creation is one per 64 MiB. AL-S8 measures the spin this costs; a
//     swap-and-write outside the latch is the follow-on if it shows.
//   - `Sync`, from any thread: the flush under the latch, the device sync
//     **outside** it - milliseconds, and nobody may spin for those - and
//     the durable watermark published as a maximum, so two syncers can
//     never pull it back.
//   - The gauges (`append_lsn`, `flushed_lsn`, `durable_lsn`, `ring_used`,
//     `sealed`) are relaxed atomics: written under the latch, readable
//     from any thread without it as the instantaneous values they are.
//
// A device under a shared stream must accept `WriteAt`/`CreateSegment` on
// one thread while `Sync` runs on another (log_device.hpp).
//
// ---- LSN arithmetic ------------------------------------------------------
//
// An LSN is a byte offset into the stream, counted across segments
// including their headers:
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
    //
    // `shared` arms the latch (the concurrency section above). Off, the
    // stream is one thread's and the latch is never touched.
    static StatusOr<std::unique_ptr<WalStream>> Open(
        LogDevice* device, std::uint32_t core_id = 0,
        std::size_t ring_capacity = kDefaultRingCapacity, bool shared = false);

    std::uint32_t core_id() const noexcept { return core_id_; }
    std::uint64_t segment_size() const noexcept { return segment_size_; }
    bool shared() const noexcept { return latch_ != nullptr; }

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
    Lsn append_lsn() const noexcept { return append_lsn_.load(std::memory_order_relaxed); }

    // Everything below this has been handed to the device; everything below
    // durable_lsn() has been synced by it. The gap between them is exactly
    // what a crash would lose.
    Lsn flushed_lsn() const noexcept { return flushed_lsn_.load(std::memory_order_relaxed); }
    Lsn durable_lsn() const noexcept { return durable_lsn_.load(std::memory_order_acquire); }

    bool sealed() const noexcept { return sealed_.load(std::memory_order_relaxed); }
    std::size_t ring_used() const noexcept { return ring_used_.load(std::memory_order_relaxed); }
    std::size_t ring_capacity() const noexcept { return ring_.size(); }
    std::size_t ring_free() const noexcept { return ring_.size() - ring_used(); }

    // The device under this stream, for the one caller that has to reach it
    // past the stream: the WAL writer thread syncs the device while this
    // stream keeps staging and writing (wal/writer.hpp). It is deliberately
    // not an owning handle - the device outlives both.
    LogDevice* device() const noexcept { return device_; }

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

    // Flush() plus a device sync; advances durable_lsn() to what that flush
    // had handed over, only if the device reports success, so a failed
    // sync never moves the durable point.
    Status Sync();

    // Seals the current segment with a PAD marker and drains the ring, so
    // nothing more is ever written to it. The next Append rolls. Sealing an
    // already-sealed stream is a no-op.
    Status Seal();

private:
    WalStream(LogDevice* device, std::uint32_t core_id, std::size_t ring_capacity, bool shared);

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
        const std::uint64_t offset = OffsetOf(append_lsn());
        return offset == 0 ? 0 : segment_size_ - offset;
    }

    // The `*Locked` halves run with the latch held (or unshared). Every
    // public entry point takes the latch once and calls one of them; the
    // roll on Append's path reaches Flush and Seal through these, never
    // through the public names, so the latch is never taken twice.
    Status StartSegment(std::uint64_t segment_no);
    Status Roll();
    Status FlushLocked();
    Status SealLocked();
    // Walks segment `segment_no` forward and leaves append_lsn_ at its
    // durable end, or seals it if a PAD marker is found. Open() only.
    Status ScanTail(std::uint64_t segment_no);
    // Moves the durable watermark forward to `lsn`, never back: a plain
    // store unshared, a compare-exchange maximum shared.
    void PublishDurable(Lsn lsn) noexcept;

    LogDevice* device_;
    std::uint32_t core_id_;
    std::uint64_t segment_size_ = 0;

    std::vector<std::byte> ring_;
    std::atomic<std::size_t> ring_used_{0};

    std::atomic<Lsn> append_lsn_{0};
    std::atomic<Lsn> flushed_lsn_{0};
    std::atomic<Lsn> durable_lsn_{0};
    std::atomic<bool> sealed_{false};

    // `latch_` points at `latch_storage_` when shared and is null when not
    // (spin_latch.hpp: a null guard costs one branch and no atomic).
    SpinLatch latch_storage_;
    SpinLatch* latch_ = nullptr;

    // Preallocated so writing a segment header never allocates.
    std::vector<std::byte> header_block_;
};

}  // namespace kds::wal
