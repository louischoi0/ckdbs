#pragma once

#include <string_view>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
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
// are the common case and say nothing about what changed.

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

    StatusOr<TableAccess> InitTableAccess(Oid namespace_oid, Oid oid);

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

    Status InsertObjectRow(Oid oid, Oid namespace_oid, Oid type_oid, std::string_view name);
    Status InsertRelationRow(Oid oid, Oid namespace_oid, std::string_view name,
                              PageId desc_page_id, ClusteredType clustered_type);

    Status InsertIndexRow(Oid index_oid, Oid table_oid, std::uint32_t col_pos,
                           std::uint32_t col_type, std::uint8_t flags);
    StatusOr<std::vector<SysIndexRow>> FindIndexesForTable(Oid table_oid);
    StatusOr<SysIndexRow> FindIndexOnColumn(Oid table_oid, std::uint32_t col_pos);

    const SysObjectRegistry& sys_objects() const noexcept { return sys_objects_; }

private:
    Status InsertColumnRow(Oid oid, Oid rel_id, std::uint32_t pos, std::string_view name,
                            std::uint32_t type_val, std::uint32_t len, bool notnull);
    Status InsertTypeRow(Oid oid, std::string_view name, std::uint32_t type_val,
                          std::uint32_t len);

    storage::PageStore& store_;
    Logger* log_ = nullptr;
    Oid next_user_oid_ = kUserOidStart;
    SysObjectRegistry sys_objects_;
};

}  // namespace kds::catalog
