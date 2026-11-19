#include "kds/stats/leaf_stats.hpp"

#include "kds/base/current_core.hpp"

namespace kds::stats {

LeafStats::Parts& LeafStats::parts() noexcept {
    static Parts p;
    return p;
}

void LeafStats::CountDivision(catalog::Oid owner_oid) noexcept {
    if (owner_oid == 0) return;
    parts().With(CurrentCore(), owner_oid, [](LeafActivity& a) { ++a.divisions; });
}

void LeafStats::CountCompaction(catalog::Oid owner_oid) noexcept {
    if (owner_oid == 0) return;
    parts().With(CurrentCore(), owner_oid, [](LeafActivity& a) { ++a.compactions; });
}

std::unordered_map<catalog::Oid, LeafActivity> LeafStats::Snapshot() {
    return parts().Snapshot();
}

std::uint64_t LeafStats::Dropped() noexcept { return parts().Dropped(); }

void LeafStats::ResetForTest() noexcept { parts().Reset(); }

}  // namespace kds::stats
