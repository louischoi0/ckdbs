#include "kds/catalog/foreign_key.hpp"

#include <string>

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
    if (parent.schema.heap()) {
        // BZ-R7: the child's column holds the parent's tuple key (F1), and a
        // heap's tuple key is hidden - no statement reads or names it, so no
        // client could write a value this constraint would accept.
        return Status::Unsupported(
            "the parent relation is a heap: its tuple key is hidden, so no child value could "
            "name a parent row - reference a relation with a declared tuple key");
    }
    if (parent.clustered_type != ClusteredType::kBtree) {
        // A system relation's catalog chain has no descent to check against.
        return Status::InvalidArgument("the parent relation is a system relation");
    }
    return Status::OK();
}

}  // namespace kds::catalog
