#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/wal/log_device.hpp"
#include "kds/wal/record.hpp"

// Reading a stream back (docs/workplan-wal-recovery.md RC01) - the piece
// recovery's analysis and redo phases both walk, and the first code in the
// engine that consumes the log rather than producing it.
//
// `RecordReader` (record.hpp) already walks the records in **one buffer**
// and already stops at the durable end the way wal.md section 4.2 defines
// it. What it cannot do is span segments: it does not know that an LSN maps
// onto a (segment, offset) pair, that offset 0 of every segment is a 4 KiB
// header rather than a record, or that a PAD marker means "this segment
// ends here, continue in the next one". That is this file, and it is all it
// is - the phases above it are RC02 onward.
//
// ---- What a scan stops at, and why none of it is an error ---------------
//
// A scan ends at the **durable end of the stream**, which is whichever of
// these comes first:
//
//   - a record whose length is impossible or whose CRC fails. That is a
//     torn tail: the writer was interrupted mid-record, so the bytes after
//     it were never acknowledged to anyone (section 4.2). `stopped_early()`
//     reports it because it is worth metering, not because it is a fault.
//   - the last created segment running out of records. A clean stop.
//
// A PAD marker is neither: it seals a segment, so the walk continues at the
// next one if that segment exists, and stops if it does not. Reading past a
// PAD within its own segment is forbidden - whatever those bytes decode to
// is not part of the stream, and this is the one place a scan must ignore
// something that decodes cleanly.
//
// **A bad segment *header*, by contrast, is Corruption and fails the scan.**
// The distinction is the whole reliability argument: a torn record tail is
// the expected shape of a crash, while a segment stamped for another core
// or another position means the file set is not the stream it claims to be,
// and continuing would interleave two logs. Recovery must refuse a mount it
// cannot explain rather than replay a plausible prefix of it (wal.md
// section 12, and txn.md section 8's standing instruction against partial
// recovery).
//
// ---- Concurrency and cost ------------------------------------------------
//
// Core-local like everything under it: one device, one stream, one core.
// A segment's body is read whole into a caller-owned buffer, which is what
// WalStream::ScanTail already does and is fine at the sizes recovery sees;
// at a 64 MiB segment size this becomes a streaming read and nothing about
// the walk below changes.

namespace kds::wal {

// Validates that `header_block` is segment `segment_no` of core `core_id`'s
// stream, and returns its decoded header.
//
// Shared by this scanner and WalStream::ScanTail deliberately: "is this
// segment mine, and does it start where I think it does" is one question,
// and two implementations of it are two answers that can drift - the rule
// exec/tuple_verify.hpp states for its own verifier. The messages are
// WalStream's originals, since it asked first.
StatusOr<SegmentHeaderFields> ValidateSegmentHeader(std::span<const std::byte> header_block,
                                                    std::uint32_t core_id,
                                                    std::uint64_t segment_no,
                                                    std::uint64_t segment_size);

// What a scan reports about where it ended.
struct ScanOutcome {
    // Stream offset just past the last record the scan accepted - the
    // durable end. An empty stream reports its first legal record position.
    Lsn end_lsn = 0;

    // The last segment the scan touched, and whether it was sealed by a PAD
    // marker. WalStream resumes appending from exactly this pair.
    std::uint64_t last_segment = 0;
    bool sealed = false;

    // A record failed to decode before the bytes ran out: the torn tail of
    // section 4.2. Metered rather than raised - see the header comment.
    bool stopped_early = false;

    // How many records the visitor was handed. Recovery reports it; the
    // tests assert on it, which is what keeps "read back exactly what was
    // appended" a statement about records and not about bytes.
    std::uint64_t records = 0;
};

// Called once per record, in stream order. Returning a non-ok Status stops
// the scan and fails it with that Status - the phases above use it to
// surface an unknown record type, which wal.md section 5.2 makes a hard
// recovery error rather than something to skip.
//
// The record's payload views the scanner's segment buffer and is valid for
// the duration of the call only. A visitor that needs the bytes afterwards
// copies them, exactly as parser-v2.md I15's R1 requires of a page span:
// the next record may come from a different segment, which replaces the
// buffer under it.
using RecordVisitor = std::function<Status(const DecodedRecord&)>;

// Walks `device`'s segments from `from_lsn` to the durable end, calling
// `visit` for every record that is part of the stream.
//
// `from_lsn` is a *position*, normally a checkpoint's redo start read from
// the superblock anchor. It must name a legal record position - inside an
// existing segment and at or after its header block - or the scan fails
// with InvalidArgument rather than guessing at a nearby one. Zero is a
// legal spelling of "from the beginning": no record ever has LSN 0, so it
// cannot be confused with a real position (record.hpp), and it resolves to
// segment 0's first record.
//
// PAD records are consumed by the scan and never reach `visit`: they are
// framing, not content.
StatusOr<ScanOutcome> ScanLog(LogDevice& device, std::uint32_t core_id, Lsn from_lsn,
                              const RecordVisitor& visit);

// The same walk with no visitor, for the caller that wants only the end:
// WalStream::Open's resume, and recovery's "is there anything here at all"
// probe. Equivalent to ScanLog with a visitor that accepts everything, and
// implemented as exactly that so the two cannot disagree.
StatusOr<ScanOutcome> ScanLogToEnd(LogDevice& device, std::uint32_t core_id, Lsn from_lsn);

}  // namespace kds::wal
