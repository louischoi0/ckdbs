#include "kds/exec/range_eligible.hpp"

namespace kds::exec {

std::string_view RangeGateName(RangeGate gate) noexcept {
    switch (gate) {
        case RangeGate::kNone: return "eligible";
        case RangeGate::kBtree: return "btree-clustered";
        case RangeGate::kIndex: return "indexed";
        case RangeGate::kCabin: return "cabined";
        case RangeGate::kSpill: return "spilling-schema";
        case RangeGate::kForeignKey: return "fk-linked";
        case RangeGate::kAssertion: return "asserted";
    }
    // Unreachable - the switch is exhaustive over the enum. Deliberately
    // *not* "eligible": this names a decline, so a value from outside the
    // enum must not read as permission to split.
    return "unknown-gate";
}

RangeGate RangeEligible(const catalog::TableAccess& access,
                        const AssertionEnforcer& enforcer) noexcept {
    // D1 first: heap only, inherited not chosen (workplan §1) — a btree
    // range's top levels belong to whoever owns the root, and that hop is
    // the shared-structure access mechanism, still [OPEN] (CC8).
    if (access.clustered_type == catalog::ClusteredType::kBtree) {
        return RangeGate::kBtree;
    }
    // **`kIndex` and `kCabin` were here and are gone** (SB3,
    // `instructions/v2.7.1/workorder-sb.md`), and the two were dropped for
    // opposite reasons - which is why both are stated here rather than
    // summarised as one.
    //
    // The **index** arm tested `!access.indexes.empty()` and was **dead
    // code**: a secondary index is btree-only (IX3,
    // `Catalog::CheckIndexDef`), so every relation that could trip it was
    // already declined by `kBtree` directly above. Dropping it is
    // behaviour-preserving for *that* reason and not because §6a's
    // question was answered - it was not. **The arm is owed again the day
    // D1 lifts**, not at IX11: the day a btree relation may split, an
    // indexed one must not, until `index.md` §13 decides per-range vs
    // global. SA-R2 narrows the *re-added* arm to UNIQUE-indexed, which
    // additionally waits on IX11's `unique` flag - a second condition on
    // the arm's shape, never the trigger for its return.
    //
    // The **Cabin** arm was answered rather than deferred: an
    // Observational set is authoritative for (observed value × the ranges
    // its core owns), so a boundary narrows a set instead of falsifying
    // it, and the sets banked under the older claim are dropped in CC10's
    // pre-grant window (`range_alloc.cpp`). What survives of the Cabin
    // question is the **Bound** class, and `kAssertion` below is where it
    // is asked.

    // §6a's own words: invariant 13 makes every relation fixed-length, so
    // the *spill* is the gate — one kVarHeap page may hold values
    // referenced from both sides of a boundary, and a core faults only
    // pages it owns.
    if (catalog::SchemaCanSpill(access.schema)) {
        return RangeGate::kSpill;
    }
    // Both directions: a child's forward check reads the parent, a
    // parent's reverse check reads the child, and either read crosses
    // cores once the relation splits.
    if (!access.fkeys_out.empty() || !access.fkeys_in.empty()) {
        return RangeGate::kForeignKey;
    }
    // C2's fifth gate (§9): live *or* known-and-unenforceable. Both mean
    // "a Bound Cabin exists whose chain is one core's" — the live one
    // this owner appends to, the unenforceable one a pre-PW1c-6c file's
    // core-0 chain — and either way a split puts a writer on a core the
    // cabin cannot follow.
    if (enforcer.AnyOn(access.oid) || enforcer.CannotEnforce(access.oid)) {
        return RangeGate::kAssertion;
    }
    return RangeGate::kNone;
}

}  // namespace kds::exec
