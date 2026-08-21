#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "kds/wal/log_device.hpp"

// The simulated LogDevice: the same contract as FileLogDevice, backed by
// heap buffers, with the fault injection a crash-consistency test needs
// (rules.md section 4, wal.md section 16). The split between durable and
// pending bytes is the whole point - Crash() drops exactly what a power
// loss would, which is every write since the last Sync().
//
// The trace makes ordering assertions possible: wal.md section 8 says no
// data page may be written before its log is durable, and proving that
// needs a record of what the device was asked to do and when.

namespace kds::wal {

class MemoryLogDevice final : public LogDevice {
public:
    enum class OpKind { kCreate, kWrite, kRead, kSync };

    struct TraceEntry {
        OpKind kind;
        std::uint64_t segment_no;
        std::uint64_t offset;
        std::size_t length;
    };

    struct Stats {
        std::uint64_t writes = 0;
        std::uint64_t reads = 0;
        std::uint64_t syncs = 0;
        std::uint64_t segments_created = 0;
        std::uint64_t bytes_written = 0;
        // One-shot injections that actually fired - MemoryPageDevice's
        // counter, for its reason: a driver that arms a fault cannot
        // otherwise tell whether it was consumed (SIM05).
        std::uint64_t injections_fired = 0;
    };

    // `segment_size` must be non-zero; the factory exists because that
    // check makes construction fallible (rules.md section 1).
    static StatusOr<std::unique_ptr<MemoryLogDevice>> Create(
        std::uint64_t segment_size = kDefaultSegmentSize);

    std::uint64_t segment_size() const noexcept override { return segment_size_; }
    std::uint64_t segment_count() const noexcept override { return base_.size(); }

    // Segments durable as of the last Sync() - what Crash() reverts to.
    std::uint64_t durable_segment_count() const noexcept { return durable_segment_count_; }

    Status CreateSegment(std::uint64_t segment_no) override;
    Status WriteAt(std::uint64_t segment_no, std::uint64_t offset,
                   std::span<const std::byte> in) override;
    Status ReadAt(std::uint64_t segment_no, std::uint64_t offset,
                  std::span<std::byte> out) override;
    Status Sync() override;

    // ---- Fault injection -------------------------------------------------

    // The next matching operation reports `status` and has no effect. Each
    // is one-shot; setting one twice replaces the pending injection.
    void FailNextWrite(Status status);
    void FailNextSync(Status status);

    // The next write transfers only its first `prefix_bytes` bytes and then
    // stops, as an interrupted physical write would, and still reports OK -
    // a torn write is not an error the device knows about, which is why
    // records carry CRCs. 0 means the write is lost entirely.
    void TearNextWrite(std::size_t prefix_bytes);

    // Disarms every pending injection; MemoryPageDevice::ClearInjections'
    // contract verbatim.
    void ClearInjections() noexcept;

    // Discards every write and every segment creation since the last
    // Sync(), modelling power loss.
    void Crash();

    // ---- Instrumentation -------------------------------------------------

    const Stats& stats() const noexcept { return stats_; }
    const std::vector<TraceEntry>& trace() const noexcept { return trace_; }
    void ClearTrace() noexcept { trace_.clear(); }

private:
    explicit MemoryLogDevice(std::uint64_t segment_size) noexcept : segment_size_(segment_size) {}

    // Segments are sparse maps rather than flat buffers so a 64 MiB
    // default costs nothing until something is actually written.
    using Segment = std::unordered_map<std::uint64_t, std::byte>;

    // Durable base plus un-synced overlay — MemoryPageDevice's shape, for
    // MemoryPageDevice's reason: Sync() merges the overlay into the base,
    // so its cost is proportional to the bytes written *since the last
    // sync*, never to the log's whole history. (It used to deep-copy every
    // segment per sync; under group durability that is one full-history
    // copy per committed statement, which made any long-running driver
    // quadratic.) Crash() drops the overlay and the segments created since
    // the last sync — the same observable semantics as before.
    std::uint64_t segment_size_;
    std::vector<Segment> base_;     // content as of the last Sync()
    std::vector<Segment> pending_;  // writes since; parallel to base_
    std::uint64_t durable_segment_count_ = 0;

    std::optional<Status> fail_next_write_;
    std::optional<Status> fail_next_sync_;
    std::optional<std::size_t> tear_next_write_;

    Stats stats_;
    std::vector<TraceEntry> trace_;
};

}  // namespace kds::wal
