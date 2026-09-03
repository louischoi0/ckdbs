#include "kds/txn/instance_visibility.hpp"

#include <algorithm>

namespace kds::txn {

void InstanceVisibility::PublishIssueCursor(std::uint32_t core, std::uint64_t cursor) noexcept {
    if (core >= slots_.size()) return;
    CoreVisibilitySlot& slot = slots_[core];
    // Never backwards. A cursor only rises in the sequence itself, so a
    // lower value here is a late attach or a stale caller - and taking it
    // would lower the floor's bound *after* the floor had already been
    // raised past it, which is the one thing the bound exists to prevent.
    std::uint64_t held = slot.issue_cursor.load(std::memory_order_relaxed);
    if (held != kUnboundedBound && cursor <= held) return;
    slot.issue_cursor.store(cursor, std::memory_order_release);
}

void InstanceVisibility::PublishOldestUnresolved(std::uint32_t core,
                                                 std::uint64_t trx_id) noexcept {
    if (core >= slots_.size()) return;
    // Moves in both directions: a transaction ending raises it, and a new
    // transaction with a lower id lowers it. It can never fall below the
    // floor, because the floor never passes this core's issue cursor and
    // every id this core issues is at or above that cursor.
    slots_[core].oldest_unresolved.store(trx_id, std::memory_order_release);
}

void InstanceVisibility::PublishSnapshotBound(std::uint32_t core, std::uint64_t lsn) noexcept {
    if (core >= slots_.size()) return;
    slots_[core].min_snapshot_lsn.store(lsn, std::memory_order_release);
}

std::uint64_t InstanceVisibility::HorizonLsn() const noexcept {
    std::uint64_t horizon = kUnboundedBound;
    for (const CoreVisibilitySlot& slot : slots_) {
        const std::uint64_t bound = slot.min_snapshot_lsn.load(std::memory_order_acquire);
        if (bound < horizon) horizon = bound;
    }
    return horizon;
}

std::uint64_t InstanceVisibility::FloorCandidate() const noexcept {
    std::uint64_t candidate = kUnboundedBound;
    for (const CoreVisibilitySlot& slot : slots_) {
        const std::uint64_t cursor = slot.issue_cursor.load(std::memory_order_acquire);
        // An unattached core publishes nothing and constrains nothing - but
        // it also proves nothing, so its *unresolved* field is skipped with
        // it rather than read as "none live".
        if (cursor == kUnboundedBound) continue;
        if (cursor < candidate) candidate = cursor;
        const std::uint64_t oldest = slot.oldest_unresolved.load(std::memory_order_acquire);
        if (oldest < candidate) candidate = oldest;
    }
    return candidate;
}

void InstanceVisibility::PublishCommit(std::uint64_t trx_id, std::uint64_t commit_lsn) {
    bool reclaim = false;
    {
        LatchGuard guard(&window_latch_);
        window_[trx_id] = commit_lsn;
        reclaim = window_.size() >= reclaim_at_;
    }
    if (reclaim) Reclaim();
}

std::uint64_t InstanceVisibility::CommitLsnOf(std::uint64_t trx_id) const {
    LatchGuard guard(&window_latch_);
    auto it = window_.find(trx_id);
    return it == window_.end() ? kNoCommitLsn : it->second;
}

std::size_t InstanceVisibility::window_size() const {
    LatchGuard guard(&window_latch_);
    return window_.size();
}

std::size_t InstanceVisibility::Reclaim() {
    const std::uint64_t candidate = FloorCandidate();
    // No core has attached: nothing is known about what may still be
    // issued, so nothing may be dropped. Raising the floor here would be
    // raising it on no evidence at all.
    if (candidate == kUnboundedBound) return 0;

    const std::uint64_t horizon = HorizonLsn();

    LatchGuard guard(&window_latch_);

    // An entry whose commit is above the horizon pins the floor at its own
    // id: a live snapshot below that commit must still be told this writer
    // had not committed, and the floor's branch would tell it otherwise.
    std::uint64_t reachable = candidate;
    for (const auto& [trx_id, commit_lsn] : window_) {
        if (trx_id < reachable && commit_lsn > horizon) reachable = trx_id;
    }

    std::size_t dropped = 0;
    for (auto it = window_.begin(); it != window_.end();) {
        if (it->first < reachable) {
            it = window_.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }

    // Monotone. `reachable` can be below the floor when a core begins a
    // transaction whose id is lower than one already resolved - which is
    // ordinary under block leases - and the floor must not follow it down.
    // It is still sound: the floor only ever reached its current value
    // because every cursor was at or above it, and cursors do not fall.
    std::uint64_t held = floor_.load(std::memory_order_relaxed);
    while (reachable > held && !floor_.compare_exchange_weak(held, reachable,
                                                             std::memory_order_release,
                                                             std::memory_order_relaxed)) {
    }

    reclaim_at_ = std::max(kReclaimFloor, window_.size() * 2);
    return dropped;
}

}  // namespace kds::txn
