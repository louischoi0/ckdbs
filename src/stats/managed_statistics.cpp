#include "kds/stats/managed_statistics.hpp"

#include <algorithm>
#include <string>

#include "kds/stats/plan_stats.hpp"

namespace kds::stats {

namespace {

std::string OidText(catalog::Oid oid) { return "relation oid " + std::to_string(oid); }

}  // namespace

const char* StatisticsOriginName(StatisticsOrigin origin) noexcept {
    switch (origin) {
        case StatisticsOrigin::kGathered:
            return "gathered";
        case StatisticsOrigin::kImported:
            return "imported";
    }
    return "unknown";
}

Status ManagedStatistics::CheckUnlocked(const Slot& slot, bool force) const {
    if (!slot.locked || force) return Status::OK();
    // CY-Q28 (a): a lock stops every act that would change the published
    // object, the operator's own included, until `FORCE` says otherwise.
    return Status::InvalidArgument(
        "the relation's statistics are locked; UNLOCK STATISTICS, or write FORCE");
}

void ManagedStatistics::Publish(Slot& slot, const RelationStatistic& stat, bool fresh) {
    const double rows = std::max(static_cast<double>(stat.rows), 1.0);
    if (slot.published.has_value()) {
        slot.history.push_back(*slot.published);
        while (slot.history.size() > kHistoryVersions) slot.history.pop_front();
    }
    // CY-Q19 (a), PS19: a gather over a published gather moves its band with
    // CC's hysteresis, so a re-gather that moves the count by a few percent
    // publishes nothing new to the planner; anything else takes its band.
    bool banded = slot.published.has_value() &&
                  slot.published->origin == StatisticsOrigin::kGathered && !fresh;
    if (banded) {
        (void)PlanStats::Reband(slot.band, banded, rows);
    } else {
        slot.band = BandOf(rows);
    }
    slot.published = stat;
}

StatusOr<RelationStatistic> ManagedStatistics::Store(catalog::Oid oid, RelationStatistic stat,
                                                     bool pending, bool force) {
    const LatchGuard hold(&latch_);
    if (forgotten_.count(oid) != 0) {
        return Status::NotFound(OidText(oid) + " was dropped while its statistics were made");
    }
    Slot& slot = slots_[oid];
    if (!pending) {
        if (Status s = CheckUnlocked(slot, force); !s.ok()) return s;
    }
    stat.version = ++slot.last_version;
    if (pending) {
        slot.pending = stat;
    } else {
        Publish(slot, stat, /*fresh=*/stat.origin == StatisticsOrigin::kImported);
    }
    return stat;
}

StatusOr<RelationStatistic> ManagedStatistics::PublishPending(catalog::Oid oid, bool force) {
    const LatchGuard hold(&latch_);
    const auto it = slots_.find(oid);
    if (it == slots_.end() || !it->second.pending.has_value()) {
        return Status::NotFound(OidText(oid) + " has no pending statistics");
    }
    Slot& slot = it->second;
    if (Status s = CheckUnlocked(slot, force); !s.ok()) return s;
    const RelationStatistic stat = *slot.pending;
    slot.pending.reset();
    Publish(slot, stat, /*fresh=*/stat.origin == StatisticsOrigin::kImported);
    return stat;
}

Status ManagedStatistics::DiscardPending(catalog::Oid oid) {
    const LatchGuard hold(&latch_);
    const auto it = slots_.find(oid);
    if (it == slots_.end() || !it->second.pending.has_value()) {
        return Status::NotFound(OidText(oid) + " has no pending statistics");
    }
    it->second.pending.reset();
    return Status::OK();
}

StatusOr<RelationStatistic> ManagedStatistics::Restore(catalog::Oid oid, std::uint64_t version,
                                                       bool force) {
    const LatchGuard hold(&latch_);
    const auto it = slots_.find(oid);
    if (it == slots_.end()) {
        return Status::NotFound(OidText(oid) + " has no statistics version " +
                                std::to_string(version));
    }
    Slot& slot = it->second;
    const RelationStatistic* found = nullptr;
    if (slot.published.has_value() && slot.published->version == version) {
        found = &*slot.published;
    }
    for (const RelationStatistic& kept : slot.history) {
        if (kept.version == version) found = &kept;
    }
    if (found == nullptr) {
        return Status::NotFound(OidText(oid) + " keeps no statistics version " +
                                std::to_string(version) + " (the last " +
                                std::to_string(kHistoryVersions) + " replaced are kept)");
    }
    if (Status s = CheckUnlocked(slot, force); !s.ok()) return s;
    RelationStatistic again = *found;  // copied: the publish moves the history
    again.version = ++slot.last_version;
    Publish(slot, again, /*fresh=*/true);
    return again;
}

Status ManagedStatistics::Lock(catalog::Oid oid) {
    const LatchGuard hold(&latch_);
    if (forgotten_.count(oid) != 0) return Status::NotFound(OidText(oid) + " was dropped");
    slots_[oid].locked = true;
    return Status::OK();
}

Status ManagedStatistics::Unlock(catalog::Oid oid) {
    const LatchGuard hold(&latch_);
    const auto it = slots_.find(oid);
    if (it == slots_.end() || !it->second.locked) {
        return Status::InvalidArgument(OidText(oid) + "'s statistics are not locked");
    }
    it->second.locked = false;
    return Status::OK();
}

Status ManagedStatistics::Delete(catalog::Oid oid, bool force) {
    const LatchGuard hold(&latch_);
    const auto it = slots_.find(oid);
    if (it == slots_.end() || (!it->second.published.has_value() &&
                               !it->second.pending.has_value() && it->second.history.empty())) {
        return Status::NotFound(OidText(oid) + " has no statistics to delete");
    }
    Slot& slot = it->second;
    if (Status s = CheckUnlocked(slot, force); !s.ok()) return s;
    slot.published.reset();
    slot.pending.reset();
    slot.history.clear();
    slot.band = 0;
    return Status::OK();
}

bool ManagedStatistics::Forget(catalog::Oid oid) {
    const LatchGuard hold(&latch_);
    forgotten_.insert(oid);
    const auto it = slots_.find(oid);
    if (it == slots_.end()) return false;
    const bool published = it->second.published.has_value();
    slots_.erase(it);
    return published;
}

Status ManagedStatistics::CheckPublishable(catalog::Oid oid, bool force) const {
    const LatchGuard hold(&latch_);
    const auto it = slots_.find(oid);
    return it == slots_.end() ? Status::OK() : CheckUnlocked(it->second, force);
}

std::vector<std::pair<catalog::Oid, std::int16_t>> ManagedStatistics::PublishedBands() const {
    const LatchGuard hold(&latch_);
    std::vector<std::pair<catalog::Oid, std::int16_t>> out;
    for (const auto& [oid, slot] : slots_) {
        if (slot.published.has_value()) out.emplace_back(oid, slot.band);
    }
    return out;
}

std::vector<StatisticsSlotView> ManagedStatistics::Slots() const {
    const LatchGuard hold(&latch_);
    std::vector<StatisticsSlotView> out;
    out.reserve(slots_.size());
    for (const auto& [oid, slot] : slots_) {
        StatisticsSlotView view;
        view.oid = oid;
        view.published = slot.published;
        view.band = slot.band;
        view.pending = slot.pending;
        view.history.assign(slot.history.begin(), slot.history.end());
        view.locked = slot.locked;
        view.last_version = slot.last_version;
        out.push_back(std::move(view));
    }
    return out;
}

void ManagedStatistics::Load(std::vector<StatisticsSlotView> slots) {
    const LatchGuard hold(&latch_);
    slots_.clear();
    for (StatisticsSlotView& view : slots) {
        Slot& slot = slots_[view.oid];
        // CY-Q19 (a): a mount publishes fresh, so one stored value gives one
        // band on every instance; no hysteresis is carried across it.
        if (view.published.has_value()) {
            slot.band = BandOf(std::max(static_cast<double>(view.published->rows), 1.0));
        }
        slot.published = view.published;
        slot.pending = view.pending;
        slot.history.assign(view.history.begin(), view.history.end());
        while (slot.history.size() > kHistoryVersions) slot.history.pop_front();
        slot.locked = view.locked;
        slot.last_version = view.last_version;
    }
}

}  // namespace kds::stats
