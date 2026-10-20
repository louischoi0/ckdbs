#include "kds/stats/relation_stats.hpp"

#include <new>

namespace kds::stats {

std::array<RelationStats::Part, kStatsParts>& RelationStats::parts() noexcept {
    static std::array<Part, kStatsParts> p;
    return p;
}

void RelationStats::Record(std::uint32_t core, catalog::Oid oid, RelationWrite kind,
                           std::uint64_t rows) noexcept {
    if (rows == 0) return;
    Part& part = parts()[core % kStatsParts];
    const LatchGuard held(&part.latch);
    auto found = part.relations.find(oid);
    if (found == part.relations.end()) {
        if (part.relations.size() >= kRelationsPerCore) {
            ++part.dropped;
            return;
        }
        // The one allocation, on a relation's first write here; a failure
        // drops the count rather than the statement (the header's rule).
        try {
            found = part.relations.emplace(oid, RelationActivity{}).first;
        } catch (const std::bad_alloc&) {
            ++part.dropped;
            return;
        }
    }
    RelationActivity& a = found->second;
    switch (kind) {
        case RelationWrite::kInsert: a.inserts += rows; break;
        case RelationWrite::kUpdate: a.updates += rows; break;
        case RelationWrite::kDelete: a.deletes += rows; break;
        case RelationWrite::kPurge: a.purged += rows; break;
    }
}

std::unordered_map<catalog::Oid, RelationActivity> RelationStats::Snapshot() {
    std::unordered_map<catalog::Oid, RelationActivity> merged;
    for (Part& part : parts()) {
        const LatchGuard held(&part.latch);
        for (const auto& [oid, a] : part.relations) {
            RelationActivity& into = merged[oid];
            into.inserts += a.inserts;
            into.updates += a.updates;
            into.deletes += a.deletes;
            into.purged += a.purged;
        }
    }
    return merged;
}

std::uint64_t RelationStats::Dropped() noexcept {
    std::uint64_t total = 0;
    for (Part& part : parts()) {
        const LatchGuard held(&part.latch);
        total += part.dropped;
    }
    return total;
}

void RelationStats::ResetForTest() noexcept {
    for (Part& part : parts()) {
        const LatchGuard held(&part.latch);
        part.relations.clear();
        part.dropped = 0;
    }
}

}  // namespace kds::stats
