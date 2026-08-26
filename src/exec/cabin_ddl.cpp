#include "kds/exec/cabin_ddl.hpp"

#include <string>

#include "kds/exec/step_chain.hpp"

namespace kds::exec {
namespace {

// The relation and the column position `stmt` names.
//
// Resolution is exact-match, like `Schema::FindColumn` everywhere else: the
// catalog stores a column's name as it was declared, and case folding here
// alone would make `CREATE CABIN` accept a spelling no other statement does.
struct Resolved {
    catalog::Oid rel_oid = 0;
    const catalog::TableAccess* access = nullptr;
    std::uint16_t col_pos = 0;
};

StatusOr<Resolved> Resolve(catalog::Catalog& catalog, const parser::CabinStmt& stmt) {
    auto oid = catalog.FindTableOidByName(stmt.table_name);
    if (!oid.ok()) {
        return Status::NotFound("no relation named '" + stmt.table_name + "' (byte " +
                                std::to_string(stmt.byte_offset) + ")");
    }

    auto access = catalog.InitTableAccess(oid.value());
    if (!access.ok()) return access.status();

    for (std::size_t i = 0; i < access.value()->schema.columns.size(); ++i) {
        if (catalog::NameView(access.value()->schema.columns[i].name) != stmt.column_name) {
            continue;
        }
        Resolved out;
        out.rel_oid = oid.value();
        out.access = access.value();
        out.col_pos = static_cast<std::uint16_t>(i);
        return out;
    }
    return Status::NotFound("relation '" + stmt.table_name + "' has no column '" +
                            stmt.column_name + "' (byte " +
                            std::to_string(stmt.column_byte_offset) + ")");
}

// Whether anything has ever filtered this relation on this column.
//
// This is the interlock spec §7 describes, in its cheapest form: the access
// statistics already record which columns the workload searches on
// (`docs/spec/heap-and-tuple.md` §7), and a Cabin on a column nothing filters is
// the definition of a structure that works and will disappoint. It is a
// *warning* and not a refusal, because an operator declaring a Cabin ahead
// of the traffic that will use it is doing exactly what declaration is for.
//
// A failure to read the statistics is not a failure to create a Cabin: the
// warning is dropped and the statement proceeds.
bool AnyFilterRecordedOn(catalog::Catalog& catalog, catalog::Oid rel_oid,
                         std::uint16_t col_pos) {
    if (col_pos >= 64) return true;  // outside the mask; say nothing rather than warn wrongly
    auto shapes = catalog.ListAccessStats();
    if (!shapes.ok()) return true;

    const std::uint64_t bit = std::uint64_t{1} << col_pos;
    for (const catalog::SysAccessStatRow& row : shapes.value()) {
        if (row.rel_id != rel_oid) continue;
        if ((row.column_mask & bit) != 0) return true;
    }
    return false;
}

}  // namespace

StatusOr<CabinDdlResult> CreateCabin(catalog::Catalog& catalog, const parser::CabinStmt& stmt) {
    auto resolved = Resolve(catalog, stmt);
    if (!resolved.ok()) return resolved.status();

    // The pk refusal and the duplicate refusal both live in the catalog, and
    // are deliberately not restated here: two answers to "why not" is how one
    // of them ends up wrong.
    auto cabin_id = catalog.CreateCabin(resolved.value().rel_oid, resolved.value().col_pos,
                                         catalog::kCabinOriginUser);
    if (!cabin_id.ok()) return cabin_id.status();

    CabinDdlResult out;
    out.cabin_id = cabin_id.value();
    out.rel_oid = resolved.value().rel_oid;
    out.col_pos = resolved.value().col_pos;

    // Warnings are collected *after* the write, because none of them is a
    // reason not to write - and computing them first would make a statement's
    // success depend on a statistics read that has nothing to do with it.
    if (!AnyFilterRecordedOn(catalog, out.rel_oid, out.col_pos)) {
        out.warnings.push_back("no recorded access has filtered '" + stmt.table_name + "." +
                               stmt.column_name +
                               "'; this cabin will stay empty until traffic probes that column "
                               "by equality (SHOW ACCESS lists what has been recorded)");
    }
    if (catalog.FindIndexOnColumn(out.rel_oid, out.col_pos).ok()) {
        out.warnings.push_back("column '" + stmt.column_name +
                               "' already carries an index; a cabin on it duplicates work the "
                               "index already does for every value, not just observed ones");
    }
    return out;
}

StatusOr<std::uint64_t> DropCabin(catalog::Catalog& catalog, const parser::CabinStmt& stmt) {
    auto resolved = Resolve(catalog, stmt);
    if (!resolved.ok()) return resolved.status();

    auto row = catalog.FindCabinOnColumn(resolved.value().rel_oid, resolved.value().col_pos);
    if (!row.ok()) {
        return Status::NotFound("no cabin on '" + stmt.table_name + "." + stmt.column_name +
                                "' (byte " + std::to_string(stmt.column_byte_offset) + ")");
    }

    if (Status s = catalog.DropCabin(row.value().cabin_id); !s.ok()) return s;
    return row.value().cabin_id;
}

}  // namespace kds::exec
