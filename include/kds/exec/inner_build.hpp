#pragma once

#include <cstddef>
#include <cstdint>
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
// ---- Storage: one arena, chained per key ----------------------------------
//
// Entries live in **one append-only vector** and a key's rows are linked
// through a parallel index vector, head and tail per key. A bucket is
// therefore a chain walk, not a contiguous span, and the price of a
// distinct key is one hash-map node — where a vector per key cost a second
// allocation and a growth realloc per bucket besides.
//
// This is not a free-standing preference: the build is paid **per inner
// row, on the walk the statement was going to run anyway**, so its constant
// is what decides at which k the build beats the per-row walk (the JB5
// gate measured 83.7 ns/row and a break-even of k ≈ 2.6 with a vector per
// key; the arena is the largest single term of that constant).
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
// here because JB4's replay and JB6's resumed walk rely on it: **a `Bucket`
// stays valid across any number of `Add`s, including `Add`s under its own
// key.** A Bucket holds an index, never a pointer, and its iterator reads
// the arena through the map on each dereference, so a growth realloc of
// either vector cannot invalidate it — an `Add` under the same key appends
// to the chain's tail, which a walk in flight either reaches or does not,
// exactly as a walk-order prefix should behave (JB6's resumed walk is what
// wants this). What is *not* stable is a `CabinEntry&` or a pointer taken
// out of the arena and held: that dies on the next growth like any vector
// element. Hold the Bucket, never a reference into it.
//
// ---- The one load-bearing property -----------------------------------------
//
// **Buckets append in walk order and replay front to back.** Spec §3's
// third fact: a probe emits each key's matches in exactly the order the
// walk would have — for both key modes, because build order *is* walk
// order whichever that is (`ASSIGNED` pk order, `EXPLICIT` page-slot
// order). The chain is appended at the tail for exactly this reason; a
// head-insert list would be one instruction cheaper and would reverse
// every reply. The contrast to keep in view: the Cabin's recording SORTS
// its entry set by page and slot before committing (step_vm.cpp,
// WalkAndRecord) — correct there, an emission-order change here, because
// the map captures order rather than reconstructing it.
// inner_build_test.cpp pins this on its own rather than through an
// integration test.

namespace kds::exec {

class InnerBuild {
public:
    // The chain terminator, and the "no rows under this key" head.
    static constexpr std::uint32_t kNoEntry = 0xFFFFFFFFu;

    // One key's entries, in walk order. A value, not a pointer: an empty
    // Bucket *is* the "no rows here" answer, so there is no null to
    // forget. What emptiness **means** belongs to the caller — after a
    // completed walk it is a conclusive no-match (the walk was the full
    // relation); under JB6's prefix map it means "walk from the mark".
    // The container reports; it never concludes.
    class Bucket {
    public:
        // Exactly what a range-for and the probe need, and nothing a
        // general iterator would also carry: no post-increment, no
        // `iterator_category`. Both are one line each to add the day an
        // algorithm wants them, and neither has a caller today.
        class iterator {
        public:
            iterator(const InnerBuild* build, std::uint32_t at) : build_(build), at_(at) {}

            const stats::CabinEntry& operator*() const { return build_->entries_[at_]; }
            const stats::CabinEntry* operator->() const { return &build_->entries_[at_]; }
            iterator& operator++() {
                at_ = build_->next_[at_];
                return *this;
            }
            bool operator==(const iterator& other) const noexcept { return at_ == other.at_; }
            bool operator!=(const iterator& other) const noexcept { return at_ != other.at_; }

        private:
            const InnerBuild* build_ = nullptr;
            std::uint32_t at_ = kNoEntry;
        };

        Bucket() = default;
        Bucket(const InnerBuild* build, std::uint32_t head) : build_(build), head_(head) {}

        iterator begin() const { return iterator(build_, head_); }
        iterator end() const { return iterator(build_, kNoEntry); }
        bool empty() const noexcept { return head_ == kNoEntry; }

    private:
        const InnerBuild* build_ = nullptr;
        std::uint32_t head_ = kNoEntry;
    };

    // Appends `entry` to `key`'s bucket. Walk order in, walk order out.
    //
    // **False means the entry was not stored**, and the caller owes the
    // map the same verdict JB5's cap gets: decline the build, walk per
    // outer row. It can only happen past `kMaxEntries` — an index type's
    // limit, not a policy — and `join_build_max_rows` (whose config
    // accepts any unsigned value) is the only thing that would let a
    // walk reach it. A dropped row would be worse than a decline by the
    // whole distance between "slower" and "wrong": the map's published
    // form claims to be the entire relation.
    [[nodiscard]] bool Add(const stats::CabinKey& key, const stats::CabinEntry& entry) {
        if (entries_.size() >= kMaxEntries) return false;
        const auto idx = static_cast<std::uint32_t>(entries_.size());
        entries_.push_back(entry);
        next_.push_back(kNoEntry);
        Chain& chain = chains_[key];
        if (chain.head == kNoEntry) {
            chain.head = idx;
        } else {
            next_[chain.tail] = idx;
        }
        chain.tail = idx;
        return true;
    }

    // The entries bucketed under `key`, in walk order; empty when the walk
    // bucketed none.
    Bucket Find(const stats::CabinKey& key) const {
        auto it = chains_.find(key);
        return it == chains_.end() ? Bucket() : Bucket(this, it->second.head);
    }

    // Entries across all buckets — what JB5's `join_build_max_rows` cap is
    // checked against. Entries, not values: the map's memory is per entry
    // (spec §7, following `aggregate_max_groups`' argument).
    std::size_t rows() const noexcept { return entries_.size(); }

private:
    // One below the terminator: an entry at kNoEntry could not be linked.
    static constexpr std::size_t kMaxEntries = kNoEntry;

    struct Chain {
        std::uint32_t head = kNoEntry;
        std::uint32_t tail = kNoEntry;
    };

    std::vector<stats::CabinEntry> entries_;
    std::vector<std::uint32_t> next_;
    std::unordered_map<stats::CabinKey, Chain, stats::CabinKeyHash> chains_;
};

}  // namespace kds::exec
