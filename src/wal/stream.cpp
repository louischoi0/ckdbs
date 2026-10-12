#include "kds/wal/stream.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "kds/base/contention.hpp"
#include "kds/base/current_core.hpp"
#include "kds/wal/log_scanner.hpp"

namespace kds::wal {
namespace {

// The steady clock's reading in nanoseconds since its epoch: the roll
// window's unit (`RollRecord`).
std::uint64_t SteadyNowNs() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// One writer per counter (`roll_stats_`): a load and a store, not a
// read-modify-write.
void Bump(std::atomic<std::uint64_t>& cell, std::uint64_t by) noexcept {
    cell.store(cell.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

}  // namespace

WalStream::WalStream(LogDevice* device, std::uint32_t core_id, std::size_t ring_capacity,
                     bool shared)
    : device_(device),
      core_id_(core_id),
      segment_size_(device->segment_size()),
      ring_capacity_(ring_capacity),
      ring_(ring_capacity),
      flush_buf_(ring_capacity),
      latch_(shared ? &latch_storage_ : nullptr),
      header_block_(kSegmentHeaderSize) {}

StatusOr<std::unique_ptr<WalStream>> WalStream::Open(LogDevice* device, std::uint32_t core_id,
                                                     std::size_t ring_capacity, bool shared) {
    if (device == nullptr) {
        return Status::InvalidArgument("WalStream: device must not be null");
    }
    if (ring_capacity < kMinRingCapacity) {
        return Status::InvalidArgument("WalStream: ring capacity " +
                                       std::to_string(ring_capacity) + " is below the " +
                                       std::to_string(kMinRingCapacity) + "-byte minimum");
    }

    const std::uint64_t segment_size = device->segment_size();
    // A segment has to hold its header plus the largest split (`stream.hpp`'s
    // floors), and every record is 8-byte aligned, so the segment boundary
    // must be too - an unaligned size would put a record at an unaligned LSN.
    if (segment_size < kMinSegmentSize) {
        return Status::InvalidArgument("WalStream: segment size " + std::to_string(segment_size) +
                                       " is below the " + std::to_string(kMinSegmentSize) +
                                       "-byte minimum");
    }
    if ((segment_size % kRecordAlignment) != 0) {
        return Status::InvalidArgument("WalStream: segment size " + std::to_string(segment_size) +
                                       " is not a multiple of the record alignment");
    }

    auto stream = std::unique_ptr<WalStream>(new WalStream(device, core_id, ring_capacity, shared));

    // Open() is single-threaded by contract: nothing else holds the stream
    // yet, so no latch is taken here.
    if (device->end_segment() == 0) {
        if (Status s = stream->StartSegment(0); !s.ok()) {
            return s;
        }
    } else {
        if (Status s = stream->ScanTail(device->end_segment() - 1); !s.ok()) {
            return s;
        }
    }
    // Whatever is already on the device is durable by definition; nothing is
    // staged yet.
    stream->durable_lsn_.store(stream->append_lsn(), std::memory_order_relaxed);
    return stream;
}

Status WalStream::FailStop(const Status& cause) {
    stopped_.store(true, std::memory_order_release);
    return Status::IoError("WalStream: the log stopped after a refused log write or sync (" +
                           cause.message() + "); restart the instance to recover");
}

WalStream::SyncTotals WalStream::ReadSyncTotals() const {
    std::lock_guard<std::mutex> guard(sync_totals_mutex_);
    return sync_totals_;
}

Status WalStream::SyncDevice(RollRecord* roll) {
    // The running totals before the wait: whatever moved them by the time
    // the gate is held finished while this roll waited (H3).
    const SyncTotals before = roll != nullptr ? ReadSyncTotals() : SyncTotals{};
    const std::uint64_t waited = sync_mutex_.LockReportingWait();
    std::lock_guard<Latch> guard(sync_mutex_, std::adopt_lock);
    // A roll that did not wait reports nothing waited out, even if a sync
    // finished between its read and its acquisition.
    if (roll != nullptr && waited > 0) {
        const SyncTotals after = ReadSyncTotals();
        roll->sync_gate_ns = waited;
        roll->waited_syncs = static_cast<std::uint32_t>(after.syncs - before.syncs);
        roll->waited_sync.tail_ns = after.tail_ns - before.tail_ns;
        roll->waited_sync.rest_ns = after.rest_ns - before.rest_ns;
        roll->waited_sync.segments = static_cast<std::uint32_t>(after.segments - before.segments);
    }
    if (stopped()) return StoppedStatus();
    DeviceSyncTiming timing;
    const auto start = std::chrono::steady_clock::now();
    const Status status = device_->SyncTimed(timing);
    {
        std::lock_guard<std::mutex> totals(sync_totals_mutex_);
        ++sync_totals_.syncs;
        sync_totals_.tail_ns += timing.tail_ns;
        sync_totals_.rest_ns += timing.rest_ns;
        sync_totals_.segments += timing.segments;
    }
    if (roll != nullptr) {
        roll->sync_ns = NsSince(start);
        roll->sync = timing;
    }
    if (!status.ok()) return FailStop(status);
    return Status::OK();
}

Status WalStream::StoppedStatus() {
    return Status::IoError(
        "WalStream: the log is stopped after a refused log write or sync; restart the instance "
        "to recover");
}

Status WalStream::StartSegment(std::uint64_t segment_no, RollRecord* roll) {
    // **A roll makes the segment it leaves durable first** - its records and
    // the seal marker the roll just flushed - through the sync gate, which
    // stops the log on a failure. A segment then never exists ahead of an
    // unsealed predecessor on the device. Without it a power loss could keep
    // the new segment and drop the old one's seal marker; the stream would
    // resume in the new segment while every scan stops at the old one's end,
    // and every record written after the restart would be invisible to
    // recovery. The fresh stream's first segment has no predecessor.
    if (device_->end_segment() != 0) {
        if (Status s = SyncDevice(roll); !s.ok()) return s;
        PublishDurable(flushed_lsn());
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
    // The header goes in with the creation, not through the ring and not
    // after it: the device makes the segment and its header durable
    // together, so a crash never leaves a segment whose header is zeroes.
    SegmentCreateTiming create;
    const auto create_start = std::chrono::steady_clock::now();
    if (Status s = device_->CreateSegmentTimed(segment_no, header_block_, create); !s.ok()) {
        return FailStop(s);
    }
    if (roll != nullptr) {
        roll->create_ns = NsSince(create_start);
        roll->create = create;
    }

    const Lsn first = fields.start_lsn + kSegmentHeaderSize;
    append_lsn_.store(first, std::memory_order_relaxed);
    flushed_lsn_.store(first, std::memory_order_relaxed);
    ring_used_.store(0, std::memory_order_relaxed);
    sealed_.store(false, std::memory_order_relaxed);
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

    const Lsn resume = sealed ? start_lsn + segment_size_ : end;
    append_lsn_.store(resume, std::memory_order_relaxed);
    flushed_lsn_.store(resume, std::memory_order_relaxed);
    ring_used_.store(0, std::memory_order_relaxed);
    sealed_.store(sealed, std::memory_order_relaxed);
    return Status::OK();
}

Status WalStream::Roll(RollRecord* roll) {
    const auto flush_start = std::chrono::steady_clock::now();
    if (Status s = FlushLocked(); !s.ok()) {
        return s;
    }
    roll->flush_ns = NsSince(flush_start);
    // Numbered from the device rather than from append_lsn_, so the two can
    // never disagree about which segment comes next.
    return StartSegment(device_->end_segment(), roll);
}

void WalStream::RecordRoll(const RollRecord& roll) noexcept {
    const auto at = [this](RollStat stat) -> std::atomic<std::uint64_t>& {
        return roll_stats_[static_cast<std::size_t>(stat)];
    };
    Bump(at(RollStat::kRolls), 1);
    Bump(at(RollStat::kHoldNs), roll.hold_ns());
    if (roll.hold_ns() > at(RollStat::kHoldMaxNs).load(std::memory_order_relaxed)) {
        at(RollStat::kHoldMaxNs).store(roll.hold_ns(), std::memory_order_relaxed);
    }
    Bump(at(RollStat::kSealNs), roll.seal_ns);
    Bump(at(RollStat::kFlushNs), roll.flush_ns);
    Bump(at(RollStat::kSyncGateNs), roll.sync_gate_ns);
    Bump(at(RollStat::kSyncNs), roll.sync_ns);
    Bump(at(RollStat::kSyncTailNs), roll.sync.tail_ns);
    Bump(at(RollStat::kSyncRestNs), roll.sync.rest_ns);
    Bump(at(RollStat::kSyncSegments), roll.sync.segments);
    if (roll.sync.segments > at(RollStat::kSyncSegmentsMax).load(std::memory_order_relaxed)) {
        at(RollStat::kSyncSegmentsMax).store(roll.sync.segments, std::memory_order_relaxed);
    }
    Bump(at(RollStat::kCreateNs), roll.create_ns);
    Bump(at(RollStat::kBodyWaitNs), roll.create.body_wait_ns);
    Bump(at(RollStat::kBodyNs), roll.create.body_ns);
    Bump(at(RollStat::kHeaderNs), roll.create.header_ns);
    Bump(at(RollStat::kRenameNs), roll.create.rename_ns);
    Bump(at(RollStat::kDirSyncNs), roll.create.dir_sync_ns);
    switch (roll.create.body) {
        case BodyOutcome::kHit: Bump(at(RollStat::kBodyHit), 1); break;
        case BodyOutcome::kWaited: Bump(at(RollStat::kBodyWaited), 1); break;
        case BodyOutcome::kInline: Bump(at(RollStat::kBodyInline), 1); break;
        case BodyOutcome::kUnreported: break;
    }
    last_roll_start_ns_ = roll.start_ns;
    last_roll_end_ns_ = roll.end_ns;
}

void WalStream::ChargeRollOverlap(std::uint64_t waited_ns) const noexcept {
    if (waited_ns == 0) return;
    // The wait ran over [now - waited, now]; the last roll over
    // [last_roll_start_ns_, last_roll_end_ns_], ended by now since this
    // thread holds the latch the roll held. Only the last roll is tested:
    // two rolls inside one wait would be under-counted.
    const std::uint64_t now = SteadyNowNs();
    const std::uint64_t wait_start = now > waited_ns ? now - waited_ns : 0;
    const std::uint64_t from = std::max(wait_start, last_roll_start_ns_);
    if (last_roll_end_ns_ <= from) return;
    Contention::Add(Tally::kWalRollBlockedWaits);
    Contention::Add(Tally::kWalRollBlockedWaitNs, last_roll_end_ns_ - from);
}

Status WalStream::Seal() {
    const StreamLatchHold hold(*this);
    if (stopped()) return StoppedStatus();
    return SealLocked();
}

Status WalStream::DetachBelow(Lsn lsn) {
    const StreamLatchHold hold(*this);
    // A flush in flight is inside the device's `WriteAt`, which reads the
    // segment table unlocked on the promise that no table change runs beside
    // it (file_log_device.hpp); the detach waits it out, as a roll does.
    WaitOutFlushInFlight();
    const std::uint64_t end = device_->end_segment();
    if (end == 0) return Status::OK();
    // Segment `s` is wholly below `lsn` exactly when `s < lsn / segment_size`;
    // the last segment holds the append point and stays whatever `lsn` says.
    const std::uint64_t bound = std::min(SegmentOf(lsn), end - 1);
    if (bound <= device_->first_segment()) return Status::OK();
    return device_->DetachBelow(bound);
}

Status WalStream::SealLocked() {
    if (sealed()) {
        return Status::OK();
    }

    const std::uint64_t remaining = SegmentRemaining();
    // A PAD marker is header-only: it says "this segment ends here", it does
    // not physically cover the tail. Filling megabytes of slack with padding
    // bytes would put that much traffic on the log for no information, and a
    // reader stops at the marker either way (wal.md section 4.1).
    if (remaining >= kRecordHeaderSize) {
        if (ring_free() < kRecordHeaderSize) {
            if (Status s = FlushLocked(); !s.ok()) {
                return s;
            }
        }
        const RecordSpec spec{RecordType::kPad, kNoTxnId, kInvalidPageId, 0};
        const std::size_t used = ring_used();
        const Lsn lsn = append_lsn();
        auto written = EncodeRecord(std::span(ring_).subspan(used), spec, lsn, {});
        if (!written.ok()) {
            return written.status();
        }
        ring_used_.store(used + written.value(), std::memory_order_relaxed);
        append_lsn_.store(lsn + written.value(), std::memory_order_relaxed);
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

    if (Status s = FlushLocked(); !s.ok()) {
        return s;
    }
    sealed_.store(true, std::memory_order_relaxed);
    return Status::OK();
}

StatusOr<Lsn> WalStream::Append(const RecordSpec& spec, std::span<const std::byte> payload,
                                RollRecord* rolled) {
    const std::size_t total = EncodedRecordSize(payload.size());
    // Neither refusal meets a BTREE_SPLIT: `Open`'s floors hold the largest.
    if (total > usable_segment_bytes()) {
        return Status::InvalidArgument("WalStream: record of " + std::to_string(total) +
                                       " bytes cannot fit a segment; records never span segments");
    }
    if (total > ring_capacity_) {
        return Status::InvalidArgument("WalStream: record of " + std::to_string(total) +
                                       " bytes is larger than the ring");
    }

    const StreamLatchHold hold(*this);
    if (stopped()) return StoppedStatus();

    if (sealed() || SegmentRemaining() < total) {
        // **The roll's hold, timed** (BQ-S1): only here, so an append that
        // does not roll reads no clock.
        RollRecord roll;
        roll.core = CurrentCore();
        roll.start_ns = SteadyNowNs();
        if (Status s = SealLocked(); !s.ok()) {
            return s;
        }
        roll.seal_ns = SteadyNowNs() - roll.start_ns;
        if (Status s = Roll(&roll); !s.ok()) {
            return s;
        }
        roll.end_ns = SteadyNowNs();
        roll.segment_no = SegmentOf(append_lsn());
        RecordRoll(roll);
        if (rolled != nullptr) *rolled = roll;
    }

    const std::size_t used = ring_used();
    if (ring_capacity_ - used < total) {
        // wal.md section 6-4: the appending task waits for the drain. There
        // is no task to suspend yet, so the caller is told to drain.
        return Status::OutOfSpace("WalStream: ring full (" + std::to_string(used) + "/" +
                                  std::to_string(ring_capacity_) + " bytes staged)");
    }

    const Lsn lsn = append_lsn();
    auto written = EncodeRecord(std::span(ring_).subspan(used), spec, lsn, payload);
    if (!written.ok()) {
        return written.status();
    }
    ring_used_.store(used + written.value(), std::memory_order_relaxed);
    append_lsn_.store(lsn + written.value(), std::memory_order_relaxed);
    // **Past half a segment, the next one's body is asked for** (BA-S12):
    // built off this path by the device, so the roll that creates it writes
    // only a header and a name. Half way rather than at the roll, so it is
    // ready before it is needed, and rather than at once, so a log that
    // never fills a segment never builds a second one.
    const std::uint64_t next = SegmentOf(lsn) + 1;
    if (prepare_asked_ != next && OffsetOf(lsn) > segment_size_ / 2) {
        device_->PrepareSegment(next);
        prepare_asked_ = next;
    }
    return lsn;
}

Status WalStream::Flush() {
    // Everything appended before this call reaches the device before it
    // returns: this thread writes what is staged when it claims the flush,
    // and a flush already in flight is waited out and the claim retried.
    for (;;) {
        std::size_t used = 0;
        Lsn from = 0;
        Lsn to = 0;
        {
            const StreamLatchHold hold(*this);
            // The claim is tested before the stop: a failed flush stops the
            // log before it releases its claim, so a thread that sees the
            // claim free sees that stop too.
            if (!flush_in_flight_.load(std::memory_order_acquire)) {
                if (stopped()) return StoppedStatus();
                used = ring_used();
                if (used == 0) return Status::OK();
                std::swap(ring_, flush_buf_);
                ring_used_.store(0, std::memory_order_relaxed);
                from = flushed_lsn();
                to = append_lsn();
                flush_in_flight_.store(true, std::memory_order_relaxed);
            }
        }
        if (used == 0) {
            // Another thread's flush is writing; it finishes without the latch.
            WaitOutFlushInFlight();
            continue;
        }
        // The ring never spans a segment boundary - the seal drains before a
        // roll, and a roll waits out this write - so this is one write into
        // one segment.
        const Status status =
            device_->WriteAt(SegmentOf(from), OffsetOf(from), std::span(flush_buf_).first(used));
        // Fail-stop (stream.hpp), as `FlushLocked` says - and **before the
        // claim is released**: released first, another thread could claim,
        // write the bytes above this failed range, sync them and publish
        // them durable before the stop landed - a hole acknowledged.
        const Status result = status.ok() ? status : FailStop(status);
        if (status.ok()) flushed_lsn_.store(to, std::memory_order_release);
        flush_in_flight_.store(false, std::memory_order_release);
        return result;
    }
}

void WalStream::WaitOutFlushInFlight() const noexcept {
    while (flush_in_flight_.load(std::memory_order_acquire)) std::this_thread::yield();
}

Status WalStream::FlushLocked() {
    // An unlatched flush in flight wrote bytes below the staged ones; they
    // reach the segment first.
    WaitOutFlushInFlight();
    if (stopped()) return StoppedStatus();
    const std::size_t used = ring_used();
    if (used == 0) {
        return Status::OK();
    }

    // The ring never spans a segment boundary - the seal drains before a
    // roll - so this is always one write into one segment.
    const Lsn from = flushed_lsn();
    const Status status =
        device_->WriteAt(SegmentOf(from), OffsetOf(from), std::span(ring_).first(used));
    if (!status.ok()) {
        // Fail-stop (stream.hpp): the staged records may describe pages
        // already mutated, and nothing may be logged after them that redo
        // could meet without them.
        return FailStop(status);
    }
    ring_used_.store(0, std::memory_order_relaxed);
    flushed_lsn_.store(append_lsn(), std::memory_order_release);
    return Status::OK();
}

Status WalStream::Sync() {
    // The flush under the latch, the sync outside it: what the device
    // confirms is everything handed over before the call, and `flushed` is
    // the watermark as it stood when this thread stopped holding the
    // latch - a later flush by another thread is not this sync's to claim.
    //
    // Through the unlatched flush (BA-S12): every byte below `flushed_lsn()`
    // was written before this read, so the device sync that follows covers
    // it whoever wrote it.
    if (Status s = Flush(); !s.ok()) return s;
    const Lsn flushed = flushed_lsn();
    if (Status s = SyncDevice(); !s.ok()) {
        // durable_lsn_ deliberately untouched, and the stream is stopped
        // (`SyncDevice`): a sync that failed proves nothing about what
        // reached the platter, and a retry proves nothing either.
        return s;
    }
    PublishDurable(flushed);
    return Status::OK();
}

void WalStream::PublishDurable(Lsn lsn) noexcept {
    if (latch_ == nullptr) {
        // One thread: a plain store, and the comparison only keeps a
        // no-op sync from moving the watermark backwards.
        if (lsn > durable_lsn_.load(std::memory_order_relaxed)) {
            durable_lsn_.store(lsn, std::memory_order_release);
        }
        return;
    }
    Lsn seen = durable_lsn_.load(std::memory_order_relaxed);
    while (lsn > seen && !durable_lsn_.compare_exchange_weak(seen, lsn, std::memory_order_release,
                                                             std::memory_order_relaxed)) {
    }
}

}  // namespace kds::wal
