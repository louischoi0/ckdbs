#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <unordered_map>

#include "kds/base/latch.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/stats/per_core_ring.hpp"

// **Counts keyed by a catalog oid, one part per core**: the shape
// `RelationStats` and `IndexStats` share (BO-Q5 (b), BO-Q7 (a)), written
// once so a store cannot bound, drop or merge differently from another.
//
// One part per core under a latch of its own (`LatchKind::kStatsRing`): the
// core an event happened on is the only writer of its part; a reader on any
// core takes each part's latch in turn and sums the parts by oid with `V`'s
// `operator+=`. **A write never fails, delays or reorders what it
// describes**: the one allocation is a part's first sight of an oid; a new
// oid past `kBound`, or one that cannot be allocated, is dropped and
// counted. An oid is never reissued, so a dropped object's counts stay in
// the parts and count toward the bound.

namespace kds::stats {

template <typename V, std::size_t kBound>
class OidParts {
public:
    OidParts() = default;
    OidParts(const OidParts&) = delete;
    OidParts& operator=(const OidParts&) = delete;

    // `fn(V&)` on `core`'s counts for `oid`, under the part's latch; where
    // the part cannot take a new oid, nothing but the drop's count. **The
    // `V` it is handed stays where it is until `Reset`**: nothing else
    // removes or moves a part's counts, so a store may hand the address out
    // (`IndexStats::CountsOf`) - and such a store is never reset.
    template <typename Fn>
    void With(std::uint32_t core, catalog::Oid oid, Fn&& fn) noexcept {
        Part& part = parts_[core % kStatsParts];
        const LatchGuard held(&part.latch);
        auto found = part.counts.find(oid);
        if (found == part.counts.end()) {
            if (part.counts.size() >= kBound) {
                ++part.dropped;
                return;
            }
            try {
                found = part.counts.emplace(oid, V{}).first;
            } catch (const std::bad_alloc&) {
                ++part.dropped;
                return;
            }
        }
        fn(found->second);
    }

    // Every oid's counts, the cores' parts summed.
    std::unordered_map<catalog::Oid, V> Snapshot() {
        std::unordered_map<catalog::Oid, V> merged;
        for (Part& part : parts_) {
            const LatchGuard held(&part.latch);
            for (const auto& [oid, counts] : part.counts) merged[oid] += counts;
        }
        return merged;
    }

    // New oids no part could take, over every core.
    std::uint64_t Dropped() noexcept {
        std::uint64_t total = 0;
        for (Part& part : parts_) {
            const LatchGuard held(&part.latch);
            total += part.dropped;
        }
        return total;
    }

    // Empties every part, and with it every address `With` ever handed
    // out: for a store nothing keeps a pointer into.
    void Reset() noexcept {
        for (Part& part : parts_) {
            const LatchGuard held(&part.latch);
            part.counts.clear();
            part.dropped = 0;
        }
    }

private:
    struct Part {
        Latch latch{LatchKind::kStatsRing};
        std::unordered_map<catalog::Oid, V> counts;
        std::uint64_t dropped = 0;
    };
    std::array<Part, kStatsParts> parts_;
};

}  // namespace kds::stats
