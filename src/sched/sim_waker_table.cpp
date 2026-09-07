#include "kds/sched/sim_waker_table.hpp"

#include <algorithm>

namespace kds::sched {

SimWakerTable::SimWakerTable(WakerTable& real, SimWakerConfig config)
    : real_(real), config_(config) {}

std::uint64_t SimWakerTable::DelayFor(std::uint32_t dst, std::uint64_t tick) const noexcept {
    if (config_.max_delay_ticks <= config_.min_delay_ticks) return config_.min_delay_ticks;
    // SplitMix64's mixer over `(seed, dst, tick)` and nothing else - no
    // stream, no state - so the delay is a function of what the log
    // records (the header says why that is the claim and not more).
    std::uint64_t z = config_.seed + 0x9E37'79B9'7F4A'7C15ULL * (tick + 1) +
                      0xD6E8'FEB8'6659'FD93ULL * (static_cast<std::uint64_t>(dst) + 1);
    z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
    z ^= z >> 31;
    const std::uint64_t span = config_.max_delay_ticks - config_.min_delay_ticks + 1;
    return config_.min_delay_ticks + z % span;
}

void SimWakerTable::Kick(std::uint32_t core) const noexcept {
    bool forward_now = false;
    {
        LatchGuard guard(&latch_);
        const Record record{tick_, core, tick_ + DelayFor(core, tick_)};
        log_.push_back(record);
        if (record.due == tick_) {
            forward_now = true;
        } else {
            pending_.push_back(record);
        }
    }
    // Outside the latch: the real `Kick` is the wake path, and a lock on it
    // would be a second thing for a kicked reactor to wait on.
    if (forward_now) {
        real_.Kick(core);
        forwarded_.fetch_add(1, std::memory_order_release);
    }
}

void SimWakerTable::Advance(std::uint64_t ticks) {
    std::vector<Record> due;
    {
        LatchGuard guard(&latch_);
        tick_ += ticks;
        // Stable, so kicks due at the same tick keep the order they were
        // asked in - `pending_` is that order, and the partition only
        // separates the due from the held.
        auto held = std::stable_partition(pending_.begin(), pending_.end(),
                                          [&](const Record& r) { return r.due <= tick_; });
        due.assign(pending_.begin(), held);
        pending_.erase(pending_.begin(), held);
        std::stable_sort(due.begin(), due.end(),
                         [](const Record& a, const Record& b) { return a.due < b.due; });
    }
    for (const Record& r : due) {
        real_.Kick(r.dst);
        forwarded_.fetch_add(1, std::memory_order_release);
    }
}

std::uint64_t SimWakerTable::tick() const {
    LatchGuard guard(&latch_);
    return tick_;
}

std::vector<SimWakerTable::Record> SimWakerTable::log() const {
    LatchGuard guard(&latch_);
    return log_;
}

std::size_t SimWakerTable::in_flight() const {
    LatchGuard guard(&latch_);
    return pending_.size();
}

}  // namespace kds::sched
