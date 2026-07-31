#include "kds/catalog/catalog.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "kds/stats/waystone_dir.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"

namespace kds::catalog {

namespace {

// Scans every live row of type RowT out of the heap page at `page_id`.
// Dead slots (RowT::Decode's caller never sees them - ReadTuple() itself
// reports NotFound for a dead slot) are skipped, mirroring the legacy
// engine's "r == -ENOENT -> continue" scan idiom throughout catalog.c.
template <typename RowT>
StatusOr<std::vector<RowT>> ScanAll(storage::PageStore& store, PageId page_id) {
    auto bytes = store.Get(page_id);
    if (!bytes.ok()) return bytes.status();

    heap::PageView page(bytes.value());
    std::vector<RowT> rows;
    std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) {
            if (tuple.status().code() == StatusCode::kNotFound) continue;
            return tuple.status();
        }
        auto row = RowT::Decode(tuple.value().payload);
        if (!row.ok()) return row.status();
        rows.push_back(row.value());
    }
    return rows;
}

template <typename RowT>
Status InsertRow(storage::PageStore& store, PageId page_id, const RowT& row,
                  std::uint64_t trx_id) {
    auto bytes = store.Get(page_id);
    if (!bytes.ok()) return bytes.status();

    heap::PageView page(bytes.value());
    auto encoded = row.Encode();
    auto slot = page.InsertTuple(encoded, trx_id);
    if (!slot.ok()) return slot.status();
    return Status::OK();
}

}  // namespace

void Catalog::InitWellKnownObjects() {
    auto register_namespace = [this](Oid oid, std::string_view name) {
        SysObjectRow obj{};
        obj.oid = oid;
        obj.namespace_oid = oid;  // a namespace's own namespace is itself
        obj.type_oid = kTypeNamespace;
        obj.rel_id = 0;
        SetName(obj.name, name);
        sys_objects_.Register(obj);
    };
    auto register_type = [this](Oid oid, std::string_view name) {
        SysObjectRow obj{};
        obj.oid = oid;
        obj.namespace_oid = kNamespaceSys;
        obj.type_oid = oid;  // a type object's type is itself
        obj.rel_id = 0;
        SetName(obj.name, name);
        sys_objects_.Register(obj);
    };

    register_namespace(kNamespaceSys, "namespaceSys");
    register_namespace(kNamespacePublic, "namespacePublic");

    register_type(kTypeInt, "type_int");
    register_type(kTypeVarchar, "type_varchar");
    register_type(kTypeChar, "type_char");
    register_type(kTypeSchema, "type_schema");
    register_type(kTypeBool, "type_bool");
    register_type(kTypeBytes, "type_bytes");
    register_type(kTypeNamespace, "type_namespace");
    register_type(kTypeAttribute, "type_attribute");
    register_type(kTypeColumn, "type_column");
    register_type(kTypePage, "type_page");
    register_type(kTypeTable, "type_table");
    register_type(kTypeOperator, "type_operator");
    register_type(kTypeIndex, "type_index");

    register_type(kTypeInt8, "type_int8");
    register_type(kTypeInt16, "type_int16");
    register_type(kTypeInt32, "type_int32");
    register_type(kTypeFloat, "type_float");
    register_type(kTypeDecimal, "type_decimal");
    register_type(kTypeUint64, "type_uint64");
}

Status Catalog::Bootstrap() {
    InitWellKnownObjects();

    struct SysTableBootstrap {
        Oid oid;
        std::string_view name;
        PageId page_id;
    };
    static constexpr std::array<SysTableBootstrap, 5> kSysTables{{
        {kSysTypesTable, "types", kCatalogPageTypes},
        {kSysObjectsTable, "objects", kCatalogPageObjects},
        {kSysColumnsTable, "columns", kCatalogPageColumns},
        {kSysTablesTable, "tables", kCatalogPageTables},
        {kSysIndexesTable, "indexes", kCatalogPageIndexes},
    }};

    // Phase 1: allocate the fixed catalog heap pages. min_key=0: catalog
    // tables are always scanned in full by oid/name (ScanAll above), never
    // pruned by key range, so there is no meaningful min_key to choose.
    for (const auto& t : kSysTables) {
        auto created = store_.CreateAt(t.page_id);
        if (!created.ok()) {
            // Bootstrap runs once, on a store that should have none of
            // these ids; a conflict here means it is being run against an
            // existing database, which is the destructive mistake
            // bootstrap.hpp's fresh/existing branch exists to prevent.
            if (log_ != nullptr && log_->enabled(LogLevel::kError)) {
                log_->Error("catalog", "bootstrap could not create catalog page " +
                                           std::to_string(t.page_id) + " for sys." +
                                           std::string(t.name) + ": " +
                                           created.status().message());
            }
            return created.status();
        }
        auto page = heap::PageView::CreateEmpty(created.value(), 0);
        if (!page.ok()) return page.status();
    }

    // Phase 2: sys.objects rows for the catalog tables themselves.
    for (const auto& t : kSysTables) {
        if (Status s = InsertObjectRow(t.oid, kNamespaceSys, kTypeTable, t.name); !s.ok()) {
            return s;
        }
    }

    // Phase 3: sys.tables rows.
    for (const auto& t : kSysTables) {
        if (Status s = InsertRelationRow(t.oid, kNamespaceSys, t.name, t.page_id,
                                          ClusteredType::kHeap);
            !s.ok()) {
            return s;
        }
    }

    // Phase 4: sys.types rows for the well-known scalar types. type_val
    // below (well_known.hpp's kTypeVal* constants) is a placeholder tag:
    // the legacy engine pulled these from its types.c registry (not yet
    // ported to this project - see storage/). It is no longer numbering
    // trivia though - src/exec/row_codec.cpp switches on these values to
    // pick an on-disk encoding, and Catalog::ResolveTypeByName() below
    // resolves a parsed CREATE TABLE column's type_name to one of these
    // rows by name. Replace with real registry-sourced values later; the
    // numbers themselves still don't need to match anything external.
    struct SysTypeBootstrap {
        Oid oid;
        std::string_view name;
        std::uint32_t type_val;
        std::uint32_t len;
    };
    static constexpr std::array<SysTypeBootstrap, 10> kTypes{{
        {kTypeInt8, "int8", kTypeValInt8, 1},
        {kTypeInt16, "int16", kTypeValInt16, 2},
        {kTypeInt32, "int32", kTypeValInt32, 4},
        {kTypeInt64, "int64", kTypeValInt64, 8},
        {kTypeUint64, "uint64", kTypeValUint64, 8},
        {kTypeFloat, "float", kTypeValFloat, 4},
        {kTypeDecimal, "decimal", kTypeValDecimal, 8},
        {kTypeBool, "bool", kTypeValBool, 1},
        {kTypeVarchar, "varchar", kTypeValVarchar, 0},
        {kTypeChar, "char", kTypeValChar, 1},
    }};
    for (const auto& t : kTypes) {
        if (Status s = InsertTypeRow(t.oid, t.name, t.type_val, t.len); !s.ok()) {
            return s;
        }
    }

    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "bootstrapped " + std::to_string(kSysTables.size()) +
                                  " system tables and " + std::to_string(kTypes.size()) +
                                  " types on the fixed catalog pages");
    }
    return Status::OK();
}

Oid Catalog::GenerateUserOid() noexcept { return next_user_oid_++; }

void Catalog::BumpVersion(std::string_view what) {
    ++catalog_version_;
    cache_.Invalidate();
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "cache invalidated by " + std::string(what) + ": version " +
                                   std::to_string(catalog_version_));
    }
}

Status Catalog::InsertObjectRow(Oid oid, Oid namespace_oid, Oid type_oid,
                                 std::string_view name) {
    SysObjectRow row{};
    row.oid = oid;
    row.namespace_oid = namespace_oid;
    row.type_oid = type_oid;
    row.rel_id = 0;
    SetName(row.name, name);
    Status s = InsertRow(store_, kCatalogPageObjects, row, kBootstrapXid);
    // A new sys.objects row changes what FindTableOidByName and ListTables
    // would answer, so it stales cached name lookups.
    if (s.ok()) BumpVersion("sys.objects insert");
    return s;
}

Status Catalog::InsertRelationRow(Oid oid, Oid namespace_oid, std::string_view name,
                                   PageId desc_page_id, ClusteredType clustered_type) {
    SysTableRow row{};
    row.oid = oid;
    row.namespace_oid = namespace_oid;
    SetName(row.name, name);
    row.desc_page_id = desc_page_id;
    row.clustered_type = clustered_type;
    row.next_id = kFirstRowId;
    // Waystone is opt-in and off at creation (spec section 7). Turning it
    // on is a separate DDL step (SetWaystoneDirectory), which keeps
    // CREATE TABLE's cost identical to what it was.
    row.waystone_state = WaystoneState::kDisabled;
    row.waystone_dir_root = kInvalidPageId;
    row.waystone_dir_depth = 0;
    Status s = InsertRow(store_, kCatalogPageTables, row, kBootstrapXid);
    if (s.ok()) BumpVersion("sys.tables insert");
    return s;
}

Status Catalog::InsertColumnRow(Oid oid, Oid rel_id, std::uint32_t pos, std::string_view name,
                                 std::uint32_t type_val, std::uint32_t len, bool notnull) {
    SysColumnRow row{};
    row.oid = oid;
    row.rel_id = rel_id;
    row.pos = pos;
    SetName(row.name, name);
    row.type_val = type_val;
    row.len = len;
    row.notnull = notnull;
    Status s = InsertRow(store_, kCatalogPageColumns, row, kBootstrapXid);
    // A new column row changes the schema half of a cached TableAccess.
    if (s.ok()) BumpVersion("sys.columns insert");
    return s;
}

Status Catalog::InsertTypeRow(Oid oid, std::string_view name, std::uint32_t type_val,
                               std::uint32_t len) {
    SysTypeRow row{};
    row.oid = oid;
    SetName(row.name, name);
    row.type_val = type_val;
    row.len = len;
    Status s = InsertRow(store_, kCatalogPageTypes, row, kBootstrapXid);
    // The cache treats sys.types as immutable and keeps its snapshot across
    // Invalidate(). This is the only writer, so it is the only place that
    // assumption can be broken - drop the snapshot here rather than rely on
    // "Bootstrap() runs first" staying true.
    if (s.ok()) cache_.InvalidateTypes();
    return s;
}

// No version bump: nothing cached is derived from sys.indexes (see
// catalog_cache.hpp - index rows have no production reader yet).
Status Catalog::InsertIndexRow(Oid index_oid, Oid table_oid, std::uint32_t col_pos,
                                std::uint32_t col_type, std::uint8_t flags) {
    SysIndexRow row{};
    row.index_oid = index_oid;
    row.table_oid = table_oid;
    row.col_pos = col_pos;
    row.col_type = col_type;
    row.flags = flags;
    return InsertRow(store_, kCatalogPageIndexes, row, kBootstrapXid);
}

StatusOr<Oid> Catalog::CreateTable(Oid namespace_oid, std::string_view name, const Schema& schema,
                                    ClusteredType clustered_type) {
    // Refused at definition time rather than at the first INSERT: a table
    // whose first column cannot hold the Keystone id is one no row can
    // ever be written to (heap-and-tuple.md section 4).
    if (Status s = CheckKeystoneColumn(schema); !s.ok()) return s;

    auto created = store_.CreateNew();
    if (!created.ok()) return created.status();
    auto [root_id, root_bytes] = created.value();

    // Both clustered types root at `desc_page_id` and both start as one
    // page - a heap page for kHeap, a B+ tree leaf for kBtree (btree.hpp).
    // A btree relation grows its first internal level only when that leaf
    // splits, so a small table costs exactly what it did before, and the
    // choice is invisible to every layer above until the relation is big
    // enough for it to matter.
    Status formatted = Status::OK();
    switch (clustered_type) {
        case ClusteredType::kHeap: {
            auto root_page = heap::PageView::CreateEmpty(root_bytes, 0);
            if (!root_page.ok()) formatted = root_page.status();
            break;
        }
        case ClusteredType::kBtree:
            formatted = btree::FormatRoot(root_bytes);
            break;
    }
    if (!formatted.ok()) return formatted;

    Oid new_oid = GenerateUserOid();

    if (Status s = InsertObjectRow(new_oid, namespace_oid, kTypeTable, name); !s.ok()) {
        return s;
    }
    if (Status s = InsertRelationRow(new_oid, namespace_oid, name, root_id, clustered_type);
        !s.ok()) {
        return s;
    }

    for (const auto& col : schema.columns) {
        Status s = InsertColumnRow(GenerateUserOid(), new_oid, col.pos, NameView(col.name),
                                    col.type_val, col.len, col.notnull);
        if (!s.ok()) return s;
    }

    // Debug, not Info: the dispatcher already reports the client-visible
    // DDL at Info ("[ddl] created table ..."), and two Info lines for one
    // statement is how a log stops being readable. What this one adds is
    // the physical detail the client never sees - the root page.
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "created table '" + std::string(name) + "' oid=" +
                                  std::to_string(new_oid) + " root_page=" +
                                  std::to_string(root_id) + " columns=" +
                                  std::to_string(schema.columns.size()));
    }
    return new_oid;
}

StatusOr<SysTableRow> Catalog::GetSysTableRow(Oid table_oid) {
    auto rows = ScanAll<SysTableRow>(store_, kCatalogPageTables);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.oid == table_oid) return row;
    }
    return Status::NotFound("no sys.tables row for this oid");
}

StatusOr<Oid> Catalog::FindTableOidByName(std::string_view name) {
    if (const Oid* cached = cache_.FindOidByName(name); cached != nullptr) {
        return *cached;
    }

    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.type_oid == kTypeTable && NameView(row.name) == name) {
            cache_.PutOidByName(name, row.oid);
            return row.oid;
        }
    }
    // An absent name is not remembered. Two callers depend on that:
    // HandleCreateTable / HandleCreateTableSql look a name up *expecting*
    // NotFound before creating it, and a second Catalog over the same store
    // can create a table this instance never saw. A remembered absence
    // makes both answers wrong, where a repeated scan is only slow.
    return Status::NotFound("no table with this name");
}

StatusOr<std::vector<SysObjectRow>> Catalog::ListTables() {
    if (const std::vector<SysObjectRow>* cached = cache_.FindTableList(); cached != nullptr) {
        return *cached;
    }

    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects);
    if (!rows.ok()) return rows.status();

    std::vector<SysObjectRow> tables;
    for (auto& row : rows.value()) {
        if (row.type_oid == kTypeTable) tables.push_back(row);
    }
    return *cache_.PutTableList(std::move(tables));
}

StatusOr<Schema> Catalog::BuildSchemaFromColumns(Oid rel_id) {
    // Serves the cached copy when the relation has been opened before. The
    // return stays by value: this is the only caller-facing schema API that
    // hands out an owned Schema, DESCRIBE is not a hot path, and a copy
    // costs one vector allocation against the page scan below.
    if (const TableAccess* cached = cache_.FindTableAccess(rel_id); cached != nullptr) {
        return cached->schema;
    }
    return ScanSchemaFromColumns(rel_id);
}

StatusOr<Schema> Catalog::ScanSchemaFromColumns(Oid rel_id) {
    auto rows = ScanAll<SysColumnRow>(store_, kCatalogPageColumns);
    if (!rows.ok()) return rows.status();

    Schema schema;
    for (const auto& row : rows.value()) {
        if (row.rel_id == rel_id) schema.columns.push_back(row);
    }

    if (schema.columns.empty()) {
        return Status::NotFound("no columns for this rel_id");
    }

    // ScanAll() returns rows in heap slot order, which happens to match
    // insertion order (CreateTable() inserts columns in position order)
    // but isn't guaranteed to stay that way once updates/compaction exist.
    // Row codec callers (src/exec/row_codec.cpp) depend on schema.columns
    // being in `pos` order to line up positionally with an INSERT's value
    // list, so pin that ordering explicitly here rather than leaning on
    // scan-order coincidence.
    std::sort(schema.columns.begin(), schema.columns.end(),
              [](const SysColumnRow& a, const SysColumnRow& b) { return a.pos < b.pos; });

    return schema;
}

StatusOr<const std::vector<SysTypeRow>*> Catalog::EnsureTypes() {
    if (const std::vector<SysTypeRow>* cached = cache_.FindTypes(); cached != nullptr) {
        return cached;
    }
    auto rows = ScanAll<SysTypeRow>(store_, kCatalogPageTypes);
    if (!rows.ok()) return rows.status();
    return cache_.PutTypes(std::move(rows.value()));
}

StatusOr<SysTypeRow> Catalog::ResolveTypeByName(std::string_view name) {
    // One snapshot per process, and unlike every other cached fact its
    // *misses* are authoritative: only InsertTypeRow() writes sys.types and
    // it drops the snapshot, so "not in the snapshot" means "not in the
    // table" and needs no rescan. That takes the page read off the CREATE
    // TABLE error path too.
    auto types = EnsureTypes();
    if (!types.ok()) return types.status();

    for (const auto& row : *types.value()) {
        std::string_view row_name = NameView(row.name);
        if (row_name.size() == name.size() &&
            std::equal(row_name.begin(), row_name.end(), name.begin(), [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) ==
                       std::tolower(static_cast<unsigned char>(y));
            })) {
            return row;
        }
    }
    return Status::NotFound("unknown type '" + std::string(name) + "'");
}

StatusOr<const TableAccess*> Catalog::InitTableAccess(Oid oid) {
    if (const TableAccess* cached = cache_.FindTableAccess(oid); cached != nullptr) {
        return cached;
    }

    auto table_row = GetSysTableRow(oid);
    if (!table_row.ok()) return table_row.status();

    // Scan, not BuildSchemaFromColumns(): that one would probe the same
    // cache entry this call is in the middle of filling.
    auto schema = ScanSchemaFromColumns(oid);
    if (!schema.ok()) return schema.status();

    TableAccess access{};
    // The relation's namespace comes from its sys.tables row rather than
    // from the caller. It used to be a parameter echoed straight into the
    // result, which is untenable once the entry is shared: whichever caller
    // filled it first would decide the field for everyone else. CreateTable
    // already persists the right value.
    access.namespace_oid = table_row.value().namespace_oid;
    access.oid = oid;
    access.schema = std::move(schema.value());
    access.desc_page_id = table_row.value().desc_page_id;
    access.clustered_type = table_row.value().clustered_type;
    access.waystone_state = table_row.value().waystone_state;
    access.waystone_dir_root = table_row.value().waystone_dir_root;
    access.waystone_dir_depth = table_row.value().waystone_dir_depth;
    return cache_.PutTableAccess(std::move(access));
}

StatusOr<SysTypeRow> Catalog::ResolveTypeByVal(std::uint32_t type_val) {
    auto types = EnsureTypes();
    if (!types.ok()) return types.status();

    for (const auto& row : *types.value()) {
        if (row.type_val == type_val) return row;
    }
    return Status::NotFound("no sys.types row for this type_val");
}

StatusOr<std::uint64_t> Catalog::AllocateRowId(Oid table_oid) {
    auto bytes = store_.Get(kCatalogPageTables);
    if (!bytes.ok()) return bytes.status();

    heap::PageView page(bytes.value());
    std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) {
            if (tuple.status().code() == StatusCode::kNotFound) continue;
            return tuple.status();
        }

        auto row = SysTableRow::Decode(tuple.value().payload);
        if (!row.ok()) return row.status();
        if (row.value().oid != table_oid) continue;

        const std::uint64_t id = row.value().next_id;
        if (id > kMaxKeystoneId) {
            // The sequence is exhausted, not wrapped: reissuing from the
            // bottom would alias ids that Waystone addresses directly.
            // Id-reuse / low-range reclamation is an open decision
            // (CLAUDE.md), so this refuses rather than picking one.
            return Status::OutOfRange("relation has exhausted the Keystone id space");
        }

        // Bumped and persisted before the caller inserts anything. A crash
        // between here and the insert burns an id, which is harmless - the
        // sequence only has to be unique and monotonic, never gapless. The
        // reverse order would reissue an id after a crash, which is not.
        row.value().next_id = id + 1;
        auto encoded = row.value().Encode();
        if (Status s = page.OverwriteTuple(i, encoded, tuple.value().trx_id,
                                            tuple.value().undo_ptr);
            !s.ok()) {
            return s;
        }
        // One line per issued id: the sequence is the tuple identity
        // (invariant 10), so "which id did this row get" is a question
        // that gets asked about every insert that later looks wrong.
        if (log_ != nullptr && log_->enabled(LogLevel::kTrace)) {
            log_->Trace("catalog", "issued row id " + std::to_string(id) + " for table oid " +
                                       std::to_string(table_oid));
        }
        return id;
    }

    return Status::NotFound("no sys.tables row for this oid");
}

Status Catalog::UpdateRelationDescPage(Oid table_oid, PageId new_desc_page_id) {
    auto bytes = store_.Get(kCatalogPageTables);
    if (!bytes.ok()) return bytes.status();

    heap::PageView page(bytes.value());
    std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) {
            if (tuple.status().code() == StatusCode::kNotFound) continue;
            return tuple.status();
        }

        auto row = SysTableRow::Decode(tuple.value().payload);
        if (!row.ok()) return row.status();
        if (row.value().oid != table_oid) continue;

        const PageId old_desc_page_id = row.value().desc_page_id;
        row.value().desc_page_id = new_desc_page_id;
        auto encoded = row.value().Encode();
        Status s = page.OverwriteTuple(i, encoded, tuple.value().trx_id, tuple.value().undo_ptr);
        // desc_page_id is a field of every cached TableAccess, so a relink
        // stales it. Bumped only on success: a failed overwrite moved
        // nothing.
        if (s.ok()) BumpVersion("desc-page relink");
        if (s.ok() && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            // A relation's entry page moving is a structural change to the
            // relation, rare enough to deserve Debug rather than Trace.
            log_->Debug("catalog", "table oid " + std::to_string(table_oid) +
                                       " desc page " + std::to_string(old_desc_page_id) + " -> " +
                                       std::to_string(new_desc_page_id));
        }
        return s;
    }

    return Status::NotFound("no sys.tables row for this oid");
}

Status Catalog::SetWaystoneDirectory(Oid table_oid, WaystoneState state, PageId dir_root,
                                     std::uint8_t dir_depth) {
    // Checked before the page is touched: a half-written triple is the one
    // outcome that would leave the relation unwalkable.
    if (state == WaystoneState::kDisabled) {
        if (dir_root != kInvalidPageId || dir_depth != 0) {
            return Status::InvalidArgument(
                "catalog: a disabled Waystone must carry no directory root and depth 0");
        }
    } else {
        if (dir_root == kInvalidPageId) {
            return Status::InvalidArgument(
                "catalog: an enabled Waystone needs a directory root page");
        }
        if (dir_depth < 1 || dir_depth > stats::kMaxDirDepth) {
            return Status::InvalidArgument(
                "catalog: Waystone directory depth " + std::to_string(dir_depth) +
                " is outside 1.." + std::to_string(stats::kMaxDirDepth));
        }
    }

    auto bytes = store_.Get(kCatalogPageTables);
    if (!bytes.ok()) return bytes.status();

    heap::PageView page(bytes.value());
    std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) {
            if (tuple.status().code() == StatusCode::kNotFound) continue;
            return tuple.status();
        }

        auto row = SysTableRow::Decode(tuple.value().payload);
        if (!row.ok()) return row.status();
        if (row.value().oid != table_oid) continue;

        row.value().waystone_state = state;
        row.value().waystone_dir_root = dir_root;
        row.value().waystone_dir_depth = dir_depth;
        auto encoded = row.value().Encode();
        Status s = page.OverwriteTuple(i, encoded, tuple.value().trx_id, tuple.value().undo_ptr);
        // All three are TableAccess fields, so a cached entry is stale the
        // moment they change. Bumped only on success: a failed overwrite
        // changed nothing and invalidating would just cost a re-scan.
        if (s.ok()) BumpVersion("waystone directory change");
        if (s.ok() && log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
            // Info, alongside the other DDL: enabling Waystone changes the
            // relation's storage form and its per-insert cost, which is
            // the kind of thing an operator wants in a default log.
            log_->Info("catalog", "table oid " + std::to_string(table_oid) +
                                      " waystone state=" +
                                      std::to_string(static_cast<int>(state)) + " root=" +
                                      std::to_string(dir_root) + " depth=" +
                                      std::to_string(dir_depth));
        }
        return s;
    }

    return Status::NotFound("no sys.tables row for this oid");
}

StatusOr<std::vector<SysIndexRow>> Catalog::FindIndexesForTable(Oid table_oid) {
    auto rows = ScanAll<SysIndexRow>(store_, kCatalogPageIndexes);
    if (!rows.ok()) return rows.status();

    std::vector<SysIndexRow> out;
    for (const auto& row : rows.value()) {
        if (row.table_oid == table_oid) out.push_back(row);
    }
    return out;
}

StatusOr<SysIndexRow> Catalog::FindIndexOnColumn(Oid table_oid, std::uint32_t col_pos) {
    auto rows = ScanAll<SysIndexRow>(store_, kCatalogPageIndexes);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.table_oid == table_oid && row.col_pos == col_pos) return row;
    }
    return Status::NotFound("no index on this column");
}

}  // namespace kds::catalog
