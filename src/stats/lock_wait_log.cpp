#include "kds/stats/lock_wait_log.hpp"

namespace kds::stats {

const char* LockWaitOutcomeName(LockWaitOutcome outcome) noexcept {
    switch (outcome) {
        case LockWaitOutcome::kGranted: return "granted";
        case LockWaitOutcome::kDeadlock: return "deadlock";
        case LockWaitOutcome::kFaultNet: return "fault_net";
        case LockWaitOutcome::kCancelled: return "cancelled";
    }
    return "?";
}

LockWaitLog::Ring& LockWaitLog::ring() noexcept {
    static Ring r;
    return r;
}

PendingLockWait::PendingLockWait(const sched::Clock* clock, LockWaitRecord row) noexcept
    : clock_(clock), start_ns_(clock != nullptr ? clock->Now() : 0), row_(row) {}

void PendingLockWait::End(LockWaitOutcome outcome) noexcept {
    if (ended_) return;
    ended_ = true;
    const sched::MonoTimeNs now = clock_ != nullptr ? clock_->Now() : start_ns_;
    row_.waited_us = now > start_ns_ ? (now - start_ns_) / 1000 : 0;
    row_.outcome = outcome;
    LockWaitLog::Record(row_);
}

}  // namespace kds::stats
