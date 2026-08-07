#include "kds/exec/index_ddl.hpp"

#include <string>
#include <vector>

#include "kds/catalog/schema.hpp"
#include "kds/exec/index_key.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"

namespace kds::exec {
namespace {

// Resolution is exact-match, like `Schema::FindColumn` everywhere else: the
// catalog stores a column's name as it was declared, and case folding here
// alone would make `CREATE INDEX` accept a spelling no other statement does.
StatusOr<std::uint16_t> ResolveColumn(const catalog::TableAccess& access,
                                       const parser::IndexColumnRef& col,
                                       const std::string& table_name) {
    for (std::size_t i = 0; i < access.schema.columns.size(); ++i) {
        if (catalog::NameView(access.schema.columns[i].name) == col.name) {
            return static_cast<std::uint16_t>(i);
        }
    }
    return Status::NotFound("relation '" + table_name + "' has no column '" + col.name +
                            "' (byte " + std::to_string(col.byte_offset) + ")");
}

}  // namespace

StatusOr<IndexDdlResult> CreateIndex(catalog::Catalog& catalog, storage::PageStore& store,
                                     const parser::IndexStmt& stmt) {
    auto oid = catalog.FindTableOidByName(stmt.table_name);
    if (!oid.ok()) {
        return Status::NotFound("no relation named '" + stmt.table_name + "' (byte " +
                                std::to_string(stmt.table_byte_offset) + ")");
    }
    auto access = catalog.InitTableAccess(oid.value());
    if (!access.ok()) return access.status();

    // The backfill refusal (see the header). Read from the sequence rather
    // than by walking: `next_id` counts ids ever issued, which is exactly
    // "has this relation ever held a row" - and the versions of a deleted
    // row are still reachable through the undo chain, so a relation that
    // looks empty today is still one IX09 would have to walk.
    auto table_row = catalog.GetSysTableRow(oid.value());
    if (!table_row.ok()) return table_row.status();
    if (table_row.value().next_id > 1) {
        return Status::Unsupported(
            "relation '" + stmt.table_name +
            "' has held rows, and building an index over existing versions is not implemented "
            "(docs/workplan-index.md IX09); an index created now would be empty and would answer "
            "'no rows' for every value");
    }

    catalog::Catalog::IndexDef def;
    def.table_oid = oid.value();
    def.name = stmt.index_name;

    // The key's width, from the encoder that will produce it. Asking
    // `exec::IndexKeyWidth` rather than re-deriving here is what keeps the
    // stored constant and the bytes from ever disagreeing.
    std::vector<catalog::SysColumnRow> key_cols;
    for (const parser::IndexColumnRef& col : stmt.key_columns) {
        auto pos = ResolveColumn(*access.value(), col, stmt.table_name);
        if (!pos.ok()) return pos.status();
        def.key_cols.push_back(pos.value());
        key_cols.push_back(access.value()->schema.columns[pos.value()]);
    }
    auto key_width = IndexKeyWidth(key_cols);
    if (!key_width.ok()) {
        return key_width.status().WithContext("index '" + stmt.index_name + "'");
    }

    // A covered column is stored as its **inline cell bytes verbatim**, so
    // its width is the row layout's - tag included, which is what lets a
    // spilled value's pointer ride along and be resolved from the base row
    // exactly as it would have been.
    std::uint32_t covered_width = 0;
    for (const parser::IndexColumnRef& col : stmt.covered_columns) {
        auto pos = ResolveColumn(*access.value(), col, stmt.table_name);
        if (!pos.ok()) return pos.status();
        def.covered_cols.push_back(pos.value());
        auto width = catalog::RowLayout::ColumnWidth(access.value()->schema.columns[pos.value()],
                                                      access.value()->layout.inline_cell_width);
        if (!width.ok()) {
            return width.status().WithContext("covered column '" + col.name + "'");
        }
        covered_width += width.value();
    }

    index::IndexLayout layout;
    layout.key_width = static_cast<std::uint16_t>(key_width.value());
    layout.covered_width = static_cast<std::uint16_t>(covered_width);
    // Where a wide declaration is refused: by arithmetic, at declaration,
    // rather than by an insert that fails much later.
    if (Status s = index::CheckIndexLayout(layout); !s.ok()) {
        return s.WithContext("index '" + stmt.index_name + "'");
    }
    def.key_width = layout.key_width;
    def.entry_width = static_cast<std::uint16_t>(layout.leaf_entry_width());

    // The root, allocated and formatted before the row that names it. The
    // reverse order would leave a catalog row pointing at a page that does
    // not exist, which is a worse failure than a page nothing points at.
    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto [root_id, root_bytes] = created.value();
    if (Status s = index::FormatRoot(root_bytes, layout); !s.ok()) return s;
    def.root_page_id = root_id;

    // The heap-relation, pk-column, duplicate-column, over-cap, UNIQUE and
    // duplicate-name refusals all live in the catalog and are deliberately
    // not restated here: two answers to "why not" is how one of them ends up
    // wrong.
    auto index_oid = catalog.CreateIndex(def);
    if (!index_oid.ok()) return index_oid.status();

    IndexDdlResult out;
    out.index_oid = index_oid.value();
    out.rel_oid = def.table_oid;
    out.root_page_id = root_id;
    out.key_width = def.key_width;
    out.entry_width = def.entry_width;

    // Collected after the write, because none of them is a reason not to
    // write. An index is complete for every value where a Cabin is
    // authoritative only for observed ones, so the Cabin becomes dead weight
    // - but dropping it is the operator's call, not the engine's.
    if (catalog.FindCabinOnColumn(def.table_oid, def.key_cols[0]).ok()) {
        out.warnings.push_back("column '" + stmt.key_columns[0].name +
                               "' already carries a cabin; this index supersedes it for every "
                               "value, not just observed ones, and the cabin's write hook and "
                               "memory now buy nothing (DROP CABIN to reclaim them)");
    }
    return out;
}

StatusOr<catalog::Oid> DropIndex(catalog::Catalog& catalog, const parser::IndexStmt& stmt) {
    auto row = catalog.FindIndexByName(stmt.index_name);
    if (!row.ok()) {
        return Status::NotFound("no index named '" + stmt.index_name + "' (byte " +
                                std::to_string(stmt.byte_offset) + ")");
    }
    if (Status s = catalog.DropIndex(row.value().index_oid); !s.ok()) return s;
    return row.value().index_oid;
}

}  // namespace kds::exec
