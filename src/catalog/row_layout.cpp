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
        // docs/spec/types.md TY1/TY2/TY4. All three are fixed-width by
        // construction, which is why they are expressible at all: a
        // relation's row size is a schema constant (invariant 13), so a
        // type with no decided width cannot be part of one.
        case kTypeValDate: return 4;        // int32 epoch days
        case kTypeValTimestamp: return 8;   // int64 UTC micros
        case kTypeValDecimal: return 8;      // int64 unscaled, 1 <= p <= 18
        case kTypeValDecimalWide: return 16; // int128 unscaled, 19 <= p <= 38
        case kTypeValFloat:
            return Status::Unsupported(
                "column '" + std::string(NameView(col.name)) +
                "' has type float, which this engine does not store (docs/spec/types.md TY1): "
                "IEEE comparison and aggregation semantics conflict with its exactness "
                "discipline. Use decimal(p, s) for money and int64 for counts");
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
    layout.null_bits.reserve(schema.columns.size());

    // The Keystone word leads every tuple and the pk is carried *only*
    // there, never also as a body column (invariant 11), so column 0
    // occupies the word and the body starts after it - and it can never
    // be nullable, because the word has no NULL encoding. CREATE TABLE
    // refuses the declaration with its byte position; this is the layout's
    // own defense for a schema that arrives by another door.
    if (!schema.columns.empty() && !schema.columns[0].notnull) {
        return Status::InvalidArgument(
            "the first column is the primary key and cannot be nullable (invariant 11: the "
            "Keystone word has no NULL encoding)");
    }
    layout.offsets.push_back(0);
    layout.null_bits.push_back(kNoNullBit);
    std::uint32_t offset = kKeystoneWordSize;
    std::uint16_t next_bit = 0;

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
        // One bit per nullable column, in ascending schema position
        // (null.md §2); NOT NULL columns consume no bit, which is
        // what keeps every existing relation's bitmap at zero bytes.
        layout.null_bits.push_back(schema.columns[i].notnull ? kNoNullBit : next_bit++);
    }

    // Appended after the last column, so every offset above is unchanged
    // and a zero-filled payload still means "nothing is NULL".
    layout.null_bitmap_bytes = (static_cast<std::uint32_t>(next_bit) + 7) / 8;
    offset += layout.null_bitmap_bytes;

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
