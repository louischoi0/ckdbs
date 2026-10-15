#include "kds/exec/cabin_ddl.hpp"

#include <memory>
#include <string>

#include "kds/exec/cabin_expr.hpp"
#include "kds/exec/declaration_relation.hpp"
#include "kds/exec/step_chain.hpp"

namespace kds::exec {
namespace {

// The relation and the column position `stmt` names.
//
// Resolution is `Schema::FindColumn`'s, every column name's: ignoring case,
// two or more matches refused as ambiguous (docs/spec/parser-v2.md).
struct Resolved {
    catalog::Oid rel_oid = 0;
    const catalog::TableAccess* access = nullptr;
    std::uint16_t col_pos = 0;
};

StatusOr<Resolved> ResolveRelation(catalog::Catalog& catalog, const parser::CabinStmt& stmt) {
    auto oid = catalog.FindTableOidByName(stmt.table_name);
    if (!oid.ok()) {
        return Status::NotFound("no relation named '" + stmt.table_name + "' (byte " +
                                std::to_string(stmt.byte_offset) + ")");
    }
    // AF-T3: `ns.t` asserts where the relation lives, and a wrong assertion
    // is refused rather than absorbed.
    if (Status s = catalog.CheckRelationQualifier(stmt.schema, stmt.table_name, oid.value(),
                                                  stmt.byte_offset);
        !s.ok()) {
        return s;
    }

    auto access = catalog.InitTableAccess(oid.value());
    if (!access.ok()) return access.status();
    Resolved out;
    out.rel_oid = oid.value();
    out.access = access.value();
    return out;
}

StatusOr<Resolved> Resolve(catalog::Catalog& catalog, const parser::CabinStmt& stmt) {
    auto relation = ResolveRelation(catalog, stmt);
    if (!relation.ok()) return relation.status();
    const catalog::Schema& schema = relation.value().access->schema;
    auto found = schema.FindColumn(stmt.column_name, stmt.column_byte_offset);
    if (!found.ok()) return found.status();
    if (found.value() != nullptr) {
        Resolved out = relation.value();
        out.col_pos = static_cast<std::uint16_t>(found.value()->pos);
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

// A Cabin's sys.cabin_exprs row is named for its `cabin_id` - the name is
// what `InsertDeclaration`'s uniqueness check reads; every lookup here is by
// the relation and `aux`, as the loader's is.
std::string TextName(std::uint64_t cabin_id) { return "cabin_" + std::to_string(cabin_id); }

// The stored text of each of `rel_oid`'s expression and predicate Cabins,
// by `cabin_id`.
StatusOr<std::vector<DeclarationRow>> TextsOf(catalog::Catalog& catalog,
                                              storage::PageStore& store, catalog::Oid rel_oid) {
    return DeclarationsOnRelation(catalog, store, catalog::kSysCabinExprsTable, rel_oid);
}

const DeclarationRow* TextFor(const std::vector<DeclarationRow>& texts, std::uint64_t cabin_id) {
    for (const DeclarationRow& row : texts) {
        if (static_cast<std::uint64_t>(row.aux) == cabin_id) return &row;
    }
    return nullptr;
}

// Whether Cabin `row` has the compiled `F` that is `f` - AQ-R2's
// confirmation, which a matching `expr_id` only nominates.
StatusOr<bool> HoldsKey(catalog::Catalog& catalog, storage::PageStore& store,
                        const catalog::SysCabinRow& row, const CompiledCabinExpr& f) {
    if (row.shape != f.shape || row.expr_id != f.expr_id) return false;
    auto texts = TextsOf(catalog, store, row.rel_oid);
    if (!texts.ok()) return texts.status();
    const DeclarationRow* text = TextFor(texts.value(), row.cabin_id);
    return text != nullptr && text->source_text == f.text;
}

StatusOr<CabinDdlResult> CreateExprCabin(catalog::Catalog& catalog, storage::PageStore& store,
                                         wal::WalManager* wal, const parser::CabinStmt& stmt) {
    auto resolved = ResolveRelation(catalog, stmt);
    if (!resolved.ok()) return resolved.status();
    const catalog::Oid rel_oid = resolved.value().rel_oid;
    auto f = CompileCabinExpr(stmt, resolved.value().access->schema);
    if (!f.ok()) return f.status();

    CabinDdlResult out;
    out.rel_oid = rel_oid;
    out.key = RenderCabinExpr(f.value(), resolved.value().access->schema);

    // Issued here, before `CreateExprCabin` takes sys.cabins' root, so the
    // hold nests no sys.tables page (CreateCabin()'s rule).
    auto text_id = catalog.AllocateRowId(catalog::kSysCabinExprsTable);
    if (!text_id.ok()) return text_id.status();

    const auto same_f = [&](const catalog::SysCabinRow& row) {
        return HoldsKey(catalog, store, row, f.value());
    };
    const auto record_f = [&](std::uint64_t cabin_id) -> Status {
        DeclarationRow row;
        row.id = text_id.value();
        row.target_oid = rel_oid;
        row.aux = static_cast<std::int64_t>(cabin_id);
        row.name = TextName(cabin_id);
        row.source_text = f.value().text;
        return InsertDeclaration(catalog, store, wal, catalog::kSysCabinExprsTable, "cabin", row);
    };
    auto cabin_id = catalog.CreateExprCabin(rel_oid, f.value().shape, f.value().expr_id,
                                            catalog::kCabinOriginUser, same_f, record_f);
    if (!cabin_id.ok()) {
        if (cabin_id.status().code() == StatusCode::kAlreadyExists) {
            return Status::AlreadyExists("relation '" + stmt.table_name +
                                         "' already has a cabin on " + out.key + " (byte " +
                                         std::to_string(stmt.expr_byte_offset) + ")");
        }
        return cabin_id.status();
    }
    out.cabin_id = cabin_id.value();
    return out;
}

// The Cabin of `rel_oid` whose compiled `F` is `f`'s.
StatusOr<std::optional<std::uint64_t>> FindExprCabin(catalog::Catalog& catalog,
                                                     storage::PageStore& store,
                                                     catalog::Oid rel_oid,
                                                     const CompiledCabinExpr& f) {
    auto rows = catalog.ListCabins();
    if (!rows.ok()) return rows.status();
    for (const catalog::SysCabinRow& row : rows.value()) {
        if (row.rel_oid != rel_oid) continue;
        auto holds = HoldsKey(catalog, store, row, f);
        if (!holds.ok()) return holds.status();
        if (holds.value()) return std::optional<std::uint64_t>(row.cabin_id);
    }
    return std::optional<std::uint64_t>();
}

}  // namespace

StatusOr<CabinDdlResult> CreateCabin(catalog::Catalog& catalog, storage::PageStore& store,
                                     wal::WalManager* wal, const parser::CabinStmt& stmt) {
    if (stmt.shape != catalog::kCabinShapeColumn) return CreateExprCabin(catalog, store, wal, stmt);
    auto resolved = Resolve(catalog, stmt);
    if (!resolved.ok()) return resolved.status();

    // The tuple key refusal and the duplicate refusal both live in the catalog, and
    // are deliberately not restated here: two answers to "why not" is how one
    // of them ends up wrong.
    auto cabin_id = catalog.CreateCabin(resolved.value().rel_oid, resolved.value().col_pos,
                                         catalog::kCabinOriginUser);
    if (!cabin_id.ok()) return cabin_id.status();

    CabinDdlResult out;
    out.cabin_id = cabin_id.value();
    out.rel_oid = resolved.value().rel_oid;
    out.col_pos = resolved.value().col_pos;
    // The column's declared name, not the spelling the statement used: a
    // name matches ignoring case, and the reply names the Cabin as `SHOW
    // CABINS` will.
    out.key = std::string(
        catalog::NameView(resolved.value().access->schema.columns[out.col_pos].name));

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

StatusOr<std::uint64_t> DropCabin(catalog::Catalog& catalog, storage::PageStore& store,
                                  wal::WalManager* wal, const parser::CabinStmt& stmt) {
    if (stmt.shape != catalog::kCabinShapeColumn) {
        auto relation = ResolveRelation(catalog, stmt);
        if (!relation.ok()) return relation.status();
        auto f = CompileCabinExpr(stmt, relation.value().access->schema);
        if (!f.ok()) return f.status();
        auto found = FindExprCabin(catalog, store, relation.value().rel_oid, f.value());
        if (!found.ok()) return found.status();
        if (!found.value().has_value()) {
            return Status::NotFound("no cabin on '" + stmt.table_name + "' with this key (byte " +
                                    std::to_string(stmt.expr_byte_offset) + ")");
        }
        const std::uint64_t cabin_id = *found.value();
        // The row first, so a failure between the two leaves an inert text
        // and never a Cabin with none (Catalog::CreateExprCabin's order).
        // Once the row is gone the Cabin is, so the text's removal cannot
        // fail the statement: a text no row names is read by nothing, and
        // `DROP TABLE` collects it (`ForgetCabinTexts`).
        if (Status s = catalog.DropCabin(cabin_id); !s.ok()) return s;
        (void)DeleteDeclaration(catalog, store, wal, catalog::kSysCabinExprsTable, "cabin",
                                TextName(cabin_id));
        return cabin_id;
    }
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

Status LoadExprCabins(catalog::Catalog& catalog, storage::PageStore& store,
                      catalog::TableAccess& access) {
    if (access.expr_cabins.empty()) return Status::OK();
    auto texts = TextsOf(catalog, store, access.oid);
    if (!texts.ok()) return texts.status();
    for (catalog::TableAccess::ExprCabinRef& ref : access.expr_cabins) {
        const DeclarationRow* text = TextFor(texts.value(), ref.id);
        if (text == nullptr) {
            return Status::Corruption("cabin " + std::to_string(ref.id) +
                                      " has no key function in sys.cabin_exprs");
        }
        auto f = DecodeCabinExpr(ref.shape, text->source_text, access.schema);
        if (!f.ok()) return f.status();
        if (f.value().expr_id != ref.expr_id) {
            return Status::Corruption("cabin " + std::to_string(ref.id) +
                                      "'s key function does not hash to its expr_id");
        }
        ref.f = std::make_shared<const CompiledCabinExpr>(std::move(f.value()));
    }
    return Status::OK();
}

StatusOr<bool> AnyCabinKeyReads(catalog::Catalog& catalog, storage::PageStore& store,
                                const catalog::TableAccess& access, std::uint16_t col_pos) {
    auto rows = catalog.ListCabins();
    if (!rows.ok()) return rows.status();
    std::optional<std::vector<DeclarationRow>> texts;
    for (const catalog::SysCabinRow& row : rows.value()) {
        if (row.rel_oid != access.oid || row.shape == catalog::kCabinShapeColumn) continue;
        if (!texts.has_value()) {
            auto read = TextsOf(catalog, store, access.oid);
            if (!read.ok()) return read.status();
            texts = std::move(read.value());
        }
        const DeclarationRow* text = TextFor(*texts, row.cabin_id);
        if (text == nullptr) continue;  // a Cabin with no text fails its fill, not this check
        auto f = DecodeCabinExpr(row.shape, text->source_text, access.schema);
        if (!f.ok()) return f.status();
        if (std::binary_search(f.value().inputs.begin(), f.value().inputs.end(), col_pos)) {
            return true;
        }
    }
    return false;
}

Status ForgetCabinTexts(catalog::Catalog& catalog, storage::PageStore& store,
                        wal::WalManager* wal, catalog::Oid rel_oid) {
    auto texts = TextsOf(catalog, store, rel_oid);
    if (!texts.ok()) return texts.status();
    Status out;
    for (const DeclarationRow& text : texts.value()) {
        auto gone = DeleteDeclaration(catalog, store, wal, catalog::kSysCabinExprsTable, "cabin",
                                      text.name);
        if (!gone.ok()) out = gone.status();
    }
    return out;
}

}  // namespace kds::exec
