#include "kds/txn/instance_visibility.hpp"

#include <algorithm>

namespace kds::txn {

void InstanceVisibility::NoteSlot(std::uint32_t core) noexcept {
    // Widened *after* the field it announces is stored, so a walker that
    // sees the wider bound sees the field with it. Monotone; a core never
    // detaches.
    std::uint32_t in_use = slots_in_use_.load(std::memory_order_relaxed);
    while (in_use < core + 1 && !slots_in_use_.compare_exchange_weak(
                                    in_use, core + 1, std::memory_order_release,
                                    std::memory_order_relaxed)) {
    }
}

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
    NoteSlot(core);
}

void InstanceVisibility::PublishOldestUnresolved(std::uint32_t core,
                                                 std::uint64_t trx_id) noexcept {
    if (core >= slots_.size()) return;
    // Moves in both directions: a transaction ending raises it, and a new
    // transaction with a lower id lowers it. It can never fall below the
    // floor, because the floor never passes this core's issue cursor and
    // every id this core issues is at or above that cursor.
    slots_[core].oldest_unresolved.store(trx_id, std::memory_order_release);
    NoteSlot(core);
}

void InstanceVisibility::PublishSnapshotBound(std::uint32_t core, std::uint64_t lsn) noexcept {
    if (core >= slots_.size()) return;
    // `seq_cst`: the header's Concurrency note, the store-buffer pattern
    // between a mint and a pass.
    slots_[core].min_snapshot_lsn.store(lsn);
    NoteSlot(core);
}

void InstanceVisibility::LowerSnapshotBound(std::uint32_t core, std::uint64_t lsn) noexcept {
    if (core >= slots_.size()) return;
    // Only the owning core writes this slot, so a load-compare-store is
    // not a race with anyone but a reader, and a reader that sees the old
    // value sees a higher one: the mint that follows re-reads the ceiling
    // after this store, and the total order makes that read see any
    // publication a pass that missed this store already saw.
    if (lsn < slots_[core].min_snapshot_lsn.load()) slots_[core].min_snapshot_lsn.store(lsn);
    NoteSlot(core);
}

void InstanceVisibility::BeginCommit(std::uint32_t core) {
    if (core >= slots_.size()) return;
    // The ceiling as this core can see it, stored before the append. The
    // LSN the append assigns is above every commit this value covers, so
    // a snapshot capped by it cannot cover the commit. Reading the
    // *published* maximum rather than `SnapshotCeiling()` is deliberate:
    // the cap only has to sit below this core's own LSN, and the tighter
    // value would cap every mint below other cores' pending commits twice.
    //
    // **Under the window latch**, so the read and the store are one event
    // against every publication and every pass (the header's Concurrency
    // note): a pass ordered after this sees the marker, and a pass ordered
    // before it published nothing this value fails to cover.
    LatchGuard guard(&window_latch_);
    slots_[core].pending_commit_bound.store(commit_ceiling_.load());
    NoteSlot(core);
}

void InstanceVisibility::EndCommit(std::uint32_t core) noexcept {
    if (core >= slots_.size()) return;
    slots_[core].pending_commit_bound.store(kUnboundedBound);
}

std::uint64_t InstanceVisibility::SnapshotCeiling() const noexcept {
    std::uint64_t ceiling = commit_ceiling_.load();
    // **The maximum first, the caps after, and the order is load-bearing.**
    // Take a commit K whose LSN is at or below what this returns. K's LSN
    // is at or below the maximum read above, and that maximum was published
    // before the read - so K's append preceded that publication, and K's
    // marker was therefore stored before the read too. The loop below then
    // sees either the marker (which caps the answer below K's LSN, so K is
    // not covered) or a cleared slot, whose store `EndCommit` makes after
    // K's entry is in - so K is in the window. Either way the answer
    // covers no unpublished commit.
    //
    // Reading the caps *first* breaks that and reopens AN-Q3 exactly: core
    // A's marker reads clear, A then begins a commit and is assigned 160,
    // core B publishes 170, and this read of the maximum returns 170 -
    // covering 160, whose entry is not in yet. Do not swap these two.
    const std::uint32_t in_use = slots_in_use_.load(std::memory_order_acquire);
    for (std::uint32_t core = 0; core < in_use; ++core) {
        const std::uint64_t bound = slots_[core].pending_commit_bound.load();
        if (bound < ceiling) ceiling = bound;
    }
    return ceiling;
}

std::uint64_t InstanceVisibility::PendingCommitBound() const noexcept {
    std::uint64_t bound = kUnboundedBound;
    const std::uint32_t in_use = slots_in_use_.load(std::memory_order_acquire);
    for (std::uint32_t core = 0; core < in_use; ++core) {
        const std::uint64_t marker = slots_[core].pending_commit_bound.load();
        if (marker < bound) bound = marker;
    }
    return bound;
}

std::uint64_t InstanceVisibility::HorizonLsn() const noexcept {
    std::uint64_t horizon = kUnboundedBound;
    const std::uint32_t in_use = slots_in_use_.load(std::memory_order_acquire);
    for (std::uint32_t core = 0; core < in_use; ++core) {
        const std::uint64_t bound = slots_[core].min_snapshot_lsn.load();
        if (bound < horizon) horizon = bound;
    }
    return horizon;
}

std::uint64_t InstanceVisibility::FloorCandidate() const noexcept {
    std::uint64_t candidate = kUnboundedBound;
    const std::uint32_t in_use = slots_in_use_.load(std::memory_order_acquire);
    for (std::uint32_t core = 0; core < in_use; ++core) {
        const CoreVisibilitySlot& slot = slots_[core];
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

std::size_t InstanceVisibility::attached_cores() const noexcept {
    std::size_t attached = 0;
    const std::uint32_t in_use = slots_in_use_.load(std::memory_order_acquire);
    for (std::uint32_t core = 0; core < in_use; ++core) {
        if (slots_[core].issue_cursor.load(std::memory_order_acquire) != kUnboundedBound) {
            ++attached;
        }
    }
    return attached;
}

bool InstanceVisibility::PinsFloor(std::uint32_t core) const noexcept {
    if (core >= slots_.size()) return false;
    const std::uint64_t cursor = slots_[core].issue_cursor.load(std::memory_order_acquire);
    if (cursor == kUnboundedBound) return false;
    if (attached_cores() < 2) return false;
    return cursor == FloorCandidate();
}

std::uint64_t InstanceVisibility::PublishCommit(std::uint64_t trx_id, std::uint64_t commit_lsn) {
    bool reclaim = false;
    {
        LatchGuard guard(&window_latch_);
        // The unlogged order: one past the highest published. Under the
        // hold, so two unlogged managers over one instance never take the
        // same position.
        if (commit_lsn == kNoCommitLsn) {
            commit_lsn = commit_ceiling_.load(std::memory_order_relaxed) + 1;
        }
        // `try_emplace`, not `operator[]`: a transaction id is issued once
        // and never reissued (invariant 12 and `trx_id.hpp`'s "unique and
        // monotonic, never gapless"), so a second commit under one id is a
        // defect the map would otherwise absorb by overwriting the first
        // one's order with the second's. Keeping the first is the
        // conservative half of the choice; the assignment is simply dropped.
        window_.try_emplace(trx_id, commit_lsn);
        // **After the entry, under the same hold** (AN-R9). A snapshot that
        // takes this ceiling must be able to find every commit it covers;
        // raising it first would publish a commit's *order* before its
        // *entry* and let one snapshot answer that commit invisible and
        // then visible. Both writes are inside the window latch, so a
        // reader taking the pair sees them together or not at all.
        if (commit_lsn > commit_ceiling_.load(std::memory_order_relaxed)) {
            commit_ceiling_.store(commit_lsn);  // seq_cst: the header's Concurrency note
        }
        reclaim = window_.size() >= reclaim_at_;
    }
    if (reclaim) Reclaim();
    return commit_lsn;
}

InstanceVisibility::CommitLookup InstanceVisibility::LookupCommit(std::uint64_t trx_id) const {
    // One hold, both reads (AN-R12). `Reclaim()` erases and raises the
    // floor under this same latch, so what this returns is either wholly
    // before that pass or wholly after it - never the half-and-half that
    // answers "not committed" for a reclaimed winner.
    LatchGuard guard(&window_latch_);
    CommitLookup found;
    auto it = window_.find(trx_id);
    if (it != window_.end()) found.commit_lsn = it->second;
    found.floor = floor_.load(std::memory_order_acquire);
    return found;
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

    LatchGuard guard(&window_latch_);

    // **The bound, read under the hold** (the header's "reclamation"
    // note): the oldest published snapshot, capped by every pending
    // commit's marker - a future mint may be capped to it, and would then
    // find a dropped entry above it committed by the floor. Under the hold
    // rather than before it, so a marker set under the same hold is either
    // seen here or set after this pass, and an unpublished mint's lowered
    // slot is ordered against the ceiling read that follows it.
    const std::uint64_t horizon = std::min(HorizonLsn(), PendingCommitBound());

    // An entry whose commit is above the bound pins the floor at its own
    // id: a live snapshot below that commit must still be told this writer
    // had not committed, and the floor's branch would tell it otherwise.
    std::uint64_t reachable = candidate;
    // Skipped outright with no reader and no commit in flight anywhere:
    // `commit_lsn > horizon` is unsatisfiable at `kUnboundedBound`, which
    // is every pass in an instance with no live view. Halves the work in
    // that case rather than proving the same thing per entry.
    if (horizon != kUnboundedBound) {
        for (const auto& [trx_id, commit_lsn] : window_) {
            if (trx_id < reachable && commit_lsn > horizon) reachable = trx_id;
        }
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

    // Monotone, and the CAS is not decoration: `reachable` is at or above
    // the floor in every state this instance can reach, so the guard exists
    // for the one state it cannot. A live transaction's id is at or above
    // its own core's published cursor, which is at or above the floor, so
    // `oldest_unresolved` never lowers the candidate below it; and an entry
    // the floor has passed was erased by the pass that passed it, so no
    // window entry pins `reachable` below the floor either. **The one way
    // down is a core attaching with a first cursor below the floor** - which
    // the wiring forbids (a core's sequence opens at the superblock's
    // `next_trx_id`, at or above every attached core's cursor) and this
    // class cannot check, because refusing the cursor would leave the floor
    // above that core's ids anyway. The CAS keeps a monotone floor rather
    // than repairing an instance that is already wrong.
    std::uint64_t held = floor_.load(std::memory_order_relaxed);
    while (reachable > held && !floor_.compare_exchange_weak(held, reachable,
                                                             std::memory_order_release,
                                                             std::memory_order_relaxed)) {
    }

    reclaim_at_ = std::max(kReclaimFloor, window_.size() * 2);
    return dropped;
}

}  // namespace kds::txn
