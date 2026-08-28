#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/storage/keystone.hpp"

// RD3 — the range directory as a **router** reads it, and the resolver
// over it (`docs/spec/crosscore.md` CC9 and §2a;
// `docs/inflight/in-progress/workplan-range-directory.md` RD3 and §12,
// which owns this row's conclusions and the alternatives it declined;
// work order `instructions/v2.5.0/range-directory.md` RB1).
//
// `SysRangeRow` (rows.hpp) is the durable form; `RangeTarget` here is the
// resolved one, and the difference between them is exactly `hi`. The row
// deliberately does not store it - CC9's `hi` is the next row's `lo`, so
// storing it would let two rows disagree about one boundary - which means
// **every reader would otherwise derive it**, and a derivation repeated
// per reader is a derivation that will eventually be done differently.
// `RangeTargetsFrom` does it once, at the catalog fill, and nothing
// downstream of that fill sees a row again.
//
// ---- Two rules this file holds, each stated once ---------------------
//
// **The zero-cost invariant.** CC9: *a relation with no rows in the
// directory is one range owned by `sys.tables.owner_core`*; §2a: *a
// one-range relation on its owner core must add zero instructions over
// today*. So the unsplit path must not reach this file at all - it reads
// `access.ranges.empty()`, one load from an entry it is already holding
// and one predictable branch, and runs the code it ran before.
// `ResolveRanges` therefore **refuses an empty directory** instead of
// answering it, so a caller that wandered here off the unsplit path fails
// a test rather than quietly paying the fan-out.
//
// **The plan-time rule.** §2c: *a resolved range set is a plan-time
// value, re-resolved after any park, never re-validated with
// `catalog_version()`* - because `InvalidateFromPeer()` clears a peer's
// cache without advancing that counter, so a set guarded by it is wrong
// on every peer. The resolved set is a **span into
// `TableAccess::ranges`**, the storage the catalog cache owns and
// `CatalogCache::Invalidate()` frees.
//
// Be exact about what that buys, because RB3 and RB4 will build on it: a
// span is trivially copyable and will sit in a coroutine frame across a
// park as happily as a vector would, so **the rule stays the caller's to
// keep**. What the shape changes is how a broken caller fails - a set
// that outlives its borrow is a use-after-free, which a sanitizer
// catches, rather than a stale answer, which nothing catches - and that
// it carries no version to re-validate against, which is the half §2c
// states literally.
//
// Concurrency: every function here is pure over its arguments - no state,
// no locks, nothing retained. The spans returned alias the caller's input
// and carry its lifetime.

namespace kds::catalog {

// One past the highest id any Keystone word can carry, and therefore the
// exclusive upper bound of the id space every relation's ranges
// partition. Derived from invariant 5's 40 bits, never written as a
// literal.
inline constexpr std::uint64_t kIdSpaceEnd = kMaxKeystoneId + 1;

// One range, resolved: the half-open id span `[lo, hi)` CC8 defines, plus
// the two facts routing needs about it.
//
// `owner_core` and `entry_page` are the row's, unchanged - the core that
// owns the span, and where its own sub-structure starts (CC8: a heap
// range's chain head, a btree range's subtree entry). What a
// `SysRangeRow` does not carry and this does is `hi`.
struct RangeTarget {
    std::uint64_t lo = 0;
    // Exclusive. `kIdSpaceEnd` on the last range of a relation, which is
    // what makes the rows a partition of the whole space rather than of
    // the ids that happen to exist.
    std::uint64_t hi = kIdSpaceEnd;
    std::uint32_t owner_core = 0;
    PageId entry_page = kInvalidPageId;
};

// The pk bounds a plan reduces its predicate to, half-open like a range
// so one convention governs both and no site converts between them
// (§2a's routing rules: a pk equality or pk range names its ranges
// arithmetically; a non-pk read predicate names none, and the default is
// every range).
struct PkSpan {
    std::uint64_t lo = 0;
    std::uint64_t hi = kIdSpaceEnd;

    // A predicate that names no pk at all - every range of the relation.
    static constexpr PkSpan Whole() noexcept { return PkSpan{}; }
    // `WHERE pk = id`. Half-open, so an equality is a one-id span and
    // needs no second spelling.
    static constexpr PkSpan Equality(std::uint64_t id) noexcept { return PkSpan{id, id + 1}; }
};

// Derives `hi` down a relation's rows and drops what a router does not
// read. **Requires the rows `Catalog::RangesOf` returns**: ascending by
// `lo`, opening at `lo = 0`, no two at one boundary, none above
// `kMaxKeystoneId` - CC9's partition rules plus the row's own ceiling,
// which that call has already refused a violation of. The fourth is
// this function's requirement rather than CC9's: `hi` on the last row is
// `kIdSpaceEnd`, so a `lo` above the space would derive a range with
// `lo > hi`. Nothing here re-checks them; the reader's door is the one
// place they are enforced, and a second copy of the check is a second
// place for them to differ.
std::vector<RangeTarget> RangeTargetsFrom(std::span<const SysRangeRow> rows);

// The contiguous run of `ranges` that `span` can touch, in ascending
// order - which is range order, which is the order RD7 concatenates
// results in.
//
// Contiguous is not an optimisation but a property of the input: the
// ranges partition `[0, kIdSpaceEnd)`, so the set overlapping any span is
// a run, and the answer is two `partition_point`s rather than a filter.
// It is never empty for an accepted call, since `span` is non-empty and
// sits inside a space the ranges cover completely.
//
// **InvalidArgument, never a quiet empty answer**, on all three refusals,
// because an empty result would read downstream as "no rows" - a wrong
// answer with nothing logged. Two of them are load-bearing: an empty
// directory is the zero-cost branch above, and an empty span is the
// quiet-empty case itself. The third, a span above the id space, is an
// assertion against a caller that computed a bound wrong rather than a
// guard against a wrong answer - it would resolve correctly - and it is
// kept because accepting a bound this file knows is unspellable is
// exactly the convenience the engine's refusal discipline forbids.
StatusOr<std::span<const RangeTarget>> ResolveRanges(std::span<const RangeTarget> ranges,
                                                    PkSpan span);

}  // namespace kds::catalog
