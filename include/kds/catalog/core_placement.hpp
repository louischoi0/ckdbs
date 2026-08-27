#pragma once

#include <cstdint>

// Which core owns a newly created relation (docs/inflight/in-progress/workplan-crosscore.md M1).
//
// ---- Why this is a free function and not a Catalog method ---------------
//
// The catalog *records* ownership; it does not decide it. Keeping the
// decision here means the placement policy can be replaced - by the
// Waystone/pattern-driven placement crosscore.md section 9 leaves open, by
// an operator-declared placement, by anything - without touching a catalog
// that would otherwise have grown a reason to know how many cores exist.
// `sys.tables.owner_core` is the interface between the two, and it is the
// only one: workplan guideline 4 forbids deriving ownership from page ids,
// hashes, or topology anywhere else.
//
// ---- The policy, and its status ----------------------------------------
//
// **Round-robin over the non-system cores. `[PROPOSED]`, not settled.**
// M1 proposes it and nothing in the engine may depend on it: what the
// engine depends on is that ownership is *recorded*, and any assignment
// satisfying that is correct, only differently fast. crosscore.md section 9
// states the position outright - placement is an optimization concern and
// cross-core execution is the correctness path regardless of placement.
//
// Co-location is not expressed here because it cannot be spelled wrong: a
// relation's unique indexes, Cabin, Waystone pages and var-heap hang off
// its own catalog row and have no owner of their own (rows.hpp). What M1's
// co-location rule will need a say in, when it exists, is FK-linked
// relations - and `docs/spec/foreign-keys.md` keeps those co-located in v1
// by deferring cross-core FK entirely, so there is nothing to encode yet.

namespace kds::catalog {

// Core 0 owns the superblock, the free map, file growth, extent leasing and
// the catalog pages (M5). It is therefore excluded from user-relation
// placement whenever there is anywhere else to put one - a system core that
// also serves the busiest relation is the one core whose queue everybody
// waits behind.
inline constexpr std::uint32_t kSystemCore = 0;

// The owner for a user relation created on `creating_core`.
//
// ---- The invariant this has to satisfy ----------------------------------
//
// **A relation's owner must be the core that allocates its pages.**
// Ownership is two facts that have to agree: `sys.tables.owner_core` says
// which core may run statements against a relation, and a page belongs to
// whichever core's lease it came from (storage/extent_lease.hpp). A
// relation owned by a core that cannot fault its own pages is not a
// placement, it is an unreachable relation.
//
// Today DDL runs on the system core and allocates from the system core's
// free map, so **the answer is always the creating core**. The round-robin
// M1 proposes is written out below rather than performed, because the thing
// that would make it correct does not exist yet:
//
//     if (core_count > 1) {
//         return kSystemCore + 1 + (relation_seq % (core_count - 1));
//     }
//
// Enabling that needs CREATE TABLE to allocate the relation's root - and
// every page it later grows into - from the *owner's* lease. Either DDL
// gains a cross-core allocation, or core 0 reserves an extent and hands it
// to the owner before the relation is visible. Both are real designs;
// neither is built.
//
// ---- How this was found -------------------------------------------------
//
// The rotation *was* performed, from P0 until the affinity guard existed.
// Nothing detected it, because core 0 allocated and faulted the pages
// regardless and no code compared the two facts. The moment
// `CheckReadAffinity` started asking, every statement on a two-core
// instance failed: placement said core 1, execution ran on core 0. The
// guard was right and the placement was wrong.
//
// `relation_seq` is kept in the signature for the rotation above, and is
// deliberately not the oid: oids restart at kUserOidStart every boot
// (docs/rules/keystoneid-k0-findings.md), so a placement keyed on one would
// re-walk the same rotation after every restart.
// Which placement rule CreateTable applies (workplan P6c, config key
// `placement`). `kCreatingCore` is the default and the mode a
// statement-serving instance should run until the step pipeline is
// finished: rotation places relations on cores that CAN fault their pages
// (CC7's flush-then-grant handoff, P6b) and, since P4a-P4c (2026-08-10),
// CAN serve exactly one statement shape - a single-step star SELECT with
// no aggregate, no quota, no sub-chain and a descriptor-encodable step,
// opened on the owning core and streamed back under credit. Every other
// shape still meets core 0's retryable affinity refusal, so rotation is
// one shape wide rather than useless. `kRotate` stays an exercise mode
// and becomes the real policy when P4d wires multi-step pipelines and
// converts the executor.
enum class PlacementPolicy : std::uint8_t { kCreatingCore, kRotate };

constexpr std::uint32_t AssignOwnerCore(PlacementPolicy policy, std::uint32_t creating_core,
                                        std::uint32_t core_count,
                                        std::uint64_t relation_seq) noexcept {
    if (policy == PlacementPolicy::kRotate && core_count > 1) {
        // M1's rotation over the non-system cores. Legal since CC7/P6b:
        // ownership follows the catalog, and the publish handoff grants the
        // owner fault rights over pages the system core allocated.
        return kSystemCore + 1 + static_cast<std::uint32_t>(relation_seq % (core_count - 1));
    }
    return creating_core;
}

}  // namespace kds::catalog
