#pragma once

#include <cstdint>
#include <span>

#include "kds/base/status.hpp"

// The log's block-device seam, and the append-only counterpart to
// storage::PageDevice. WAL segments are streams, not randomly-addressed
// pages, so they deliberately do not go through PageStore (wal.md section
// 2): this is the seam WAL owns instead.
//
// Addressing is (segment_no, offset) rather than a flat stream offset,
// because segments are the unit of creation, sealing, archiving, and
// recycling (wal.md sections 4.1 and 11). Converting an LSN to that pair
// is WalStream's arithmetic, not the device's.
//
// Everything crosses this boundary so the whole log is testable under
// deterministic simulation with fault injection (rules.md section 4) -
// MemoryLogDevice is that implementation, and its Crash() is what makes a
// crash-consistency test a unit test.
//
// Durability: a write is not durable until Sync() returns OK. Reads see
// writes immediately whether or not they are durable, exactly like the
// page device - a crash is what separates the two.
//
// Concurrency: one device, one stream. Under an unshared stream every call
// is one core's (rules.md section 3). Under the **shared** stream of AR0 M0
// (wal/stream.hpp) the stream serializes `CreateSegment`/`WriteAt`/`ReadAt`
// with its latch, and a `Sync` may run on another thread **concurrently
// with them** - the writer thread's, or an inline syncer's. An
// implementation must accept that pairing: `FileLogDevice` does by POSIX
// semantics (`pwrite` beside `fdatasync` on one descriptor), and
// `MemoryLogDevice` takes a mutex for it, being a test double.
//
// **Recycling** (BC, `instructions/v3.0.0/workorder-bc-wal-recycling.md`)
// adds two calls, split so the table change and the I/O land on different
// threads (BC-R4, BC-R5):
//
//   - `DetachBelow` changes the table and does no I/O. Its caller
//     serializes it with `CreateSegment`/`WriteAt`/`ReadAt` exactly as those
//     are serialized with each other - the stream latch, or the one thread
//     of an unshared stream - and it may run beside a `Sync`, which keeps
//     every segment it started with until it returns.
//   - `ReclaimDetached` does the I/O - the removal and the directory sync
//     that makes it durable - and may run on any thread, beside any call.
//
// A detached segment is gone from the device's live run `[first_segment(),
// end_segment())` at once, and gone from the medium only once a
// `ReclaimDetached` has returned OK. A crash between the two may leave any
// of them behind; the next open skips them (`FileLogDevice::Open`).

namespace kds::wal {

// Proposed default segment size, wal.md section 4.1 - marked [OPEN: size]
// there, so this is a default implementations take as a parameter, never a
// constant anything may depend on.
inline constexpr std::uint64_t kDefaultSegmentSize = 64ull * 1024 * 1024;

class LogDevice {
public:
    virtual ~LogDevice() = default;

    // Fixed for the life of the device; every segment is exactly this big.
    virtual std::uint64_t segment_size() const noexcept = 0;

    // The live run: segments first_segment()..end_segment()-1 exist, and
    // recovery walks inside it. Equal on an empty device. The first is 0
    // until a segment is detached or a device is opened past recycled ones.
    virtual std::uint64_t first_segment() const noexcept = 0;
    virtual std::uint64_t end_segment() const noexcept = 0;

    // Creates the next segment (segment_no must equal end_segment()),
    // sized at segment_size() and readable as zeroes but for `header`, which
    // is written at offset 0 **as part of the creation**: on success the
    // segment's name and its header are both durable. A failure, or a crash
    // during the call, leaves no segment of that number in the live run.
    // Fails with InvalidArgument for any other number - segments are created
    // in order, never sparsely. `header` is never larger than a segment; the
    // stream's is one block.
    virtual Status CreateSegment(std::uint64_t segment_no,
                                 std::span<const std::byte> header = {}) = 0;

    // Writes into a segment of the live run. Fails with OutOfRange if the
    // segment is not in it or the write would run past its end, IoError on
    // a device failure. Not durable until Sync().
    virtual Status WriteAt(std::uint64_t segment_no, std::uint64_t offset,
                           std::span<const std::byte> in) = 0;

    // Reads back. Bytes never written read as zeroes. Same failure modes.
    virtual Status ReadAt(std::uint64_t segment_no, std::uint64_t offset,
                          std::span<std::byte> out) = 0;

    // Makes every prior write - and the existence of every segment created
    // so far - durable.
    virtual Status Sync() = 0;

    // Removes segments first_segment()..segment_no-1 from the live run, with
    // no I/O (the concurrency section). `segment_no` may equal
    // first_segment(), which detaches nothing, and may not reach
    // end_segment(): the last segment holds the stream's append point and is
    // never detached. InvalidArgument otherwise.
    virtual Status DetachBelow(std::uint64_t segment_no) = 0;

    // Removes every detached segment from the medium and makes the removal
    // durable. A segment that could not be removed stays queued for the next
    // call, and the call reports the first failure. Never touches the live run.
    virtual Status ReclaimDetached() = 0;

    // Segments `ReclaimDetached` has durably removed, over the device's life
    // (`SHOW META`'s `wal_segments_removed`, BC-R6). Any thread.
    virtual std::uint64_t segments_removed() const noexcept = 0;
};

// ---- Shared argument validation -----------------------------------------

// Rejects a range outside a segment of the live run `[first, end)`. Shared
// so both implementations answer identically for identical bad arguments.
Status CheckSegmentRange(std::uint64_t segment_no, std::uint64_t offset, std::size_t length,
                         std::uint64_t first, std::uint64_t end, std::uint64_t segment_size);

// `DetachBelow`'s argument check, shared for the same reason.
Status CheckDetachBound(std::uint64_t segment_no, std::uint64_t first, std::uint64_t end);

}  // namespace kds::wal
