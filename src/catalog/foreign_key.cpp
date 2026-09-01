#include "kds/catalog/foreign_key.hpp"

#include <string>

namespace kds::catalog {

Status CheckForeignKeyDeclaration(const TableAccess& parent, const SysColumnRow& child_column,
                                  std::uint16_t child_column_pos) {
    const std::string column(NameView(child_column.name));

    if (child_column_pos == 0) {
        return Status::InvalidArgument("column '" + column +
                                       "' is the primary key and cannot be a foreign key - the "
                                       "Keystone id is the row's identity, not a field of it");
    }
    if (!IsIntegerTypeVal(child_column.type_val)) {
        return Status::InvalidArgument("column '" + column +
                                       "' cannot hold a Keystone id, so it cannot reference one");
    }
    if (parent.clustered_type != ClusteredType::kBtree) {
        // Named as the parent's property rather than as "unsupported", so
        // the operator reads the one thing that would fix it.
        return Status::NotImplemented(
            "the parent relation is a heap relation and has no primary-key index, so every check "
            "of this foreign key would scan it - declare the parent CLUSTERED BTREE");
    }
    return Status::OK();
}

Status CheckForeignKeyColocation(const TableAccess& parent, const TableAccess& child) {
    // **Converted from constraint to recommendation 2026-09-01** (AH-T4,
    // the operator's AH-R6 mark; `docs/spec/foreign-keys.md` F5 as
    // amended). F5 read "parent and child must be owned by the same core"
    // and refused otherwise, because the forward check descended the
    // parent locally and had nowhere to ask. It has somewhere now: the
    // check hoists to the dispatch fork and probes the parent's owner, one
    // round per owner, leaving a reference intent behind (§2a).
    //
    // So the pair is **admitted**, and what survives is advice rather than
    // a gate: colocation is still the cheaper shape, and a **namespace** is
    // how a user asks for it (`ratification-af-namespace.md` AF-P5) - a
    // constraint that never has to cross beats one that crosses correctly.
    // The advice is not spelled here, because this function no longer
    // refuses and a message nobody reads is not advice; it belongs where a
    // user is choosing, which is AF-T3's `CREATE NAMESPACE` surface.
    //
    // **What a cross-owner pair costs, and it is not nothing** - written
    // here so the admission is not read as "free":
    //
    //   - every INSERT/UPDATE of the child pays one ring round trip per
    //     distinct parent owner, at the dispatch fork, before any row work;
    //   - a **`DELETE` of the parent is refused** while the child lives on
    //     another core. RESTRICT needs an authoritative "no children" and
    //     this core cannot see them; the fan-out that would ask is not
    //     built (`fk_check.cpp`'s owner arm, `foreign-keys.md` §3a).
    //
    // Both are fail-closed and neither is a wrong answer. The parameters
    // stay so the signature - and every caller - is unchanged the day a
    // reason to refuse returns.
    (void)parent;
    (void)child;
    return Status::OK();
}

}  // namespace kds::catalog
