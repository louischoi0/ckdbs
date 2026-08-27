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
// function *returns* the gate's name; where that name surfaces (a
// counter, a SHOW field, a log line) is C3's decision (order §3, RA4) and
// deliberately not taken here — a surface added now would read
// structurally 0 until RD5 exists.
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
// facts every core caches identically, but the `AssertionEnforcer` is
// core-local and only the owner's registry holds the relation's live
// directory (PW1c-6c — the owner builds and holds the Bound Cabin) or
// its unenforceable record. RD5 allocates on the owner (§6b: a range
// opens where the owner's lease carves its id block), so the authority
// and the caller coincide by construction.
//
// **Scope, and a hole RD5 must close somewhere: a *catalog* relation
// answers `kNone` here.** None of §6a's five facts is true of
// `sys.tables` — heap-clustered, fixed-width, unindexed, un-cabined,
// FK-free, un-asserted — so every gate below passes it, and yet it is
// categorically unsplittable: its pages are core 0's by construction
// (`core_runtime.hpp`'s peer rules 1-2), its chain head is a
// compile-time `kCatalogPage*` constant rather than a directory row
// (so RD6's per-range heads do not apply), and CC9 puts the directory
// itself in the catalog. Nor is one unreachable by construction:
// `Catalog::FindTableOidByName` does not filter on namespace, so a
// `TableAccess` for a system relation is constructible through the
// ordinary door. §6a lists no such gate, so **this function does not
// invent one** — but whichever of §6a or RD5 takes the scope must take
// it explicitly; the engine's existing idiom for the question is
// `namespace_oid != catalog::kNamespacePublic` (AL7, DT3's drop and
// rename refusals). Reported by the RD4 review, 2026-08-27.
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
enum class RangeGate : std::uint8_t {
    kNone = 0,
    kBtree,       // D1: the shared-structure access mechanism is [OPEN]
    kIndex,       // §6a: per-range vs global secondary index undecided
    kCabin,       // §6a: entry sets core-resident; miss path does not self-heal
    kSpill,       // §6a: var-heap partition under a boundary undesigned
    kForeignKey,  // §6a: validation reads the linked relation
    kAssertion,   // §6a fifth gate (C2): one core's chain, one core's registry
};

// The gate's name for the decline's log line — short, stable tokens a
// log reader can grep. kNone answers "eligible".
std::string_view RangeGateName(RangeGate gate) noexcept;

RangeGate RangeEligible(const catalog::TableAccess& access,
                        const AssertionEnforcer& enforcer) noexcept;

}  // namespace kds::exec
