#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "kds/catalog/oid.hpp"
#include "kds/stats/counter_table.hpp"
#include "kds/stats/per_core_ring.hpp"

// **Counts keyed by a catalog oid, one part per core**: the shape
// `RelationStats`, `LeafStats` and `IndexStats` share (BO-Q5 (b), BO-Q7 (a)),
// written once so a store cannot bound, drop or merge differently from
// another.
//
// **No latch** (`docs/rules/rules.md` §3a; DL-S2): a part is a
// `CounterTable` (`stats/counter_table.hpp`) - a fixed insert-only table
// allocated at the core's first sight of any oid, a row of relaxed
// single-writer words an oid. The core an event happened on is the only
// writer of its part and takes nothing to write; a reader on any core walks
// each part and sums the rows by oid with `V`'s `operator+=`, reading the
// words as they stand, so a row's columns may differ by an event in flight.
// **A write never fails, delays or reorders what it describes** - a count
// under a page's exclusive hold included (`LeafStats`): the one allocation
// is a part's first sight; a new oid past `kBound`, or a part whose table
// could not be allocated, is dropped and counted. An oid is never reissued,
// so a dropped object's counts stay in the parts and count toward the
// bound.

namespace kds::stats {

template <typename V, std::size_t kBound>
class OidParts {
public:
    OidParts() = default;
    OidParts(const OidParts&) = delete;
    OidParts& operator=(const OidParts&) = delete;

    // `core`'s counts for `oid`, made at its first sight; null, and one drop
    // counted, where the part cannot take a new oid. Oid 0 names no object
    // and counts nothing. **The address holds for the store's life** - a
    // slot is never moved or released - so a store may hand it out
    // (`IndexStats::CountsOf`), and only `core` writes through it.
    V* CountsOf(std::uint32_t core, catalog::Oid oid) noexcept {
        return oid == 0 ? nullptr : parts_[core % kStatsParts].CountsOf(oid);
    }

    // `fn(V&)` on `core`'s counts for `oid`; where the part cannot take a
    // new oid, nothing but the drop's count.
    template <typename Fn>
    void With(std::uint32_t core, catalog::Oid oid, Fn&& fn) noexcept {
        if (V* counts = CountsOf(core, oid); counts != nullptr) fn(*counts);
    }

    // Every oid's counts, the cores' parts summed as their words stand.
    std::unordered_map<catalog::Oid, V> Snapshot() const {
        std::unordered_map<catalog::Oid, V> merged;
        for (const Part& part : parts_) {
            part.ForEach([&](std::uint64_t oid, const V& counts) { merged[oid] += counts; });
        }
        return merged;
    }

    // New oids no part could take, over every core.
    std::uint64_t Dropped() const noexcept {
        std::uint64_t total = 0;
        for (const Part& part : parts_) total += part.dropped();
        return total;
    }

    // Zeroes every part where it stands, and with it every row a caller
    // kept the address of; a test's alone, with no writer running.
    void Reset() noexcept {
        for (Part& part : parts_) part.ResetForTest();
    }

private:
    using Part = CounterTable<V, kBound>;
    std::array<Part, kStatsParts> parts_;
};

}  // namespace kds::stats
