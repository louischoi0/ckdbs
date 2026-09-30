#include "kds/catalog/schema.hpp"

#include <string>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/tagged_cell.hpp"  // varchar(N)'s bounds are the cell width's

namespace kds::catalog {

const SysColumnRow* Schema::FindColumn(std::string_view name) const noexcept {
    for (const auto& col : columns) {
        if (NameView(col.name) == name) {
            return &col;
        }
    }
    return nullptr;
}

bool IsIntegerTypeVal(std::uint32_t type_val) noexcept {
    switch (type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        case kTypeValUint64: return true;
        default: return false;
    }
}

bool SchemaCanSpill(const Schema& schema) noexcept {
    for (const auto& col : schema.columns) {
        // varchar is the only tagged-cell type today. `char` is fixed by
        // declaration and every numeric type by its width, so neither can
        // ever exceed its cell.
        if (col.type_val == kTypeValVarchar) return true;
    }
    return false;
}

Status CheckDeclarableColumnTypes(const Schema& schema) {
    for (const auto& col : schema.columns) {
        // A declared varchar cell width, checked at the catalog's own door
        // for the reason the decimal check below is here: the dispatcher
        // bounds it and the parser does not, so a schema built by neither -
        // a tool, a test, a future caller - is exactly what this is for.
        // **And nothing else catches it**: `RowLayout::Build` accepts an
        // 8-byte varchar cell happily, and the relation then refuses every
        // INSERT with a message about a cell width nobody can trace to a
        // declaration.
        //
        // `char` needs no arm here, deliberately: `Build` runs on the same
        // schema seven lines after this function and already refuses a
        // zero-width column, so a second refusal would be a third wording
        // of one condition (the phase-A review's S-1).
        //
        // A varchar's `len` of 0 is not unset: it is "the instance's
        // width", which every pre-v2.5.0 column carries, so only a
        // *declared* width is checked.
        if (col.type_val == kTypeValVarchar && col.len != 0) {
            if (Status s = storage::CheckInlineCellWidth(col.len); !s.ok()) {
                return s.WithContext("column '" + std::string(NameView(col.name)) + "'");
            }
        }
        // A decimal with no scale stored has values with no defined
        // meaning. The parser refuses a bare `decimal` and the dispatcher
        // packs the pair, so reaching here with an unset one means the
        // schema was built by neither - which is exactly the case a check
        // at the catalog's own door is for.
        if ((col.type_val == kTypeValDecimal || col.type_val == kTypeValDecimalWide) &&
            DecimalPrecisionOf(col.len) == 0) {
            return Status::InvalidArgument(
                "column '" + std::string(NameView(col.name)) +
                "' is decimal with no precision recorded (docs/spec/types.md TY2)");
        }
    }
    return Status::OK();
}

Status CheckKeystoneColumn(const Schema& schema) {
    if (schema.columns.empty()) {
        return Status::InvalidArgument(
            "relation has no columns; the first column is the mandatory Keystone primary key");
    }
    if (!IsIntegerTypeVal(schema.columns.front().type_val)) {
        return Status::InvalidArgument("primary-key column '" +
                                        std::string(NameView(schema.columns.front().name)) +
                                        "' must be an integer type - it carries the Keystone id");
    }
    return Status::OK();
}

}  // namespace kds::catalog
