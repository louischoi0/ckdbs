#include "kds/stats/lock_wait_log.hpp"

#include <mutex>
#include <new>

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

std::array<LockWaitLog::Part, LockWaitLog::kParts>& LockWaitLog::parts() noexcept {
    static std::array<Part, kParts> p;
    return p;
}

void LockWaitLog::Record(LockWaitRecord row) noexcept {
    Part& part = parts()[row.core % kParts];
    const std::lock_guard<Latch> held(part.latch);
    row.seq = ++part.recorded;
    if (part.ring.size() < kRingCapacity) {
        // The one allocation, on this part's first row; a failure drops the
        // row rather than the wait (the header's rule).
        try {
            if (part.ring.capacity() == 0) part.ring.reserve(kRingCapacity);
            part.ring.push_back(row);
        } catch (const std::bad_alloc&) {
            --part.recorded;
            ++part.dropped;
        }
        return;
    }
    part.ring[part.next] = row;
    part.next = (part.next + 1) % kRingCapacity;
    ++part.overwritten;
}

std::vector<LockWaitRecord> LockWaitLog::Snapshot() {
    std::vector<LockWaitRecord> out;
    for (Part& part : parts()) {
        const std::lock_guard<Latch> held(part.latch);
        // Oldest first: once the ring has wrapped, `next` is the oldest slot.
        const std::size_t n = part.ring.size();
        const std::size_t first = n == kRingCapacity ? part.next : 0;
        for (std::size_t i = 0; i < n; ++i) out.push_back(part.ring[(first + i) % n]);
    }
    return out;
}

LockWaitLog::Totals LockWaitLog::Read() noexcept {
    Totals t;
    for (Part& part : parts()) {
        const std::lock_guard<Latch> held(part.latch);
        t.recorded += part.recorded;
        t.overwritten += part.overwritten;
        t.dropped += part.dropped;
    }
    return t;
}

void LockWaitLog::ResetForTest() noexcept {
    for (Part& part : parts()) {
        const std::lock_guard<Latch> held(part.latch);
        part.ring.clear();
        part.next = 0;
        part.recorded = 0;
        part.overwritten = 0;
        part.dropped = 0;
    }
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
