#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"

// In-memory cache of the facts Catalog otherwise re-derives from the
// catalog pages on every statement. Each of those derivations is a full
// page scan that heap-allocates a vector of every live row on the page and
// memcpy-decodes each one (ScanAll() in catalog.cpp); BuildSchemaFromColumns
// decodes every column row of *every* relation before filtering by rel_id.
// Measured server-side, that work is the dominant cost of a statement: a
// DESCRIBE (3 scans + one type lookup per column) costs as much as a whole
// INSERT.
//
// What may live here is decided by one question: can the fact change
// without DDL? If yes, it is not cacheable.
//
//   cached, dropped on the DDL version bump:
//     name -> oid                    (sys.objects)
//     oid  -> TableAccess            (sys.tables + sys.columns)
//     the table list                 (sys.objects)
//     pattern_id -> PatternAccess    (sys.patterns)
//   cached, never dropped:
//     sys.types              written only by Catalog::Bootstrap()
//   never cached, always read from the page:
//     next_id, and therefore GetSysTableRow()/AllocateRowId()
//     a pattern's use_count/last_seen, and therefore GetSysPatternRow()
//
// That last line is load-bearing twice over. AllocateRowId() bumps a field
// of the same sys.tables row a TableAccess is built from, and it runs
// *while* a statement holds a cached TableAccess (command_dispatcher.cpp's
// HandleInsert). TableAccess deliberately carries no next_id (schema.hpp),
// so the sequence bump cannot stale it - but a cache that invalidated on
// "the sys.tables page changed" would free an entry out from under a live
// pointer. The sequence is therefore not a cached fact at all, and
// AllocateRowId() bumps no version.
//
// Concurrency: core-local, no internal synchronization (rules.md #3). One
// Catalog owns one cache; DDL cannot interleave with a statement because
// the reactor is single-threaded (sched/scheduler.hpp). Two Catalog
// instances over one PageStore therefore each cache independently, and
// neither observes the other's DDL - a pattern tests use, and one that is
// only safe while a single instance owns DDL.
//
// Not here, deliberately:
//   - No eviction and no size bound. Entries scale with the number of
//     relations, not the number of pages, and there is no DROP TABLE to
//     retire one. A bound would be a policy invented for a population of
//     tens (compare device_page_store.hpp, which says the same about
//     frames for a different reason).
//   - No negative caching. A lookup for a name that does not exist is an
//     error path, not a hot one, and caching absence buys a second
//     invalidation rule to get wrong.
//   - No TTL, so no clock dependency (rules.md #4). Staleness here is a
//     function of DDL, never of time.
//   - No per-relation invalidation. Clearing costs one re-scan of a page
//     the next statement was going to touch anyway; the DDL choke point is
//     one call, which is what keeps finer-grained invalidation (parser.md
//     I5 / PR20's statement stamps) implementable without re-plumbing.

namespace kds::catalog {

class CatalogCache {
public:
    // Counters ship with the feature rather than after it (wal.md section 13's
    // convention). `fills` counts entries added, which is also the number of
    // page scans the cache could not avoid.
    struct Stats {
        std::uint64_t hits = 0;           // lookups served from memory
        std::uint64_t misses = 0;         // lookups that had to scan a page
        std::uint64_t fills = 0;          // entries inserted after a miss
        std::uint64_t invalidations = 0;  // DDL-driven Invalidate() calls
    };

    // ---- name -> oid (sys.objects) ---------------------------------------

    // nullptr on a miss. The returned pointer is valid until the next
    // Invalidate().
    const Oid* FindOidByName(std::string_view name) noexcept;

    // Copies `name` into the cache: a key that outlives the request must
    // not borrow the request's buffer (parser.md I4's copy-at-the-boundary
    // rule). Already-present keys are left alone, so a pointer handed out
    // earlier keeps pointing at the same value.
    void PutOidByName(std::string_view name, Oid oid);

    // ---- oid -> TableAccess (sys.tables + sys.columns) -------------------

    const TableAccess* FindTableAccess(Oid oid) noexcept;

    // Returns the cached entry, which is reference-stable: the map is
    // node-based, so growing it never moves an existing TableAccess. That
    // is what lets a statement hold the pointer across its own work. An
    // entry already present wins over `access`, for the same reason.
    const TableAccess* PutTableAccess(TableAccess access);

    // ---- pattern_id -> PatternAccess (sys.patterns) ----------------------
    //
    // Keyed by the fingerprint rather than by oid, because that is what
    // every caller arrives holding (rows.hpp's note on SysPatternRow).
    // What may be cached is decided by this header's one question, and the
    // answer is written into PatternAccess itself (schema.hpp): heat moves
    // without DDL, so it is not in the struct and cannot be cached by
    // accident.

    const PatternAccess* FindPattern(std::uint64_t pattern_id) noexcept;

    // Reference-stable and already-present-wins, exactly like
    // PutTableAccess() and for the same reason.
    const PatternAccess* PutPattern(PatternAccess access);

    // Points a cached pattern at a new directory, in place.
    //
    // An *update* rather than an invalidation, which is the one place this
    // cache departs from "drop everything at one choke point" - so the
    // reason is worth stating. The fact being changed belongs to exactly
    // one pattern and is read by nothing else, so dropping every cached
    // relation to publish it would be collateral damage; and it is
    // precisely that collateral damage, in the deleted per-relation
    // Waystone, that dangled the `const TableAccess*` a running INSERT was
    // holding. Updating in place keeps the entry's address, so a caller
    // holding the pointer sees the new root and keeps a valid pointer.
    //
    // A no-op when the pattern is not cached: there is nothing stale to
    // fix, and filling the entry here would cache a fact the caller may
    // never ask for.
    void UpdatePatternWaystone(std::uint64_t pattern_id, PageId root,
                               std::uint8_t depth) noexcept;

    // The same in-place update for a pattern's lifecycle policy, when
    // CREATE PATTERN adopts an auto-registered row. Identical argument, and
    // the same no-op on a miss: origin and pinning belong to one pattern
    // and are read by nothing else, so a global drop would dangle every
    // other held pointer for nothing.
    void UpdatePatternOrigin(std::uint64_t pattern_id, std::uint8_t origin,
                             std::uint16_t flags) noexcept;

    // Points a cached relation's index at a new root page, in place.
    //
    // **The third in-place update, and the one that had to exist.** An index
    // root moves when a split grows the tree, which happens inside an
    // ordinary INSERT - so a global drop here would dangle the
    // `const TableAccess*` the running statement is holding, and a
    // multi-row UPDATE would be holding it across every later row. That is
    // precisely the collateral damage the deleted per-relation Waystone
    // caused, and the reason the two updates above exist.
    //
    // The fact qualifies by the same test they do: a root belongs to one
    // index and is read by nothing else, so dropping every cached relation
    // to publish it would be damage for nothing.
    //
    // A no-op when the relation is not cached: there is nothing stale to
    // fix, and filling the entry here would cache a fact the caller may
    // never ask for.
    void UpdateIndexRoot(Oid rel_oid, Oid index_oid, PageId root) noexcept;

    // The clustered root's twin (PW2-4): a level-grow inside an ordinary
    // INSERT moves desc_page_id, and dropping the whole cache for it - the
    // pre-anchor arrangement - destroyed the entry the running statement
    // was holding. Same in-place license, same one-field/one-owner test.
    void UpdateDescPage(Oid rel_oid, PageId root) noexcept;

    // The fifth, and the one that is not a page id (heap-and-tuple.md §4.1):
    // a relation turns kUnordered the first time `AdmitExplicitRowId` admits
    // an id below its high-water mark, which happens **inside an ordinary
    // INSERT** - so a global drop here dangles the `const TableAccess*` the
    // running statement holds, exactly as the four above would. Not a
    // hypothetical: the first form of the flip did call `BumpVersion`, and
    // it re-derived the pk from the freed body vector, so a second insert on
    // one relation answered "tuple's Keystone id N does not match the id
    // being inserted".
    //
    // Same one-field/one-owner test: the flag belongs to one relation and is
    // read by nothing else. One-way, which is why there is no value
    // parameter - kAscending never comes back, and a setter that could write
    // it would be a way to lose the fact.
    //
    // A no-op when the relation is not cached, like its four neighbours.
    void MarkKeysUnordered(Oid rel_oid) noexcept;

    // ---- sys.types (bootstrap-immutable) --------------------------------

    // nullptr means "not loaded yet, scan the page"; a non-null empty
    // vector would mean a database whose sys.types is genuinely empty. The
    // cache stores the rows and does not match against them: type name
    // matching is case-insensitive and type_val matching is not, and both
    // comparisons belong with the callers that have always owned them
    // (Catalog::ResolveTypeByName / ResolveTypeByVal) rather than being
    // reimplemented here.
    //
    // Types survive Invalidate(): only Bootstrap() ever writes sys.types,
    // and it runs once against a database that has no types yet.
    const std::vector<SysTypeRow>* FindTypes() noexcept;

    // Returns the stored snapshot, so a caller that just filled it does not
    // have to look it back up (and does not record a second hit for one
    // logical lookup). An already-loaded snapshot wins.
    const std::vector<SysTypeRow>* PutTypes(std::vector<SysTypeRow> types);

    // Drops the type snapshot. Invalidate() does not, so this is the one
    // way sys.types staleness is expressible - it exists so the "only
    // Bootstrap() writes sys.types" assumption is *enforced* by the one
    // writer calling it, rather than only asserted in a comment.
    void InvalidateTypes() noexcept;

    // ---- the table list (sys.objects) -----------------------------------

    // The set of relations. Unlike the per-relation facts this is a
    // *completeness* claim over a table that DDL appends to, so it is only
    // sound because every insert into sys.objects goes through
    // Catalog::BumpVersion(). A relation created through a different
    // Catalog over the same store is not reflected until this one
    // invalidates - the same instance-scoped limit the header states above.
    const std::vector<SysObjectRow>* FindTableList() noexcept;
    const std::vector<SysObjectRow>* PutTableList(std::vector<SysObjectRow> tables);

    // ---- lifecycle ------------------------------------------------------

    // The single DDL choke point: drops every fact that DDL can change,
    // keeping sys.types. Cheap by construction - it frees, it does not
    // rebuild.
    void Invalidate() noexcept;

    const Stats& stats() const noexcept { return stats_; }

    // Gauges, for tests and for whatever reports cache size later. Unlike
    // the Find* calls these do not count a hit or a miss, so a test can ask
    // about cache state without perturbing what it is measuring.
    bool types_loaded() const noexcept { return types_.has_value(); }
    std::size_t table_access_entries() const noexcept { return table_access_.size(); }
    std::size_t name_entries() const noexcept { return name_to_oid_.size(); }
    std::size_t pattern_entries() const noexcept { return patterns_.size(); }

private:
    // Transparent hash so a lookup by string_view does not have to
    // materialize a std::string key just to throw it away. The map still
    // *owns* std::string keys - see PutOidByName().
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view name) const noexcept {
            return std::hash<std::string_view>{}(name);
        }
    };

    // Node-based on purpose: reference stability across rehash is the
    // property PutTableAccess() promises (see above). A flat map would
    // break every held pointer on growth.
    std::unordered_map<Oid, TableAccess> table_access_;
    std::unordered_map<std::string, Oid, NameHash, std::equal_to<>> name_to_oid_;

    // Node-based for the same reference-stability reason as table_access_,
    // and additionally because UpdatePatternWaystone() mutates an entry a
    // caller may be holding.
    std::unordered_map<std::uint64_t, PatternAccess> patterns_;

    // A vector with a linear scan, not a map: sys.types has ten rows and
    // both lookups (by name, by type_val) are single-field compares, the
    // same trade sys_object_registry.hpp makes for the same reason.
    // std::optional distinguishes "loaded and empty" from
    // "not loaded", which a bare empty vector cannot.
    std::optional<std::vector<SysTypeRow>> types_;
    std::optional<std::vector<SysObjectRow>> table_list_;

    Stats stats_;
};

}  // namespace kds::catalog
