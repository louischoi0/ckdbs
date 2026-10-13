#include "kds/exec/catalog_view.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <utility>

#include "kds/catalog/well_known.hpp"

namespace kds::exec {

namespace {

bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

parser::AstValue Int(std::uint64_t v) {
    parser::AstValue out;
    out.type = parser::ValueType::kInt;
    out.int_val = static_cast<std::int64_t>(v);
    // Set for the same reason DecodeRow sets it: a value that does not fit
    // int64 must still format and compare correctly, and raw_int_text is
    // what carries the full unsigned range (row_codec.hpp).
    out.raw_int_text = std::to_string(v);
    return out;
}

parser::AstValue Str(std::string v) {
    parser::AstValue out;
    out.type = parser::ValueType::kStr;
    out.str_val = std::move(v);
    return out;
}

// A view's columns, each name with the type its builder emits: `Int` is a
// uint64 and `Str` a varchar, so these are the only two a view declares.
constexpr std::uint32_t kInt = catalog::kTypeValUint64;
constexpr std::uint32_t kStr = catalog::kTypeValVarchar;

void Define(CatalogView& view,
            std::initializer_list<std::pair<const char*, std::uint32_t>> columns) {
    for (const auto& [name, type] : columns) {
        view.column_names.emplace_back(name);
        view.column_types.push_back(type);
    }
}

// Rendered rather than reported as a number: `HEAP` is what a client wrote
// in the CREATE TABLE clause and what DESCRIBE echoes, so the view says the
// same thing rather than making the reader map 0 and 1 themselves.
std::string ClusteredName(catalog::ClusteredType type) {
    return type == catalog::ClusteredType::kBtree ? "BTREE" : "HEAP";
}

// ---- The views ----------------------------------------------------------
//
// One function each, and each one is the *only* description of that view's
// shape - the column names and the row builder sit together so a field
// added to a catalog row cannot be added to one and forgotten in the other.

StatusOr<CatalogView> TablesView(catalog::Catalog& catalog, const txn::ReadView* view_of) {
    CatalogView view;
    Define(view, {{"oid", kInt},
                  {"namespace_oid", kInt},
                  {"name", kStr},
                  {"desc_page_id", kInt},
                  {"clustered_type", kStr},
                  {"next_id", kInt}});

    auto objects = catalog.ListTables(view_of);
    if (!objects.ok()) return objects.status();

    for (const catalog::SysObjectRow& object : objects.value()) {
        auto row = catalog.GetSysTableRow(object.oid);
        if (!row.ok()) {
            // A sys.objects entry with no sys.tables row is a catalog the
            // engine cannot explain. Reported rather than skipped: a view
            // that quietly omits rows is worse than one that fails, since
            // the omission is invisible in the result.
            return row.status();
        }
        view.rows.push_back({Int(row.value().oid), Int(row.value().namespace_oid),
                             Str(std::string(catalog::NameView(row.value().name))),
                             Int(row.value().desc_page_id),
                             Str(ClusteredName(row.value().clustered_type)),
                             Int(row.value().next_id)});
    }
    return view;
}

StatusOr<CatalogView> ObjectsView(catalog::Catalog& catalog, const txn::ReadView* view_of) {
    CatalogView view;
    Define(view, {{"oid", kInt},
                  {"namespace_oid", kInt},
                  {"type_oid", kInt},
                  {"rel_id", kInt},
                  {"name", kStr}});

    // ListTables() is the only enumerator the catalog exposes, and it
    // filters to type_oid == kTypeTable. So this view shows table objects
    // and says so, rather than implying it is every object there is.
    auto objects = catalog.ListTables(view_of);
    if (!objects.ok()) return objects.status();

    // The fourth column keeps its name, `rel_id`: the word is
    // `pending_roots` in code (BF-R2), and on a live table - the only kind
    // listed here - it is 0, which is all `rel_id` ever showed.
    for (const catalog::SysObjectRow& object : objects.value()) {
        view.rows.push_back({Int(object.oid), Int(object.namespace_oid), Int(object.type_oid),
                             Int(object.pending_roots),
                             Str(std::string(catalog::NameView(object.name)))});
    }
    return view;
}

StatusOr<CatalogView> ColumnsView(catalog::Catalog& catalog, const txn::ReadView* view_of) {
    CatalogView view;
    Define(view, {{"rel_id", kInt},
                  {"rel_name", kStr},
                  {"pos", kInt},
                  {"name", kStr},
                  {"type_val", kInt},
                  {"type", kStr},
                  {"notnull", kInt}});

    auto objects = catalog.ListTables(view_of);
    if (!objects.ok()) return objects.status();

    for (const catalog::SysObjectRow& object : objects.value()) {
        auto schema = catalog.BuildSchemaFromColumns(object.oid);
        if (!schema.ok()) {
            // The catalog's own tables have no column rows - their layout
            // is compiled in (catalog/rows.hpp), which is the whole reason
            // these views exist. Skipped rather than failed: they really
            // do have no columns to report, and refusing the view because
            // of it would make sys.columns unreadable on every database.
            continue;
        }
        // `rel_name` is not a stored field. It is joined in here because a
        // column list keyed only by oid is unreadable, and joining against
        // sys.tables is exactly what a view cannot do.
        const std::string rel_name(catalog::NameView(object.name));
        for (const catalog::SysColumnRow& column : schema.value().columns) {
            // **The declared type, not `len`**: for a decimal column `len`
            // is the packed (precision, scale) pair (catalog/rows.hpp), so
            // the integer would print `2562` for `decimal(10,2)`.
            // `ColumnTypeText` is the one place that knows its meaning.
            auto type_row = catalog.ResolveTypeByVal(column.type_val);
            const std::string base =
                type_row.ok() ? std::string(catalog::NameView(type_row.value().name))
                              : "?type_val=" + std::to_string(column.type_val);
            view.rows.push_back({Int(column.rel_id), Str(rel_name), Int(column.pos),
                                 Str(std::string(catalog::NameView(column.name))),
                                 Int(column.type_val),
                                 Str(catalog::ColumnTypeText(column, base)),
                                 Int(column.notnull ? 1 : 0)});
        }
    }
    return view;
}

StatusOr<CatalogView> TypesView(catalog::Catalog& catalog, const txn::ReadView*) {
    CatalogView view;
    Define(view, {{"oid", kInt}, {"name", kStr}, {"type_val", kInt}, {"len", kInt}});

    auto types = catalog.ListTypes();
    if (!types.ok()) return types.status();

    for (const catalog::SysTypeRow& type : *types.value()) {
        view.rows.push_back({Int(type.oid), Str(std::string(catalog::NameView(type.name))),
                             Int(type.type_val), Int(type.len)});
    }
    return view;
}

StatusOr<CatalogView> PatternsView(catalog::Catalog& catalog, const txn::ReadView*) {
    CatalogView view;
    // `fetch_id`, not `pattern_id`, since AP-S2: the row is keyed by the
    // fetch shape. A row of another `fingerprint_version` holds what that
    // version keyed by - a version-1 row holds a pattern_id - and the
    // version column beside it is how a reader tells.
    Define(view, {{"oid", kInt},
                  {"fetch_id", kInt},
                  {"fingerprint_version", kInt},
                  {"stmt_class", kInt},
                  {"use_count", kInt},
                  {"last_seen", kInt},
                  {"waystone_root", kInt},
                  {"dir_depth", kInt}});

    auto patterns = catalog.ListPatterns();
    if (!patterns.ok()) return patterns.status();

    for (const catalog::SysPatternRow& pattern : patterns.value()) {
        view.rows.push_back({Int(pattern.oid), Int(pattern.fetch_id),
                             Int(pattern.fingerprint_version), Int(pattern.stmt_class),
                             Int(pattern.use_count), Int(pattern.last_seen),
                             Int(pattern.waystone_root), Int(pattern.dir_depth)});
    }
    return view;
}

struct ViewEntry {
    std::string_view name;
    StatusOr<CatalogView> (*read)(catalog::Catalog&, const txn::ReadView*);
};

// The registry, in the order CatalogViewNames() reports them.
constexpr ViewEntry kViews[] = {
    {"tables", &TablesView},   {"columns", &ColumnsView}, {"objects", &ObjectsView},
    {"types", &TypesView},     {"patterns", &PatternsView},
};

}  // namespace

bool IsCatalogView(std::string_view name) noexcept {
    for (const ViewEntry& entry : kViews) {
        if (IEquals(name, entry.name)) return true;
    }
    return false;
}

std::vector<std::string> CatalogViewNames() {
    std::vector<std::string> out;
    for (const ViewEntry& entry : kViews) out.emplace_back(entry.name);
    return out;
}

StatusOr<CatalogView> ReadCatalogView(catalog::Catalog& catalog, std::string_view name,
                                      const txn::ReadView* view) {
    for (const ViewEntry& entry : kViews) {
        if (IEquals(name, entry.name)) return entry.read(catalog, view);
    }
    std::string known;
    for (const ViewEntry& entry : kViews) {
        if (!known.empty()) known += ", ";
        known += "sys.";
        known += entry.name;
    }
    return Status::NotFound("no catalog view named 'sys." + std::string(name) + "' (known: " +
                            known + ")");
}

}  // namespace kds::exec
