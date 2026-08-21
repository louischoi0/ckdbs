#include "kds/wal/memory_log_device.hpp"

#include <algorithm>
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

}  // namespace kds::wal
