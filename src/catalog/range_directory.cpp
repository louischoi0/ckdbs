#include "kds/catalog/range_directory.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

namespace kds::catalog {

std::vector<RangeTarget> RangeTargetsFrom(std::span<const SysRangeRow> rows) {
    std::vector<RangeTarget> targets;
    targets.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        // CC9's `hi`: the next row's `lo`, and the id space's end for the
        // last one. Derived here and nowhere else.
        targets.push_back(RangeTarget{rows[i].lo,
                                      i + 1 < rows.size() ? rows[i + 1].lo : kIdSpaceEnd,
                                      rows[i].owner_core, rows[i].entry_page});
    }
    return targets;
}

StatusOr<std::span<const RangeTarget>> ResolveRanges(std::span<const RangeTarget> ranges,
                                                    PkSpan span) {
    // The zero-cost invariant, enforced rather than answered (header).
    if (ranges.empty()) {
        return Status::InvalidArgument(
            "ResolveRanges: this relation has no sys.ranges rows, so it is one range owned by "
            "sys.tables.owner_core (CC9) - the caller reads that cached field and stops rather "
            "than resolving");
    }
    if (span.lo >= span.hi) {
        return Status::InvalidArgument(
            "ResolveRanges: the pk span [" + std::to_string(span.lo) + ", " +
            std::to_string(span.hi) +
            ") is empty, and an empty span names no range; a predicate that reduces to no id has "
            "no rows and is folded at plan time, not routed");
    }
    if (span.hi > kIdSpaceEnd) {
        return Status::InvalidArgument(
            "ResolveRanges: the pk span ends at " + std::to_string(span.hi) +
            ", above the 40-bit Keystone id space (" + std::to_string(kIdSpaceEnd) +
            "), which the ranges partition and no id can fall outside of");
    }

    // Both bounds are `partition_point`s over predicates that are monotone
    // down a partition - `hi` and `lo` both ascend - so neither needs the
    // set searched twice or an iterator stepped back off the front.
    const auto first = std::partition_point(
        ranges.begin(), ranges.end(),
        [&](const RangeTarget& r) { return r.hi <= span.lo; });
    const auto last = std::partition_point(
        first, ranges.end(), [&](const RangeTarget& r) { return r.lo < span.hi; });
    return ranges.subspan(static_cast<std::size_t>(first - ranges.begin()),
                          static_cast<std::size_t>(last - first));
}

}  // namespace kds::catalog
