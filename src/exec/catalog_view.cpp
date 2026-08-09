#include "kds/exec/catalog_view.hpp"

#include <algorithm>
#include <cctype>

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

StatusOr<CatalogView> TablesView(catalog::Catalog& catalog) {
    CatalogView view;
    view.column_names = {"oid", "namespace_oid", "name", "desc_page_id", "clustered_type",
                         "next_id"};

    auto objects = catalog.ListTables();
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

StatusOr<CatalogView> ObjectsView(catalog::Catalog& catalog) {
    CatalogView view;
    view.column_names = {"oid", "namespace_oid", "type_oid", "rel_id", "name"};

    // ListTables() is the only enumerator the catalog exposes, and it
    // filters to type_oid == kTypeTable. So this view shows table objects
    // and says so, rather than implying it is every object there is.
    auto objects = catalog.ListTables();
    if (!objects.ok()) return objects.status();

    for (const catalog::SysObjectRow& object : objects.value()) {
        view.rows.push_back({Int(object.oid), Int(object.namespace_oid), Int(object.type_oid),
                             Int(object.rel_id),
                             Str(std::string(catalog::NameView(object.name)))});
    }
    return view;
}

StatusOr<CatalogView> ColumnsView(catalog::Catalog& catalog) {
    CatalogView view;
    view.column_names = {"rel_id", "rel_name", "pos", "name", "type_val", "type", "notnull"};

    auto objects = catalog.ListTables();
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
            // **The declared type, not `len`.** `len` used to be readable
            // as "the width", and for a decimal column it is now the packed
            // (precision, scale) pair (catalog/rows.hpp) - so printing the
            // integer would show `2562` where the client wrote
            // `decimal(10,2)`. `ColumnTypeText` is the one place that knows
            // the field's per-type meaning.
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

StatusOr<CatalogView> TypesView(catalog::Catalog& catalog) {
    CatalogView view;
    view.column_names = {"oid", "name", "type_val", "len"};

    auto types = catalog.ListTypes();
    if (!types.ok()) return types.status();

    for (const catalog::SysTypeRow& type : *types.value()) {
        view.rows.push_back({Int(type.oid), Str(std::string(catalog::NameView(type.name))),
                             Int(type.type_val), Int(type.len)});
    }
    return view;
}

StatusOr<CatalogView> PatternsView(catalog::Catalog& catalog) {
    CatalogView view;
    view.column_names = {"oid", "pattern_id", "fingerprint_version", "stmt_class", "use_count",
                         "last_seen", "waystone_root", "dir_depth"};

    auto patterns = catalog.ListPatterns();
    if (!patterns.ok()) return patterns.status();

    for (const catalog::SysPatternRow& pattern : patterns.value()) {
        view.rows.push_back({Int(pattern.oid), Int(pattern.pattern_id),
                             Int(pattern.fingerprint_version), Int(pattern.stmt_class),
                             Int(pattern.use_count), Int(pattern.last_seen),
                             Int(pattern.waystone_root), Int(pattern.dir_depth)});
    }
    return view;
}

struct ViewEntry {
    std::string_view name;
    StatusOr<CatalogView> (*read)(catalog::Catalog&);
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

StatusOr<CatalogView> ReadCatalogView(catalog::Catalog& catalog, std::string_view name) {
    for (const ViewEntry& entry : kViews) {
        if (IEquals(name, entry.name)) return entry.read(catalog);
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
