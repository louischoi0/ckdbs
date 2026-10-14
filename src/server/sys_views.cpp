#include "kds/server/sys_views.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <span>
#include <utility>

#include "kds/catalog/keystone_budget.hpp"
#include "kds/catalog/well_known.hpp"
#include "kds/exec/assertion_catalog.hpp"
#include "kds/exec/catalog_view.hpp"
#include "kds/exec/transition_catalog.hpp"
#include "kds/exec/step_chain.hpp"
#include "kds/server/command_dispatcher.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/stats/optimizer_signals.hpp"
#include "kds/stats/relayout_planner.hpp"
#include "kds/stats/statement_stats.hpp"

// The builders of every `sys` view (`include/kds/server/sys_views.hpp`).
//
// ---- Locking ---------------------------------------------------------------
//
// A builder reads what the `SHOW` handler it replaced read, under the same
// holds: the catalog's typed readers, the page store's pinned reads, the
// Cabin store's and the enforcer's own latched accessors, and the cabin
// optimizer under `cabin_optimizer_view_latch_`. It takes no hold of its
// own and keeps none past its return; the rows it returns are copies.

namespace kds::server {

namespace {

bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

constexpr std::uint32_t kInt = catalog::kTypeValUint64;
constexpr std::uint32_t kStr = catalog::kTypeValVarchar;
// UTC epoch microseconds (`docs/spec/types.md` TY4), compared and rendered
// as a timestamp, so `WHERE started > '...'` reads as written.
constexpr std::uint32_t kTimestamp = catalog::kTypeValTimestamp;

// The registry, in the order `SysViewNames()` reports them. The first five
// are the catalog views that predate BO.
constexpr SysViewSpec kViews[] = {
    {"tables"},
    {"columns", ViewKeyMode::kChecked, "rel_name", kStr},
    {"objects"},
    {"types"},
    {"patterns"},
    {"namespaces"},
    {"indexes"},
    {"fkeys"},
    {"access_stats"},
    {"relation_stats"},
    {"cabins"},
    {"assertions"},
    {"transitions"},
    {"relayout", ViewKeyMode::kChecked, "name", kStr},
    {"cabin_optimizer"},
    {"lock_waits"},
    {"statement_stats"},
    {"page_slots", ViewKeyMode::kRequired, "page_id", kInt},
    {"meta"},
};

parser::AstValue Int(std::uint64_t v) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = static_cast<std::int64_t>(v);
    // `raw_int_text` carries a uint64 above INT64_MAX (row_codec.hpp).
    if (v > static_cast<std::uint64_t>(INT64_MAX)) out.raw_int_text = std::to_string(v);
    return out;
}

parser::AstValue Str(std::string_view v) {
    parser::AstValue out;
    out.type = parser::ValueType::kStr;
    out.str_val = std::string(v);
    return out;
}

// A column that has no value on this row - an unreadable index's height,
// a Cabin's counters with no store - rather than a 0 that reads as one.
parser::AstValue Null() {
    parser::AstValue out;
    out.type = parser::ValueType::kNull;
    return out;
}

parser::AstValue Bool(bool v) { return Int(v ? 1 : 0); }

// A UTC epoch-microsecond instant, declared `kTimestamp`.
parser::AstValue Timestamp(std::int64_t us) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = us;
    return out;
}

// A transaction id, NULL where there is none: 0 names no transaction.
parser::AstValue TrxId(std::uint64_t id) { return id == 0 ? Null() : Int(id); }

// A relation's name from `RelationNameOf`, or NULL where the oid names no
// relation: its `oid=N` fallback is a label, not a name - no identifier
// holds `=` - and the view's `*_oid` column already carries the number.
parser::AstValue RelName(std::string name) {
    return name.find('=') == std::string::npos ? Str(name) : Null();
}

// A percentage of `of`, NULL where nothing was counted: a rate over no
// lookups is unknown, not 0.
parser::AstValue PercentOf(std::uint64_t part, std::uint64_t of) {
    return of == 0 ? Null() : Int(part * 100 / of);
}

// A slot's payload in hex: any byte value, ASCII out.
std::string HexEncode(std::span<const std::byte> bytes) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::byte b : bytes) {
        const auto v = static_cast<unsigned char>(b);
        out.push_back(kHexDigits[v >> 4]);
        out.push_back(kHexDigits[v & 0x0F]);
    }
    return out;
}

void Define(exec::CatalogView& view,
            std::initializer_list<std::pair<const char*, std::uint32_t>> columns) {
    for (const auto& [name, type] : columns) {
        view.column_names.emplace_back(name);
        view.column_types.push_back(type);
    }
}

std::string ClusteredName(catalog::ClusteredType type) {
    return type == catalog::ClusteredType::kBtree ? "BTREE" : "HEAP";
}

// A column's name, or its position when the schema cannot say - a dropped
// column's included, whose name left the SQL surface (BW-R6) while a
// `sys.access_stats` mask or a Cabin controller entry may still carry it.
std::string ColumnLabel(const StatusOr<const catalog::TableAccess*>& access, std::size_t col) {
    if (access.ok() && col < access.value()->schema.columns.size() &&
        !access.value()->schema.columns[col].dropped()) {
        return std::string(catalog::NameView(access.value()->schema.columns[col].name));
    }
    return std::to_string(col);
}

}  // namespace

const SysViewSpec* FindSysView(std::string_view name) noexcept {
    for (const SysViewSpec& spec : kViews) {
        if (IEquals(name, spec.name)) return &spec;
    }
    return nullptr;
}

std::vector<std::string> SysViewNames() {
    std::vector<std::string> out;
    for (const SysViewSpec& spec : kViews) out.emplace_back(spec.name);
    return out;
}

StatusOr<exec::CatalogView> CommandDispatcher::ReadSysView(const SysViewSpec& spec,
                                                           Session& session,
                                                           const parser::AstValue* key,
                                                           std::uint32_t byte) {
    // Engine state first: these read no session view, so none is taken -
    // `ViewFor` ends a statement boundary under an open DDL, a side effect
    // the old `SHOW` handlers never had.
    const std::string_view n = spec.name;
    if (n == "types") return TypesView();
    if (n == "patterns") return PatternsView();
    if (n == "access_stats") return AccessStatsView();
    if (n == "relation_stats") return RelationStatsView();
    if (n == "cabins") return CabinsView();
    if (n == "assertions") return AssertionsView();
    if (n == "transitions") return TransitionsView();
    if (n == "relayout") return RelayoutView(key, byte);
    if (n == "cabin_optimizer") return CabinOptimizerView(byte);
    if (n == "lock_waits") return LockWaitsView();
    if (n == "statement_stats") return StatementStatsView();
    if (n == "page_slots") return PageSlotsView(*key);
    if (n == "meta") return MetaView();

    // **BO-Q2**: a schema-object view reads under the session's view, as
    // `SHOW TABLES` always did.
    const std::optional<txn::ReadView> read_view = ViewFor(session);
    const txn::ReadView* view = read_view.has_value() ? &*read_view : nullptr;
    if (n == "tables") return TablesView(view);
    if (n == "columns") return ColumnsView(view, key);
    if (n == "objects") return ObjectsView(view);
    if (n == "namespaces") return NamespacesView(view);
    if (n == "indexes") return IndexesView(view);
    if (n == "fkeys") return FkeysView(view);
    return Status::NotFound("no catalog view named 'sys." + std::string(n) + "'");
}

// ---- The catalog's own relations ------------------------------------------

StatusOr<exec::CatalogView> CommandDispatcher::TablesView(const txn::ReadView* view_of) {
    exec::CatalogView view;
    Define(view, {{"oid", kInt},
                  {"namespace_oid", kInt},
                  {"name", kStr},
                  {"desc_page_id", kInt},
                  {"root_page_id", kInt},
                  {"clustered_type", kStr},
                  {"next_id", kInt},
                  {"spare_slots", kInt},
                  {"spare_free", kInt}});

    auto objects = catalog_.ListTables(view_of);
    if (!objects.ok()) return objects.status();
    for (const catalog::SysObjectRow& object : objects.value()) {
        auto row = catalog_.GetSysTableRow(object.oid);
        // A sys.objects entry with no sys.tables row is a catalog the engine
        // cannot explain: refused, never skipped, since an omission is
        // invisible in the result.
        if (!row.ok()) return row.status();
        auto root = CurrentRootOf(row.value());
        if (!root.ok()) return root.status();
        // A slot is free until a column takes it; a dropped column keeps its
        // slot (BW-R6, BW-R12). Only a relation with slots is asked.
        std::uint32_t free_slots = 0;
        if (row.value().spare_slots > 0) {
            auto access = catalog_.InitTableAccess(object.oid);
            if (!access.ok()) return access.status();
            free_slots = access.value()->layout.vacant_slot_bit_end -
                         access.value()->layout.vacant_slot_bit_begin;
        }
        view.rows.push_back({Int(row.value().oid), Int(row.value().namespace_oid),
                             Str(catalog::NameView(row.value().name)),
                             Int(row.value().desc_page_id), Int(root.value()),
                             Str(ClusteredName(row.value().clustered_type)),
                             Int(row.value().next_id), Int(row.value().spare_slots),
                             Int(free_slots)});
    }
    return view;
}

StatusOr<PageId> CommandDispatcher::CurrentRootOf(const catalog::SysTableRow& row) {
    // The clustered root the anchor names, where the relation has one; a
    // relation that predates its anchor is rooted at its descriptor page.
    if (row.anchor_page_id == kInvalidPageId) return row.desc_page_id;
    auto anchor = page_store_.GetForRead(row.anchor_page_id);
    if (anchor.ok() &&
        storage::ValidatePageHeader(anchor.value().bytes(), PageType::kAnchor).ok()) {
        return storage::AnchorClusteredRoot(anchor.value().bytes());
    }
    if (!anchor.ok() && anchor.status().code() == StatusCode::kCorruption) {
        return anchor.status();
    }
    return row.desc_page_id;
}

StatusOr<exec::CatalogView> CommandDispatcher::ObjectsView(const txn::ReadView* view_of) {
    exec::CatalogView view;
    Define(view, {{"oid", kInt},
                  {"namespace_oid", kInt},
                  {"type_oid", kInt},
                  {"rel_id", kInt},
                  {"name", kStr}});
    // `ListTables` is the only enumerator, and it lists table objects; the
    // view shows those and says so. `rel_id` is `pending_roots` in code
    // (BF-R2), 0 on every live table.
    auto objects = catalog_.ListTables(view_of);
    if (!objects.ok()) return objects.status();
    for (const catalog::SysObjectRow& object : objects.value()) {
        view.rows.push_back({Int(object.oid), Int(object.namespace_oid), Int(object.type_oid),
                             Int(object.pending_roots), Str(catalog::NameView(object.name))});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::ColumnsView(const txn::ReadView* view_of,
                                                           const parser::AstValue* rel_name) {
    exec::CatalogView view;
    Define(view, {{"rel_id", kInt},
                  {"rel_name", kStr},
                  {"pos", kInt},
                  {"name", kStr},
                  {"type_val", kInt},
                  {"type", kStr},
                  {"notnull", kInt},
                  {"tuple_key", kInt},
                  {"cabin", kStr},
                  {"references", kStr}});

    std::vector<catalog::SysObjectRow> relations;
    if (rel_name != nullptr) {
        // `DESCRIBE <t>`'s refusal, kept (BO-Q11): a name that resolves to
        // nothing is refused, under the session's view and with its
        // qualifier checked, never answered with no rows.
        const auto [qualifier, bare] = SplitQualifiedArg(rel_name->str_val);
        auto oid = catalog_.FindTableOidByName(bare, view_of);
        if (!oid.ok()) {
            return oid.status().WithMessage(oid.status().message() + " '" + rel_name->str_val +
                                            "' (byte " + std::to_string(rel_name->byte_offset) +
                                            ")");
        }
        if (Status s = catalog_.CheckRelationQualifier(qualifier, bare, oid.value(),
                                                       rel_name->byte_offset, view_of);
            !s.ok()) {
            return s;
        }
        auto all = catalog_.ListTables(view_of);
        if (!all.ok()) return all.status();
        for (const catalog::SysObjectRow& object : all.value()) {
            if (object.oid == oid.value()) relations.push_back(object);
        }
    } else {
        auto all = catalog_.ListTables(view_of);
        if (!all.ok()) return all.status();
        relations = std::move(all.value());
    }

    for (const catalog::SysObjectRow& object : relations) {
        // The catalog's own tables have no column rows - their layout is
        // compiled in (catalog/rows.hpp) - so they have no columns to list.
        auto schema = catalog_.BuildSchemaFromColumns(object.oid);
        if (!schema.ok()) {
            if (schema.status().code() == StatusCode::kNotFound) continue;
            return schema.status();
        }
        auto access = catalog_.InitTableAccess(object.oid);
        const std::string rel(catalog::NameView(object.name));
        for (std::size_t i = 0; i < schema.value().columns.size(); ++i) {
            const catalog::SysColumnRow& column = schema.value().columns[i];
            // `sys.columns` and `DESCRIBE` list live columns (BW-R6, BW-R12).
            if (column.dropped()) continue;
            // The declared type, not `len`, which for a decimal is the packed
            // (precision, scale); `ColumnTypeText` is its one reader.
            auto type_row = catalog_.ResolveTypeByVal(column.type_val);
            const std::string base =
                type_row.ok() ? std::string(catalog::NameView(type_row.value().name))
                              : "?type_val=" + std::to_string(column.type_val);
            const bool tuple_key = i == 0;
            parser::AstValue cabin = Null();
            parser::AstValue references = Null();
            if (!tuple_key) {
                const std::uint8_t policy = catalog::EffectiveCabinPolicy(column.cabin_policy);
                cabin = Str(policy == catalog::kCabinPolicyDisabled  ? "no"
                            : policy == catalog::kCabinPolicyEnabled ? "yes"
                                                                     : "auto");
                if (access.ok()) {
                    if (const catalog::ForeignKeyRef* fk =
                            access.value()->ForeignKeyOn(static_cast<std::uint16_t>(i));
                        fk != nullptr) {
                        references = RelName(RelationNameOf(fk->rel_oid));
                    }
                }
            }
            view.rows.push_back({Int(column.rel_id), Str(rel), Int(column.pos),
                                 Str(catalog::NameView(column.name)), Int(column.type_val),
                                 Str(catalog::ColumnTypeText(column, base)),
                                 Bool(column.notnull), Bool(tuple_key), std::move(cabin),
                                 std::move(references)});
        }
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::TypesView() {
    exec::CatalogView view;
    Define(view, {{"oid", kInt}, {"name", kStr}, {"type_val", kInt}, {"len", kInt}});
    auto types = catalog_.ListTypes();
    if (!types.ok()) return types.status();
    for (const catalog::SysTypeRow& type : *types.value()) {
        view.rows.push_back({Int(type.oid), Str(catalog::NameView(type.name)),
                             Int(type.type_val), Int(type.len)});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::PatternsView() {
    exec::CatalogView view;
    // `fetch_id`, not `pattern_id`, since AP-S2; a row of another
    // `fingerprint_version` holds what that version keyed by, and the
    // version column is how a reader tells. `use_count` and `last_seen` are
    // frozen at 0 since BL-S4 deleted `TouchPattern`. A pattern with no
    // directory has a NULL root and depth, never the invalid page id read
    // as a page.
    Define(view, {{"oid", kInt},
                  {"fetch_id", kInt},
                  {"fingerprint_version", kInt},
                  {"stmt_class", kInt},
                  {"origin", kStr},
                  {"pinned", kInt},
                  {"use_count", kInt},
                  {"last_seen", kInt},
                  {"waystone_root", kInt},
                  {"dir_depth", kInt}});
    auto patterns = catalog_.ListPatterns();
    if (!patterns.ok()) return patterns.status();
    for (const catalog::SysPatternRow& p : patterns.value()) {
        // `dir_depth` is the one authority on a directory (rows.hpp): a
        // zeroed row's root reads as page 0, not as the invalid id.
        const bool directory = catalog::HasWaystoneDirectory(p);
        view.rows.push_back({Int(p.oid), Int(p.fetch_id), Int(p.fingerprint_version),
                             Int(p.stmt_class),
                             Str(p.origin == catalog::kOriginUser ? "user" : "auto"),
                             Bool((p.flags & catalog::kPatternPinned) != 0), Int(p.use_count),
                             Int(p.last_seen), directory ? Int(p.waystone_root) : Null(),
                             directory ? Int(p.dir_depth) : Null()});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::NamespacesView(const txn::ReadView* view_of) {
    exec::CatalogView view;
    Define(view, {{"oid", kInt}, {"name", kStr}});
    // The two built-in namespaces have no row; they are listed first, as
    // `SHOW NAMESPACES` always listed them.
    view.rows.push_back({Int(catalog::kNamespaceSys), Str(catalog::kNamespaceSysName)});
    view.rows.push_back({Int(catalog::kNamespacePublic), Str(catalog::kNamespacePublicName)});
    auto live = catalog_.ListNamespaces(view_of);
    if (!live.ok()) return live.status();
    for (const catalog::SysObjectRow& row : live.value()) {
        view.rows.push_back({Int(row.oid), Str(catalog::NameView(row.name))});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::IndexesView(const txn::ReadView* view_of) {
    exec::CatalogView view;
    Define(view, {{"index_oid", kInt},
                  {"name", kStr},
                  {"rel_oid", kInt},
                  {"rel_name", kStr},
                  {"keys", kStr},
                  {"covering", kStr},
                  {"root_page", kInt},
                  {"key_width", kInt},
                  {"entry_width", kInt},
                  {"height", kInt},
                  {"entries", kInt}});
    auto rows = catalog_.ListIndexes(view_of);
    if (!rows.ok()) return rows.status();
    for (const catalog::SysIndexRow& row : rows.value()) {
        auto access = catalog_.InitTableAccess(row.table_oid);
        const auto columns = [&](const std::uint16_t* cols, std::size_t n) {
            std::string out;
            for (std::size_t i = 0; i < n; ++i) {
                if (i > 0) out += ',';
                out += ColumnLabel(access, cols[i]);
            }
            return out;
        };
        // The root the relation's access names, which a split may have
        // moved past the row's.
        PageId root = row.root_page_id;
        if (access.ok()) {
            for (const auto& ref : access.value()->indexes) {
                if (ref.index_oid == row.index_oid) {
                    root = ref.root_page_id;
                    break;
                }
            }
        }
        const index::IndexLayout layout{
            row.key_width,
            static_cast<std::uint16_t>(row.entry_width - row.key_width - index::kIndexTupleKeyWidth)};
        auto height = index::IndexHeight(page_store_, root, layout);
        auto entries = index::IndexEntryCount(page_store_, root, layout);
        view.rows.push_back(
            {Int(row.index_oid), Str(catalog::NameView(row.name)), Int(row.table_oid),
             RelName(RelationNameOf(row.table_oid)), Str(columns(row.key_cols.data(), row.nkeys)),
             row.ncovered == 0 ? Null() : Str(columns(row.covered_cols.data(), row.ncovered)),
             Int(root), Int(row.key_width),
             Int(row.entry_width), height.ok() ? Int(height.value()) : Null(),
             entries.ok() ? Int(entries.value()) : Null()});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::FkeysView(const txn::ReadView* view_of) {
    exec::CatalogView view;
    Define(view, {{"fk_id", kInt},
                  {"child", kStr},
                  {"column", kStr},
                  {"parent", kStr},
                  {"action", kStr},
                  {"nullable", kInt}});
    auto rows = catalog_.ListForeignKeys();
    if (!rows.ok()) return rows.status();
    // **A foreign key is a schema object** (BO-Q2): listed only where its
    // child relation, which declared it, is visible to the session.
    auto visible = catalog_.ListTables(view_of);
    if (!visible.ok()) return visible.status();
    for (const catalog::SysFkeyRow& row : rows.value()) {
        const bool seen = std::any_of(
            visible.value().begin(), visible.value().end(),
            [&](const catalog::SysObjectRow& obj) { return obj.oid == row.child_rel_oid; });
        if (!seen) continue;
        auto child = catalog_.InitTableAccess(row.child_rel_oid);
        view.rows.push_back({Int(row.fk_id), RelName(RelationNameOf(row.child_rel_oid)),
                             Str(ColumnLabel(child, row.child_column_no)),
                             RelName(RelationNameOf(row.parent_rel_oid)), Str("RESTRICT"),
                             Bool((row.flags & catalog::kFkNullable) != 0)});
    }
    return view;
}

// ---- Engine state ----------------------------------------------------------

StatusOr<exec::CatalogView> CommandDispatcher::AccessStatsView() {
    exec::CatalogView view;
    Define(view, {{"rel_oid", kInt},
                  {"rel_name", kStr},
                  {"kind", kStr},
                  {"columns", kStr},
                  {"column_mask", kInt},
                  {"uses", kInt},
                  {"last_seen", kInt}});
    auto rows = catalog_.ListAccessStats();
    if (!rows.ok()) return rows.status();
    for (const catalog::SysAccessStatRow& row : rows.value()) {
        auto kind = exec::AccessKindOfStored(row.kind);
        auto access = catalog_.InitTableAccess(row.rel_id);
        std::string columns;
        for (std::uint16_t col = 0; col < 64; ++col) {
            if ((row.column_mask & (std::uint64_t{1} << col)) == 0) continue;
            if (!columns.empty()) columns += ',';
            columns += ColumnLabel(access, col);
        }
        view.rows.push_back({Int(row.rel_id), RelName(RelationNameOf(row.rel_id)),
                             Str(kind.has_value() ? exec::AccessKindName(*kind) : "?"),
                             Str(std::move(columns)), Int(row.column_mask), Int(row.use_count),
                             Int(row.last_seen)});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::RelationStatsView() {
    exec::CatalogView view;
    // The budget columns are exact, derived at read through `BudgetOf`; the
    // shape columns walk the clustered tree, as `DESCRIBE` did.
    Define(view, {{"oid", kInt},
                  {"name", kStr},
                  {"next_id", kInt},
                  {"ids_issued", kInt},
                  {"ids_remaining", kInt},
                  {"budget_used_ppm", kInt},
                  {"budget_warning", kInt},
                  {"budget_exhausted", kInt},
                  {"height", kInt},
                  {"leaves", kInt}});
    auto tables = catalog_.ListTables();
    if (!tables.ok()) return tables.status();
    for (const catalog::SysObjectRow& obj : tables.value()) {
        auto row = catalog_.GetSysTableRow(obj.oid);
        if (!row.ok()) return row.status();
        const catalog::KeystoneBudget budget = catalog::BudgetOf(row.value().next_id);
        parser::AstValue height = Null();
        parser::AstValue leaves = Null();
        if (row.value().clustered_type == catalog::ClusteredType::kBtree) {
            auto root = CurrentRootOf(row.value());
            if (!root.ok()) return root.status();
            if (auto h = btree::BtreeHeight(page_store_, root.value()); h.ok()) {
                height = Int(h.value());
            }
            if (auto l = btree::BtreeLeafCount(page_store_, root.value()); l.ok()) {
                leaves = Int(l.value());
            }
        }
        // Parts per million of the capacity, floored, in integers: the
        // double's product can land a unit either side of the floor.
        const auto used_ppm = static_cast<std::uint64_t>(
            static_cast<unsigned __int128>(budget.issued) * 1'000'000 / budget.capacity);
        view.rows.push_back({Int(obj.oid), Str(catalog::NameView(obj.name)),
                             Int(row.value().next_id), Int(budget.issued), Int(budget.remaining),
                             Int(used_ppm), Bool(budget.warn), Bool(budget.exhausted),
                             std::move(height), std::move(leaves)});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::CabinsView() {
    exec::CatalogView view;
    // The store's counters are NULL where the instance runs no store
    // (`cabins = off`): unknown, not zero.
    Define(view, {{"cabin_id", kInt},
                  {"rel_oid", kInt},
                  {"rel_name", kStr},
                  {"column", kStr},
                  {"origin", kStr},
                  {"status", kStr},
                  {"observed", kInt},
                  {"entries", kInt},
                  {"hits", kInt},
                  {"misses", kInt}});
    auto rows = catalog_.ListCabins();
    if (!rows.ok()) return rows.status();
    for (const catalog::SysCabinRow& row : rows.value()) {
        auto access = catalog_.InitTableAccess(row.rel_oid);
        std::vector<parser::AstValue> out{
            Int(row.cabin_id),
            Int(row.rel_oid),
            RelName(RelationNameOf(row.rel_oid)),
            Str(ColumnLabel(access, row.column_no)),
            Str(row.origin == catalog::kCabinOriginUser ? "user" : "auto"),
            Str(row.status == catalog::kCabinStatusActive     ? "active"
                : row.status == catalog::kCabinStatusBuilding ? "building"
                                                              : "demoted")};
        if (cabins_ != nullptr) {
            const stats::CabinStore::CabinInfo info = cabins_->InfoFor(row.cabin_id);
            out.push_back(Int(info.values));
            out.push_back(Int(info.entries));
            out.push_back(Int(info.hits));
            out.push_back(Int(info.misses));
        } else {
            out.insert(out.end(), 4, Null());
        }
        view.rows.push_back(std::move(out));
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::AssertionsView() {
    exec::CatalogView view;
    Define(view, {{"assertion_id", kInt},
                  {"name", kStr},
                  {"rel_oid", kInt},
                  {"rel_name", kStr},
                  {"enforcing", kInt},
                  {"checks", kInt},
                  {"violations", kInt},
                  {"reserved", kInt},
                  {"aborted", kInt},
                  {"def", kStr}});
    auto rows = exec::ListAssertions(catalog_, page_store_);
    if (!rows.ok()) return rows.status();
    for (const exec::AssertionDef& def : rows.value()) {
        std::vector<parser::AstValue> out{
            Int(def.id), Str(def.name), Int(def.target_oid), RelName(RelationNameOf(def.target_oid)),
            // Enforced when it has a directory and the registry holds it
            // (`assertion.md` §6.1, §8.1).
            Bool(def.cabin_root != kInvalidPageId && enforcer_->Holds(def.id))};
        if (const auto counters = enforcer_->CountersOf(def.id); counters.has_value()) {
            out.push_back(Int(counters->checks));
            out.push_back(Int(counters->violations));
            out.push_back(Int(counters->reserved));
            out.push_back(Int(counters->aborted));
        } else {
            out.insert(out.end(), 4, Null());
        }
        out.push_back(Str(def.source_text));
        view.rows.push_back(std::move(out));
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::TransitionsView() {
    exec::CatalogView view;
    // Every declared Transition (BV, `exec/transition_catalog.hpp`), what
    // `SHOW TRANSITIONS` printed - an engine-state view as `sys.assertions`
    // is. The relation and column are resolved at read: a relation gone (a
    // drop whose cleanup a crash cut short) reads NULL beside its oid, and a
    // column the schema cannot name reads its position. `def` is the
    // statement verbatim - last, as a value that can hold commas.
    Define(view, {{"transition_id", kInt},
                  {"name", kStr},
                  {"rel_oid", kInt},
                  {"rel_name", kStr},
                  {"column_no", kInt},
                  {"column", kStr},
                  {"def", kStr}});
    auto rows = exec::ListTransitions(catalog_, page_store_);
    if (!rows.ok()) return rows.status();
    for (const exec::TransitionDef& def : rows.value()) {
        auto access = catalog_.InitTableAccess(def.target_oid);
        view.rows.push_back({Int(def.id), Str(def.name), Int(def.target_oid),
                             RelName(RelationNameOf(def.target_oid)), Int(def.column_no),
                             Str(ColumnLabel(access, def.column_no)), Str(def.source_text)});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::RelayoutView(const parser::AstValue* name,
                                                            std::uint32_t byte) {
    if (relayout_mode_ == PhysicalOptimizerMode::kOff) {
        return Status::InvalidArgument(
            "sys.relayout has no plans to report: the physical optimizer is off "
            "(physical_optimizer=off) (byte " + std::to_string(byte) + ")");
    }
    exec::CatalogView view;
    // One row per plan, the relation's columns repeated on each; a relation
    // with no plan is one row whose `plan` is `none` and whose `blocked_on`
    // says why. The survey columns are NULL where no survey ran - a bare read
    // plans every relation without walking any (`PlanAllRelations`); naming
    // one walks it (`PlanRelation`), as `SHOW RELAYOUT <t>` did.
    Define(view, {{"name", kStr},
                  {"clustered", kStr},
                  {"shapes", kInt},
                  {"walk_weight_q8", kInt},
                  {"survey_pages", kInt},
                  {"survey_live", kInt},
                  {"survey_delete_marked", kInt},
                  {"survey_tuples_per_page", kInt},
                  {"plan", kStr},
                  {"blocked_on", kStr},
                  {"surveyed", kInt},
                  {"predicted_pages_saved", kInt},
                  {"predicted_benefit", kInt},
                  {"measured_pages_saved", kInt}});

    std::vector<stats::RelationReport> reports;
    if (name == nullptr) {
        auto all = stats::PlanAllRelations(catalog_, clock_, decay_half_life_ns_);
        if (!all.ok()) return all.status();
        reports = std::move(all.value());
    } else {
        const auto [qualifier, bare] = SplitQualifiedArg(name->str_val);
        auto oid = catalog_.FindTableOidByName(bare);
        if (!oid.ok()) {
            return Status::NotFound("unknown relation '" + name->str_val + "' (byte " +
                                    std::to_string(name->byte_offset) + ")");
        }
        if (Status s = catalog_.CheckRelationQualifier(qualifier, bare, oid.value(),
                                                       name->byte_offset);
            !s.ok()) {
            return s;
        }
        exec::Budget budget(budget_.limit());
        auto one = stats::PlanRelation(catalog_, page_store_, oid.value(), budget, clock_,
                                       decay_half_life_ns_);
        if (!one.ok()) return one.status();
        reports.push_back(std::move(one.value()));
    }

    for (const stats::RelationReport& report : reports) {
        std::vector<parser::AstValue> head{
            Str(report.name),
            Str(report.clustered_type == catalog::ClusteredType::kBtree ? "btree" : "heap"),
            Int(report.shapes.size()), Int(stats::WalkWeightOf(report.shapes))};
        if (report.survey.has_value()) {
            head.push_back(Int(report.survey->chain_pages));
            head.push_back(Int(report.survey->live_tuples));
            head.push_back(Int(report.survey->delete_marked));
            head.push_back(Int(report.survey->tuples_per_page));
        } else {
            head.insert(head.end(), 4, Null());
        }
        if (report.plans.empty()) {
            std::vector<parser::AstValue> row = head;
            row.push_back(Str("none"));
            row.push_back(Str(report.system_relation
                                  ? "catalog-relation-outside-mover-jurisdiction"
                                  : "btree-outside-v1-mover-scope"));
            row.insert(row.end(), 4, Null());
            view.rows.push_back(std::move(row));
            continue;
        }
        for (const stats::RelayoutPlan& plan : report.plans) {
            std::vector<parser::AstValue> row = head;
            row.push_back(Str(stats::RelayoutPlanKindName(plan.kind)));
            row.push_back(Str(stats::RelayoutGateName(plan.blocked_on)));
            row.push_back(Bool(plan.survey_backed));
            row.push_back(Int(plan.predicted_pages_saved));
            row.push_back(Int(plan.predicted_benefit));
            row.push_back(Int(plan.measured_pages_saved));
            view.rows.push_back(std::move(row));
        }
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::CabinOptimizerView(std::uint32_t byte) {
    const LatchGuard latched(cabin_optimizer_view_latch_);
    if (cabin_controller_ == nullptr) {
        return Status::InvalidArgument(
            "sys.cabin_optimizer has nothing to report: this instance runs no cabin optimizer "
            "(byte " + std::to_string(byte) + ")");
    }
    exec::CatalogView view;
    // One row per managed entry. The controller's and executor's own
    // counters are `sys.meta` fields (`cabin_optimizer_*`).
    Define(view, {{"rel_oid", kInt},
                  {"rel_name", kStr},
                  {"column", kStr},
                  {"state", kStr},
                  {"cabin_id", kInt},
                  {"pages", kInt},
                  {"streak", kInt},
                  {"benefit_q16", kInt},
                  {"cost_q16", kInt},
                  {"hint_fail_pct", kInt},
                  {"coverage_miss_pct", kInt},
                  {"last_action", kStr},
                  {"reason", kStr},
                  {"epoch", kInt}});
    const std::vector<stats::DecisionRecord> log = cabin_controller_->DecisionLog();
    for (const stats::ManagedEntryView& entry : cabin_controller_->ManagedEntries()) {
        auto access = catalog_.InitTableAccess(entry.rel_oid);
        std::vector<parser::AstValue> row{
            Int(entry.rel_oid),      RelName(RelationNameOf(entry.rel_oid)),
            Str(ColumnLabel(access, entry.col_pos)), Str(entry.state),
            Int(entry.cabin_id),     Int(entry.pages),
            Int(entry.confirm_streak), Int(entry.benefit),
            Int(entry.cost)};
        if (entry.cabin_id != 0 && optimizer_signals_ != nullptr) {
            const stats::SnapshotCabin q = optimizer_signals_->QualityOf(entry.cabin_id);
            row.push_back(PercentOf(q.hint_failures_q8, q.lookups_q8));
            row.push_back(PercentOf(q.coverage_misses_q8, q.lookups_q8));
        } else {
            row.insert(row.end(), 2, Null());
        }
        const stats::DecisionRecord* last = nullptr;
        for (const stats::DecisionRecord& record : log) {
            if (record.item.rel_oid == entry.rel_oid && record.item.col_pos == entry.col_pos) {
                last = &record;
            }
        }
        if (last != nullptr) {
            row.push_back(Str(stats::CabinActionName(last->item.action)));
            row.push_back(Str(stats::ActionReasonName(last->item.reason)));
            row.push_back(Int(last->decay_epoch));
        } else {
            row.push_back(Str("none"));
            row.insert(row.end(), 2, Null());
        }
        view.rows.push_back(std::move(row));
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::LockWaitsView() {
    exec::CatalogView view;
    // Every core's recent lock waits, one row each (BO-S3, §3.3), read from
    // the instance's store whatever core asks - an engine-state view, so it
    // filters nothing. `unit`, `rel_oid` and `key` name the unit that refused
    // and `mode` what was asked (`stats/lock_wait_log.hpp`); `key` is the
    // unit's low bound: a tuple's id, a range's or slice's first id, 0 for a
    // relation. `granted` is a flipped slot, not a grant. A waiter or holder
    // that is no transaction (an autocommit statement holding nothing, a
    // read borrow) is NULL.
    static_assert(stats::LockWaitLog::kParts == kMaxCores,
                  "a part for every core an instance runs");
    Define(view, {{"seq", kInt},
                  {"core", kInt},
                  {"started", kTimestamp},
                  {"waited_us", kInt},
                  {"unit", kStr},
                  {"mode", kStr},
                  {"rel_oid", kInt},
                  {"key", kInt},
                  {"waiter_trx", kInt},
                  {"holder_trx", kInt},
                  {"outcome", kStr}});
    for (const stats::LockWaitRecord& r : stats::LockWaitLog::Snapshot()) {
        view.rows.push_back({Int(r.seq), Int(r.core), Timestamp(r.started_us), Int(r.waited_us),
                             Str(txn::LockUnitName(r.unit)), Str(txn::LockModeName(r.mode)),
                             Int(r.rel_oid), Int(r.key), TrxId(r.waiter_trx),
                             TrxId(r.holder_trx), Str(stats::LockWaitOutcomeName(r.outcome))});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::StatementStatsView() {
    exec::CatalogView view;
    // What each statement shape costs (BO-S5, §3.1): one row per
    // `(fetch_id, kind)` for the instance, the cores' parts merged at read
    // - an engine-state view, so it filters nothing. `fetch_id` is 0 for a
    // kind with no fingerprint. `shape_text` is the first statement of the
    // shape seen, cut to 256 bytes - last, as a value that can hold commas.
    Define(view, {{"fetch_id", kInt},
                  {"kind", kStr},
                  {"calls", kInt},
                  {"refused", kInt},
                  {"total_us", kInt},
                  {"max_us", kInt},
                  {"first_seen", kTimestamp},
                  {"last_seen", kTimestamp},
                  {"shape_text", kStr}});
    for (const stats::StatementShape& s : stats::StatementStats::Snapshot()) {
        view.rows.push_back({Int(s.fetch_id), Str(s.kind), Int(s.calls), Int(s.refused),
                             Int(s.total_us), Int(s.max_us), Timestamp(s.first_seen_us),
                             Timestamp(s.last_seen_us), Str(s.shape_text)});
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::PageSlotsView(const parser::AstValue& page_key) {
    exec::CatalogView view;
    // One header row (`slot` NULL), then one row per slot - a heap or leaf
    // page's slots, an internal node's entries. A column a page kind does
    // not have is NULL on its rows. `value` is the slot's payload in hex.
    Define(view, {{"page_id", kInt},
                  {"page_type", kStr},
                  {"page_lsn", kInt},
                  {"level", kInt},
                  {"min_key", kInt},
                  {"nr_slots", kInt},
                  {"lower", kInt},
                  {"upper", kInt},
                  {"free_space", kInt},
                  {"next_page_id", kInt},
                  {"slot", kInt},
                  {"offset", kInt},
                  {"length", kInt},
                  {"dead", kInt},
                  {"sep_key", kInt},
                  {"child", kInt},
                  {"value", kStr}});
    auto id = exec::ValueAsUint64(page_key);
    if (!id.ok()) return id.status();
    if (id.value() > std::numeric_limits<PageId>::max()) {
        return Status::OutOfRange("page id " + std::to_string(id.value()) +
                                  " is past the page id space (byte " +
                                  std::to_string(page_key.byte_offset) + ")");
    }
    const auto page_id = static_cast<PageId>(id.value());
    auto page = page_store_.GetForRead(page_id);
    if (!page.ok()) return page.status();
    const auto bytes = page.value().bytes();
    const std::uint64_t lsn = storage::GetPageLsn(bytes);

    // The header's ten columns, then the slot's seven.
    const auto row_of = [&](std::vector<parser::AstValue> header) {
        header.insert(header.end(), 7, Null());
        return header;
    };

    if (storage::RawPageType(bytes) == static_cast<std::uint8_t>(PageType::kBtreeInternal)) {
        btree::InternalView node(bytes);
        view.rows.push_back(row_of({Int(page_id), Str("BTREE_INTERNAL"), Int(lsn),
                                    Int(node.level()), Null(), Int(node.entry_count()), Null(),
                                    Null(), Null(), Null()}));
        // The leftmost child is the node's entry before its first separator.
        std::vector<parser::AstValue> leftmost = row_of(std::vector<parser::AstValue>(10, Null()));
        leftmost[0] = Int(page_id);
        leftmost[15] = Int(node.leftmost_child());
        view.rows.push_back(std::move(leftmost));
        for (std::uint16_t i = 0; i < node.entry_count(); ++i) {
            auto entry = node.Entry(i);
            if (!entry.ok()) return entry.status();
            std::vector<parser::AstValue> row = row_of(std::vector<parser::AstValue>(10, Null()));
            row[0] = Int(page_id);
            row[10] = Int(i);
            row[14] = Int(entry.value().sep_key);
            row[15] = Int(entry.value().child);
            view.rows.push_back(std::move(row));
        }
        return view;
    }

    heap::PageView page_view(bytes);
    const bool is_leaf =
        storage::RawPageType(bytes) == static_cast<std::uint8_t>(PageType::kBtreeLeaf);
    view.rows.push_back(row_of({Int(page_id), Str(is_leaf ? "BTREE_LEAF" : "HEAP"), Int(lsn),
                                Null(), Int(page_view.min_key()), Int(page_view.slot_count()),
                                Int(page_view.lower()), Int(page_view.upper()),
                                Int(page_view.free_space()), Int(page_view.next_page_id())}));
    for (std::uint16_t i = 0; i < page_view.slot_count(); ++i) {
        auto slot = page_view.DebugSlotInfo(i);
        if (!slot.ok()) return slot.status();
        std::vector<parser::AstValue> row = row_of(std::vector<parser::AstValue>(10, Null()));
        row[0] = Int(page_id);
        row[10] = Int(i);
        row[11] = Int(slot.value().offset);
        row[12] = Int(slot.value().length);
        row[13] = Bool(slot.value().dead);
        if (!slot.value().dead) {
            if (auto tuple = page_view.ReadTuple(i); tuple.ok()) {
                row[16] = Str(HexEncode(tuple.value().payload));
            }
        }
        view.rows.push_back(std::move(row));
    }
    return view;
}

StatusOr<exec::CatalogView> CommandDispatcher::MetaView() {
    exec::CatalogView view;
    // **Tall, one row per field** (BO-Q4): the field list changes with the
    // engine, and a row per field needs no schema change when it does.
    // Every value is text, the counters included, since one column holds
    // them all; the fields are the session's core's.
    Define(view, {{"section", kStr}, {"field", kStr}, {"value", kStr}});
    for (const auto& [section, text] : MetaSections()) {
        std::string_view rest = text;
        while (!rest.empty()) {
            const std::size_t end = rest.find(' ');
            const std::string_view token = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            if (token.empty()) continue;  // the separator a section opens with
            const std::size_t eq = token.find('=');
            // A token with no `=` is a field whose writer broke the line's
            // one rule; refused, never shown as a field with no value.
            if (eq == std::string_view::npos || eq == 0) {
                return Status::Corruption("sys.meta field '" + std::string(token) +
                                          "' has no value");
            }
            view.rows.push_back(
                {Str(section), Str(token.substr(0, eq)), Str(token.substr(eq + 1))});
        }
    }
    return view;
}

}  // namespace kds::server
