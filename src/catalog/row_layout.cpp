#include <string>

#include "kds/catalog/schema.hpp"

#include "kds/catalog/well_known.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/tagged_cell.hpp"

namespace kds::catalog {

StatusOr<std::uint32_t> RowLayout::ColumnWidth(const SysColumnRow& col,
                                                std::uint32_t inline_cell_width) {
    switch (col.type_val) {
        case kTypeValInt8: return 1;
        case kTypeValInt16: return 2;
        case kTypeValInt32: return 4;
        case kTypeValInt64: return 8;
        case kTypeValUint64: return 8;
        case kTypeValBool: return 1;
        // Already fixed-width by declaration - the one variable-width type
        // that never needed a tagged cell.
        case kTypeValChar: return col.len;
        // The tagged cell: one width for every value, whatever it holds.
        case kTypeValVarchar: return inline_cell_width;
        case kTypeValFloat:
        case kTypeValDecimal:
            return Status::Unsupported(
                "column '" + std::string(NameView(col.name)) +
                "' has type float/decimal, which has no on-disk encoding yet - and under the "
                "fixed-length rule a relation's row size is a schema constant, so a column with "
                "no decided width cannot be part of one (docs/heap-and-tuple.md section 3.3)");
        default:
            return Status::InvalidArgument("column '" + std::string(NameView(col.name)) +
                                            "' has an unrecognized type_val " +
                                            std::to_string(col.type_val));
    }
}

StatusOr<RowLayout> RowLayout::Build(const Schema& schema, std::uint32_t inline_cell_width) {
    if (Status s = CheckKeystoneColumn(schema); !s.ok()) return s;
    if (Status s = storage::CheckInlineCellWidth(inline_cell_width); !s.ok()) return s;

    RowLayout layout;
    layout.inline_cell_width = inline_cell_width;
    layout.offsets.reserve(schema.columns.size());

    // The Keystone word leads every tuple and the pk is carried *only*
    // there, never also as a body column (invariant 11), so column 0
    // occupies the word and the body starts after it.
    layout.offsets.push_back(0);
    std::uint32_t offset = kKeystoneWordSize;

    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        auto width = ColumnWidth(schema.columns[i], inline_cell_width);
        if (!width.ok()) return width.status();

        // A zero-width column would give two columns the same offset, which
        // makes the layout ambiguous rather than merely useless. `char`
        // with len 0 is the only way to reach it.
        if (width.value() == 0) {
            return Status::InvalidArgument("column '" +
                                            std::string(NameView(schema.columns[i].name)) +
                                            "' has zero width; every column must occupy bytes");
        }

        layout.offsets.push_back(offset);
        offset += width.value();
    }

    if (offset > heap::kMaxTuplePayloadSize) {
        return Status::Unsupported(
            "row size " + std::to_string(offset) + " exceeds the " +
            std::to_string(heap::kMaxTuplePayloadSize) +
            " bytes a heap page can hold; no row of this relation could ever be inserted");
    }

    layout.row_size = offset;
    return layout;
}

}  // namespace kds::catalog
