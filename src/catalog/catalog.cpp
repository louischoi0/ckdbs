#include "kds/catalog/catalog.hpp"

#include "kds/parser/fingerprint.hpp"

#include <algorithm>
#include <array>
#include <cctype>

#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"

namespace kds::catalog {

namespace {

// Scans every live row of type RowT out of the heap page at `page_id`.
// Dead slots (RowT::Decode's caller never sees them - ReadTuple() itself
// reports NotFound for a dead slot) are skipped.
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
    static constexpr std::array<SysTableBootstrap, 6> kSysTables{{
        {kSysTypesTable, "types", kCatalogPageTypes},
        {kSysObjectsTable, "objects", kCatalogPageObjects},
        {kSysColumnsTable, "columns", kCatalogPageColumns},
        {kSysTablesTable, "tables", kCatalogPageTables},
        {kSysIndexesTable, "indexes", kCatalogPageIndexes},
        // sys.patterns gets no sys.columns rows, exactly like the five
        // above: the catalog relations are read through their typed row
        // codecs (rows.hpp), never through a schema, so a column list for
        // them would describe nothing anyone reads.
        {kSysPatternsTable, "patterns", kCatalogPagePatterns},
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
    // there is no type registry to source them from yet. They are not
    // numbering trivia - src/exec/row_codec.cpp switches on these values to
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
            // bottom would hand out an id that is still some tuple's
            // identity. Id-reuse / low-range reclamation is an open
            // decision (CLAUDE.md), so this refuses rather than picking one.
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

// ---- sys.patterns ----------------------------------------------------

namespace {

// The identity + location half of a pattern row - everything DDL alone can
// change. Heat is dropped on the floor here on purpose; see schema.hpp.
PatternAccess AccessOf(const SysPatternRow& row) noexcept {
    PatternAccess access{};
    access.oid = row.oid;
    access.pattern_id = row.pattern_id;
    access.fingerprint_version = row.fingerprint_version;
    access.waystone_root = row.waystone_root;
    access.stmt_class = row.stmt_class;
    access.dir_depth = row.dir_depth;
    return access;
}

// Whether a stored root/depth pair may be written. Checked before the page
// is touched, because a half-written pair is the one outcome that leaves a
// pattern's waystones unreachable.
Status CheckWaystonePair(PageId root, std::uint8_t depth) {
    if (depth == 0) {
        if (root != kInvalidPageId) {
            return Status::InvalidArgument(
                "catalog: a pattern with no waystone directory must carry no root");
        }
        return Status::OK();
    }
    if (root == kInvalidPageId) {
        return Status::InvalidArgument("catalog: a waystone directory needs a root page");
    }
    if (depth > kMaxPatternDirDepth) {
        return Status::InvalidArgument("catalog: waystone directory depth " +
                                       std::to_string(depth) + " exceeds " +
                                       std::to_string(kMaxPatternDirDepth));
    }
    return Status::OK();
}

}  // namespace

StatusOr<const std::vector<SysTypeRow>*> Catalog::ListTypes() { return EnsureTypes(); }

StatusOr<std::vector<SysPatternRow>> Catalog::ListPatterns() {
    return ScanAll<SysPatternRow>(store_, kCatalogPagePatterns);
}

StatusOr<SysPatternRow> Catalog::GetSysPatternRow(std::uint64_t pattern_id) {
    auto rows = ScanAll<SysPatternRow>(store_, kCatalogPagePatterns);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.pattern_id != pattern_id) continue;
        // Rows from another fingerprint revision are invisible here, and
        // this is the only place that decision is made. Putting the filter
        // in the row lookup rather than in each caller is what stops a
        // stale row from shadowing a current one when both are on the page
        // - which is the state a version bump leaves behind, since nothing
        // rewrites the old rows.
        if (!parser::IsCurrentFingerprintVersion(row.fingerprint_version)) continue;
        return row;
    }
    return Status::NotFound("no current sys.patterns row for this pattern_id");
}

StatusOr<const PatternAccess*> Catalog::FindPattern(std::uint64_t pattern_id) {
    if (const PatternAccess* cached = cache_.FindPattern(pattern_id); cached != nullptr) {
        return cached;
    }

    // Version filtering lives in GetSysPatternRow(), so a row from another
    // revision arrives here as NotFound and never reaches the cache. That
    // ordering is the point: the cache holds current-version entries only,
    // by construction rather than by a check every reader has to remember.
    auto row = GetSysPatternRow(pattern_id);
    if (!row.ok()) return row.status();

    return cache_.PutPattern(AccessOf(row.value()));
}

StatusOr<const PatternAccess*> Catalog::RegisterPattern(std::uint64_t pattern_id,
                                                         std::uint8_t stmt_class) {
    // Read the page, not the cache: absences are never cached, so a cache
    // miss says nothing about whether the row exists. A row left behind by
    // an older revision does not count as existing - GetSysPatternRow()
    // does not return one - so a version bump leaves every shape
    // re-learnable rather than permanently blocked. The stale row stays
    // where it is; reclaiming it is retention's job (P15), and rewriting it
    // here would be an overwrite of a live tuple for no gain.
    if (GetSysPatternRow(pattern_id).ok()) {
        return Status::AlreadyExists("catalog: this pattern is already registered");
    }

    auto oid = AllocateRowId(kSysPatternsTable);
    if (!oid.ok()) return oid.status();

    SysPatternRow row{};
    row.oid = oid.value();
    row.pattern_id = pattern_id;
    row.last_seen = 0;
    row.fingerprint_version = parser::kFingerprintVersion;
    row.waystone_root = kInvalidPageId;
    row.use_count = 0;
    row.stmt_class = stmt_class;
    row.dir_depth = 0;  // no directory until the first trail is recorded

    if (Status s = InsertRow(store_, kCatalogPagePatterns, row, kBootstrapXid); !s.ok()) {
        return s;
    }
    // No BumpVersion(): nothing cached can go stale from a pattern
    // appearing (catalog.hpp states the argument, and the statement path
    // depends on it).
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "registered pattern " + std::to_string(pattern_id) + " as oid " +
                                   std::to_string(row.oid));
    }
    return cache_.PutPattern(AccessOf(row));
}

Status Catalog::SetPatternWaystoneRoot(std::uint64_t pattern_id, PageId root,
                                        std::uint8_t depth) {
    if (Status s = CheckWaystonePair(root, depth); !s.ok()) return s;

    auto bytes = store_.Get(kCatalogPagePatterns);
    if (!bytes.ok()) return bytes.status();

    heap::PageView page(bytes.value());
    std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) {
            if (tuple.status().code() == StatusCode::kNotFound) continue;
            return tuple.status();
        }

        auto row = SysPatternRow::Decode(tuple.value().payload);
        if (!row.ok()) return row.status();
        if (row.value().pattern_id != pattern_id) continue;
        if (!parser::IsCurrentFingerprintVersion(row.value().fingerprint_version)) continue;

        row.value().waystone_root = root;
        row.value().dir_depth = depth;
        auto encoded = row.value().Encode();
        Status s = page.OverwriteTuple(i, encoded, tuple.value().trx_id, tuple.value().undo_ptr);
        // Updated in place rather than invalidated, and only on success: a
        // failed overwrite moved nothing, and publishing the new pair into
        // the cache would make it disagree with the page.
        if (s.ok()) cache_.UpdatePatternWaystone(pattern_id, root, depth);
        if (s.ok() && log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("catalog", "pattern " + std::to_string(pattern_id) +
                                       " waystone root=" + std::to_string(root) +
                                       " depth=" + std::to_string(depth));
        }
        return s;
    }

    return Status::NotFound("no sys.patterns row for this pattern_id");
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
