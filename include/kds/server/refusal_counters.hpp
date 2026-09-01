#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <ostream>
#include <string_view>

#include "kds/catalog/oid.hpp"

// The shape `crosscore.md` §6's refusal counters have, once — a tally by a
// composite key, ordered, with a total.
//
// Two of them exist: cross-core write refusals (`core_affinity.hpp`, §6)
// and declined range openings (`range_alloc.hpp`, RD5's C3). They were the
// same class written twice, down to the sentence explaining why the map is
// ordered, which is the duplication `rules.md` names and which would have
// let two counters that are read together diverge in how they order or
// total.
//
// **What is deliberately *not* here is what each counter means.** The key
// tuple, the reading a number supports, and the era a series spans are
// per-counter facts and stay at each one; this holds only the tally.
//
// Ordered rather than hashed, and that is the one property with a reason
// beyond convenience: a report of these must be stable run to run
// (`sched.md` §8's determinism rule for anything observable), and
// `SHOW META` prints them in this order.

namespace kds::server {

template <typename Key>
class RefusalCounters {
public:
    void Add(const Key& key) { ++counts_[key]; }

    // `n` events at once, for a counter whose unit is finer than its call
    // site - the pre-grant Cabin discard drops a relation's value sets in
    // one pass. A loop at the call site would be `+=` written where it can
    // be got wrong.
    void Add(const Key& key, std::uint64_t n) { counts_[key] += n; }

    std::uint64_t CountFor(const Key& key) const {
        auto it = counts_.find(key);
        return it == counts_.end() ? 0 : it->second;
    }

    std::uint64_t total() const noexcept {
        std::uint64_t n = 0;
        for (const auto& [key, count] : counts_) n += count;
        return n;
    }

    const std::map<Key, std::uint64_t>& counts() const noexcept { return counts_; }

private:
    std::map<Key, std::uint64_t> counts_;
};

// The `SHOW META` form these print in: `<name>s=N <name>_keys=K` and,
// where anything was counted, `<name>_detail=<key>=count,...` in the map's
// order — capped, and **the cap says it truncated**, because a silent cut
// reads as "these were all of them".
//
// One implementation for both counters. Two hand-written copies is what
// this replaced, each with its own `kMaxDetailKeys` in the same function,
// which is one cap in two places and two chances to print a number the
// other would have truncated.
//
// `even_when_zero` is the one place they legitimately differ: the
// cross-core triple predates `SHOW META`'s absent-rather-than-zeroed rule
// and prints unconditionally so its series does not break, where a
// counter added under that rule is absent until it has something to say.
template <typename Key, typename FormatKey>
void PrintRefusalCounters(std::ostream& os, std::string_view name,
                          const std::map<Key, std::uint64_t>& counts, FormatKey format_key,
                          bool even_when_zero) {
    std::uint64_t total = 0;
    for (const auto& [key, count] : counts) total += count;
    if (total == 0 && !even_when_zero) return;

    os << ' ' << name << "s=" << total << ' ' << name << "_keys=" << counts.size();
    if (counts.empty()) return;

    constexpr std::size_t kMaxDetailKeys = 16;
    os << ' ' << name << "_detail=";
    std::size_t printed = 0;
    for (const auto& [key, count] : counts) {
        if (printed == kMaxDetailKeys) {
            os << ",+" << (counts.size() - printed) << "more";
            break;
        }
        if (printed != 0) os << ',';
        format_key(os, key);
        os << '=' << count;
        ++printed;
    }
}

// **What CC10's pre-grant Cabin discard dropped**, keyed by relation
// (SB-R4, `instructions/v2.7.1/workorder-sb.md`). Here, with the template,
// because two headers need the type and a `using` in one of them would be
// a declaration the other could not see.
//
// **One count per value set, not per Cabin**: the unit is what
// re-observation has to rebuild, and a Cabin with a thousand observed
// values costs a thousand times what one with a single value costs. The
// reading is the price of the transition rule `docs/spec/cabin.md` §4b
// states — the discard buys a true authority claim, and this is the bill,
// arriving later as misses on the same relation. Written on **core 0**,
// which is where a split runs and, today, the only core holding a Cabin
// store at all; read through core 0's `SHOW META` beside
// `cabin_scope_fallthroughs`, because the pair is one reading: what the
// discard cost, and whether anything was served afterwards.
using CabinSplitDiscardCounters = RefusalCounters<catalog::Oid>;

}  // namespace kds::server
