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
        return Status::Unsupported(
            "the parent relation is a heap relation and has no primary-key index, so every check "
            "of this foreign key would scan it - declare the parent CLUSTERED BTREE");
    }
    return Status::OK();
}

Status CheckForeignKeyColocation(const TableAccess& parent, const TableAccess& child) {
    if (parent.owner_core != child.owner_core) {
        return Status::Unsupported(
            "a foreign key may not cross cores: the child relation is owned by core " +
            std::to_string(child.owner_core) + " and the parent by core " +
            std::to_string(parent.owner_core));
    }
    return Status::OK();
}

}  // namespace kds::catalog
