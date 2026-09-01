#pragma once

#include <cstdint>
#include <string_view>

#include "kds/catalog/schema.hpp"
#include "kds/exec/assertion_check.hpp"

// RD4: the range allocator's admission check (docs/spec/crosscore.md §6a;
// docs/inflight/in-progress/workplan-range-directory.md §9).
//
// Whether a relation may take a second range. §6a's conservative rule:
// every write-coupled auxiliary whose placement under a split is
// undecided gates the split, and the gates are lifted by the owning
// decisions, never by relaxing the decline. There is no user-facing range
// DDL, so a decline is a logged engine decision naming the gate — no
// statement asked, so no offending token and no byte position. This
// function *returns* the gate's name; where that name surfaces was
// decided at RA4 (workplan §9e): the caller's log line plus per-core
// `range_split_declines` counters on `SHOW META` in `crosscore.md` §6's
// refusal-counter form, both landing with RD5's first caller —
// deliberately not landed here, because a surface with no caller reads
// structurally 0 until RD5 exists. The increment is the caller's, never
// this function's: pure over its arguments, per the concurrency note
// below.
//
// **No caller until RD5, deliberately** (H2, order §4): the allocator
// that would ask does not exist yet, and this function landing first is
// what guarantees nothing can open a second range unasked. If a statement
// path grows a call before RD5, that is RD5's shape leaking and the
// workplan's §2b decision being taken by accident — stop and report.
//
// Concurrency: a pure function over its two arguments — no state, no
// locks, nothing retained. The answer is **authoritative on the
// relation's owner core only**: the four `TableAccess` gates are catalog
// facts every core caches from the same core-0-written rows — a peer's
// copy can be behind, which is the two windows named below — and the
// `AssertionEnforcer` is core-local, so only the owner's registry holds
// the relation's live directory (PW1c-6c — the owner builds and holds
// the Bound Cabin) or its unenforceable record. RD5 allocates on the
// owner (§6b: a range opens where the owner's lease carves its id
// block), so the authority and the caller coincide by construction.
//
// **Scope: a *catalog* relation answers `kNone` here.** Every gate
// passes `sys.tables`, yet a catalog relation is categorically
// unsplittable — and §6a lists no such gate, so this function does not
// invent one. The hole's evidence and the idiom that closes it
// (`namespace_oid != catalog::kNamespacePublic`) are
// `workplan-range-directory.md` §9b's, inherited by RD5 with the
// admission windows below; the test pins the current answer.
//
// **That spelling is the *gate's*, deliberately, and not the identity
// question's** (AF-P3, 2026-09-01): it over-declines an unknown namespace
// on purpose, where a rename or a drop asking the same words would refuse
// a user relation as a system one. `catalog::IsSystemNamespace` is the
// identity spelling; `range_alloc.cpp` carries why this one diverges.
//
// Two admission windows this function cannot see, named so RD5 closes
// them rather than discovers them (§9's enumeration): an index build in
// flight (`PendingIndexBuilds::Covers`) and an assertion between core 0's
// catalog half and the owner's adoption. Both are races against a core-0
// catalog write, and the range row is itself a core-0 catalog write
// (CC10 step 3), so core 0's single stream is the serialization point —
// RD5's obligation, not a field here.

namespace kds::exec {

// The gate that declined, or kNone when the relation is eligible. Checked
// in a fixed, documented order (D1's btree decline first, because it
// removes the whole question; then §6a's own listing order — index,
// cabin, var-heap spill, foreign keys; then assertions, C2's addition
// last as the youngest gate). A relation tripping several gates names the
// first: the decline needs one true reason, and a fixed order keeps the
// named gate deterministic for the log line that will eventually carry it.
//
// **Two of the seven no longer occur** (SB3, 2026-09-01): `kIndex` and
// `kCabin`. Their values stay — the enum is a counter's detail key, and
// renumbering would silently re-label a series that already exists — and
// the argument for each removal, with what makes it owed again, is at the
// removal site in `range_eligible.cpp` rather than here. One statement,
// one home: three copies of it is three chances to drift, which is what
// the first draft of this comment did.
enum class RangeGate : std::uint8_t {
    kNone = 0,
    kBtree,       // D1: the shared-structure access mechanism is [OPEN]
    kIndex,       // never returned since SB3; owed again at D1 — range_eligible.cpp
    kCabin,       // never returned since SB3; answered, not deferred — range_eligible.cpp
    kSpill,       // §6a: var-heap partition under a boundary undesigned
    kForeignKey,  // §6a: validation reads the linked relation
    kAssertion,   // §6a fifth gate (C2): one core's chain, one core's registry
};

// The gate's name for the decline's log line and for the counter's
// detail key (RA4, workplan §9e) — short, stable tokens a log reader can
// grep, and none carries ':' or ',', so `oid:gate=count` parses. kNone
// answers "eligible".
std::string_view RangeGateName(RangeGate gate) noexcept;

RangeGate RangeEligible(const catalog::TableAccess& access,
                        const AssertionEnforcer& enforcer) noexcept;

}  // namespace kds::exec
