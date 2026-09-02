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
// `placement`).
//
// **`kCreatingCore` is the ratified default** - DA2, 2026-08-31,
// `instructions/v2.7.0/ratification-da.md` - and the ground is a
// measurement rather than an unfinished pipeline. The pipeline argument
// this comment used to carry is spent: P4d wired multi-step pipelines,
// statement shipping (SS2) carries an autocommit single-relation statement
// to its owner, and R4-R/RS gave a spread relation a read surface from
// every core, so rotation is no longer one shape wide. What decides it now
// is `bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md` §6:
// **rotation's crossover is a step at the first core to take a second
// session, and past it rotation is negative at seven writer cores
// (0.51x)**.
//
// `kRotate` is **not** deleted and not an exercise mode either - it stays a
// configurable placement, and §6a's gates are unchanged. DA2 settles which
// one ships, not which ones exist.
//
// **`kNamespace` is the shipped default since AF-T2** (2026-09-02,
// `instructions/v2.8.0/ratification-af-namespace.md`), and it does **not**
// reverse DA2: for a relation in `public` - which is every relation until
// somebody writes `CREATE NAMESPACE` - it answers exactly what
// `kCreatingCore` answers, because an undeclared namespace is never
// rotated. What it adds is the case DA2 could not price: a group of
// relations the *user* said belong together. DA2's 0.51x is rotation spent
// blindly - it splits the pair that joins every second as readily as the
// pair that never meets - and `kNamespace` is the same rotation with the
// grouping supplied, so a join inside a group never crosses.
//
// `kCreatingCore` stays selectable, and is the right setting for a file
// whose relations were placed by some other history.
enum class PlacementPolicy : std::uint8_t { kCreatingCore, kRotate, kNamespace };

// "This namespace has no core yet." Not a core id: `kSystemCore` is a legal
// answer, so absence needs a value no core count can reach.
inline constexpr std::uint32_t kUnplacedNamespace = 0xFFFFFFFFu;

// What the catalog could **read off its own rows** about the namespace a
// relation is being created in. Every field is an observation and none of
// them is a decision, which is what keeps the policy in this file and the
// recording in the catalog - the split this header opens with.
struct NamespacePlacement {
    // The `owner_core` of the lowest-oid relation already in this
    // namespace, or `kUnplacedNamespace` when it holds none.
    //
    // **This is AF-P1**: the namespace's core is *derived* from rows that
    // exist rather than cached in a field that could drift from them, and
    // the lowest oid is the "first relation" the operator's sentence names.
    // It is also what makes a file written before AF mount unchanged - the
    // relations in a namespace answer for it.
    std::uint32_t settled_core = kUnplacedNamespace;

    // How many namespace rows precede this one on `sys.objects`, dropped
    // ones counted. The rotation input for a namespace no relation has
    // fixed yet.
    //
    // **Why a rank and not the creating core.** AF-P1 as first written said
    // an unfixed namespace takes the creating core; DDL runs on core 0 and
    // only on core 0 (`CreateTable` passes `kSystemCore` for it), so that
    // rule would place *every* namespace on core 0 and the policy would do
    // nothing at all. AF-4 requires the opposite - "AF is rotation with the
    // grouping supplied" - so the missing half is supplied here: a
    // namespace nobody has placed rotates on its declaration order. Dropped
    // rows count precisely because they are never retired
    // (`well_known.hpp`'s `kTypeDroppedNamespace`), which makes the rank
    // immutable and satisfies AF-P4 - emptying a namespace does not free it
    // to move.
    std::uint64_t rank = 0;

    // Whether anybody declared this namespace - `oid >= kUserOidStart`, the
    // oid-range idiom `mount_recovery.cpp` and `relayout_planner.cpp` use to
    // ask "is this the catalog's". False for `sys` and `public`, which is
    // what keeps DA2's answer for a relation whose writer named no
    // namespace.
    bool declared = false;
};

constexpr std::uint32_t AssignOwnerCore(PlacementPolicy policy, std::uint32_t creating_core,
                                        std::uint32_t core_count, std::uint64_t relation_seq,
                                        NamespacePlacement ns = {}) noexcept {
    if (policy == PlacementPolicy::kNamespace) {
        // An undeclared namespace is DA2's case and takes DA2's answer.
        if (!ns.declared) return creating_core;
        // A namespace its first relation already fixed. Never rebalanced
        // (AF-P4): ownership that changed without DDL is exactly what
        // `catalog_cache.hpp` forbids caching, and `TableAccess` caches it.
        if (ns.settled_core != kUnplacedNamespace) return ns.settled_core;
        // This relation is the one that fixes it.
        if (core_count > 1) {
            return kSystemCore + 1 + static_cast<std::uint32_t>(ns.rank % (core_count - 1));
        }
        return creating_core;
    }
    if (policy == PlacementPolicy::kRotate && core_count > 1) {
        // M1's rotation over the non-system cores. Legal since CC7/P6b:
        // ownership follows the catalog, and the publish handoff grants the
        // owner fault rights over pages the system core allocated.
        return kSystemCore + 1 + static_cast<std::uint32_t>(relation_seq % (core_count - 1));
    }
    return creating_core;
}

}  // namespace kds::catalog
