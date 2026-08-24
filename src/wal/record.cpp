#include "kds/wal/record.hpp"

#include <cstring>

#include "kds/storage/crc32c.hpp"

// rules.md #2: every read/write of on-disk bytes below goes field-by-field
// through a named offset. The mirror structs exist to pin those offsets
// with static_asserts, never to be memcpy'd whole.

namespace kds::wal {
namespace {

template <typename T>
T Load(std::span<const std::byte> bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void Store(std::span<std::byte> bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

std::size_t AlignUp(std::size_t n) noexcept {
    return (n + kRecordAlignment - 1) & ~(kRecordAlignment - 1);
}

}  // namespace

bool IsAssignedRecordType(std::uint8_t raw) noexcept {
    return raw != static_cast<std::uint8_t>(RecordType::kInvalid) && raw <= kMaxAssignedRecordType;
}

const char* RecordTypeName(RecordType type) noexcept {
    switch (type) {
        case RecordType::kInvalid: return "INVALID";
        case RecordType::kTxnBegin: return "TXN_BEGIN";
        case RecordType::kTxnCommit: return "TXN_COMMIT";
        case RecordType::kTxnAbort: return "TXN_ABORT";
        case RecordType::kPageInit: return "PAGE_INIT";
        case RecordType::kHeapInsert: return "HEAP_INSERT";
        case RecordType::kHeapOverwrite: return "HEAP_OVERWRITE";
        case RecordType::kHeapDeleteMark: return "HEAP_DELETE_MARK";
        case RecordType::kHeapDeleteUnmark: return "HEAP_DELETE_UNMARK";
        case RecordType::kSlotRetire: return "SLOT_RETIRE";
        case RecordType::kAlloc: return "ALLOC";
        case RecordType::kFullPageImage: return "FULL_PAGE_IMAGE";
        case RecordType::kCheckpointBegin: return "CHECKPOINT_BEGIN";
        case RecordType::kCheckpointEnd: return "CHECKPOINT_END";
        case RecordType::kPad: return "PAD";
        case RecordType::kUndoWrite: return "UNDO_WRITE";
        case RecordType::kFree: return "FREE";
        case RecordType::kVarHeapAppend: return "VARHEAP_APPEND";
        case RecordType::kIndexInsert: return "INDEX_INSERT";
        case RecordType::kAssertReserve: return "ASSERT_RESERVE";
        case RecordType::kAssertCommit: return "ASSERT_COMMIT";
        case RecordType::kAssertRollback: return "ASSERT_ROLLBACK";
        case RecordType::kAssertBuild: return "ASSERT_BUILD";
        case RecordType::kAssertDrop: return "ASSERT_DROP";
        case RecordType::kAssertSnapshot: return "ASSERT_SNAPSHOT";
        case RecordType::kPageHandoff: return "PAGE_HANDOFF";
        case RecordType::kAnchorUpdate: return "ANCHOR_UPDATE";
    }
    return "UNKNOWN";
}

std::size_t EncodedRecordSize(std::size_t payload_size) noexcept {
    return AlignUp(kRecordHeaderSize + payload_size);
}

StatusOr<std::size_t> EncodeRecord(std::span<std::byte> out, const RecordSpec& spec, Lsn lsn,
                                   std::span<const std::byte> payload) {
    if (!IsAssignedRecordType(static_cast<std::uint8_t>(spec.type))) {
        return Status::InvalidArgument("wal record: unassigned record type");
    }
    if (spec.txn_id > kMaxTxnId) {
        return Status::InvalidArgument("wal record: txn_id exceeds 48 bits");
    }

    const std::size_t total_len = EncodedRecordSize(payload.size());
    if (total_len > 0xFFFFFFFFull) {
        return Status::InvalidArgument("wal record: payload too large for total_len");
    }
    if (out.size() < total_len) {
        return Status::InvalidArgument("wal record: output buffer smaller than the record");
    }

    Store<std::uint32_t>(out, kRecordTotalLenOffset, static_cast<std::uint32_t>(total_len));
    Store<Lsn>(out, kRecordLsnOffset, lsn);
    Store<std::uint64_t>(out, kRecordTxnIdOffset, spec.txn_id);
    Store<std::uint8_t>(out, kRecordTypeOffset, static_cast<std::uint8_t>(spec.type));
    Store<std::uint8_t>(out, kRecordFlagsOffset, spec.flags);
    Store<std::uint16_t>(out, kRecordReservedOffset, 0);
    Store<PageId>(out, kRecordPageIdOffset, spec.page_id);

    if (!payload.empty()) {
        std::memcpy(out.data() + kRecordHeaderSize, payload.data(), payload.size());
    }
    // Zero the padding: an encoded record must be a pure function of its
    // inputs so the simulator can compare streams byte for byte.
    const std::size_t padding = total_len - kRecordHeaderSize - payload.size();
    if (padding > 0) {
        std::memset(out.data() + kRecordHeaderSize + payload.size(), 0, padding);
    }

    // Last, over everything the CRC covers - which is now final.
    const std::uint32_t crc = storage::Crc32c(
        out.subspan(kRecordCrcCoverageOffset, total_len - kRecordCrcCoverageOffset));
    Store<std::uint32_t>(out, kRecordCrcOffset, crc);

    return total_len;
}

StatusOr<DecodedRecord> DecodeRecord(std::span<const std::byte> in) {
    if (in.size() < kRecordHeaderSize) {
        return Status::Corruption("wal record: fewer bytes than a header");
    }

    const auto total_len = Load<std::uint32_t>(in, kRecordTotalLenOffset);
    if (total_len < kRecordHeaderSize || (total_len % kRecordAlignment) != 0 ||
        total_len > in.size()) {
        return Status::Corruption("wal record: impossible total_len");
    }

    const std::uint32_t stored_crc = Load<std::uint32_t>(in, kRecordCrcOffset);
    const std::uint32_t computed_crc = storage::Crc32c(
        in.subspan(kRecordCrcCoverageOffset, total_len - kRecordCrcCoverageOffset));
    if (stored_crc != computed_crc) {
        return Status::Corruption("wal record: crc mismatch");
    }

    DecodedRecord record{};
    record.header.total_len = total_len;
    record.header.crc32c = stored_crc;
    record.header.lsn = Load<Lsn>(in, kRecordLsnOffset);
    record.header.txn_id = Load<std::uint64_t>(in, kRecordTxnIdOffset);
    record.header.type = Load<std::uint8_t>(in, kRecordTypeOffset);
    record.header.flags = Load<std::uint8_t>(in, kRecordFlagsOffset);
    record.header.reserved = Load<std::uint16_t>(in, kRecordReservedOffset);
    record.header.page_id = Load<PageId>(in, kRecordPageIdOffset);

    // The payload keeps its padding out: a reader that wants the bytes back
    // must get exactly what was handed to EncodeRecord, which means the
    // payload length has to be recoverable. It is not stored separately -
    // per-type payloads are self-describing or fixed-size, and PAD has no
    // payload at all - so this reports the whole aligned tail and the type
    // codec above it takes what it needs.
    record.payload = in.subspan(kRecordHeaderSize, total_len - kRecordHeaderSize);
    return record;
}

std::optional<DecodedRecord> RecordReader::Next() {
    if (cursor_ >= buffer_.size()) {
        return std::nullopt;
    }

    // ---- Slack is an end; damage is a torn tail -------------------------
    //
    // A segment's never-written bytes read as zeroes, and a zeroed header
    // decodes as `total_len == 0` - which is "impossible total_len", the
    // same failure a torn record gives. Telling them apart matters because
    // `stopped_early` is what a caller reads to decide whether the stream
    // ended where the writer left it or was cut short: every clean scan ends
    // on slack, so treating slack as damage marks *every* stream torn.
    //
    // Zeroes mean nothing was durably written here, so there is nothing that
    // could have been torn. Only a header with content that fails to decode
    // is a real torn tail - which is still reported, because it means bytes
    // reached the device and did not survive.
    const std::size_t remaining = buffer_.size() - cursor_;
    const std::size_t header_span = remaining < kRecordHeaderSize ? remaining : kRecordHeaderSize;
    bool all_zero = true;
    for (std::size_t i = 0; i < header_span; ++i) {
        if (buffer_[cursor_ + i] != std::byte{0}) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        return std::nullopt;  // the writer stopped here
    }

    auto decoded = DecodeRecord(buffer_.subspan(cursor_));
    if (!decoded.ok()) {
        // A bad length or CRC is the durable end of the stream, not an
        // error (wal.md section 4.2) - a torn tail looks exactly like this.
        stopped_early_ = true;
        return std::nullopt;
    }

    // A record stamped with an LSN other than where it physically sits
    // means the stream was assembled wrong; treat it as the end rather
    // than trusting a record that disagrees with its own address.
    if (decoded.value().header.lsn != base_lsn_ + cursor_) {
        stopped_early_ = true;
        return std::nullopt;
    }

    cursor_ += decoded.value().header.total_len;
    return decoded.value();
}

Status EncodeSegmentHeader(std::span<std::byte> out, const SegmentHeaderFields& fields) {
    if (out.size() < kSegmentHeaderSize) {
        return Status::InvalidArgument("wal segment: output buffer smaller than the header block");
    }

    std::memset(out.data(), 0, kSegmentHeaderSize);
    Store<std::uint64_t>(out, kSegmentMagicOffset, kSegmentMagic);
    Store<std::uint32_t>(out, kSegmentFormatVersionOffset, fields.format_version);
    Store<std::uint32_t>(out, kSegmentCoreIdOffset, fields.core_id);
    Store<std::uint64_t>(out, kSegmentNoOffset, fields.segment_no);
    Store<Lsn>(out, kSegmentStartLsnOffset, fields.start_lsn);

    const std::uint32_t crc = storage::Crc32c(out.subspan(0, kSegmentCrcOffset));
    Store<std::uint32_t>(out, kSegmentCrcOffset, crc);
    return Status::OK();
}

StatusOr<SegmentHeaderFields> DecodeSegmentHeader(std::span<const std::byte> in) {
    if (in.size() < kSegmentHeaderSize) {
        return Status::Corruption("wal segment: fewer bytes than a header block");
    }

    SegmentHeaderFields f{};
    f.magic = Load<std::uint64_t>(in, kSegmentMagicOffset);
    if (f.magic != kSegmentMagic) {
        return Status::Corruption("wal segment: magic mismatch");
    }

    const std::uint32_t stored_crc = Load<std::uint32_t>(in, kSegmentCrcOffset);
    if (stored_crc != storage::Crc32c(in.subspan(0, kSegmentCrcOffset))) {
        return Status::Corruption("wal segment: header crc mismatch");
    }

    f.format_version = Load<std::uint32_t>(in, kSegmentFormatVersionOffset);
    if (f.format_version == 0 || f.format_version > kSegmentFormatVersion) {
        return Status::Corruption("wal segment: format_version " +
                                  std::to_string(f.format_version) +
                                  " is not supported by this build");
    }
    if (f.format_version < kMinReadableSegmentFormatVersion) {
        // Refused rather than decoded: the record payloads moved (record.hpp on
        // `kMinReadableSegmentFormatVersion`), so reading one of these would
        // misparse rather than fail. Says which version and which this build
        // needs, because the operator's next step is to discard the stream and
        // there is no migration to point at.
        return Status::Corruption(
            "wal segment: format_version " + std::to_string(f.format_version) +
            " predates this build's record layout and cannot be read; the oldest readable is " +
            std::to_string(kMinReadableSegmentFormatVersion));
    }

    f.core_id = Load<std::uint32_t>(in, kSegmentCoreIdOffset);
    f.segment_no = Load<std::uint64_t>(in, kSegmentNoOffset);
    f.start_lsn = Load<Lsn>(in, kSegmentStartLsnOffset);
    f.crc32c = stored_crc;
    return f;
}

}  // namespace kds::wal
