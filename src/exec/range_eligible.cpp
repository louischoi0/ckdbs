#include "kds/exec/range_eligible.hpp"

#include <algorithm>

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
    if (!access.indexes.empty()) {
        return RangeGate::kIndex;
    }
    // The live-id test, the same shape CheckWriteAffinity's whitelist uses
    // (command_dispatcher.cpp) and for the same reason: `cabin_ids` is
    // per-column-parallel with id 0 meaning "no Cabin", so emptiness is
    // the wrong test, and so is `cabin_mask != 0` — a Cabin on a column
    // past 64 folds into no bit. Not factored with that site: this order
    // must not touch CheckWriteAffinity's cross-core arm (range-foundation
    // §1's out-list), so unification waits for whichever of RD5/R6-8
    // rewrites that decision point.
    if (std::any_of(access.cabin_ids.begin(), access.cabin_ids.end(),
                    [](const catalog::TableAccess::CabinRef& c) { return c.id != 0; })) {
        return RangeGate::kCabin;
    }
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
