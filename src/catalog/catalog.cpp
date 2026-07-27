#include "kds/catalog/catalog.hpp"

#include <array>

#include "kds/storage/heap/heap_page.hpp"

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
                  std::uint64_t xmin) {
    auto bytes = store.Get(page_id);
    if (!bytes.ok()) return bytes.status();

    heap::PageView page(bytes.value());
    auto encoded = row.Encode();
    auto slot = page.InsertTuple(encoded, xmin);
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
        if (!created.ok()) return created.status();
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
    // below is a placeholder tag: the legacy engine pulled these from its
    // types.c registry (not yet ported to this project - see storage/),
    // so there is nothing to look them up from yet. Replace with real
    // values once that registry exists; nothing in this codebase depends
    // on the specific numbers today.
    struct SysTypeBootstrap {
        Oid oid;
        std::string_view name;
        std::uint32_t type_val;
        std::uint32_t len;
    };
    static constexpr std::array<SysTypeBootstrap, 10> kTypes{{
        {kTypeInt8, "int8", 1, 1},
        {kTypeInt16, "int16", 2, 2},
        {kTypeInt32, "int32", 3, 4},
        {kTypeInt64, "int64", 4, 8},
        {kTypeUint64, "uint64", 5, 8},
        {kTypeFloat, "float", 6, 4},
        {kTypeDecimal, "decimal", 7, 8},
        {kTypeBool, "bool", 8, 1},
        {kTypeVarchar, "varchar", 9, 0},
        {kTypeChar, "char", 10, 1},
    }};
    for (const auto& t : kTypes) {
        if (Status s = InsertTypeRow(t.oid, t.name, t.type_val, t.len); !s.ok()) {
            return s;
        }
    }

    return Status::OK();
}

Oid Catalog::GenerateUserOid() noexcept { return next_user_oid_++; }

Status Catalog::InsertObjectRow(Oid oid, Oid namespace_oid, Oid type_oid,
                                 std::string_view name) {
    SysObjectRow row{};
    row.oid = oid;
    row.namespace_oid = namespace_oid;
    row.type_oid = type_oid;
    row.rel_id = 0;
    SetName(row.name, name);
    return InsertRow(store_, kCatalogPageObjects, row, kBootstrapXid);
}

Status Catalog::InsertRelationRow(Oid oid, Oid namespace_oid, std::string_view name,
                                   PageId desc_page_id, ClusteredType clustered_type) {
    SysTableRow row{};
    row.oid = oid;
    row.namespace_oid = namespace_oid;
    SetName(row.name, name);
    row.desc_page_id = desc_page_id;
    row.clustered_type = clustered_type;
    return InsertRow(store_, kCatalogPageTables, row, kBootstrapXid);
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
    return InsertRow(store_, kCatalogPageColumns, row, kBootstrapXid);
}

Status Catalog::InsertTypeRow(Oid oid, std::string_view name, std::uint32_t type_val,
                               std::uint32_t len) {
    SysTypeRow row{};
    row.oid = oid;
    SetName(row.name, name);
    row.type_val = type_val;
    row.len = len;
    return InsertRow(store_, kCatalogPageTypes, row, kBootstrapXid);
}

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
    if (clustered_type != ClusteredType::kHeap) {
        // ClusteredType::kBtree needs the not-yet-ported btree code
        // (kernel/kds/btree.c) - rejected rather than half-implemented.
        return Status::InvalidArgument("btree clustered tables are not implemented yet");
    }

    auto created = store_.CreateNew();
    if (!created.ok()) return created.status();
    auto [root_id, root_bytes] = created.value();

    auto root_page = heap::PageView::CreateEmpty(root_bytes, 0);
    if (!root_page.ok()) return root_page.status();

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
    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects);
    if (!rows.ok()) return rows.status();

    for (const auto& row : rows.value()) {
        if (row.type_oid == kTypeTable && NameView(row.name) == name) {
            return row.oid;
        }
    }
    return Status::NotFound("no table with this name");
}

StatusOr<std::vector<SysObjectRow>> Catalog::ListTables() {
    auto rows = ScanAll<SysObjectRow>(store_, kCatalogPageObjects);
    if (!rows.ok()) return rows.status();

    std::vector<SysObjectRow> tables;
    for (auto& row : rows.value()) {
        if (row.type_oid == kTypeTable) tables.push_back(row);
    }
    return tables;
}

StatusOr<Schema> Catalog::BuildSchemaFromColumns(Oid rel_id) {
    auto rows = ScanAll<SysColumnRow>(store_, kCatalogPageColumns);
    if (!rows.ok()) return rows.status();

    Schema schema;
    for (const auto& row : rows.value()) {
        if (row.rel_id == rel_id) schema.columns.push_back(row);
    }

    if (schema.columns.empty()) {
        return Status::NotFound("no columns for this rel_id");
    }
    return schema;
}

StatusOr<TableAccess> Catalog::InitTableAccess(Oid namespace_oid, Oid oid) {
    auto table_row = GetSysTableRow(oid);
    if (!table_row.ok()) return table_row.status();

    auto schema = BuildSchemaFromColumns(oid);
    if (!schema.ok()) return schema.status();

    TableAccess access{};
    access.namespace_oid = namespace_oid;
    access.oid = oid;
    access.schema = std::move(schema.value());
    access.desc_page_id = table_row.value().desc_page_id;
    access.clustered_type = table_row.value().clustered_type;
    return access;
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

        row.value().desc_page_id = new_desc_page_id;
        auto encoded = row.value().Encode();
        return page.OverwriteTuple(i, encoded, tuple.value().xmin, tuple.value().xmax,
                                    tuple.value().undo_ptr);
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
