#include "kds/catalog/catalog.hpp"

#include "kds/catalog/foreign_key.hpp"
#include "kds/parser/fingerprint.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/varheap.hpp"

namespace kds::catalog {

namespace {

// Scans every live row of type RowT out of the heap page at `page_id`.
// Dead slots (RowT::Decode's caller never sees them - ReadTuple() itself
// reports NotFound for a dead slot) are skipped.
// **GetForRead, not Get.** A scan reads and modifies nothing, and Get()
// marks the frame dirty by convention rather than by what the caller
// actually wrote (page_store.hpp) - so every catalog lookup used to dirty a
// catalog page, and every checkpoint wrote all nine of them back having
// changed none.
//
// Multicore turned that waste into a refusal: a peer may read the catalog
// pages and may never write one (workplan-crosscore.md P6), so a read that
// dirties is a read a peer cannot do at all. Which is the ownership check
// doing exactly its job - the bug predates it by a long way.
template <typename RowT>
StatusOr<std::vector<RowT>> ScanAll(storage::PageStore& store, PageId page_id) {
    auto bytes = store.GetForRead(page_id);
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
    static constexpr std::array<SysTableBootstrap, 9> kSysTables{{
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
        // sys.access_stats is a fixed-offset typed row like the six above,
        // so it needs nothing beyond a page and its two catalog rows -
        // unlike sys.pattern_defs, which stores text and therefore had to
        // become a real row-codec relation in phase 5.
        {kSysAccessStatsTable, "access_stats", kCatalogPageAccessStats},
        // sys.cabins, same shape again: a page and its two catalog rows.
        // It issues Keystone ids (a Cabin's `cabin_id` comes from this
        // relation's own `next_id`, like sys.patterns' oid), which is why
        // it needs the sys.tables row and not just the page.
        {kSysCabinsTable, "cabins", kCatalogPageCabins},
        // sys.fkeys, same shape again. It issues Keystone ids for the same
        // reason sys.cabins does - an `fk_id` comes from this relation's own
        // `next_id`, not from GenerateUserOid(), which restarts every boot.
        {kSysFkeysTable, "fkeys", kCatalogPageFkeys},
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
        // kInvalidPageId: the bootstrap catalog tables encode their rows
        // through SysXxxRow::Encode(), never the schema-driven row codec,
        // so nothing of theirs can ever spill.
        if (Status s = InsertRelationRow(t.oid, kNamespaceSys, t.name, t.page_id,
                                          ClusteredType::kHeap, kInvalidPageId);
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

    // Phase 5: sys.pattern_defs, the one catalog relation stored in
    // ordinary user tuple format (well_known.hpp explains why). It is built
    // here by hand rather than through CreateTable() for two reasons:
    // CreateTable() takes its oids from GenerateUserOid(), which is
    // in-memory and restarts every boot, and it roots the relation on a
    // dynamically allocated page, which nothing could find at bootstrap
    // without first reading the catalog it is part of.
    //
    // It must come after phase 4: BuildPatternDefsSchema() names its column
    // types by the type_val tags phase 4 just registered.
    if (Status s = BootstrapPatternDefs(); !s.ok()) return s;

    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "bootstrapped " + std::to_string(kSysTables.size() + 1) +
                                  " system tables and " + std::to_string(kTypes.size()) +
                                  " types on the fixed catalog pages");
    }
    return Status::OK();
}

namespace {

Schema PatternDefsSchema() {
    // Five columns. The first is not in the spec's list and has to be:
    // every relation's first column is its system-generated Keystone pk
    // (invariant 11), and a relation stored in user tuple format is a
    // relation that has one.
    //
    // `param_count` is the declaration's **materialized arity** - the number
    // of value slots the body has, per spec section 3.3. Stored rather than
    // recomputed because recomputing it means re-fingerprinting the source
    // text, and the two could then disagree for a row written by an older
    // build. There is deliberately **no sibling relation for the parameters
    // themselves**: their names and types are recoverable from
    // `source_text`, which is the canon, and a second relation would be a
    // second thing to keep consistent with it.
    //
    // `name` and `source_text` are varchar, so the relation can spill and
    // gets a var-heap chain. A declaration longer than one var-heap page
    // (8144 bytes) is refused at CREATE PATTERN rather than chained - the
    // spilled-value size cap is still open, and this feature does not settle
    // it.
    Schema schema;
    auto column = [](Oid oid, std::uint32_t pos, std::string_view name, std::uint32_t type_val,
                     std::uint32_t len) {
        SysColumnRow col{};
        col.oid = oid;
        col.rel_id = kSysPatternDefsTable;
        col.pos = pos;
        SetName(col.name, name);
        col.type_val = type_val;
        col.len = len;
        col.notnull = true;
        return col;
    };
    schema.columns.push_back(column(kSysPatternDefsColumnOidBase + 0, 0, "id", kTypeValInt64, 8));
    schema.columns.push_back(
        column(kSysPatternDefsColumnOidBase + 1, 1, "pattern_id", kTypeValUint64, 8));
    schema.columns.push_back(
        column(kSysPatternDefsColumnOidBase + 2, 2, "param_count", kTypeValInt32, 4));
    schema.columns.push_back(
        column(kSysPatternDefsColumnOidBase + 3, 3, "name", kTypeValVarchar, 0));
    schema.columns.push_back(
        column(kSysPatternDefsColumnOidBase + 4, 4, "source_text", kTypeValVarchar, 0));
    return schema;
}

}  // namespace

Status Catalog::BootstrapPatternDefs() {
    const Schema schema = PatternDefsSchema();

    // Refused here rather than at the first CREATE PATTERN, for the reason
    // CreateTable() gives: a relation whose row size cannot be computed is
    // one no row could ever be written to, and finding that out at bootstrap
    // is finding it out before there is anything to lose. It also proves the
    // schema above is expressible under whatever inline_cell_width this
    // instance pinned.
    if (auto layout = RowLayout::Build(schema, inline_cell_width_); !layout.ok()) {
        return layout.status();
    }

    auto created = store_.CreateAt(kCatalogPagePatternDefs);
    if (!created.ok()) return created.status();
    // min_key 0: like the other catalog pages, this relation is scanned in
    // full - by name or by pattern_id, never by key range.
    auto root = heap::PageView::CreateEmpty(created.value(), 0);
    if (!root.ok()) return root.status();

    auto varheap_root = varheap::CreateChain(store_);
    if (!varheap_root.ok()) return varheap_root.status();

    if (Status s = InsertObjectRow(kSysPatternDefsTable, kNamespaceSys, kTypeTable,
                                    "pattern_defs");
        !s.ok()) {
        return s;
    }
    if (Status s = InsertRelationRow(kSysPatternDefsTable, kNamespaceSys, "pattern_defs",
                                      kCatalogPagePatternDefs, ClusteredType::kHeap,
                                      varheap_root.value());
        !s.ok()) {
        return s;
    }
    for (const auto& col : schema.columns) {
        if (Status s = InsertColumnRow(col.oid, kSysPatternDefsTable, col.pos,
                                        NameView(col.name), col.type_val, col.len, col.notnull);
            !s.ok()) {
            return s;
        }
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
    // After the local invalidation, never before: the hook flushes the
    // catalog pages and tells peers to re-read, and a peer that re-read
    // while this instance still held stale entries would be reading a
    // catalog its own owner disagrees with.
    if (on_invalidate_) on_invalidate_();
}

void Catalog::InvalidateFromPeer() {
    // No version bump. `catalog_version_` is the counter parser-v2.md I5
    // stamps *this instance's* bound statements with; another core's DDL is
    // not an event in this instance's numbering, and advancing it here
    // would invalidate bound statements for a reason they cannot check.
    // What has to go is the cached content.
    cache_.Invalidate();
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "cache invalidated by a peer's DDL");
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
                                   PageId desc_page_id, ClusteredType clustered_type,
                                   PageId varheap_page_id, std::uint32_t owner_core) {
    SysTableRow row{};
    row.oid = oid;
    row.namespace_oid = namespace_oid;
    SetName(row.name, name);
    row.desc_page_id = desc_page_id;
    row.clustered_type = clustered_type;
    row.next_id = kFirstRowId;
    row.varheap_page_id = varheap_page_id;
    row.owner_core = owner_core;
    Status s = InsertRow(store_, kCatalogPageTables, row, kBootstrapXid);
    if (s.ok()) BumpVersion("sys.tables insert");
    return s;
}

Status Catalog::InsertColumnRow(Oid oid, Oid rel_id, std::uint32_t pos, std::string_view name,
                                 std::uint32_t type_val, std::uint32_t len, bool notnull,
                                 std::uint8_t cabin_policy) {
    SysColumnRow row{};
    row.oid = oid;
    row.rel_id = rel_id;
    row.pos = pos;
    SetName(row.name, name);
    row.type_val = type_val;
    row.len = len;
    row.notnull = notnull;
    row.cabin_policy = cabin_policy;
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

    // Same argument, extended by the fixed-length rule: the relation's row
    // size is a schema constant, so if it cannot be computed - a column
    // with no decided on-disk width, or a row wider than a page - there is
    // no row this table could hold either. Computing it here also means
    // the layout every later InitTableAccess() builds is known to succeed.
    if (auto layout = RowLayout::Build(schema, inline_cell_width_); !layout.ok()) {
        return layout.status();
    }

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

    // The var-heap root, allocated here or not at all. Eager rather than
    // on-first-spill, and the reason is the catalog cache's rule
    // (catalog_cache.hpp): a root allocated lazily would be a fact that
    // changes without DDL, and this one is cached on every TableAccess.
    // Allocating at CREATE TABLE keeps it DDL-immutable. The cost is one
    // page per relation that *could* spill, whether or not it ever does -
    // which is why relations that cannot spill get kInvalidPageId and no
    // page at all.
    PageId varheap_root = kInvalidPageId;
    if (SchemaCanSpill(schema)) {
        auto created_varheap = varheap::CreateChain(store_);
        if (!created_varheap.ok()) return created_varheap.status();
        varheap_root = created_varheap.value();
    }

    Oid new_oid = GenerateUserOid();

    // Placement (docs/workplan-crosscore.md M1). The rotation counter is
    // how many relations already exist, read off the page rather than held
    // in memory: it has to survive a restart, and the oid cannot serve
    // because GenerateUserOid() restarts at kUserOidStart every boot
    // (core_placement.hpp says why that matters). Nothing here decides the
    // policy - AssignOwnerCore does, and it is `[PROPOSED]`.
    auto existing_relations = ScanAll<SysTableRow>(store_, kCatalogPageTables);
    if (!existing_relations.ok()) return existing_relations.status();
    // DDL runs on the system core and allocates from its free map, so the
    // relation's pages are the system core's - and a relation must be owned
    // by the core that can fault its pages (core_placement.hpp).
    const std::uint32_t owner_core =
        AssignOwnerCore(kSystemCore, core_count_, existing_relations.value().size());

    if (Status s = InsertObjectRow(new_oid, namespace_oid, kTypeTable, name); !s.ok()) {
        return s;
    }
    if (Status s = InsertRelationRow(new_oid, namespace_oid, name, root_id, clustered_type,
                                      varheap_root, owner_core);
        !s.ok()) {
        return s;
    }

    for (const auto& col : schema.columns) {
        Status s = InsertColumnRow(GenerateUserOid(), new_oid, col.pos, NameView(col.name),
                                    col.type_val, col.len, col.notnull, col.cabin_policy);
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
    access.varheap_page_id = table_row.value().varheap_page_id;
    access.owner_core = table_row.value().owner_core;

    // The row-size constant, computed once here and carried for the life of
    // the entry (invariant 13). This is the only place it is derived: a
    // second computation on an execute path is a second chance to disagree
    // with the bytes already on disk.
    auto layout = RowLayout::Build(access.schema, inline_cell_width_);
    if (!layout.ok()) return layout.status();
    access.layout = std::move(layout.value());

    // The relation's cabins, in **one** sys.cabins scan rather than one per
    // column. A DDL fact, so it belongs on this entry and dies with it
    // (schema.hpp says why it is a mask rather than a per-step probe).
    //
    // A failure here is not fatal to opening the relation: a Cabin the
    // compiler cannot see costs the acceleration and never a row, since the
    // step compiles to the scan it would have compiled to anyway. But an
    // unreadable catalog page is not something to swallow either, so it is
    // reported - the relation is opened by the same call that would then
    // fail on its first read.
    auto cabins = ListCabins();
    if (!cabins.ok()) return cabins.status();
    access.cabin_ids.assign(access.schema.columns.size(), TableAccess::CabinRef{});
    for (const SysCabinRow& cabin : cabins.value()) {
        if (cabin.rel_oid != oid) continue;
        if (!IsCabinServing(cabin)) continue;
        if (cabin.column_no == 0 || cabin.column_no >= access.cabin_ids.size()) continue;
        access.cabin_ids[cabin.column_no] = TableAccess::CabinRef{cabin.cabin_id, cabin.origin};
        if (cabin.column_no < 64) {
            access.cabin_mask |= (std::uint64_t{1} << cabin.column_no);
        }
    }

    // The relation's foreign keys at **both** ends, in one sys.fkeys scan
    // (schema.hpp says why both lists live here). Unlike the Cabin scan
    // above, a failure here is not a lost accelerator: a foreign key the
    // write path cannot see is a constraint that does not run, so an
    // unreadable sys.fkeys must fail opening the relation rather than open
    // it unconstrained.
    auto fkeys = ListForeignKeys();
    if (!fkeys.ok()) return fkeys.status();
    for (const SysFkeyRow& fk : fkeys.value()) {
        if (fk.child_rel_oid == oid) {
            access.fkeys_out.push_back(
                ForeignKeyRef{fk.fk_id, fk.parent_rel_oid, fk.child_column_no, fk.flags});
        }
        if (fk.parent_rel_oid == oid) {
            access.fkeys_in.push_back(
                ForeignKeyRef{fk.fk_id, fk.child_rel_oid, fk.child_column_no, fk.flags});
        }
    }

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

// The identity + location + policy halves of a pattern row - everything DDL
// alone can change. Heat is dropped on the floor here on purpose; see
// schema.hpp.
PatternAccess AccessOf(const SysPatternRow& row) noexcept {
    PatternAccess access{};
    access.oid = row.oid;
    access.pattern_id = row.pattern_id;
    access.fingerprint_version = row.fingerprint_version;
    access.waystone_root = row.waystone_root;
    access.stmt_class = row.stmt_class;
    access.dir_depth = row.dir_depth;
    access.origin = row.origin;
    access.flags = row.flags;
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
                                                         std::uint8_t stmt_class,
                                                         std::uint8_t origin,
                                                         std::uint16_t flags) {
    if (origin != kOriginAuto && origin != kOriginUser) {
        return Status::InvalidArgument("catalog: unknown pattern origin");
    }

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
    // No directory until one is built. A declared pattern gets its
    // directory immediately afterwards, through SetPatternWaystoneRoot(),
    // because `expected_instances` is a pre-sizing knob and pre-sizing a
    // directory that does not exist yet means nothing; an auto pattern
    // waits for its first trail. Either way this row is written with no
    // directory, so the root/depth pair is only ever set by its one writer.
    row.dir_depth = 0;
    row.origin = origin;
    row.flags = flags;

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

Status Catalog::MutatePatternRow(std::uint64_t pattern_id,
                                  const std::function<void(SysPatternRow&)>& mutate) {
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
        // The same version filter GetSysPatternRow() applies, and for the
        // same reason: a row from another revision names a shape that is
        // not the one it claims, so it is not this pattern and must not be
        // written through.
        if (!parser::IsCurrentFingerprintVersion(row.value().fingerprint_version)) continue;

        mutate(row.value());
        auto encoded = row.value().Encode();
        // In place, never delete+insert: the row size is unchanged, and a
        // pattern that briefly does not exist is a pattern a concurrent
        // lookup misses.
        return page.OverwriteTuple(i, encoded, tuple.value().trx_id, tuple.value().undo_ptr);
    }

    return Status::NotFound("no sys.patterns row for this pattern_id");
}

Status Catalog::SetPatternWaystoneRoot(std::uint64_t pattern_id, PageId root,
                                        std::uint8_t depth) {
    if (Status s = CheckWaystonePair(root, depth); !s.ok()) return s;

    Status s = MutatePatternRow(pattern_id, [root, depth](SysPatternRow& row) {
        row.waystone_root = root;
        row.dir_depth = depth;
    });
    if (!s.ok()) return s;

    // Updated in place rather than invalidated, and only on success: a
    // failed overwrite moved nothing, and publishing the new pair into the
    // cache would make it disagree with the page.
    cache_.UpdatePatternWaystone(pattern_id, root, depth);
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "pattern " + std::to_string(pattern_id) +
                                   " waystone root=" + std::to_string(root) +
                                   " depth=" + std::to_string(depth));
    }
    return Status::OK();
}

Status Catalog::SetPatternOrigin(std::uint64_t pattern_id, std::uint8_t origin,
                                  std::uint16_t flags) {
    if (origin != kOriginAuto && origin != kOriginUser) {
        return Status::InvalidArgument("catalog: unknown pattern origin");
    }

    Status s = MutatePatternRow(pattern_id, [origin, flags](SysPatternRow& row) {
        row.origin = origin;
        row.flags = flags;
    });
    if (!s.ok()) return s;

    // **`waystone_root` and `dir_depth` are deliberately untouched.** This
    // is the adoption path, and adopting an auto-registered pattern must
    // not throw away a warm cache: the trails recorded under it are still
    // trails for the same shape, and the directory that indexes them is
    // still the right depth for the traffic that built it.
    cache_.UpdatePatternOrigin(pattern_id, origin, flags);
    if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
        log_->Debug("catalog", "pattern " + std::to_string(pattern_id) + " origin=" +
                                   std::to_string(origin) + " flags=" + std::to_string(flags));
    }
    return Status::OK();
}

Status Catalog::TouchPattern(std::uint64_t pattern_id, std::uint64_t last_seen) {
    return MutatePatternRow(pattern_id, [last_seen](SysPatternRow& row) {
        // Saturating, not wrapping: a use_count that rolled over would make
        // the hottest pattern in the database look like the coldest, which
        // is the one reading retention must never be handed.
        if (row.use_count != std::numeric_limits<std::uint32_t>::max()) ++row.use_count;
        row.last_seen = last_seen;
    });
    // No cache update: heat is not a cached fact (catalog.hpp), so there is
    // nothing to keep coherent.
}

Status Catalog::RetirePattern(std::uint64_t pattern_id) {
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

        if (Status s = page.RetireSlot(i); !s.ok()) return s;

        // The cache holds an entry keyed on this pattern_id, and it is now
        // a fact about a row that no longer exists. There is no
        // "UpdatePattern..." for removal, so this drops everything - which
        // is heavier than the in-place updates beside it and deliberately
        // so: a dangling PatternAccess would outlive its row, and DROP is
        // rare enough that one re-scan costs nothing worth protecting.
        BumpVersion("sys.patterns retire");
        if (log_ != nullptr && log_->enabled(LogLevel::kDebug)) {
            log_->Debug("catalog", "retired pattern " + std::to_string(pattern_id));
        }
        return Status::OK();
    }
    return Status::NotFound("no sys.patterns row for this pattern_id");
}

Status Catalog::RecordAccess(std::uint8_t kind, Oid rel_id, std::uint64_t column_mask,
                              std::uint64_t last_seen) {
    if (kind == kAccessKindUnset) {
        return Status::InvalidArgument("catalog: access kind 0 is reserved for an unset row");
    }

    auto bytes = store_.Get(kCatalogPageAccessStats);
    if (!bytes.ok()) return bytes.status();

    heap::PageView page(bytes.value());
    const std::uint16_t n = page.slot_count();
    std::size_t live = 0;
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) {
            if (tuple.status().code() == StatusCode::kNotFound) continue;
            return tuple.status();
        }
        ++live;

        auto row = SysAccessStatRow::Decode(tuple.value().payload);
        if (!row.ok()) return row.status();
        if (row.value().kind != kind || row.value().rel_id != rel_id ||
            row.value().column_mask != column_mask) {
            continue;
        }

        // Saturating for the reason rows.hpp gives: a wrapped count would
        // invert the ranking this exists to produce.
        if (row.value().use_count != std::numeric_limits<std::uint64_t>::max()) {
            ++row.value().use_count;
        }
        row.value().last_seen = last_seen;
        auto encoded = row.value().Encode();
        // In place - the row size is fixed, so there is nothing to move.
        return page.OverwriteTuple(i, encoded, tuple.value().trx_id, tuple.value().undo_ptr);
    }

    // A shape nobody has recorded. Admitted only under the cap: an
    // unbounded catalog relation written from the statement path is the
    // failure this guards, and refusing a *new* shape while continuing to
    // count every known one degrades the statistic rather than the server.
    if (live >= kMaxAccessShapes) {
        return Status::ResourceExhausted("catalog: sys.access_stats is at its shape cap");
    }

    SysAccessStatRow row{};
    row.rel_id = rel_id;
    row.column_mask = column_mask;
    row.use_count = 1;
    row.last_seen = last_seen;
    row.kind = kind;
    // No BumpVersion(): nothing cached is derived from these rows, and the
    // argument is the one sys.patterns' registration already makes - a
    // statistic appearing cannot stale a cached relation, and this runs on
    // the statement path where a cache drop would dangle a held pointer.
    return InsertRow(store_, kCatalogPageAccessStats, row, kBootstrapXid);
}

StatusOr<std::uint64_t> Catalog::CreateCabin(Oid rel_oid, std::uint16_t col_pos,
                                              std::uint8_t origin) {
    if (origin != kCabinOriginAuto && origin != kCabinOriginUser) {
        return Status::InvalidArgument("catalog: unknown cabin origin");
    }
    if (col_pos == 0) {
        // Not a policy choice and not a limitation: the pk column's Cabin
        // is the clustered tree, which is already authoritative for every
        // value rather than for the observed ones (spec section 2).
        return Status::InvalidArgument(
            "catalog: the primary-key column needs no cabin - the clustered tree is its cabin");
    }

    // Through InitTableAccess() rather than the sys.columns scan, because
    // the column count has to come from the same schema the compiler will
    // see. A cabin on a column that relation does not have would compile to
    // nothing and be invisible until someone read the catalog by hand.
    auto access = InitTableAccess(rel_oid);
    if (!access.ok()) return access.status();
    if (col_pos >= access.value()->schema.columns.size()) {
        return Status::InvalidArgument("catalog: relation oid " + std::to_string(rel_oid) +
                                       " has no column at position " + std::to_string(col_pos));
    }

    // The column's declared policy, enforced here because this is the one
    // door every Cabin comes through - an operator's `CREATE CABIN`, a
    // `CABIN` clause at CREATE TABLE, and whatever auto-creation is built
    // later. `NO CABIN` means never, by any route (rows.hpp), and a check
    // that lived in the DDL layer instead would be a check the future
    // promotion pipeline could forget.
    const SysColumnRow& column = access.value()->schema.columns[col_pos];
    if (!CabinPolicyPermitsCreation(column.cabin_policy)) {
        return Status::InvalidArgument("catalog: column '" +
                                       std::string(NameView(column.name)) +
                                       "' was declared NO CABIN");
    }

    if (FindCabinOnColumn(rel_oid, col_pos).ok()) {
        return Status::AlreadyExists("catalog: this column already has a cabin");
    }

    auto cabin_id = AllocateRowId(kSysCabinsTable);
    if (!cabin_id.ok()) return cabin_id.status();

    SysCabinRow row{};
    row.cabin_id = cabin_id.value();
    row.rel_oid = rel_oid;
    // Nothing is observed by creating a Cabin: the read path's miss branch
    // fills it (spec section 4). A count written here would be a claim about
    // runtime state made by DDL.
    row.observed_ct = 0;
    row.column_no = col_pos;
    row.origin = origin;
    // Active immediately. kCabinStatusBuilding exists for a phase-2
    // background build and has no writer while the only way to fill a Cabin
    // is a statement that was going to scan anyway.
    row.status = kCabinStatusActive;

    if (Status s = InsertRow(store_, kCatalogPageCabins, row, kBootstrapXid); !s.ok()) {
        return s;
    }

    // **Bumped, unlike RegisterPattern().** A pattern appearing stales
    // nothing cached; a Cabin appearing stales `TableAccess::cabin_mask` on
    // every held entry for this relation, which is a fact the compiler
    // reads. That is what makes this DDL and not a statement-path write.
    BumpVersion("sys.cabins create");
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "created cabin " + std::to_string(row.cabin_id) + " on relation oid " +
                                  std::to_string(rel_oid) + " column " + std::to_string(col_pos));
    }
    return row.cabin_id;
}

Status Catalog::DropCabin(std::uint64_t cabin_id) {
    auto bytes = store_.Get(kCatalogPageCabins);
    if (!bytes.ok()) return bytes.status();

    heap::PageView page(bytes.value());
    const std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) {
            if (tuple.status().code() == StatusCode::kNotFound) continue;
            return tuple.status();
        }
        auto row = SysCabinRow::Decode(tuple.value().payload);
        if (!row.ok()) return row.status();
        if (row.value().cabin_id != cabin_id) continue;

        // Retired, not delete-marked - RetirePattern() states the argument,
        // and it is the same one: a catalog read has no snapshot to filter a
        // mark against, so a marked row would still be found by every lookup
        // and a re-created Cabin would collide with a row nobody can see.
        if (Status s = page.RetireSlot(i); !s.ok()) return s;

        BumpVersion("sys.cabins drop");
        if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
            log_->Info("catalog", "dropped cabin " + std::to_string(cabin_id));
        }
        return Status::OK();
    }
    return Status::NotFound("no sys.cabins row for this cabin_id");
}

StatusOr<std::vector<SysCabinRow>> Catalog::ListCabins() {
    return ScanAll<SysCabinRow>(store_, kCatalogPageCabins);
}

StatusOr<SysCabinRow> Catalog::FindCabinOnColumn(Oid rel_oid, std::uint16_t col_pos) {
    auto rows = ListCabins();
    if (!rows.ok()) return rows.status();
    for (const SysCabinRow& row : rows.value()) {
        if (row.rel_oid == rel_oid && row.column_no == col_pos) return row;
    }
    return Status::NotFound("no cabin on this column");
}

StatusOr<std::uint64_t> Catalog::CreateForeignKey(Oid child_rel_oid, std::uint16_t child_column_no,
                                                  Oid parent_rel_oid, std::uint16_t flags) {
    auto child = InitTableAccess(child_rel_oid);
    if (!child.ok()) return child.status();
    auto parent = InitTableAccess(parent_rel_oid);
    if (!parent.ok()) return parent.status();

    if (child_column_no >= child.value()->schema.columns.size()) {
        return Status::InvalidArgument("catalog: relation oid " + std::to_string(child_rel_oid) +
                                       " has no column at position " +
                                       std::to_string(child_column_no));
    }

    // The declaration's own checks, shared with the CREATE TABLE pre-check
    // (foreign_key.hpp) so the two doors cannot answer differently.
    if (Status s = CheckForeignKeyDeclaration(*parent.value(),
                                              child.value()->schema.columns[child_column_no],
                                              child_column_no);
        !s.ok()) {
        return s.WithContext("catalog");
    }
    if (Status s = CheckForeignKeyColocation(*parent.value(), *child.value()); !s.ok()) {
        return s.WithContext("catalog");
    }

    // One foreign key per (child, column). A second would mean a column
    // referencing two parents, which under F1 - the value *is* a parent's
    // Keystone id - would require one id to name a row in both.
    if (FindForeignKeyOnColumn(child_rel_oid, child_column_no).ok()) {
        return Status::AlreadyExists("catalog: this column already has a foreign key");
    }

    auto fk_id = AllocateRowId(kSysFkeysTable);
    if (!fk_id.ok()) return fk_id.status();

    SysFkeyRow row{};
    row.fk_id = fk_id.value();
    row.child_rel_oid = child_rel_oid;
    row.parent_rel_oid = parent_rel_oid;
    row.child_column_no = child_column_no;
    row.flags = flags;

    if (Status s = InsertRow(store_, kCatalogPageFkeys, row, kBootstrapXid); !s.ok()) {
        return s;
    }

    // **Bumped**, and note which entries it has to reach: a new foreign key
    // stales `fkeys_out` on the child *and* `fkeys_in` on the parent, which
    // is a relation this call's arguments do not otherwise touch. That is
    // why this is a global invalidation and not an in-place update of the
    // child's cached entry.
    BumpVersion("sys.fkeys create");
    if (log_ != nullptr && log_->enabled(LogLevel::kInfo)) {
        log_->Info("catalog", "created foreign key " + std::to_string(row.fk_id) + " on relation "
                                  "oid " + std::to_string(child_rel_oid) + " column " +
                                  std::to_string(child_column_no) + " referencing relation oid " +
                                  std::to_string(parent_rel_oid));
    }
    return row.fk_id;
}

StatusOr<std::vector<SysFkeyRow>> Catalog::ListForeignKeys() {
    return ScanAll<SysFkeyRow>(store_, kCatalogPageFkeys);
}

StatusOr<SysFkeyRow> Catalog::FindForeignKeyOnColumn(Oid child_rel_oid,
                                                     std::uint16_t child_column_no) {
    auto rows = ListForeignKeys();
    if (!rows.ok()) return rows.status();
    for (const SysFkeyRow& row : rows.value()) {
        if (row.child_rel_oid == child_rel_oid && row.child_column_no == child_column_no) {
            return row;
        }
    }
    return Status::NotFound("no foreign key on this column");
}

StatusOr<std::vector<SysAccessStatRow>> Catalog::ListAccessStats() {
    return ScanAll<SysAccessStatRow>(store_, kCatalogPageAccessStats);
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
