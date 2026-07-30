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

// SQL catalog (sys.objects/sys.tables/sys.columns/sys.types/sys.indexes),
// ported from the legacy kernel engine's catalog.c/kds_catalog.h, itself a
// C port of a Python POC's catalog.py.
//
// Differences from the legacy kernel version, and why:
//   - Depends on kds::storage::PageStore (an abstract seam) instead of
//     the buffer pool (kds_buf_lookup_or_load()/kds_buf_alloc_new()),
//     which doesn't exist in this project yet - see page_store.hpp.
//     Swap in a real buffer-pool-backed PageStore later without touching
//     this file.
//   - CreateTable() only supports ClusteredType::kHeap for now:
//     ClusteredType::kBtree needs the not-yet-ported btree code
//     (kernel/kds/btree.c) and is rejected with InvalidArgument rather
//     than half-implemented.
//   - No transaction manager exists yet, so every row this file writes
//     is stamped with kBootstrapXid, same as the legacy engine's
//     approach before its transaction manager existed.
//   - Object oid generation (GenerateUserOid()) is in-memory only and
//     resets on restart - same KNOWN GAP the legacy engine had (see
//     kUserOidStart's comment in well_known.hpp).
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

    // Creates a new table: allocates its storage root page and inserts
    // the corresponding sys.objects/sys.tables/sys.columns rows. Only
    // ClusteredType::kHeap is implemented today (see file comment).
    StatusOr<Oid> CreateTable(Oid namespace_oid, std::string_view name, const Schema& schema,
                               ClusteredType clustered_type);

    StatusOr<SysTableRow> GetSysTableRow(Oid table_oid);

    // Scans sys.objects (disk, not the in-memory well-known registry -
    // CreateTable() only writes the disk row) for a row named `name`
    // with type_oid == kTypeTable.
    StatusOr<Oid> FindTableOidByName(std::string_view name);

    // Lists every table registered in sys.objects (type_oid == kTypeTable),
    // including the catalog's own bootstrap tables - not just user-created
    // ones. Not part of the legacy engine's catalog.c; added for the
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

    // Updates the desc_page_id field of table_oid's sys.tables row in
    // place - for a future btree root split/collapse to repoint at a new
    // root page. Uses an in-place overwrite (row size is unchanged), not
    // delete+insert, mirroring the legacy kds_catalog_update_relation_desc_page().
    Status UpdateRelationDescPage(Oid table_oid, PageId new_desc_page_id);

    // Sets the relation's Waystone fields as one unit (spec section 7):
    // enabling with a provisioned directory root, recording a deepened
    // directory, promoting kBackfilling to kCovered, or disabling.
    //
    // The three are written together because they are one fact. A root
    // without its depth is unwalkable, and a depth that disagrees with the
    // root sends every lookup to the wrong leaf - so there is deliberately
    // no setter for one of them.
    //
    // Validated: kDisabled requires kInvalidPageId and depth 0, and any
    // other state requires a real root and a depth in 1..kMaxDirDepth.
    // Refusing an inconsistent pair here is what lets every reader treat
    // the triple as trustworthy without re-checking it.
    //
    // Bumps the catalog version on success, because TableAccess caches all
    // three. **A caller holding a `const TableAccess*` must re-acquire it
    // afterwards** - the bump clears the cache and the old pointer dangles.
    // That matters most for directory growth, which happens in the middle
    // of an insert that is already holding one.
    //
    // Fails with NotFound if no sys.tables row names `table_oid`, and with
    // InvalidArgument for an inconsistent triple. Dropping the old
    // directory's pages on disable is the caller's business: this writes
    // the row, it does not own page lifetime.
    Status SetWaystoneDirectory(Oid table_oid, WaystoneState state, PageId dir_root,
                                std::uint8_t dir_depth);

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
