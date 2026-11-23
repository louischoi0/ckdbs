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
    // **A parent whose Keystone is hidden is a parent as any other is**
    // (DR-S3c, DR-Q4 (a)): F1 is unchanged - the child's column holds the
    // parent's Keystone id - and a client reads that id by name, at the
    // parent's insert (`RETURNING keyidentity`) and by its key afterwards
    // (`SELECT keyidentity`), which is what BZ-R7's refusal said no
    // statement could do. A reference by the parent's primary key *value*
    // is not this and is not built: that value can move (DR-R3).
    //
    // DN-Q5 (a): a heap is a parent as a btree is - F1's Keystone, found
    // through the Keystone index (`exec::LocateRow`).
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
