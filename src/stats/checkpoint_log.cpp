#include "kds/stats/checkpoint_log.hpp"

namespace kds::stats {

CheckpointLog::Ring& CheckpointLog::ring() noexcept {
    static Ring r;
    return r;
}

void CheckpointLog::Record(std::uint32_t core, const CheckpointRunReport& report,
                           std::int64_t ended_wall_us) noexcept {
    const auto us = [](std::uint64_t ns) { return ns / 1000; };
    const auto phase = [&report](CheckpointPhase p) {
        return report.sum_ns[static_cast<std::size_t>(p)];
    };
    CheckpointRecord row;
    row.duration_us = us(report.wall_ns());
    row.started_us = ended_wall_us - static_cast<std::int64_t>(row.duration_us);
    row.steps = report.steps;
    row.pages_flushed = report.pages;
    row.dirty_at_start = report.dirty_at_start;
    row.log_wait_us = us(report.log_park_ns + phase(CheckpointPhase::kLogGateBatch) +
                         phase(CheckpointPhase::kLogGateRun) + phase(CheckpointPhase::kEndWait));
    row.data_wait_us = us(report.data_park_ns + phase(CheckpointPhase::kDataSync) +
                          phase(CheckpointPhase::kPublishSync));
    row.core = core;
    row.resumed = report.resumed;
    row.completed = report.completed;
    ring().Record(core, row);
}

}  // namespace kds::stats
