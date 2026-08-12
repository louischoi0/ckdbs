#include "kds/wal/stream.hpp"

#include <memory>
#include <string>
#include <utility>

#include "kds/wal/log_scanner.hpp"

namespace kds::wal {

WalStream::WalStream(LogDevice* device, std::uint32_t core_id, std::size_t ring_capacity)
    : device_(device),
      core_id_(core_id),
      segment_size_(device->segment_size()),
      ring_(ring_capacity),
      header_block_(kSegmentHeaderSize) {}

StatusOr<std::unique_ptr<WalStream>> WalStream::Open(LogDevice* device, std::uint32_t core_id,
                                                     std::size_t ring_capacity) {
    if (device == nullptr) {
        return Status::InvalidArgument("WalStream: device must not be null");
    }
    if (ring_capacity < kMinRingCapacity) {
        return Status::InvalidArgument("WalStream: ring capacity " +
                                       std::to_string(ring_capacity) + " is below the " +
                                       std::to_string(kMinRingCapacity) + "-byte minimum");
    }

    const std::uint64_t segment_size = device->segment_size();
    // A segment has to hold its header plus at least one record, and every
    // record is 8-byte aligned, so the segment boundary must be too - an
    // unaligned size would put a record at an unaligned LSN.
    if (segment_size <= kSegmentHeaderSize + kRecordHeaderSize) {
        return Status::InvalidArgument("WalStream: segment size " + std::to_string(segment_size) +
                                       " leaves no room for records after the header block");
    }
    if ((segment_size % kRecordAlignment) != 0) {
        return Status::InvalidArgument("WalStream: segment size " + std::to_string(segment_size) +
                                       " is not a multiple of the record alignment");
    }

    auto stream = std::unique_ptr<WalStream>(new WalStream(device, core_id, ring_capacity));

    if (device->segment_count() == 0) {
        if (Status s = stream->StartSegment(0); !s.ok()) {
            return s;
        }
    } else {
        if (Status s = stream->ScanTail(device->segment_count() - 1); !s.ok()) {
            return s;
        }
    }
    // Whatever is already on the device is durable by definition; nothing is
    // staged yet.
    stream->durable_lsn_ = stream->append_lsn_;
    return stream;
}

Status WalStream::StartSegment(std::uint64_t segment_no) {
    if (Status s = device_->CreateSegment(segment_no); !s.ok()) {
        return s;
    }

    SegmentHeaderFields fields{};
    fields.magic = kSegmentMagic;
    fields.format_version = kSegmentFormatVersion;
    fields.core_id = core_id_;
    fields.segment_no = segment_no;
    fields.start_lsn = segment_no * segment_size_;
    if (Status s = EncodeSegmentHeader(header_block_, fields); !s.ok()) {
        return s;
    }
    // Straight to the device, not through the ring: the header block is not
    // a record, and the first record's LSN is defined to sit after it.
    if (Status s = device_->WriteAt(segment_no, 0, header_block_); !s.ok()) {
        return s;
    }

    append_lsn_ = fields.start_lsn + kSegmentHeaderSize;
    ring_used_ = 0;
    sealed_ = false;
    return Status::OK();
}

Status WalStream::ScanTail(std::uint64_t segment_no) {
    const Lsn start_lsn = segment_no * segment_size_;

    if (Status s = device_->ReadAt(segment_no, 0, header_block_); !s.ok()) {
        return s;
    }
    // Shared with the recovery scanner (wal/log_scanner.hpp): "is this
    // segment mine, and does it start where I think it does" is one
    // question, and the append path and the replay path must not answer it
    // two ways.
    if (auto header = ValidateSegmentHeader(header_block_, core_id_, segment_no, segment_size_);
        !header.ok()) {
        return header.status();
    }

    // The whole body at once. Fine at the sizes recovery sees today; when
    // segments are 64 MiB this becomes a streaming read, which changes
    // nothing about the walk below.
    //
    // Unzeroed, for `log_scanner.cpp`'s measured reason: a `vector<byte>(n)`
    // here value-initialises 64 MiB that the `ReadAt` on the next line
    // overwrites entirely. Every byte the walk below touches was written by that
    // call.
    const std::size_t body_bytes = static_cast<std::size_t>(segment_size_ - kSegmentHeaderSize);
    auto body_storage = std::make_unique_for_overwrite<std::byte[]>(body_bytes);
    const std::span<std::byte> body(body_storage.get(), body_bytes);
    if (Status s = device_->ReadAt(segment_no, kSegmentHeaderSize, body); !s.ok()) {
        return s;
    }

    RecordReader reader(body, start_lsn + kSegmentHeaderSize);
    Lsn end = start_lsn + kSegmentHeaderSize;
    bool sealed = false;
    while (std::optional<DecodedRecord> record = reader.Next()) {
        if (record->type() == RecordType::kPad) {
            // The seal marker: nothing after it in this segment is part of
            // the stream, whatever the bytes there decode to.
            sealed = true;
            break;
        }
        end = record->header.lsn + record->header.total_len;
    }

    append_lsn_ = sealed ? start_lsn + segment_size_ : end;
    ring_used_ = 0;
    sealed_ = sealed;
    return Status::OK();
}

Status WalStream::Roll() {
    if (Status s = Flush(); !s.ok()) {
        return s;
    }
    // Numbered from the device rather than from append_lsn_, so the two can
    // never disagree about which segment comes next.
    return StartSegment(device_->segment_count());
}

Status WalStream::Seal() {
    if (sealed_) {
        return Status::OK();
    }

    const std::uint64_t remaining = SegmentRemaining();
    // A PAD marker is header-only: it says "this segment ends here", it does
    // not physically cover the tail. Filling megabytes of slack with padding
    // bytes would put that much traffic on the log for no information, and a
    // reader stops at the marker either way (wal.md section 4.1).
    if (remaining >= kRecordHeaderSize) {
        if (ring_free() < kRecordHeaderSize) {
            if (Status s = Flush(); !s.ok()) {
                return s;
            }
        }
        const RecordSpec spec{RecordType::kPad, kNoTxnId, kInvalidPageId, 0};
        auto written = EncodeRecord(std::span(ring_).subspan(ring_used_), spec, append_lsn_, {});
        if (!written.ok()) {
            return written.status();
        }
        ring_used_ += written.value();
        append_lsn_ += written.value();
    }
    // A tail too short to hold even a record header is left as the zeroes the
    // segment was created with. **This is a seal, and a reader has to be told
    // so explicitly** - the comment here used to claim the zeroes "mean
    // exactly what the marker means" to a reader, and they did not:
    // `ScanLog` read the missing marker as a torn tail and stopped, dropping
    // every record in every later segment (fixed 2026-08-12, and
    // `log_scanner.cpp` now tells the two apart by this same
    // `kRecordHeaderSize` bound). `WalStream::ScanTail` was never wrong about
    // it, because it only ever reads the *last* segment.

    if (Status s = Flush(); !s.ok()) {
        return s;
    }
    sealed_ = true;
    return Status::OK();
}

StatusOr<Lsn> WalStream::Append(const RecordSpec& spec, std::span<const std::byte> payload) {
    const std::size_t total = EncodedRecordSize(payload.size());
    if (total > usable_segment_bytes()) {
        return Status::InvalidArgument("WalStream: record of " + std::to_string(total) +
                                       " bytes cannot fit a segment; records never span segments");
    }
    if (total > ring_.size()) {
        return Status::InvalidArgument("WalStream: record of " + std::to_string(total) +
                                       " bytes is larger than the ring");
    }

    if (sealed_ || SegmentRemaining() < total) {
        if (Status s = Seal(); !s.ok()) {
            return s;
        }
        if (Status s = Roll(); !s.ok()) {
            return s;
        }
    }

    if (ring_free() < total) {
        // wal.md section 6-4: the appending task waits for the drain. There
        // is no task to suspend yet, so the caller is told to drain.
        return Status::OutOfSpace("WalStream: ring full (" + std::to_string(ring_used_) + "/" +
                                  std::to_string(ring_.size()) + " bytes staged)");
    }

    const Lsn lsn = append_lsn_;
    auto written = EncodeRecord(std::span(ring_).subspan(ring_used_), spec, lsn, payload);
    if (!written.ok()) {
        return written.status();
    }
    ring_used_ += written.value();
    append_lsn_ += written.value();
    return lsn;
}

Status WalStream::Flush() {
    if (ring_used_ == 0) {
        return Status::OK();
    }

    // The ring never spans a segment boundary - Seal() drains before a roll
    // - so this is always one write into one segment.
    const Lsn from = flushed_lsn();
    const Status status = device_->WriteAt(SegmentOf(from), OffsetOf(from),
                                           std::span(ring_).first(ring_used_));
    if (!status.ok()) {
        // The bytes stay staged: a failed write has not moved the flush
        // point, and a retry must write the same range again.
        return status;
    }
    ring_used_ = 0;
    return Status::OK();
}

Status WalStream::Sync() {
    if (Status s = Flush(); !s.ok()) {
        return s;
    }
    if (Status s = device_->Sync(); !s.ok()) {
        // durable_lsn_ deliberately untouched: a sync that failed proves
        // nothing about what reached the platter.
        return s;
    }
    durable_lsn_ = flushed_lsn();
    return Status::OK();
}

}  // namespace kds::wal
