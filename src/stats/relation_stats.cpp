#include "kds/stats/relation_stats.hpp"

namespace kds::stats {

RelationStats::Parts& RelationStats::parts() noexcept {
    static Parts p;
    return p;
}

void RelationStats::Record(std::uint32_t core, catalog::Oid oid, RelationWrite kind,
                           std::uint64_t rows) noexcept {
    if (rows == 0) return;
    parts().With(core, oid, [&](RelationActivity& a) {
        switch (kind) {
            case RelationWrite::kInsert: a.inserts.add(rows); break;
            case RelationWrite::kUpdate: a.updates.add(rows); break;
            case RelationWrite::kDelete: a.deletes.add(rows); break;
            case RelationWrite::kPurge: a.purged.add(rows); break;
            case RelationWrite::kKeyUpdate: a.key_updates.add(rows); break;
        }
    });
}

void RelationStats::RecordLockWait(std::uint32_t core, catalog::Oid oid, std::uint64_t waited_us,
                                   bool deadlock, bool fault_net) noexcept {
    if (oid == 0) return;
    parts().With(core, oid, [&](RelationActivity& a) {
        a.lock_waits.add(1);
        a.lock_wait_us.add(waited_us);
        if (deadlock) a.lock_deadlocks.add(1);
        if (fault_net) a.lock_fault_nets.add(1);
    });
}

std::unordered_map<catalog::Oid, RelationActivity> RelationStats::Snapshot() {
    return parts().Snapshot();
}

std::uint64_t RelationStats::Dropped() noexcept { return parts().Dropped(); }

void RelationStats::ResetForTest() noexcept { parts().Reset(); }

}  // namespace kds::stats
