#include "kds/exec/row_codec.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/tagged_cell.hpp"
#include "kds/storage/varheap.hpp"

namespace kds::exec {

namespace {

using catalog::kTypeValBool;
using catalog::kTypeValChar;
using catalog::kTypeValDecimal;
using catalog::kTypeValFloat;
using catalog::kTypeValInt16;
using catalog::kTypeValInt32;
using catalog::kTypeValInt64;
using catalog::kTypeValInt8;
using catalog::kTypeValUint64;
using catalog::kTypeValVarchar;

void StoreLe64(std::byte* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

std::uint64_t LoadLe64(const std::byte* in) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    }
    return v;
}

void PutLE(std::span<std::byte> out, std::uint64_t v, int width) {
    for (int i = 0; i < width; ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

std::uint64_t GetLE(std::span<const std::byte> bytes, int width) {
    std::uint64_t v = 0;
    for (int i = 0; i < width; ++i) {
        v |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    return v;
}

// Sign-extends a `width`-byte little-endian two's-complement value read
// via GetLE() back to a full int64_t.
std::int64_t SignExtend(std::uint64_t raw, int width) {
    int bits = width * 8;
    std::uint64_t mask = (bits == 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1);
    raw &= mask;
    std::uint64_t sign_bit = std::uint64_t{1} << (bits - 1);
    if (raw & sign_bit) raw |= ~mask;
    return static_cast<std::int64_t>(raw);
}

bool FitsSigned(std::int64_t v, int width) {
    if (width >= 8) return true;
    std::int64_t lo = -(std::int64_t{1} << (width * 8 - 1));
    std::int64_t hi = (std::int64_t{1} << (width * 8 - 1)) - 1;
    return v >= lo && v <= hi;
}

int IntWidthFor(std::uint32_t type_val) {
    switch (type_val) {
        case kTypeValInt8: return 1;
        case kTypeValInt16: return 2;
        case kTypeValInt32: return 4;
        default: return 8;  // kTypeValInt64
    }
}

StatusOr<std::uint64_t> ParseUint64Text(const std::string& text) {
    if (text.empty() || text.front() == '-') {
        return Status::InvalidArgument(
            "expected a non-negative integer for a uint64 column, got '" + text + "'");
    }
    std::uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return Status::InvalidArgument("invalid uint64 literal '" + text + "'");
    }
    return v;
}

// Writes one column's value into `cell`, which is exactly that column's
// width in the relation's fixed row (catalog::RowLayout). Nothing here
// appends or decides a size: the size was decided by the schema.
Status EncodeOneValue(const catalog::SysColumnRow& col, const parser::AstValue& val,
                       std::span<std::byte> cell, const VarHeapSink& varheap) {
    std::string col_name(catalog::NameView(col.name));

    if (val.type == parser::ValueType::kNull) {
        return Status::InvalidArgument("column '" + col_name +
                                        "' is NULL - NULL values are not supported yet");
    }

    switch (col.type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64: {
            if (val.type != parser::ValueType::kInt) {
                return Status::InvalidArgument("column '" + col_name + "' expects an integer");
            }
            int width = IntWidthFor(col.type_val);
            if (!FitsSigned(val.int_val, width)) {
                return Status::InvalidArgument("value " + std::to_string(val.int_val) +
                                                " does not fit column '" + col_name + "'");
            }
            PutLE(cell, static_cast<std::uint64_t>(val.int_val), width);
            return Status::OK();
        }
        case kTypeValUint64: {
            if (val.type != parser::ValueType::kInt) {
                return Status::InvalidArgument("column '" + col_name + "' expects an integer");
            }
            auto u = ParseUint64Text(val.raw_int_text);
            if (!u.ok()) return u.status();
            PutLE(cell, u.value(), 8);
            return Status::OK();
        }
        case kTypeValBool: {
            if (val.type != parser::ValueType::kInt || (val.int_val != 0 && val.int_val != 1)) {
                return Status::InvalidArgument(
                    "column '" + col_name + "' expects 0 or 1 (no boolean literal in this grammar)");
            }
            PutLE(cell, static_cast<std::uint64_t>(val.int_val), 1);
            return Status::OK();
        }
        case kTypeValChar: {
            if (val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument("column '" + col_name + "' expects a string");
            }
            if (val.str_val.size() > cell.size()) {
                return Status::InvalidArgument("value too long for column '" + col_name +
                                                "' (max " + std::to_string(cell.size()) +
                                                " bytes)");
            }
            // Zero the whole cell before writing, for the reason
            // EncodeInlineCell() does: an overwrite must not leave the tail
            // of a longer previous value underneath a shorter new one.
            std::fill(cell.begin(), cell.end(), std::byte{0});
            for (std::size_t i = 0; i < val.str_val.size(); ++i) {
                cell[i] = static_cast<std::byte>(static_cast<unsigned char>(val.str_val[i]));
            }
            return Status::OK();
        }
        case kTypeValVarchar: {
            if (val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument("column '" + col_name + "' expects a string");
            }
            // One tagged cell of kds.inline_cell_width bytes, whatever the
            // value's length - that is invariant 13, and it holds whether
            // the bytes end up in the cell or in the var-heap.
            Status inlined = storage::EncodeInlineCell(cell, val.str_val);
            if (inlined.ok()) return Status::OK();
            if (inlined.code() != StatusCode::kOutOfSpace) {
                return inlined.WithContext("column '" + col_name + "'");
            }

            // Too long to inline: spill. The cell still occupies exactly
            // the same bytes; only its tag changes, which is why an UPDATE
            // crossing the boundary in either direction still cannot move
            // the tuple.
            if (!varheap.usable()) {
                return Status::Unsupported(
                    "column '" + col_name + "': value of " +
                    std::to_string(val.str_val.size()) +
                    " bytes must spill to the var-heap, and this caller supplied no var-heap "
                    "chain to spill into");
            }

            auto bytes = std::as_bytes(std::span<const char>(val.str_val));
            auto ptr = varheap::ChainAppend(*varheap.store, varheap.root, bytes);
            if (!ptr.ok()) return ptr.status().WithContext("column '" + col_name + "'");

            if (varheap.appended != nullptr) {
                varheap.appended->push_back(
                    AppendedSpill{ptr.value(), std::vector<std::byte>(bytes.begin(), bytes.end())});
            }

            return storage::EncodeSpilledCell(cell,
                                              static_cast<std::uint32_t>(val.str_val.size()),
                                              varheap::EncodePtr(ptr.value()))
                .WithContext("column '" + col_name + "'");
        }
        case kTypeValFloat:
        case kTypeValDecimal:
            // Unreachable through a catalog-built layout: RowLayout::Build()
            // refuses these columns at CREATE TABLE (schema.hpp). Kept as a
            // failure rather than an assert for a hand-built schema.
            return Status::Unsupported(
                "column '" + col_name +
                "' has type float/decimal - no on-disk encoding exists yet (open decision, see "
                "catalog/schema.hpp)");
        default:
            return Status::InvalidArgument("column '" + col_name + "' has an unrecognized type_val");
    }
}

// Decodes one column from its cell into a slot the caller already owns.
// Assigning rather than appending is what lets a chain frame reuse its
// buffer for every row instead of allocating one per row per step (V16).
Status DecodeOneValueInto(const catalog::SysColumnRow& col, std::span<const std::byte> cell,
                           std::size_t column_index, parser::AstValue& out,
                           std::vector<PendingSpill>* spills) {
    std::string col_name(catalog::NameView(col.name));

    switch (col.type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64: {
            int width = IntWidthFor(col.type_val);
            std::int64_t v = SignExtend(GetLE(cell, width), width);
            out.type = parser::ValueType::kInt;
            out.int_val = v;
            out.raw_int_text = std::to_string(v);
            out.str_val.clear();
            return Status::OK();
        }
        case kTypeValUint64: {
            std::uint64_t v = GetLE(cell, 8);
            out.type = parser::ValueType::kInt;
            out.int_val = static_cast<std::int64_t>(v);
            out.raw_int_text = std::to_string(v);
            out.str_val.clear();
            return Status::OK();
        }
        case kTypeValBool: {
            std::uint64_t v = GetLE(cell, 1);
            out.type = parser::ValueType::kInt;
            out.int_val = static_cast<std::int64_t>(v);
            out.raw_int_text = std::to_string(v);
            out.str_val.clear();
            return Status::OK();
        }
        case kTypeValChar: {
            std::string s;
            for (std::size_t i = 0; i < cell.size(); ++i) {
                auto b = static_cast<unsigned char>(cell[i]);
                if (b == 0) break;
                s.push_back(static_cast<char>(b));
            }
            out.type = parser::ValueType::kStr;
            out.str_val = std::move(s);
            out.raw_int_text.clear();
            return Status::OK();
        }
        case kTypeValVarchar: {
            auto decoded = storage::DecodeCell(cell);
            if (!decoded.ok()) {
                return decoded.status().WithContext("column '" + col_name + "'");
            }
            if (decoded.value().tag == storage::CellTag::kNull) {
                out.type = parser::ValueType::kNull;
                out.str_val.clear();
                out.raw_int_text.clear();
                return Status::OK();
            }
            if (decoded.value().tag == storage::CellTag::kSpilled) {
                // Recorded, not fetched: R1 forbids a page fetch while the
                // caller's span into this tuple's page is live (row_codec.hpp).
                if (spills == nullptr) {
                    return Status::Unsupported(
                        "column '" + col_name +
                        "' holds a spilled value and this caller cannot resolve one; pass a "
                        "pending-spill list and call ResolveSpills() after releasing the page");
                }
                out.type = parser::ValueType::kStr;
                out.str_val.clear();
                out.raw_int_text.clear();
                spills->push_back(PendingSpill{column_index,
                                               varheap::DecodePtr(decoded.value().varheap_ptr),
                                               decoded.value().len});
                return Status::OK();
            }
            std::string s(decoded.value().bytes.size(), '\0');
            for (std::size_t i = 0; i < decoded.value().bytes.size(); ++i) {
                s[i] = static_cast<char>(static_cast<unsigned char>(decoded.value().bytes[i]));
            }
            out.type = parser::ValueType::kStr;
            out.str_val = std::move(s);
            out.raw_int_text.clear();
            return Status::OK();
        }
        case kTypeValFloat:
        case kTypeValDecimal:
            return Status::Corruption(
                "column '" + col_name +
                "' has type float/decimal, which no row this codec wrote should ever contain "
                "(see row_codec.hpp)");
        default:
            return Status::Corruption("column '" + col_name + "' has an unrecognized type_val");
    }
}

// A layout is only meaningful for the schema it was built from. Checked at
// every entry point rather than trusted, because the failure mode of a
// mismatched pair is not an error but a *wrong row*: offsets that address
// the right bytes for a different relation.
Status CheckLayoutMatches(const catalog::Schema& schema, const catalog::RowLayout& layout) {
    if (layout.offsets.size() != schema.columns.size() || layout.row_size == 0) {
        return Status::InvalidArgument(
            "row layout has " + std::to_string(layout.offsets.size()) +
            " column offset(s) for a schema of " + std::to_string(schema.columns.size()) +
            " column(s)");
    }
    return Status::OK();
}

// The span of `payload` column `i` occupies: from its offset to the next
// column's, or to the end of the row for the last one.
std::span<const std::byte> CellOf(const catalog::RowLayout& layout,
                                   std::span<const std::byte> payload, std::size_t i) {
    const std::size_t begin = layout.offsets[i];
    const std::size_t end =
        (i + 1 < layout.offsets.size()) ? layout.offsets[i + 1] : layout.row_size;
    return payload.subspan(begin, end - begin);
}

std::span<std::byte> MutableCellOf(const catalog::RowLayout& layout, std::span<std::byte> payload,
                                    std::size_t i) {
    const std::size_t begin = layout.offsets[i];
    const std::size_t end =
        (i + 1 < layout.offsets.size()) ? layout.offsets[i + 1] : layout.row_size;
    return payload.subspan(begin, end - begin);
}

bool CompareInt(std::int64_t a, std::int64_t b, parser::CompareOp op) {
    switch (op) {
        case parser::CompareOp::kEq: return a == b;
        case parser::CompareOp::kNeq: return a != b;
        case parser::CompareOp::kLt: return a < b;
        case parser::CompareOp::kLte: return a <= b;
        case parser::CompareOp::kGt: return a > b;
        case parser::CompareOp::kGte: return a >= b;
    }
    return false;
}

bool CompareUint(std::uint64_t a, std::uint64_t b, parser::CompareOp op) {
    switch (op) {
        case parser::CompareOp::kEq: return a == b;
        case parser::CompareOp::kNeq: return a != b;
        case parser::CompareOp::kLt: return a < b;
        case parser::CompareOp::kLte: return a <= b;
        case parser::CompareOp::kGt: return a > b;
        case parser::CompareOp::kGte: return a >= b;
    }
    return false;
}

bool CompareStr(std::string_view a, std::string_view b, parser::CompareOp op) {
    switch (op) {
        case parser::CompareOp::kEq: return a == b;
        case parser::CompareOp::kNeq: return a != b;
        case parser::CompareOp::kLt: return a < b;
        case parser::CompareOp::kLte: return a <= b;
        case parser::CompareOp::kGt: return a > b;
        case parser::CompareOp::kGte: return a >= b;
    }
    return false;
}

}  // namespace

StatusOr<std::vector<std::byte>> EncodeRow(const catalog::Schema& schema,
                                            const catalog::RowLayout& layout, std::uint64_t id,
                                            const std::vector<parser::AstValue>& values,
                                            const VarHeapSink& varheap) {
    if (Status s = catalog::CheckKeystoneColumn(schema); !s.ok()) return s;
    if (Status s = CheckLayoutMatches(schema, layout); !s.ok()) return s;

    const std::size_t expected = schema.columns.size() - 1;
    if (values.size() != expected) {
        return Status::InvalidArgument("expected " + std::to_string(expected) +
                                        " value(s) after the primary key, got " +
                                        std::to_string(values.size()));
    }

    // The Keystone word leads every tuple (heap-and-tuple.md section 4).
    // The pk column is therefore *not* encoded into the body: storing it
    // twice is how the two copies come to disagree.
    auto word = Keystone::Encode(id, /*flags=*/0, /*reserved=*/0);
    if (!word.ok()) return word.status();

    // One buffer of exactly the schema constant, zero-filled: every byte of
    // a tuple is written by this function, including the padding inside a
    // cell that a short value does not fill (invariant 13).
    std::vector<std::byte> out(layout.row_size, std::byte{0});
    StoreLe64(out.data(), word.value());

    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        if (Status s = EncodeOneValue(schema.columns[i], values[i - 1],
                                       MutableCellOf(layout, out, i), varheap);
            !s.ok()) {
            return s;
        }
    }

    if (out.size() != layout.row_size) {
        // Not reachable through the code above - asserted rather than
        // assumed because "no code path produces a tuple whose size differs
        // from its relation's constant" is invariant 13 itself, and the
        // spec puts that check in the row codec by name (section 2).
        return Status::Corruption("encoded row is " + std::to_string(out.size()) +
                                   " bytes for a relation whose row size is " +
                                   std::to_string(layout.row_size));
    }
    return out;
}

StatusOr<std::uint64_t> RowKeystoneId(std::span<const std::byte> payload) {
    if (payload.size() < kKeystoneWordSize) {
        return Status::Corruption("tuple payload is shorter than its Keystone word");
    }
    return Keystone::Decode(LoadLe64(payload.data())).id;
}

Status DecodeRowInto(const catalog::Schema& schema, const catalog::RowLayout& layout,
                     std::span<const std::byte> payload, std::span<parser::AstValue> out,
                     std::vector<PendingSpill>* spills) {
    if (spills != nullptr) spills->clear();
    if (Status s = catalog::CheckKeystoneColumn(schema); !s.ok()) return s;
    if (Status s = CheckLayoutMatches(schema, layout); !s.ok()) return s;
    if (out.size() != schema.columns.size()) {
        return Status::InvalidArgument("decode target has " + std::to_string(out.size()) +
                                        " slot(s) for a schema of " +
                                        std::to_string(schema.columns.size()) + " column(s)");
    }

    // Checked redundancy (invariant 13): the row size is a schema constant,
    // so a stored payload of any other length is not a row this build can
    // interpret - the slot's `length` and the header's `data_len` add no
    // information the schema does not already give.
    if (payload.size() != layout.row_size) {
        return Status::Corruption("tuple payload is " + std::to_string(payload.size()) +
                                   " bytes for a relation whose row size is " +
                                   std::to_string(layout.row_size));
    }

    auto id = RowKeystoneId(payload);
    if (!id.ok()) return id.status();

    // The pk is not in the body: it lives in the Keystone word, which is
    // why the loop below starts at column 1.
    out[0].type = parser::ValueType::kInt;
    out[0].int_val = static_cast<std::int64_t>(id.value());
    out[0].raw_int_text = std::to_string(id.value());
    out[0].str_val.clear();

    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        if (Status s = DecodeOneValueInto(schema.columns[i], CellOf(layout, payload, i), i, out[i],
                                           spills);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
}

StatusOr<std::vector<parser::AstValue>> DecodeRow(const catalog::Schema& schema,
                                                   const catalog::RowLayout& layout,
                                                   std::span<const std::byte> payload,
                                                   std::vector<PendingSpill>* spills) {
    // A wrapper over DecodeRowInto, so there is exactly one decoder. Kept
    // because several callers want an owned row and allocate once anyway
    // (DESCRIBE, a point lookup, the tests); the chain executor is the one
    // that cannot afford it per row.
    std::vector<parser::AstValue> out(schema.columns.size());
    if (Status s = DecodeRowInto(schema, layout, payload, out, spills); !s.ok()) return s;
    return out;
}

Status ResolveSpills(storage::PageStore& store, const std::vector<PendingSpill>& spills,
                     std::span<parser::AstValue> out) {
    for (const PendingSpill& spill : spills) {
        if (spill.column >= out.size()) {
            return Status::Corruption("pending spill names column " +
                                       std::to_string(spill.column) + " of a row with " +
                                       std::to_string(out.size()) + " column(s)");
        }
        auto bytes = varheap::Fetch(store, spill.ptr);
        if (!bytes.ok()) return bytes.status();

        // The cell's own length and the var-heap slot's must agree. Two
        // records of one fact, so a disagreement is corruption to report
        // rather than a length to pick between.
        if (bytes.value().size() != spill.len) {
            return Status::Corruption(
                "spilled value is " + std::to_string(bytes.value().size()) +
                " bytes in the var-heap but the tuple's cell says " + std::to_string(spill.len));
        }

        std::string text(bytes.value().size(), '\0');
        for (std::size_t i = 0; i < bytes.value().size(); ++i) {
            text[i] = static_cast<char>(static_cast<unsigned char>(bytes.value()[i]));
        }
        out[spill.column].type = parser::ValueType::kStr;
        out[spill.column].str_val = std::move(text);
        out[spill.column].raw_int_text.clear();
    }
    return Status::OK();
}

std::string FormatValue(const parser::AstValue& value) {
    switch (value.type) {
        case parser::ValueType::kInt:
            return !value.raw_int_text.empty() ? value.raw_int_text : std::to_string(value.int_val);
        case parser::ValueType::kStr:
            return value.str_val;
        case parser::ValueType::kNull:
        default:
            return "NULL";
    }
}

bool CompareValues(std::uint32_t type_val, const parser::AstValue& lhs,
                   const parser::AstValue& rhs, parser::CompareOp op) {
    if (lhs.type == parser::ValueType::kNull || rhs.type == parser::ValueType::kNull) {
        return false;  // no NULL support; NULL never matches (see file comment)
    }
    if (type_val == kTypeValUint64) {
        // Through the digit text: int_val is signed and cannot represent
        // the upper half of the unsigned range, so comparing it would
        // order large ids below small ones.
        auto a = ParseUint64Text(lhs.raw_int_text);
        auto b = ParseUint64Text(rhs.raw_int_text);
        if (!a.ok() || !b.ok()) return false;
        return CompareUint(a.value(), b.value(), op);
    }
    if (lhs.type == parser::ValueType::kInt && rhs.type == parser::ValueType::kInt) {
        return CompareInt(lhs.int_val, rhs.int_val, op);
    }
    if (lhs.type == parser::ValueType::kStr && rhs.type == parser::ValueType::kStr) {
        return CompareStr(lhs.str_val, rhs.str_val, op);
    }
    return false;  // incompatible value kinds
}

}  // namespace kds::exec
