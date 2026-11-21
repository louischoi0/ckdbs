#include "kds/catalog/foreign_key.hpp"

#include <string>
#include <string_view>

namespace kds::catalog {

Status CheckForeignKeyDeclaration(const TableAccess& parent, const SysColumnRow& child_column,
                                  std::uint16_t child_column_pos) {
    const std::string column(NameView(child_column.name));

    if (child_column_pos == 0) {
        return Status::InvalidArgument("column '" + column +
                                       "' is the tuple key and cannot be a foreign key - the "
                                       "Keystone id is the row's identity, not a field of it");
    }
    if (!IsIntegerTypeVal(child_column.type_val)) {
        return Status::InvalidArgument("column '" + column +
                                       "' cannot hold a Keystone id, so it cannot reference one");
    }
    if (parent.schema.hidden_keystone()) {
        // BZ-R7: the child's column holds the parent's tuple key (F1), and a
        // heap's tuple key is hidden - no statement reads or names it, so no
        // client could write a value this constraint would accept.
        return Status::Unsupported(
            "the parent relation is a heap: its tuple key is hidden, so no child value could "
            "name a parent row - reference a relation with a declared tuple key");
    }
    // DN-Q5 (a): a heap whose Keystone is its declared `int64` primary key
    // is a parent as a btree is - F1's Keystone, found through the Keystone
    // index (`exec::LocateRow`); a hidden Keystone's heap is refused above.
    if (parent.clustered_type != ClusteredType::kBtree &&
        parent.clustered_type != ClusteredType::kHeap) {
        // A system relation's catalog chain has no descent to check against.
        return Status::InvalidArgument("the parent relation is a system relation");
    }
    return Status::OK();
}

Status RefuseSelfReference(std::string_view child_column) {
    return Status::NotImplemented(
        "column '" + std::string(child_column) +
        "' references the relation it is declared on: a self-referencing foreign key is not "
        "built (docs/spec/foreign-keys.md §1)");
}

}  // namespace kds::catalog
