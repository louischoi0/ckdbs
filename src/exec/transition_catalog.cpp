#include "kds/exec/transition_catalog.hpp"

#include <utility>

#include "kds/exec/declaration_relation.hpp"

namespace kds::exec {

namespace {

inline constexpr std::string_view kKind = "transition";

TransitionDef ToTransitionDef(DeclarationRow row) {
    TransitionDef def{};
    def.id = row.id;
    def.target_oid = row.target_oid;
    def.column_no = static_cast<std::uint16_t>(row.aux);
    def.name = std::move(row.name);
    def.source_text = std::move(row.source_text);
    return def;
}

}  // namespace

StatusOr<std::vector<TransitionDef>> ListTransitions(catalog::Catalog& catalog,
                                                     storage::PageStore& store) {
    auto rows = ScanDeclarations(catalog, store, catalog::kSysTransitionsTable);
    if (!rows.ok()) return rows.status();
    std::vector<TransitionDef> out;
    out.reserve(rows.value().size());
    for (DeclarationRow& row : rows.value()) out.push_back(ToTransitionDef(std::move(row)));
    return out;
}

StatusOr<std::optional<TransitionDef>> FindTransitionByName(catalog::Catalog& catalog,
                                                            storage::PageStore& store,
                                                            std::string_view name) {
    auto row = FindDeclarationByName(catalog, store, catalog::kSysTransitionsTable, name);
    if (!row.ok()) return row.status();
    if (!row.value().has_value()) return std::optional<TransitionDef>();
    return std::optional<TransitionDef>(ToTransitionDef(std::move(*row.value())));
}

StatusOr<std::vector<TransitionDef>> TransitionsOnRelation(catalog::Catalog& catalog,
                                                           storage::PageStore& store,
                                                           catalog::Oid target_oid) {
    auto rows = DeclarationsOnRelation(catalog, store, catalog::kSysTransitionsTable, target_oid);
    if (!rows.ok()) return rows.status();
    std::vector<TransitionDef> out;
    for (DeclarationRow& row : rows.value()) out.push_back(ToTransitionDef(std::move(row)));
    return out;
}

Status InsertTransition(catalog::Catalog& catalog, storage::PageStore& store,
                        wal::WalManager* wal, const TransitionDef& def) {
    DeclarationRow row;
    row.id = def.id;
    row.target_oid = def.target_oid;
    row.aux = def.column_no;
    row.name = def.name;
    row.source_text = def.source_text;
    return InsertDeclaration(catalog, store, wal, catalog::kSysTransitionsTable, kKind, row);
}

StatusOr<TransitionDef> DeleteTransition(catalog::Catalog& catalog, storage::PageStore& store,
                                         wal::WalManager* wal, std::string_view name) {
    auto row = DeleteDeclaration(catalog, store, wal, catalog::kSysTransitionsTable, kKind, name);
    if (!row.ok()) return row.status();
    return ToTransitionDef(std::move(row.value()));
}

}  // namespace kds::exec
