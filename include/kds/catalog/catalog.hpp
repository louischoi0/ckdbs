#pragma once

#include <string_view>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog_cache.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/catalog/sys_object_registry.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/storage/page_store.hpp"

// SQL catalog: sys.objects, sys.tables, sys.columns, sys.types,
// sys.indexes, sys.patterns. Four things to know before touching it:
//
//   - It depends on kds::storage::PageStore, an abstract seam, rather than
//     on a buffer pool - so a real buffer-pool-backed store can be swapped
//     in later without touching this file.
//   - CreateTable() supports both ClusteredType values. Either way the
//     relation is one page at creation - a heap page or a B+ tree leaf -
//     rooted at `desc_page_id`; what differs is what grows out of it
//     (heap_chain.hpp vs btree.hpp).
//   - No transaction manager exists yet, so every row written here is
//     stamped kBootstrapXid, which is visible to every read view.
//   - Object oid generation (GenerateUserOid()) is in-memory only and
//     resets on restart - a KNOWN GAP; see kUserOidStart in
//     well_known.hpp. sys.patterns rows do not use it, for that reason.
//
// Logging (component tag "catalog"): catalog pages are the pages whose
// contents explain every other page, so the writes are logged at Info
// (bootstrap, CREATE TABLE - rare, structural, and the thing an operator
// reconstructs a database's history from) and the per-row mutations at
// Trace (id issue, desc-page relink). Reads are not logged at all: they
// are the common case and say nothing about what changed - and that now
// includes cache hits, which are the most common event in this file.
// Cache invalidation is logged at Debug: it is bounded by DDL, and DDL's
// catalog side already reports there.
//
// Reads are served from an in-memory CatalogCache (catalog_cache.hpp),
// which owns the rule for what may be cached. The two things to know
// before touching this file: `next_id` is never cached, so AllocateRowId()
// and GetSysTableRow() always read the page; and every mutation of a
// cached fact goes through BumpVersion(), the single invalidation point.

namespace kds::catalog {

class Catalog {
public:
    explicit Catalog(storage::PageStore& store) noexcept : store_(store) {}

    // Diagnostic log, null (discard) by default. `log` must outlive the
    // catalog. Set rather than constructed with, because bootstrap builds
    // a Catalog before the server has decided anything about logging.
    void SetLogger(Logger* log) noexcept { log_ = log; }

    // Registers the fixed namespace/type sys-objects in the in-memory
    // registry (no disk I/O - these are well-known constants, not stored
    // as catalog rows themselves).
    void InitWellKnownObjects();

    // Allocates the fixed catalog pages (kCatalogPage*) via store_ and
    // populates them with bootstrap rows for sys.types/objects/columns/
    // tables/indexes. Must be called exactly once, against a PageStore
    // that has none of those page ids yet - fails with whatever error
    // the first conflicting CreateAt() reports.
    Status Bootstrap();

    // Allocates one fresh object oid. See kUserOidStart's comment: not
    // persisted across restarts.
    Oid GenerateUserOid() noexcept;

    // Creates a new table: allocates its storage root page, formats it for
    // the requested clustered type, and inserts the corresponding
    // sys.objects/sys.tables/sys.columns rows.
    StatusOr<Oid> CreateTable(Oid namespace_oid, std::string_view name, const Schema& schema,
                               ClusteredType clustered_type);

    StatusOr<SysTableRow> GetSysTableRow(Oid table_oid);

    // Scans sys.objects (disk, not the in-memory well-known registry -
    // CreateTable() only writes the disk row) for a row named `name`
    // with type_oid == kTypeTable.
    StatusOr<Oid> FindTableOidByName(std::string_view name);

    // Lists every table registered in sys.objects (type_oid == kTypeTable),
    // including the catalog's own bootstrap tables - not just user-created
    // ones. Added for the
    // server's command dispatcher (src/server), which needs "what tables
    // exist" without the caller already knowing a name to look up.
    StatusOr<std::vector<SysObjectRow>> ListTables();

    // Returns an owned copy, served from the cache when the relation has
    // been opened before. NotFound for a relation with no sys.columns rows
    // (the bootstrap catalog tables) - an absence, and so never cached.
    StatusOr<Schema> BuildSchemaFromColumns(Oid rel_id);

    // Resolves a CREATE TABLE column's raw, unresolved type_name (e.g.
    // "int64", "varchar") to its sys.types row, case-insensitively.
    // NotFound if no such type is registered. This is today's stand-in
    // for the not-yet-ported type registry (ast.hpp's file comment):
    // Bootstrap() already populates sys.types with every scalar type
    // Catalog knows about, so resolving by name against that table is a
    // real lookup, not a guess.
    StatusOr<SysTypeRow> ResolveTypeByName(std::string_view name);

    // The reverse lookup, for rendering a stored column's type back as a
    // name (DESCRIBE). NotFound if no sys.types row carries this type_val.
    StatusOr<SysTypeRow> ResolveTypeByVal(std::uint32_t type_val);

    // Opens a relation, caching the result. The returned pointer belongs to
    // this Catalog's cache: it stays valid until the next DDL through this
    // instance (which drops the entry) or the Catalog is destroyed, and it
    // survives caching other relations, because cache entries are
    // reference-stable (catalog_cache.hpp). Callers must not hold one past
    // the statement that took it.
    //
    // Issuing a row id does *not* invalidate it - AllocateRowId() touches
    // only `next_id`, which TableAccess does not carry - so a statement may
    // hold this across its own inserts, as HandleInsert does.
    //
    // The namespace is read from the relation's sys.tables row; it used to
    // be a caller-supplied parameter echoed into the result, which a shared
    // cache entry cannot honor.
    StatusOr<const TableAccess*> InitTableAccess(Oid oid);

    // Issues the next Keystone id for `table_oid` and persists the bumped
    // sequence, so the primary key is system-generated rather than
    // caller-supplied (CLAUDE.md invariant 10). Ids are unique and
    // monotonic by construction; they are not gapless, since a failed
    // insert after a successful allocation burns one.
    //
    // Fails with NotFound if no sys.tables row names `table_oid`, and with
    // OutOfRange once the relation has issued its 40-bit id space -
    // reclamation policy is an open decision, so exhaustion is reported
    // rather than wrapped.
    StatusOr<std::uint64_t> AllocateRowId(Oid table_oid);

    // ---- sys.patterns (docs/waystone-concpets.md section 4) --------------

    // The pattern `pattern_id` names, served from the cache after the first
    // lookup. The pointer is reference-stable and stays valid until the
    // next Invalidate() - same contract as InitTableAccess().
    //
    // Fails with NotFound when the pattern has never been registered **and
    // when the only row for it came from another fingerprint revision**.
    // Those two are deliberately one outcome: a row recorded under
    // different fingerprinting rules names a shape that is not the one it
    // claims, so the only safe reading of it is that this pattern has not
    // been seen. The caller registers it afresh and the stale row becomes
    // garbage for retention to reclaim (P15). A version mismatch is never
    // an error - nothing about it should fail a statement.
    StatusOr<const PatternAccess*> FindPattern(std::uint64_t pattern_id);

    // Records a newly observed pattern and returns its cached access, so a
    // caller that just registered does not look it back up.
    //
    // **The version is stamped here, not passed in.** No caller has any
    // business recording a pattern under a revision other than the one
    // that computed its `pattern_id`, so a version parameter could only
    // ever be passed wrong - and passing it wrong writes a row no build
    // will resolve. Removing the parameter is what makes "the cache holds
    // current-version entries only" true by construction.
    //
    // Registration bumps **no** catalog version, and that is worth stating
    // because the deleted per-relation Waystone got it wrong in exactly
    // this spot. Nothing cached can go stale from a pattern appearing:
    // absences are never cached (catalog_cache.hpp), so no entry claims
    // this pattern is missing, and no other cached fact mentions it. The
    // consequence matters on the statement path - registering a pattern
    // mid-statement cannot dangle the `const TableAccess*` that statement
    // is holding, which is the hazard `waystone-concpets.md` section 4
    // flagged for this task.
    //
    // The pattern's `oid` comes from AllocateRowId(kSysPatternsTable) - the
    // persistent sequence in sys.patterns' own sys.tables row. That is a
    // repurposing worth naming: catalog rows carry no Keystone word, so
    // this is the sequence used as an oid source rather than as a tuple id.
    // The alternative, GenerateUserOid(), is in-memory and restarts at
    // kUserOidStart every boot (well_known.hpp), which for a *persisted*
    // row means two patterns sharing an oid across a restart.
    //
    // Fails with AlreadyExists if `pattern_id` is already registered at
    // the current version. A row left by an older revision does not count
    // as registered, so a version bump leaves every shape re-learnable
    // rather than permanently blocked.
    StatusOr<const PatternAccess*> RegisterPattern(std::uint64_t pattern_id,
                                                    std::uint8_t stmt_class);

    // Points a pattern at its waystone directory, writing root and depth as
    // one unit.
    //
    // The two are one fact and there is deliberately no setter for either
    // alone: a root without its depth is unwalkable, and a depth that
    // disagrees with the root sends every walk to the wrong leaf. Validated
    // before the page is touched - depth 0 requires kInvalidPageId, and any
    // other depth requires a real root and a depth within
    // kMaxPatternDirDepth - so every reader downstream may trust the pair
    // without re-checking it.
    //
    // Updates the cached PatternAccess in place rather than invalidating
    // (catalog_cache.hpp explains why that is the exception it is), so a
    // caller holding a `const PatternAccess*` keeps a valid pointer and
    // sees the new directory.
    //
    // Fails with NotFound if no sys.patterns row carries `pattern_id`, and
    // with InvalidArgument for an incoherent pair. Page lifetime of the old
    // directory is the caller's business: this writes the row.
    Status SetPatternWaystoneRoot(std::uint64_t pattern_id, PageId root, std::uint8_t depth);

    // Every sys.patterns row, in page order.
    //
    // **Unfiltered, unlike GetSysPatternRow()** - rows from another
    // fingerprint revision are included. That is not a hole in the version
    // rule: the rule protects *lookup by pattern_id*, so a stale row can
    // never resolve as the pattern it names. This is an inspection surface,
    // and the stale rows are exactly what an operator wants to see - they
    // are the garbage a version bump left behind, waiting on retention.
    // A caller that wants the version rule applied asks for a pattern by
    // id; a caller that wants to look at the relation reads this.
    StatusOr<std::vector<SysPatternRow>> ListPatterns();

    // Every sys.types row, through the same cached snapshot
    // ResolveTypeByName() reads. Exposed for the `sys.types` catalog view
    // (exec/catalog_view.hpp), which needs to enumerate what the
    // by-name lookup can only probe one at a time.
    //
    // The snapshot is safe to hand out because types are bootstrap-only:
    // nothing creates one at runtime, so unlike the table list it cannot
    // go stale under a caller.
    StatusOr<const std::vector<SysTypeRow>*> ListTypes();

    // The pattern's row straight off the page, never from the cache.
    //
    // The counterpart of GetSysTableRow(): it exists for the fields
    // PatternAccess deliberately omits - `use_count` and `last_seen` -
    // which change on every execution and so are not cacheable facts
    // (schema.hpp). A caller that wants heat calls this; a caller that
    // wants identity or location calls FindPattern().
    //
    // **Rows from another fingerprint revision are invisible here**, and
    // this is the single place that filter lives. A version bump leaves
    // the old rows on the page - nothing rewrites them - so a lookup that
    // did not filter would let a stale row shadow the current one. Putting
    // the filter in the row lookup rather than in each caller is what
    // makes that impossible rather than merely unlikely.
    StatusOr<SysPatternRow> GetSysPatternRow(std::uint64_t pattern_id);

    // Updates the desc_page_id field of table_oid's sys.tables row in
    // place - for a future btree root split/collapse to repoint at a new
    // root page. Uses an in-place overwrite (row size is unchanged), not
    // delete+insert, since the row size is unchanged.
    Status UpdateRelationDescPage(Oid table_oid, PageId new_desc_page_id);

    Status InsertObjectRow(Oid oid, Oid namespace_oid, Oid type_oid, std::string_view name);
    Status InsertRelationRow(Oid oid, Oid namespace_oid, std::string_view name,
                              PageId desc_page_id, ClusteredType clustered_type);

    Status InsertIndexRow(Oid index_oid, Oid table_oid, std::uint32_t col_pos,
                           std::uint32_t col_type, std::uint8_t flags);
    StatusOr<std::vector<SysIndexRow>> FindIndexesForTable(Oid table_oid);
    StatusOr<SysIndexRow> FindIndexOnColumn(Oid table_oid, std::uint32_t col_pos);

    const SysObjectRegistry& sys_objects() const noexcept { return sys_objects_; }

    // Monotonic count of catalog mutations that can stale a cached fact.
    // Bumped by DDL - CREATE TABLE, a desc-page relink, a bootstrap row -
    // and deliberately *not* by AllocateRowId(), which changes only
    // `next_id`, a fact nothing caches (catalog_cache.hpp).
    //
    // This is the counter parser.md I5 / parser-workplan.md PR20 stamp
    // parsed statements with. PR20 owns the stamp policy (which version a
    // bound statement records, and what a stale stamp does); this file owns
    // only the counter and its bump points. PR20 should consume this rather
    // than introduce a second counter that can drift from it. It counts
    // mutations, not statements: one CREATE TABLE bumps it once per row it
    // writes, so the contract is "monotonic", never "+1 per DDL".
    std::uint64_t catalog_version() const noexcept { return catalog_version_; }

    const CatalogCache::Stats& cache_stats() const noexcept { return cache_.stats(); }

private:
    Status InsertColumnRow(Oid oid, Oid rel_id, std::uint32_t pos, std::string_view name,
                            std::uint32_t type_val, std::uint32_t len, bool notnull);
    Status InsertTypeRow(Oid oid, std::string_view name, std::uint32_t type_val,
                          std::uint32_t len);

    // The uncached sys.columns scan behind BuildSchemaFromColumns(). Split
    // out so InitTableAccess() can fill an entry without re-probing the
    // cache slot it is filling.
    StatusOr<Schema> ScanSchemaFromColumns(Oid rel_id);

    // The sys.types snapshot, loaded on first use. Non-owning: the vector
    // lives in the cache.
    StatusOr<const std::vector<SysTypeRow>*> EnsureTypes();

    // The single invalidation point: bumps the version and drops every
    // DDL-invalidatable cache entry. `what` names the mutation for the
    // Debug line. Call it *after* the page write succeeds - a failed
    // mutation staled nothing.
    void BumpVersion(std::string_view what);

    storage::PageStore& store_;
    Logger* log_ = nullptr;
    Oid next_user_oid_ = kUserOidStart;
    SysObjectRegistry sys_objects_;
    CatalogCache cache_;
    std::uint64_t catalog_version_ = 0;
};

}  // namespace kds::catalog
