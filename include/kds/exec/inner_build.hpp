#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "kds/stats/cabin_store.hpp"

// The statement-local inner build's map (docs/spec-join-inner-build.md §2,
// workplan JB2): one entry per inner row that passed the step's
// non-correlated residual, bucketed by join-column value, appended in walk
// order. The executor's walked-join site fills it once per statement (JB3)
// and probes it for every later outer row (JB4); `BuildKey` on the compiled
// step (step_chain.hpp) marks the shape.
//
// Keys are `stats::MakeValueKey`'s — the Cabin's value identity with
// `cabin_id` 0, which no CabinStore key can carry (MakeCabinKey refuses 0),
// so a build key handed to a CabinStore by mistake misses instead of
// matching an authoritative entry set. Entries are the Cabin's 24-byte
// `CabinEntry` (C6), reused rather than redesigned.
//
// ---- Concurrency and lifetime protocol -------------------------------------
//
// No lock, no atomic, deliberately: an InnerBuild is statement-lifetime
// execution state — owned by one executor frame (JB3), filled by that
// statement's own walk, probed by that statement's own later rows,
// destroyed with the frame. The executor parks at page boundaries (P4d)
// and other statements interleave on the core, but never on this frame,
// and nothing else can reach the map.
//
// What a parking, extending caller may hold across a suspension — stated
// here because JB4's replay and JB6's resumed walk rely on it: a bucket
// pointer from `Find` stays valid across any number of `Add`s of *other*
// keys (the map is node-based; a rehash relinks nodes and never moves a
// mapped vector), but an `Add` on the *same* key can reallocate the
// bucket's buffer, so iterators and element pointers into it die. Hold
// the `std::vector*`, never an iterator, across anything that can extend
// the map.
//
// ---- The one load-bearing property -----------------------------------------
//
// **Buckets append in walk order and replay front to back.** Spec §3's
// third fact: a probe emits each key's matches in exactly the order the
// walk would have — for both key modes, because build order *is* walk
// order whichever that is (`ASSIGNED` pk order, `EXPLICIT` page-slot
// order). The contrast to keep in view: the Cabin's recording SORTS its
// entry set by page and slot before committing (step_vm.cpp,
// WalkAndRecord) — correct there, an emission-order change here, because
// the map captures order rather than reconstructing it.
// inner_build_test.cpp pins this on its own rather than through an
// integration test.

namespace kds::exec {

class InnerBuild {
public:
    // Appends `entry` to `key`'s bucket. Walk order in, walk order out.
    void Add(const stats::CabinKey& key, const stats::CabinEntry& entry) {
        buckets_[key].push_back(entry);
        ++rows_;
    }

    // The bucket for `key`, or nullptr when the walk bucketed no row under
    // it. Every bucket returned is non-empty — Add always pushes and
    // nothing erases — so nullptr is the one "no rows here" answer. What
    // it *means* belongs to the caller: after a completed walk it is a
    // conclusive no-match (the walk was the full relation); under JB6's
    // prefix map it means "walk from the mark". The container reports; it
    // never concludes.
    const std::vector<stats::CabinEntry>* Find(const stats::CabinKey& key) const {
        auto it = buckets_.find(key);
        return it == buckets_.end() ? nullptr : &it->second;
    }

    // Entries across all buckets — what JB5's `join_build_max_rows` cap is
    // checked against. Entries, not values: the map's memory is per entry
    // (spec §7, following `aggregate_max_groups`' argument).
    std::size_t rows() const noexcept { return rows_; }

private:
    std::unordered_map<stats::CabinKey, std::vector<stats::CabinEntry>, stats::CabinKeyHash>
        buckets_;
    std::size_t rows_ = 0;
};

}  // namespace kds::exec
