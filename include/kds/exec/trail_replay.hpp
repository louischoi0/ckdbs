#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>

#include "kds/base/common.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/stats/waystone.hpp"

// A recorded trail, indexed for replay (docs/inflight/in-progress/waystone-workplan.md P11/P13).
//
// The read side of what `exec::TrailCollector` writes. Built once when a
// statement opens, consulted once per keyed step, and thrown away with the
// statement.
//
// ---- What replay is, stated so it cannot drift ---------------------------
//
// **A trail supplies a location. It never supplies a row.** A hit hands
// `(page_id, slot)` to exactly the same `AcceptTupleAt()` that a btree
// descent or a chain walk feeds, so from the tuple onward - decode,
// residual, sub-chains, projection, the next step - a replay hit and a
// replay miss run the same code on the same bytes.
//
// That is the whole correctness argument, and it is not a new one: it is
// what the probe memo already rests on (step_vm.cpp, "it caches only the
// *location* … so a memo hit and a memo miss run the same code from the
// tuple onward"). Replay is that memo sourced from a persisted trail
// instead of the last descent, and it inherits the reasoning.
//
// ---- Rule 0 is the lookup key, deliberately ------------------------------
//
// `docs/inflight/in-progress/waystone-workplan.md` P13's amendment adds a rule the storage-level
// checks cannot supply: **re-derive the probe key from the current outer row
// and require it to equal the entry's `pk`**. Every rule in spec section 2
// validates the trail against *storage*, and none looks at the query - so if
// a driving row's join column is updated between recording and replay, an
// entry for the old key passes `rel_oid`, the Keystone id, the epoch and
// visibility, and the join emits the wrong row.
//
// Here that rule is **structural**: the index is keyed on `(step_id, pk)`,
// and the caller's lookup key is the key its step just computed from the
// current outer row. An entry can only be found by matching it. There is no
// separate check to forget.
//
// ---- What this deliberately cannot check ---------------------------------
//
// **The page epoch is no longer on this list** (2026-08-09, workplan PX04):
// entries carry the epoch the recorder observed, `TrailLocation` carries it
// to the consult site, and `exec/tuple_verify.hpp` compares it against the
// page's current epoch - spec section 2's rule 2, enforceable at last.
// Until a mover exists every comparison is between two zeros; the
// hand-bumped-epoch contract test is what proves the check would fire.
//
// **That a page still belongs to the relation it was recorded from.**
// `Build()` checks `entry.rel_oid` against the step's, which is a check
// against the *query*, not against storage: nothing asks the page which
// relation owns it, because nothing can. Sufficient only while pages are
// never freed and reallocated between relations - i.e. until `DROP TABLE`
// or page reuse exists.
//
// Concurrency: built and read on one core, immutable once built.

namespace kds::exec {

// Where a recorded execution found a tuple. Advisory: every field is
// re-validated against the page before anything is read from it.
struct TrailLocation {
    PageId page_id = kInvalidPageId;
    // The page's relayout epoch as the entry recorded it, carried so the
    // consult site can hand it to the verifier (spec section 2 rule 2).
    std::uint32_t page_epoch = 0;
    std::uint16_t slot = 0;
};

class TrailReplay {
public:
    TrailReplay() = default;

    // Indexes `entries` against `chain`, into `this`, reusing whatever
    // capacity it already has.
    //
    // A member rather than a factory so a caller can keep one across
    // statements: the map is the only allocation on this path, and building
    // a fresh one per SELECT is a malloc per query for what is usually one
    // or two entries. `Clear()` keeps the buckets.
    //
    // Two kinds of entry are dropped rather than indexed, so a consultation
    // can never reach one:
    //
    //   - an entry whose `rel_oid` disagrees with the step it names. That is
    //     spec section 2 rule 1's relation half, checked **once here**
    //     rather than per row - and it is what stops a trail recorded
    //     against one relation from being read with another's schema.
    //   - an entry for a step that is not `IsTrailReplayable`. The recorder
    //     never writes one, so this is belt and braces; it is cheap, and it
    //     means a trail written by some future recorder with a different
    //     policy cannot make a search look replayable.
    //
    // An entry naming a step id the chain does not have is dropped too: a
    // trail outlives the statement text only as far as the fingerprint says
    // it does, and a chain that changed shape under one pattern_id is a
    // collision, not a trail.
    void Build(const StepChain& chain, std::span<const stats::WaystoneEntry> entries);

    // Drops every indexed entry, keeping the allocation.
    void Clear() noexcept { by_key_.clear(); }

    // The recorded location for `pk` at `step_id`, or null.
    //
    // Null is the ordinary answer and never an error: an instance nobody
    // recorded, an outer row whose key was not in the recorded set, and a
    // trail whose entry was dropped at build all arrive here identically,
    // because the caller does the same thing in all of them - descend.
    const TrailLocation* Find(std::uint32_t step_id, std::uint64_t pk) const noexcept;

    bool empty() const noexcept { return by_key_.empty(); }
    std::size_t size() const noexcept { return by_key_.size(); }

private:
    // `(step_id, pk)` packed into one key. The pk is a 40-bit Keystone id
    // (invariant 5) and step ids are 16-bit in the entry format, so the
    // pair fits a uint64 with room to spare - which keeps the map's hash a
    // single integer rather than a pair.
    static constexpr std::uint64_t PackKey(std::uint32_t step_id, std::uint64_t pk) noexcept {
        return (static_cast<std::uint64_t>(step_id) << 40) | (pk & 0xFF'FFFF'FFFFull);
    }

    std::unordered_map<std::uint64_t, TrailLocation> by_key_;
};

}  // namespace kds::exec
