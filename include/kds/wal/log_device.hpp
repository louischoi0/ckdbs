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
// Concurrency: core-local, like everything else (rules.md section 3). One
// stream, one device, one core; nothing here is internally synchronized.

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

    // Segments 0..segment_count()-1 exist. Recovery walks this range.
    virtual std::uint64_t segment_count() const noexcept = 0;

    // Creates the next segment (segment_no must equal segment_count()),
    // sized at segment_size() and readable as zeroes. Fails with
    // InvalidArgument for any other number - segments are created in
    // order, never sparsely.
    virtual Status CreateSegment(std::uint64_t segment_no) = 0;

    // Writes into an existing segment. Fails with OutOfRange if the
    // segment does not exist or the write would run past its end, IoError
    // on a device failure. Not durable until Sync().
    virtual Status WriteAt(std::uint64_t segment_no, std::uint64_t offset,
                           std::span<const std::byte> in) = 0;

    // Reads back. Bytes never written read as zeroes. Same failure modes.
    virtual Status ReadAt(std::uint64_t segment_no, std::uint64_t offset,
                          std::span<std::byte> out) = 0;

    // Makes every prior write - and the existence of every segment created
    // so far - durable.
    virtual Status Sync() = 0;
};

// ---- Shared argument validation -----------------------------------------

// Rejects a range outside an existing segment. Shared so both
// implementations answer identically for identical bad arguments.
Status CheckSegmentRange(std::uint64_t segment_no, std::uint64_t offset, std::size_t length,
                         std::uint64_t segment_count, std::uint64_t segment_size);

}  // namespace kds::wal
