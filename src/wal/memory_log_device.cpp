#include "kds/wal/memory_log_device.hpp"

#include <algorithm>
#include <vector>
#include <string>

namespace kds::wal {

StatusOr<std::unique_ptr<MemoryLogDevice>> MemoryLogDevice::Create(std::uint64_t segment_size) {
    if (segment_size == 0) {
        return Status::InvalidArgument("MemoryLogDevice: segment_size must be non-zero");
    }
    return std::unique_ptr<MemoryLogDevice>(new MemoryLogDevice(segment_size));
}

Status MemoryLogDevice::CreateSegment(std::uint64_t segment_no) {
    if (segment_no != base_.size()) {
        return Status::InvalidArgument("MemoryLogDevice: segments are created in order (expected " +
                                       std::to_string(base_.size()) + ", got " +
                                       std::to_string(segment_no) + ")");
    }
    base_.emplace_back();
    pending_.emplace_back();
    ++stats_.segments_created;
    trace_.push_back({OpKind::kCreate, segment_no, 0, 0});
    return Status::OK();
}

Status MemoryLogDevice::WriteAt(std::uint64_t segment_no, std::uint64_t offset,
                                std::span<const std::byte> in) {
    if (Status s = CheckSegmentRange(segment_no, offset, in.size(), base_.size(), segment_size_);
        !s.ok()) {
        return s;
    }

    trace_.push_back({OpKind::kWrite, segment_no, offset, in.size()});

    if (fail_next_write_.has_value()) {
        Status failure = *fail_next_write_;
        fail_next_write_.reset();
        ++stats_.injections_fired;
        return failure;
    }

    std::size_t transferred = in.size();
    if (tear_next_write_.has_value()) {
        transferred = std::min(*tear_next_write_, in.size());
        tear_next_write_.reset();
        ++stats_.injections_fired;
    }

    for (std::size_t i = 0; i < transferred; ++i) {
        pending_[segment_no][offset + i] = in[i];
    }

    ++stats_.writes;
    stats_.bytes_written += transferred;
    return Status::OK();
}

Status MemoryLogDevice::ReadAt(std::uint64_t segment_no, std::uint64_t offset,
                               std::span<std::byte> out) {
    if (Status s =
            CheckSegmentRange(segment_no, offset, out.size(), base_.size(), segment_size_);
        !s.ok()) {
        return s;
    }

    const Segment& pending = pending_[segment_no];
    const Segment& base = base_[segment_no];
    for (std::size_t i = 0; i < out.size(); ++i) {
        // Un-synced overlay first, then the durable base. Never-written
        // bytes read as zeroes, matching the sparse file FileLogDevice
        // gets from the filesystem.
        if (auto it = pending.find(offset + i); it != pending.end()) {
            out[i] = it->second;
            continue;
        }
        auto it = base.find(offset + i);
        out[i] = it == base.end() ? std::byte{0} : it->second;
    }

    ++stats_.reads;
    trace_.push_back({OpKind::kRead, segment_no, offset, out.size()});
    return Status::OK();
}

Status MemoryLogDevice::Sync() {
    trace_.push_back({OpKind::kSync, 0, 0, 0});

    if (fail_next_sync_.has_value()) {
        Status failure = *fail_next_sync_;
        fail_next_sync_.reset();
        ++stats_.injections_fired;
        return failure;
    }

    // Merge the overlay down: cost proportional to bytes written since the
    // last sync, never to the log's history.
    for (std::size_t seg = 0; seg < pending_.size(); ++seg) {
        for (const auto& [offset, value] : pending_[seg]) {
            base_[seg][offset] = value;
        }
        pending_[seg].clear();
    }
    durable_segment_count_ = base_.size();
    ++stats_.syncs;
    return Status::OK();
}

void MemoryLogDevice::FailNextWrite(Status status) { fail_next_write_ = std::move(status); }
void MemoryLogDevice::FailNextSync(Status status) { fail_next_sync_ = std::move(status); }
void MemoryLogDevice::TearNextWrite(std::size_t prefix_bytes) { tear_next_write_ = prefix_bytes; }

void MemoryLogDevice::ClearInjections() noexcept {
    fail_next_write_.reset();
    fail_next_sync_.reset();
    tear_next_write_.reset();
}

void MemoryLogDevice::Crash() {
    // Everything un-synced dies: the overlay whole, and the segments
    // created since the last sync with it.
    base_.resize(durable_segment_count_);
    pending_.clear();
    pending_.resize(durable_segment_count_);
}

std::uint64_t MemoryLogDevice::UnsyncedBytes() const noexcept {
    std::uint64_t total = 0;
    for (const Segment& segment : pending_) total += segment.size();
    return total;
}

void MemoryLogDevice::Crash(std::uint64_t keep_bytes) {
    // ---- H2: the prefix crash --------------------------------------------
    //
    // `Crash()` above drops the whole overlay, which is what a power loss
    // does to writes the device never started. It is **not** what the loss
    // does to the write it interrupted: that one had begun, so a prefix of
    // it reached the platter and the rest did not. `sim/faults.hpp` records
    // the absence of this primitive as the reason torn injection waits, and
    // `recovery_undo.cpp`'s H1 case is a state only this can produce -
    // a log whose readable prefix ends *between* two records of one
    // statement, which `Crash()` cannot express because it cuts only where
    // a `Sync()` already was.
    //
    // **The order is (segment, offset) ascending, which is append order.**
    // A log is written sequentially, so the unsynced bytes form one run and
    // "the first `keep_bytes` of it" is exactly the image an interrupted
    // flush leaves. Sorting per segment rather than assuming a dense range
    // because the overlay is a sparse map: a segment written at 0 and at
    // 4096 with a hole between has two entries, not 4097, and the hole is
    // not bytes that survived.
    //
    // What is deliberately **not** modelled is a hole in the middle - later
    // records durable while an earlier one is not. `faults.hpp` argues that
    // case is unrealistic for a power cut and that nothing in `wal.md` is
    // written against it, and this primitive keeps that scope: the cut is a
    // suffix truncation, never a scatter.
    std::uint64_t kept = 0;
    for (std::size_t seg = 0; seg < pending_.size(); ++seg) {
        Segment& segment = pending_[seg];
        if (kept >= keep_bytes) {
            segment.clear();
            continue;
        }
        if (segment.size() <= keep_bytes - kept) {
            kept += segment.size();
            continue;  // this whole segment's unsynced bytes survive
        }
        // The segment the cut falls inside: keep its lowest offsets.
        std::vector<std::uint64_t> offsets;
        offsets.reserve(segment.size());
        for (const auto& [offset, value] : segment) offsets.push_back(offset);
        std::sort(offsets.begin(), offsets.end());
        const std::size_t survivors = static_cast<std::size_t>(keep_bytes - kept);
        for (std::size_t i = survivors; i < offsets.size(); ++i) segment.erase(offsets[i]);
        kept = keep_bytes;
    }
    // A segment created since the last sync but left with no surviving
    // bytes never existed as far as the next mount is concerned - the same
    // reading `Crash()` gives, applied to the tail this cut produced.
    while (base_.size() > durable_segment_count_ && pending_.back().empty()) {
        base_.pop_back();
        pending_.pop_back();
    }
}

}  // namespace kds::wal
