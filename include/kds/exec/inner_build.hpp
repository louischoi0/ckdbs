#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kds/stats/cabin_store.hpp"

// The statement-local inner build's map (docs/spec/join-inner-build.md §2,
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
// ---- Storage: one arena, chained per key, in an open-addressed table -------
//
// Entries live in **one append-only vector** and a key's rows are linked
// through a parallel index vector, head and tail per key. A bucket is
// therefore a chain walk, not a contiguous span. Keys live in a second
// append-only vector, found through an open-addressed table of 8-byte
// slots — so **a distinct key costs no allocation at all**: not the
// bucket vector a `vector`-per-key cost, and not the node a
// `std::unordered_map` cost. It also drops that map's per-lookup integer
// division (libstdc++ indexes buckets with `hash % prime`), which a
// power-of-two mask replaces.
//
// This is not a free-standing preference: the build is paid **per inner
// row, on the walk the statement was going to run anyway**, so its
// constant is what decides at which k the build beats the per-row walk.
// The measured history, all in `docs/workplan-join-inner-build.md`: 83.7
// ns/row with a vector per key (break-even k ≈ 2.6), 43.2 with the arena
// and a Keystone-word pk (k ≈ 1.8), and this table is the third cut.
//
// What is deliberately *not* traded away for speed: the key is still the
// Cabin's whole value identity (`stats::MakeValueKey`), compared in full
// on a tag match. A map keyed on a hash alone would be faster still and
// would be safe only by the probe's re-check — a correctness argument
// this container has no business resting on (JB2's identity decision).
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
// walk would have — for either key order, because build order *is* walk
// order whichever that is (pk order while a relation's keys have only
// ascended, page-slot order once one has landed below its high-water
// mark). The chain is appended at the tail for exactly this reason; a
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
        const std::uint64_t hash = stats::CabinKeyHash{}(key);
        const std::uint32_t k = KeyFor(key, hash);
        const auto idx = static_cast<std::uint32_t>(entries_.size());
        entries_.push_back(entry);
        next_.push_back(kNoEntry);
        Key& rec = keys_[k];
        if (rec.head == kNoEntry) {
            rec.head = idx;
        } else {
            next_[rec.tail] = idx;
        }
        rec.tail = idx;
        return true;
    }

    // The entries bucketed under `key`, in walk order; empty when the walk
    // bucketed none.
    Bucket Find(const stats::CabinKey& key) const {
        if (table_.empty()) return Bucket();
        const std::uint64_t hash = stats::CabinKeyHash{}(key);
        for (std::size_t i = hash & mask_;; i = (i + 1) & mask_) {
            const Slot& slot = table_[i];
            if (slot.key == kNoEntry) return Bucket();
            if (slot.tag == TagOf(hash) && keys_[slot.key].key == key) {
                return Bucket(this, keys_[slot.key].head);
            }
        }
    }

    // Entries across all buckets — what JB5's `join_build_max_rows` cap is
    // checked against. Entries, not values: the map's memory is per entry
    // (spec §7, following `aggregate_max_groups`' argument).
    std::size_t rows() const noexcept { return entries_.size(); }

private:
    // One below the terminator: an entry at kNoEntry could not be linked.
    static constexpr std::size_t kMaxEntries = kNoEntry;
    // Slots in the first table. Large enough that a small map never
    // rehashes, small enough that a statement with a dozen keys is not
    // paying for a page of them.
    static constexpr std::size_t kInitialSlots = 64;

    // One distinct join-column value: the key itself (the Cabin's value
    // identity, unchanged), its hash kept so a growth rehashes without
    // re-hashing, and the head and tail of its walk-order chain.
    struct Key {
        stats::CabinKey key;
        std::uint64_t hash = 0;
        std::uint32_t head = kNoEntry;
        std::uint32_t tail = kNoEntry;
    };

    // Eight bytes, and the reason the table is its own thing rather than a
    // `std::unordered_map`: `tag` is the hash's high half, so a probe
    // rejects a wrong slot without touching the 48-byte key it names, and
    // a whole table fits in a fraction of the cache lines a node-per-key
    // map would touch. `key == kNoEntry` is empty; nothing is ever erased,
    // so there are no tombstones to reason about.
    struct Slot {
        std::uint32_t tag = 0;
        std::uint32_t key = kNoEntry;
    };

    static std::uint32_t TagOf(std::uint64_t hash) noexcept {
        // The high half, because the low bits already chose the slot - a
        // tag taken from them would agree with the index and filter
        // nothing.
        //
        // **The hash is used as `CabinKeyHash` produced it, unmixed**, and
        // that is measured rather than assumed. `CabinKeyHash` maps a
        // small integer key to a near-consecutive value, so consecutive
        // keys take consecutive slots: the fill writes the table
        // sequentially and collides almost never. A finalizing mix
        // (murmur3's) scatters them instead and measured *worse* on every
        // cell of the map micro-benchmark - 15.1 to 16.3 ns/row on the
        // 10,000-rows-over-2,000-keys shape, 22.9 to 43.8 on all-distinct
        // keys. The locality is worth more than the tag's filtering, and
        // a full key compare stands behind the tag in any case.
        return static_cast<std::uint32_t>(hash >> 32);
    }

    // The index of `key`'s record, inserting one if this is its first row.
    // Returns an index rather than a reference on purpose: an insertion
    // can grow `keys_`, and a reference handed back across that growth is
    // exactly the dangling the caller would never see fail.
    std::uint32_t KeyFor(const stats::CabinKey& key, std::uint64_t hash) {
        if (table_.empty()) Rehash(kInitialSlots);
        for (std::size_t i = hash & mask_;; i = (i + 1) & mask_) {
            Slot& slot = table_[i];
            if (slot.key == kNoEntry) {
                keys_.push_back(Key{key, hash, kNoEntry, kNoEntry});
                const auto k = static_cast<std::uint32_t>(keys_.size() - 1);
                slot.tag = TagOf(hash);
                slot.key = k;
                // Kept at or below three quarters full: linear probing
                // degrades sharply past that, and a build's table is
                // written once per row and read once per outer row.
                if (keys_.size() * 4 > table_.size() * 3) Rehash(table_.size() * 2);
                return k;
            }
            if (slot.tag == TagOf(hash) && keys_[slot.key].key == key) return slot.key;
        }
    }

    // Rebuilds the table at `slots` (a power of two). Only the 8-byte
    // slots move; the keys and every chain stay exactly where they are,
    // which is what makes growth cheap enough to start small.
    void Rehash(std::size_t slots) {
        table_.assign(slots, Slot{});
        mask_ = slots - 1;
        for (std::uint32_t k = 0; k < keys_.size(); ++k) {
            for (std::size_t i = keys_[k].hash & mask_;; i = (i + 1) & mask_) {
                if (table_[i].key == kNoEntry) {
                    table_[i] = Slot{TagOf(keys_[k].hash), k};
                    break;
                }
            }
        }
    }

    std::vector<stats::CabinEntry> entries_;
    std::vector<std::uint32_t> next_;
    std::vector<Key> keys_;
    std::vector<Slot> table_;
    std::size_t mask_ = 0;
};

}  // namespace kds::exec
