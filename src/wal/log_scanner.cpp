#include "kds/wal/log_scanner.hpp"

#include <memory>
#include <span>
#include <string>
#include <utility>

namespace kds::wal {

StatusOr<SegmentHeaderFields> ValidateSegmentHeader(std::span<const std::byte> header_block,
                                                    std::uint32_t core_id,
                                                    std::uint64_t segment_no,
                                                    std::uint64_t segment_size) {
    auto header = DecodeSegmentHeader(header_block);
    if (!header.ok()) {
        return header.status();
    }
    // The header says which stream and which position these bytes belong
    // to; reading past one that disagrees would interleave two streams.
    if (header.value().core_id != core_id) {
        return Status::Corruption("WalStream: segment " + std::to_string(segment_no) +
                                  " belongs to core " + std::to_string(header.value().core_id) +
                                  ", not " + std::to_string(core_id));
    }
    const Lsn start_lsn = segment_no * segment_size;
    if (header.value().segment_no != segment_no || header.value().start_lsn != start_lsn) {
        return Status::Corruption("WalStream: segment " + std::to_string(segment_no) +
                                  " is stamped as segment " +
                                  std::to_string(header.value().segment_no) + " at lsn " +
                                  std::to_string(header.value().start_lsn));
    }
    return header.value();
}

StatusOr<ScanOutcome> ScanLog(LogDevice& device, std::uint32_t core_id, Lsn from_lsn,
                              const RecordVisitor& visit) {
    const std::uint64_t segment_size = device.segment_size();
    const std::uint64_t segment_count = device.segment_count();

    if (segment_size <= kSegmentHeaderSize) {
        return Status::InvalidArgument("ScanLog: segment size " + std::to_string(segment_size) +
                                       " leaves no room for records");
    }
    if (segment_count == 0) {
        // Nothing was ever created. The first legal record position is
        // where a fresh stream would place its first record, so a caller
        // comparing end_lsn against append_lsn sees them agree.
        ScanOutcome empty;
        empty.end_lsn = kSegmentHeaderSize;
        return empty;
    }

    // Resolve the start. Zero means "from the beginning" - never a real
    // record LSN, so the two readings cannot collide (record.hpp).
    std::uint64_t segment_no = 0;
    std::uint64_t offset = kSegmentHeaderSize;
    if (from_lsn != 0) {
        segment_no = from_lsn / segment_size;
        offset = from_lsn % segment_size;
        if (segment_no >= segment_count) {
            return Status::InvalidArgument(
                "ScanLog: lsn " + std::to_string(from_lsn) + " names segment " +
                std::to_string(segment_no) + ", past the " + std::to_string(segment_count) +
                " that exist");
        }
        if (offset < kSegmentHeaderSize) {
            return Status::InvalidArgument("ScanLog: lsn " + std::to_string(from_lsn) +
                                           " points inside segment " +
                                           std::to_string(segment_no) + "'s header block");
        }
        if ((offset % kRecordAlignment) != 0) {
            return Status::InvalidArgument("ScanLog: lsn " + std::to_string(from_lsn) +
                                           " is not " + std::to_string(kRecordAlignment) +
                                           "-byte aligned, so it cannot name a record");
        }
    }

    // Only the first segment of a scan may start part-way in; every later
    // one begins at its first record. Held as their own names rather than
    // re-derived in the loop, where `from_lsn == 0` made the same test read
    // two ways.
    const std::uint64_t start_segment = segment_no;
    const std::uint64_t start_offset = offset;

    ScanOutcome out;
    out.last_segment = segment_no;
    out.end_lsn = start_segment * segment_size + start_offset;

    // One buffer, reused across segments. The visitor contract says a
    // payload does not outlive its call, which is what makes that legal.
    std::vector<std::byte> header_block(kSegmentHeaderSize);

    // **Allocated once and never zeroed**, and that is a measured decision, not
    // a micro-optimisation. `body.assign(body_bytes, std::byte{0})` per segment
    // was **78% of a scan's cost** - zeroing 64 MiB that the very next `ReadAt`
    // overwrites in full (`bench/results-wal-recovery.md`: 30.3 ms to
    // alloc+zero, 8.4 ms to read it). A mount runs two of these scans, three
    // once an assertion is declared, so it was ~60-90 ms of startup spent
    // writing bytes nobody reads.
    //
    // `make_unique_for_overwrite` is the one allocation that skips value
    // initialisation. Every byte handed to `RecordReader` below has been written
    // by `ReadAt` first: the span is exactly the range that call filled, so no
    // uninitialised byte is ever read - which is the property that makes this
    // safe rather than merely fast.
    const std::size_t body_capacity = static_cast<std::size_t>(segment_size - kSegmentHeaderSize);
    auto body_storage = std::make_unique_for_overwrite<std::byte[]>(body_capacity);

    for (; segment_no < segment_count; ++segment_no) {
        const Lsn start_lsn = segment_no * segment_size;

        if (Status s = device.ReadAt(segment_no, 0, header_block); !s.ok()) {
            return s;
        }
        auto header = ValidateSegmentHeader(header_block, core_id, segment_no, segment_size);
        if (!header.ok()) {
            return header.status();
        }

        const std::uint64_t body_offset =
            (segment_no == start_segment) ? start_offset : kSegmentHeaderSize;
        const std::uint64_t body_bytes = segment_size - body_offset;

        // The first segment of a scan may start part-way in, so the span is the
        // tail of the buffer's capacity rather than all of it.
        const std::span<std::byte> body(body_storage.get(),
                                        static_cast<std::size_t>(body_bytes));
        if (Status s = device.ReadAt(segment_no, body_offset, body); !s.ok()) {
            return s;
        }

        out.last_segment = segment_no;
        out.sealed = false;
        out.end_lsn = start_lsn + body_offset;

        RecordReader reader(body, start_lsn + body_offset);
        bool sealed = false;
        while (std::optional<DecodedRecord> record = reader.Next()) {
            if (record->type() == RecordType::kPad) {
                // The seal marker. Nothing after it in this segment is part
                // of the stream, whatever those bytes decode to - the one
                // place a scan must ignore something that decodes cleanly.
                sealed = true;
                break;
            }
            out.end_lsn = record->header.lsn + record->header.total_len;
            ++out.records;
            if (Status s = visit(*record); !s.ok()) {
                return s;
            }
        }

        out.sealed = sealed;
        if (!sealed) {
            // **A segment can be sealed without a PAD, and this used to be
            // read as the end of the stream.**
            //
            // `WalStream::Seal` writes the marker only when the tail can hold
            // a record header; a shorter tail is "left as the zeroes the
            // segment was created with" (stream.cpp), because a PAD is
            // header-only and there is nowhere to put one. That is a seal, and
            // the stream continues in the next segment.
            //
            // Read as a torn tail instead, the scan returned here and **every
            // record in every later segment was silently dropped** - which
            // recovery reported as acknowledged rows missing after a restart,
            // once a run was long enough to roll a segment. Found by SIM04's
            // crash loop at 3500 ops, 2026-08-12.
            //
            // The remainder is what tells a seal from a tear, and it is
            // measured against the same constant the writer decided with:
            // below `kRecordHeaderSize` no marker could have been written, at
            // or above it the absence of one means a record really was torn -
            // the expected shape of a crash, and the end of the stream.
            const std::uint64_t consumed = out.end_lsn - start_lsn;
            const bool sealed_by_exhaustion =
                segment_size - consumed < kRecordHeaderSize && segment_no + 1 < segment_count;
            if (!sealed_by_exhaustion) {
                out.stopped_early = reader.stopped_early();
                return out;
            }
            // Continue exactly as the PAD path does: the next iteration sets
            // `end_lsn` from that segment's first record.
            out.sealed = true;
            continue;
        }

        // Sealed. A sealed *last* segment means the roll never completed:
        // the stream ends at the segment boundary, which is exactly where
        // WalStream::ScanTail leaves append_lsn_.
        if (segment_no + 1 == segment_count) {
            out.end_lsn = start_lsn + segment_size;
            return out;
        }
    }

    return out;
}

StatusOr<ScanOutcome> ScanLogToEnd(LogDevice& device, std::uint32_t core_id, Lsn from_lsn) {
    // One implementation, per the header's promise: this is ScanLog with a
    // visitor that accepts everything, not a second walk.
    static const RecordVisitor kAcceptAll = [](const DecodedRecord&) { return Status::OK(); };
    return ScanLog(device, core_id, from_lsn, kAcceptAll);
}

}  // namespace kds::wal
