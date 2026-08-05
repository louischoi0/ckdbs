#pragma once

#include <cstdint>

// Which core owns a newly created relation (docs/workplan-crosscore.md M1).
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
// relations - and `docs/impl-foreign-keys.md` keeps those co-located in v1
// by deferring cross-core FK entirely, so there is nothing to encode yet.

namespace kds::catalog {

// Core 0 owns the superblock, the free map, file growth, extent leasing and
// the catalog pages (M5). It is therefore excluded from user-relation
// placement whenever there is anywhere else to put one - a system core that
// also serves the busiest relation is the one core whose queue everybody
// waits behind.
inline constexpr std::uint32_t kSystemCore = 0;

// The owner for the `relation_seq`-th user relation created on an instance
// running `core_count` cores.
//
// `relation_seq` is any counter that advances once per created relation;
// `Catalog::relations_created()` supplies it. It is deliberately not the
// oid: oids restart at kUserOidStart every boot
// (docs/keystoneid-k0-findings.md), so a placement keyed on one would
// re-walk the same rotation after every restart and pile the first few
// relations of each session onto the same core.
//
// A single-core instance answers 0 for everything - there is nowhere else -
// which is what makes this callable unconditionally on the CREATE TABLE
// path rather than guarded by a core-count test.
constexpr std::uint32_t AssignOwnerCore(std::uint32_t core_count,
                                        std::uint64_t relation_seq) noexcept {
    if (core_count <= 1) return kSystemCore;
    const std::uint32_t user_cores = core_count - 1;
    return kSystemCore + 1 + static_cast<std::uint32_t>(relation_seq % user_cores);
}

}  // namespace kds::catalog
