#include "kds/exec/row_codec.hpp"

#include "kds/exec/type_literals.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
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
using catalog::kTypeValDate;
using catalog::kTypeValTimestamp;
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
    // **The name is built only where an error is built.** This is called
    // once per column per row, and constructing a std::string here
    // unconditionally - for messages that are almost never produced - was
    // measured as most of the per-column decode cost (workplan AP02,
    // bench/results-aggregate.md). `NameOf` costs nothing on the path that
    // succeeds, which is every path in a working system.
    const auto NameOf = [&col] { return std::string(catalog::NameView(col.name)); };

    if (val.type == parser::ValueType::kNull) {
        // Unreachable: EncodeRow's driver intercepts NULL before the cell
        // is written (the bitmap is the authority, null.md §3), so a
        // NULL arriving here bypassed it - a caller bug, not a user error.
        return Status::Corruption("column '" + NameOf() +
                                  "': a NULL reached the cell encoder; EncodeRow's driver owns "
                                  "NULL handling");
    }
    // A declared pattern's `$param` has no value to encode, and never will:
    // a declaration is not an execution. Refused by name rather than left to
    // fall through to "expects an integer", which would send whoever hit it
    // looking for a type error that is not there.
    if (val.type == parser::ValueType::kParam) {
        return Status::Corruption("column '" + NameOf() + "': parameter '$" +
                                  val.param_name() + "' has no bound value to store");
    }

    switch (col.type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64: {
            if (val.type != parser::ValueType::kInt) {
                return Status::InvalidArgument("column '" + NameOf() + "' expects an integer");
            }
            int width = IntWidthFor(col.type_val);
            if (!FitsSigned(val.int_val, width)) {
                return Status::InvalidArgument("value " + std::to_string(val.int_val) +
                                                " does not fit column '" + NameOf() + "'");
            }
            PutLE(cell, static_cast<std::uint64_t>(val.int_val), width);
            return Status::OK();
        }
        case kTypeValUint64: {
            if (val.type != parser::ValueType::kInt) {
                return Status::InvalidArgument("column '" + NameOf() + "' expects an integer");
            }
            // `raw_int_text` carries the value only when `int_val` cannot -
            // a uint64 above INT64_MAX. A decoded row leaves it empty for
            // everything else (see DecodeOneValueInto), so re-encoding one -
            // which is what an UPDATE does to every column its SET list did
            // not touch - reads the integer it already has.
            auto u = ValueAsUint64(val);
            if (!u.ok()) return u.status().WithContext("column '" + NameOf() + "'");
            PutLE(cell, u.value(), 8);
            return Status::OK();
        }
        case kTypeValBool: {
            if (val.type != parser::ValueType::kInt || (val.int_val != 0 && val.int_val != 1)) {
                return Status::InvalidArgument(
                    "column '" + NameOf() + "' expects 0 or 1 (no boolean literal in this grammar)");
            }
            PutLE(cell, static_cast<std::uint64_t>(val.int_val), 1);
            return Status::OK();
        }
        case kTypeValChar: {
            if (val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument("column '" + NameOf() + "' expects a string");
            }
            if (val.str_val.size() > cell.size()) {
                return Status::InvalidArgument("value too long for column '" + NameOf() +
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
                return Status::InvalidArgument("column '" + NameOf() + "' expects a string");
            }
            // One tagged cell of kds.inline_cell_width bytes, whatever the
            // value's length - that is invariant 13, and it holds whether
            // the bytes end up in the cell or in the var-heap.
            Status inlined = storage::EncodeInlineCell(cell, val.str_val);
            if (inlined.ok()) return Status::OK();
            if (inlined.code() != StatusCode::kOutOfSpace) {
                return inlined.WithContext("column '" + NameOf() + "'");
            }

            // Too long to inline: spill. The cell still occupies exactly
            // the same bytes; only its tag changes, which is why an UPDATE
            // crossing the boundary in either direction still cannot move
            // the tuple.
            if (!varheap.usable()) {
                return Status::Unsupported(
                    "column '" + NameOf() + "': value of " +
                    std::to_string(val.str_val.size()) +
                    " bytes must spill to the var-heap, and this caller supplied no var-heap "
                    "chain to spill into");
            }

            auto bytes = std::as_bytes(std::span<const char>(val.str_val));
            auto appended = varheap::ChainAppend(*varheap.store, varheap.root, bytes,
                                                 varheap.owner_oid);
            if (!appended.ok()) return appended.status().WithContext("column '" + NameOf() + "'");
            const varheap::ChainAppendResult& grew = appended.value();

            if (varheap.appended != nullptr) {
                varheap.appended->push_back(
                    AppendedSpill{grew.ptr, std::vector<std::byte>(bytes.begin(), bytes.end()),
                                  grew.created_page_id, grew.linked_page_id});
            }

            return storage::EncodeSpilledCell(cell,
                                              static_cast<std::uint32_t>(val.str_val.size()),
                                              varheap::EncodePtr(grew.ptr))
                .WithContext("column '" + NameOf() + "'");
        }
        case kTypeValFloat:
            // Unreachable through a catalog-built layout: RowLayout::Build()
            // refuses a float column at CREATE TABLE, and TY1 makes that a
            // product decision rather than an undecided encoding. Kept as a
            // failure rather than an assert for a hand-built schema.
            return Status::Unsupported(
                "column '" + NameOf() +
                "' has type float, which this engine does not store (docs/spec/types.md TY1)");
        // ---- DATE / TIMESTAMP / DECIMAL (docs/spec/types.md TY7) -------
        //
        // **This is the only gate.** Each accepts two shapes and no others:
        // the *literal* a client wrote, as a string, which is parsed and
        // range-checked here; and the *decoded* form of a value already
        // stored, which an UPDATE carries back for every column its SET
        // list did not touch. A third shape would be a way for an
        // unvalidated value to reach the disk.
        case kTypeValDate: {
            std::int32_t days = 0;
            if (val.type == parser::ValueType::kStr) {
                auto parsed = ParseDateLiteral(val.str_val);
                if (!parsed.ok()) return parsed.status().WithContext("column '" + NameOf() + "'");
                days = parsed.value();
            } else if (val.type == parser::ValueType::kInt) {
                if (val.int_val < kMinEpochDay || val.int_val > kMaxEpochDay) {
                    return Status::OutOfRange("column '" + NameOf() +
                                              "' date is outside the supported range");
                }
                days = static_cast<std::int32_t>(val.int_val);
            } else {
                return Status::InvalidArgument("column '" + NameOf() +
                                                "' expects a date written as a string, "
                                                "'YYYY-MM-DD'");
            }
            PutLE(cell, static_cast<std::uint64_t>(static_cast<std::uint32_t>(days)), 4);
            return Status::OK();
        }
        case kTypeValTimestamp: {
            std::int64_t micros = 0;
            if (val.type == parser::ValueType::kStr) {
                auto parsed = ParseTimestampLiteral(val.str_val);
                if (!parsed.ok()) return parsed.status().WithContext("column '" + NameOf() + "'");
                micros = parsed.value();
            } else if (val.type == parser::ValueType::kInt) {
                if (val.int_val < kMinEpochMicros || val.int_val > kMaxEpochMicros) {
                    return Status::OutOfRange("column '" + NameOf() +
                                              "' timestamp is outside the supported range");
                }
                micros = val.int_val;
            } else {
                return Status::InvalidArgument(
                    "column '" + NameOf() +
                    "' expects a timestamp written as a string, "
                    "'YYYY-MM-DD HH:MM:SS[.ffffff]'");
            }
            PutLE(cell, static_cast<std::uint64_t>(micros), 8);
            return Status::OK();
        }
        case kTypeValDecimal: {
            const std::uint8_t precision = catalog::DecimalPrecisionOf(col.len);
            const std::uint8_t scale = catalog::DecimalScaleOf(col.len);
            std::int64_t unscaled = 0;
            if (val.type == parser::ValueType::kStr) {
                auto parsed = ParseDecimalLiteral(val.str_val, precision, scale);
                if (!parsed.ok()) return parsed.status().WithContext("column '" + NameOf() + "'");
                unscaled = parsed.value();
            } else if (val.type == parser::ValueType::kDecimal) {
                // **The scale must match, not be converted.** Rescaling
                // here would either lose digits or invent them, and TY6
                // defers cross-scale work whole rather than shipping half.
                if (val.scale != scale) {
                    return Status::InvalidArgument(
                        "column '" + NameOf() + "' has scale " + std::to_string(scale) +
                        " but the value carries scale " + std::to_string(val.scale) +
                        "; this engine does not rescale");
                }
                unscaled = val.int_val;
            } else {
                return Status::InvalidArgument("column '" + NameOf() +
                                                "' expects a decimal written as a string, "
                                                "e.g. '12.34'");
            }
            PutLE(cell, static_cast<std::uint64_t>(unscaled), 8);
            return Status::OK();
        }
        case catalog::kTypeValDecimalWide: {
            // The 16-byte sibling: the same two accepted shapes, the same
            // no-rescale rule, the wide parser. The int128 goes to the
            // cell as two LE uint64 halves, low first - invariant 6's
            // explicit encoding, never a memcpy of the builtin.
            const std::uint8_t precision = catalog::DecimalPrecisionOf(col.len);
            const std::uint8_t scale = catalog::DecimalScaleOf(col.len);
            Int128 unscaled = 0;
            if (val.type == parser::ValueType::kStr) {
                auto parsed = ParseDecimalLiteralWide(val.str_val, precision, scale);
                if (!parsed.ok()) return parsed.status().WithContext("column '" + NameOf() + "'");
                unscaled = parsed.value();
            } else if (val.type == parser::ValueType::kDecimalWide) {
                if (val.scale != scale) {
                    return Status::InvalidArgument(
                        "column '" + NameOf() + "' has scale " + std::to_string(scale) +
                        " but the value carries scale " + std::to_string(val.scale) +
                        "; this engine does not rescale");
                }
                unscaled = Int128FromHalves(val.dec_hi, val.int_val);
            } else {
                return Status::InvalidArgument("column '" + NameOf() +
                                                "' expects a decimal written as a string, "
                                                "e.g. '12.34'");
            }
            PutLE(cell.subspan(0, 8), static_cast<std::uint64_t>(Int128Low(unscaled)), 8);
            PutLE(cell.subspan(8, 8), static_cast<std::uint64_t>(Int128High(unscaled)), 8);
            return Status::OK();
        }
        default:
            return Status::InvalidArgument("column '" + NameOf() + "' has an unrecognized type_val");
    }
}


// A layout is only meaningful for the schema it was built from. Checked at
// every entry point rather than trusted, because the failure mode of a
// mismatched pair is not an error but a *wrong row*: offsets that address
// the right bytes for a different relation.
Status CheckLayoutMatches(const catalog::Schema& schema, const catalog::RowLayout& layout) {
    if (layout.offsets.size() != schema.columns.size() ||
        layout.null_bits.size() != schema.columns.size() || layout.row_size == 0) {
        return Status::InvalidArgument(
            "row layout has " + std::to_string(layout.offsets.size()) +
            " column offset(s) and " + std::to_string(layout.null_bits.size()) +
            " null bit(s) for a schema of " + std::to_string(schema.columns.size()) +
            " column(s)");
    }
    return Status::OK();
}

// The span of `payload` column `i` occupies: from its offset to the next
// column's, or - for the last one - to where the null bitmap begins
// (null.md §2.1: the bitmap is appended after the columns, so the
// last column's cell must not run to the end of the row; a write there
// would wipe every null bit. Zero bitmap bytes reduces to the old rule).
std::span<const std::byte> CellOf(const catalog::RowLayout& layout,
                                   std::span<const std::byte> payload, std::size_t i) {
    const std::size_t begin = layout.offsets[i];
    const std::size_t end = (i + 1 < layout.offsets.size())
                                ? layout.offsets[i + 1]
                                : layout.row_size - layout.null_bitmap_bytes;
    return payload.subspan(begin, end - begin);
}

std::span<std::byte> MutableCellOf(const catalog::RowLayout& layout, std::span<std::byte> payload,
                                    std::size_t i) {
    const std::size_t begin = layout.offsets[i];
    const std::size_t end = (i + 1 < layout.offsets.size())
                                ? layout.offsets[i + 1]
                                : layout.row_size - layout.null_bitmap_bytes;
    return payload.subspan(begin, end - begin);
}

// The one relational switch, for every operand type CompareValues
// dispatches: int64, uint64, Int128 and string all order by the same six
// arms. The IS forms are answered by CompareValues before any operand pair
// is formed (null.md), so their arms here are the guard for a bug,
// not an answer anyone reads.
template <typename T>
bool ApplyCompare(const T& a, const T& b, parser::CompareOp op) {
    switch (op) {
        case parser::CompareOp::kEq: return a == b;
        case parser::CompareOp::kNeq: return a != b;
        case parser::CompareOp::kLt: return a < b;
        case parser::CompareOp::kLte: return a <= b;
        case parser::CompareOp::kGt: return a > b;
        case parser::CompareOp::kGte: return a >= b;
        case parser::CompareOp::kIsNull:
        case parser::CompareOp::kIsNotNull:
            return false;
    }
    return false;
}

}  // namespace

// Decodes one column from its cell into a slot the caller already owns.
// Assigning rather than appending is what lets a chain frame reuse its
// buffer for every row instead of allocating one per row per step (V16).
//
// Declared in the header since IX11: a secondary index entry carries covered
// columns concatenated in the index's order, so the entry-side filter locates
// its own cells and needs exactly this.
Status DecodeOneValueInto(const catalog::SysColumnRow& col, std::span<const std::byte> cell,
                           std::size_t column_index, parser::AstValue& out,
                           std::vector<PendingSpill>* spills) {
    // Built only where an error is built - see EncodeOneValue above. This
    // one is the hotter of the two: every column of every row every scan
    // decodes came through here and paid for a name nobody read.
    const auto NameOf = [&col] { return std::string(catalog::NameView(col.name)); };

    switch (col.type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64: {
            int width = IntWidthFor(col.type_val);
            std::int64_t v = SignExtend(GetLE(cell, width), width);
            out.type = parser::ValueType::kInt;
            out.int_val = v;
            // **Left empty on purpose.** `raw_int_text` exists to carry a
            // value `int_val` cannot hold, which for a signed column is
            // never - and FormatValue() falls back to std::to_string(int_val)
            // for exactly this case, so the rendering is identical. Writing
            // it here cost a string conversion per integer column per row
            // decoded, on every scan and twice per UPDATE.
            out.raw_int_text.clear();
            out.str_val.clear();
            return Status::OK();
        }
        case kTypeValUint64: {
            std::uint64_t v = GetLE(cell, 8);
            out.type = parser::ValueType::kInt;
            out.int_val = static_cast<std::int64_t>(v);
            // The one case the text is needed for: above INT64_MAX the cast
            // above is lossy, so the digits are the only correct rendering
            // and the only thing a re-encode can read back.
            if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                out.raw_int_text = std::to_string(v);
            } else {
                out.raw_int_text.clear();
            }
            out.str_val.clear();
            return Status::OK();
        }
        case kTypeValBool: {
            std::uint64_t v = GetLE(cell, 1);
            out.type = parser::ValueType::kInt;
            out.int_val = static_cast<std::int64_t>(v);
            out.raw_int_text.clear();  // 0 or 1: int_val says it exactly
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
                return decoded.status().WithContext("column '" + NameOf() + "'");
            }
            if (decoded.value().tag == storage::CellTag::kNull) {
                // The bitmap said present (the driver checked it before
                // this cell was read), so a kNull tag here is the §3
                // disagreement: two authorities answering differently, on
                // the same footing as a payload whose length disagrees
                // with the schema constant.
                return Status::Corruption(
                    "column '" + std::string(catalog::NameView(col.name)) +
                    "' cell is tagged kNull while the null bitmap says present "
                    "(docs/spec/null.md §3)");
            }
            if (decoded.value().tag == storage::CellTag::kSpilled) {
                // Recorded, not fetched: R1 forbids a page fetch while the
                // caller's span into this tuple's page is live (row_codec.hpp).
                if (spills == nullptr) {
                    return Status::Unsupported(
                        "column '" + NameOf() +
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
            return Status::Corruption(
                "column '" + NameOf() +
                "' has type float, which no row this codec wrote should ever contain "
                "(see row_codec.hpp)");
        // ---- DATE / TIMESTAMP / DECIMAL -------------------------------
        //
        // **Integers out, no strings, no formatting** (TY5, workplan TY04).
        // A date's value *is* its epoch day, and rendering it is the
        // emission boundary's job - so nothing here allocates, exactly as
        // the int arm above documents, and a scan that never emits a row
        // never builds a single character of text.
        //
        // No re-validation either: these bytes were proven by
        // EncodeOneValue, which is the only gate (TY7).
        case kTypeValDate: {
            out.type = parser::ValueType::kInt;
            out.int_val = SignExtend(GetLE(cell, 4), 4);
            out.raw_int_text.clear();
            out.str_val.clear();
            return Status::OK();
        }
        case kTypeValTimestamp: {
            out.type = parser::ValueType::kInt;
            out.int_val = SignExtend(GetLE(cell, 8), 8);
            out.raw_int_text.clear();
            out.str_val.clear();
            return Status::OK();
        }
        case kTypeValDecimal: {
            // The one kind that carries something besides the integer: its
            // scale, without which the unscaled value means nothing. Taken
            // from the column, which is where the schema keeps it.
            out.type = parser::ValueType::kDecimal;
            out.int_val = SignExtend(GetLE(cell, 8), 8);
            out.scale = catalog::DecimalScaleOf(col.len);
            out.raw_int_text.clear();
            out.str_val.clear();
            return Status::OK();
        }
        case catalog::kTypeValDecimalWide: {
            // Two LE halves back into the pair the AstValue carries - low
            // into int_val, high into dec_hi - with no validation, per
            // TY7: the bytes were proven at the encode gate.
            out.type = parser::ValueType::kDecimalWide;
            out.int_val = static_cast<std::int64_t>(GetLE(cell.subspan(0, 8), 8));
            out.dec_hi = static_cast<std::int64_t>(GetLE(cell.subspan(8, 8), 8));
            out.scale = catalog::DecimalScaleOf(col.len);
            out.raw_int_text.clear();
            out.str_val.clear();
            return Status::OK();
        }
        default:
            return Status::Corruption("column '" + NameOf() + "' has an unrecognized type_val");
    }
}

// ---- Literal coercion (TY05, moved here at TY07) ------------------------
//
// `WHERE price = '12.34'` against a `DECIMAL(10,2)` column compiles to a
// comparison whose right side is **already the scaled integer 1234**
// (docs/spec/types.md §3.1). The string is parsed once, here, by the same
// routines `EncodeOneValue` calls - one parser, two callers, zero drift -
// so a literal that stores and a literal that compares can never come to
// disagree about what it means.
//
// Two properties follow, and both are the point rather than side effects.
// Per-row evaluation stays an **int64 comparison**, which keeps the
// residual path on the cost profile the scan attribution work demands. And
// a literal that does not parse is a **positioned compile error**, not a
// row-by-row false: `WHERE d = '2026-02-30'` is a statement that cannot be
// satisfied by any row, and answering it with zero rows would hide the
// typo behind a plausible-looking empty result.
Status CoerceLiteralToColumn(const catalog::SysColumnRow& col, parser::AstValue& val) {
    // A parameter is never evaluated - a declared pattern's body is
    // type-checked and fingerprinted, never run - and NULL matches nothing
    // whatever its type. Neither has a value to coerce.
    if (val.type == parser::ValueType::kParam || val.type == parser::ValueType::kNull) {
        return Status::OK();
    }

    // An integer literal wider than int64 **wrapped in the lexer**, which
    // has always been the token's documented behaviour - `int_val` cannot
    // be trusted for a range question, only `raw_int_text` can (token.hpp's
    // digits() note). The three arms below that read `int_val` as the value
    // must refuse a wrapped one, or `= 36893488147419103232` coerces as the
    // 0 it wrapped to and matches rows the client never named. Scoped to
    // those arms and not hoisted: a uint64 comparison legitimately carries
    // its upper half in the digits (`ValueAsUint64`), and the wide-decimal
    // arm reads the digits themselves.
    const bool int_literal_wrapped =
        val.type == parser::ValueType::kInt && !val.raw_int_text.empty() &&
        val.raw_int_text != std::to_string(val.int_val);

    switch (col.type_val) {
        case catalog::kTypeValDate: {
            if (val.type == parser::ValueType::kInt) {
                // Already an epoch day. Accepted, and range-checked to the
                // same bounds the encoder applies - the two sides of the
                // engine must agree on which integers are dates.
                if (int_literal_wrapped || val.int_val < kMinEpochDay ||
                    val.int_val > kMaxEpochDay) {
                    return Status::OutOfRange("date is outside the supported range");
                }
                return Status::OK();
            }
            if (val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument("a date is written as a string, 'YYYY-MM-DD'");
            }
            auto days = ParseDateLiteral(val.str_val);
            if (!days.ok()) return days.status();
            val.type = parser::ValueType::kInt;
            val.int_val = days.value();
            val.str_val.clear();
            val.raw_int_text.clear();
            return Status::OK();
        }

        case catalog::kTypeValTimestamp: {
            if (val.type == parser::ValueType::kInt) {
                if (int_literal_wrapped || val.int_val < kMinEpochMicros ||
                    val.int_val > kMaxEpochMicros) {
                    return Status::OutOfRange("timestamp is outside the supported range");
                }
                return Status::OK();
            }
            if (val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument(
                    "a timestamp is written as a string, 'YYYY-MM-DD HH:MM:SS[.ffffff]'");
            }
            auto micros = ParseTimestampLiteral(val.str_val);
            if (!micros.ok()) return micros.status();
            val.type = parser::ValueType::kInt;
            val.int_val = micros.value();
            val.str_val.clear();
            val.raw_int_text.clear();
            return Status::OK();
        }

        case catalog::kTypeValDecimal: {
            const std::uint8_t precision = catalog::DecimalPrecisionOf(col.len);
            const std::uint8_t scale = catalog::DecimalScaleOf(col.len);

            // An integer literal against a decimal column - `price = 12` -
            // is exact and worth accepting, but scaling it here would be a
            // second implementation of the range and precision rules
            // `ParseDecimalLiteral` already owns, which is precisely the
            // drift TY01's one-parser rule exists to prevent. So it is
            // rendered and handed to that parser instead: one small string
            // per predicate at *compile*, never per row.
            // **Already in storage form: accept it and change nothing.**
            // The date and timestamp arms above have always done this - an
            // epoch integer coerces to itself - and this arm not doing so
            // made the function non-idempotent for exactly two types.
            //
            // That is not academic. A write hook re-coerces a *decoded* row
            // (an UPDATE carries one), so a decimal column reached here as
            // kDecimal and was refused. The Cabin's hook absorbs a coercion
            // failure by un-observing, so a Cabin on a decimal column was
            // silently destroyed by the first UPDATE that touched its
            // relation; the index hook, which cannot absorb, would have
            // failed the statement. Found by workplan IX06.
            //
            // The scale still has to agree, and a disagreement is refused
            // rather than rescaled - the same answer EncodeOneValue gives,
            // for the same reason: rescaling either drops digits or invents
            // them.
            if (val.type == parser::ValueType::kDecimal) {
                if (val.scale != scale) {
                    return Status::InvalidArgument(
                        "column has scale " + std::to_string(scale) +
                        " but the value carries scale " + std::to_string(val.scale) +
                        "; this engine does not rescale");
                }
                return Status::OK();
            }
            if (val.type != parser::ValueType::kInt && val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument("a decimal is written as a string, e.g. '12.34'");
            }
            if (int_literal_wrapped) {
                // The true digits cannot fit any narrow decimal (p <= 18),
                // so the precision check would refuse them anyway - but
                // only if it sees them rather than the wrapped value.
                return Status::OutOfRange("integer literal " + val.raw_int_text +
                                          " has more digits than a decimal(p <= 18) can hold");
            }
            const std::string text = val.type == parser::ValueType::kInt
                                         ? std::to_string(val.int_val)
                                         : val.str_val;

            auto unscaled = ParseDecimalLiteral(text, precision, scale);
            if (!unscaled.ok()) return unscaled.status();
            val.type = parser::ValueType::kDecimal;
            val.int_val = unscaled.value();
            val.scale = scale;
            val.str_val.clear();
            val.raw_int_text.clear();
            return Status::OK();
        }

        case catalog::kTypeValDecimalWide: {
            // The wide sibling of the arm above: same two accepted kinds,
            // same integer-rendered-to-text route, the wide parser. The
            // result is the compile-time constant every per-row comparison
            // then runs against, exactly as for the narrow type.
            const std::uint8_t precision = catalog::DecimalPrecisionOf(col.len);
            const std::uint8_t scale = catalog::DecimalScaleOf(col.len);
            // Idempotent for the same reason the narrow arm above is.
            if (val.type == parser::ValueType::kDecimalWide) {
                if (val.scale != scale) {
                    return Status::InvalidArgument(
                        "column has scale " + std::to_string(scale) +
                        " but the value carries scale " + std::to_string(val.scale) +
                        "; this engine does not rescale");
                }
                return Status::OK();
            }
            if (val.type != parser::ValueType::kInt && val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument("a decimal is written as a string, e.g. '12.34'");
            }
            // The digit text when the literal carries one - `int_val`
            // wraps past 64 bits and a wide column exists precisely for
            // values past 64 bits, so the spelling is the value here.
            const std::string text =
                val.type == parser::ValueType::kInt
                    ? (val.raw_int_text.empty() ? std::to_string(val.int_val)
                                                : val.raw_int_text)
                    : val.str_val;

            auto unscaled = ParseDecimalLiteralWide(text, precision, scale);
            if (!unscaled.ok()) return unscaled.status();
            val.type = parser::ValueType::kDecimalWide;
            val.int_val = Int128Low(unscaled.value());
            val.dec_hi = Int128High(unscaled.value());
            val.scale = scale;
            val.str_val.clear();
            val.raw_int_text.clear();
            return Status::OK();
        }

        default:
            // Every other type keeps the behaviour it had before this task.
            return Status::OK();
    }
}

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
        if (values[i - 1].type == parser::ValueType::kNull) {
            // The bitmap is the sole authority (null.md §3): the bit
            // is set here, a fixed cell keeps its deterministic zeros, and
            // a tagged cell takes the kNull filler so its bytes are
            // defined rather than stale - but no reader ever consults it.
            //
            // Branched on the layout's own null_bits - the field SetNullBit
            // reads - not on the schema's notnull, so the writer and the
            // layout cannot disagree about which columns have a bit.
            if (layout.null_bits[i] == catalog::kNoNullBit) {
                return Status::InvalidArgument(
                    "column '" + std::string(catalog::NameView(schema.columns[i].name)) +
                    "' is NOT NULL and cannot take NULL (declare it NULL at CREATE TABLE to "
                    "opt in - docs/spec/null.md) (byte " +
                    std::to_string(values[i - 1].byte_offset) + ")");
            }
            catalog::SetNullBit(out, layout, i);
            if (schema.columns[i].type_val == kTypeValVarchar) {
                if (Status s = storage::EncodeNullCell(MutableCellOf(layout, out, i)); !s.ok()) {
                    return s;
                }
            }
            continue;
        }
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

namespace {

// The shared front half of both decoders: everything that has to be true
// before a single cell is read.
Status CheckDecodeInputs(const catalog::Schema& schema, const catalog::RowLayout& layout,
                         std::span<const std::byte> payload,
                         std::span<parser::AstValue> out) {
    // **The predicate first, the Status only on failure.** `Status` carries
    // a `std::string` by value, so calling a Status-returning checker
    // constructs and destroys one even when it passes - and this function
    // runs once per row per decode, twice per row on the partial path. The
    // checks are unchanged and their messages still come from the one
    // function that words them; what is gone is building an empty message
    // for a row that is fine (workplan AP02).
    if (schema.columns.empty() ||
        !catalog::IsIntegerTypeVal(schema.columns.front().type_val)) {
        return catalog::CheckKeystoneColumn(schema);
    }
    if (layout.offsets.size() != schema.columns.size() || layout.row_size == 0) {
        return CheckLayoutMatches(schema, layout);
    }
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
    return Status::OK();
}

// A decoded NULL slot, whole: type decides, and every value field is
// cleared - int_val, scale and dec_hi included - so no consumer that
// dispatches on type first can ever read a stale half (the half-clear is
// exactly the hazard SumContribution's comment names).
void SetNullSlot(parser::AstValue& out) {
    out.type = parser::ValueType::kNull;
    out.str_val.clear();
    out.raw_int_text.clear();
    out.int_val = 0;
    out.scale = 0;
    out.dec_hi = 0;
}

// Column 0 is not in the body: it lives in the Keystone word.
Status DecodeKeystoneInto(std::span<const std::byte> payload, parser::AstValue& out) {
    auto id = RowKeystoneId(payload);
    if (!id.ok()) return id.status();
    out.type = parser::ValueType::kInt;
    out.int_val = static_cast<std::int64_t>(id.value());
    out.raw_int_text.clear();
    out.str_val.clear();
    return Status::OK();
}

}  // namespace

Status DecodeColumnsInto(const catalog::Schema& schema, const catalog::RowLayout& layout,
                         std::span<const std::byte> payload, std::span<parser::AstValue> out,
                         std::uint64_t columns, std::vector<PendingSpill>* spills) {
    if (spills != nullptr) spills->clear();
    if (Status s = CheckDecodeInputs(schema, layout, payload, out); !s.ok()) return s;

    // **Iterate the mask's set bits, not the relation's columns.** A fold or
    // a projection reading one column of twelve used to test twelve bits to
    // find it; this visits exactly the columns named. `countr_zero` on a
    // cleared-lowest-bit loop is the standard idiom and needs no bound of
    // its own - the mask cannot name a column past 63, which is why the
    // compiler answers kAllColumns for a relation that wide (step_compiler
    // section 4) and why this can no longer silently skip a tail.
    // NU8's zero-cost gate: every relation with no nullable column skips
    // the bitmap read entirely - null_bitmap_bytes is on the cache line the
    // decoders already read row_size from, where null_bits is a separate
    // heap allocation.
    const bool has_nulls = layout.null_bitmap_bytes != 0;

    std::uint64_t remaining = columns;
    if (schema.columns.size() < 64) {
        // Never look at a bit the schema has no column for.
        remaining &= (std::uint64_t{1} << schema.columns.size()) - 1;
    }
    while (remaining != 0) {
        const std::size_t i = static_cast<std::size_t>(std::countr_zero(remaining));
        remaining &= remaining - 1;
        if (i == 0) {
            if (Status s = DecodeKeystoneInto(payload, out[0]); !s.ok()) return s;
            continue;
        }
        if (has_nulls && catalog::NullBitIsSet(payload, layout, i)) {
            // The bitmap decides here exactly as in DecodeRowInto below -
            // this is the executor's per-column path, and it was the
            // bypass the first NULL E2E test caught.
            SetNullSlot(out[i]);
            continue;
        }
        if (Status s = DecodeOneValueInto(schema.columns[i], CellOf(layout, payload, i), i, out[i],
                                           spills);
            !s.ok()) {
            return s;
        }
    }
    return Status::OK();
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
    // Same rule as every other integer: a 40-bit id fits int_val exactly, so
    // the text would be a string conversion per row for nothing.
    out[0].raw_int_text.clear();
    out[0].str_val.clear();

    // The same zero-cost gate as DecodeColumnsInto.
    const bool has_nulls = layout.null_bitmap_bytes != 0;

    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        if (has_nulls && catalog::NullBitIsSet(payload, layout, i)) {
            // The bitmap decides (null.md §3); the cell is the defined
            // filler and is not read.
            SetNullSlot(out[i]);
            continue;
        }
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
        storage::PageRef pin;
        auto bytes = varheap::Fetch(store, spill.ptr, pin);
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

StatusOr<std::uint64_t> ValueAsUint64(const parser::AstValue& value) {
    if (value.type != parser::ValueType::kInt) {
        return Status::InvalidArgument("expected an integer value");
    }
    if (!value.raw_int_text.empty()) return ParseUint64Text(value.raw_int_text);
    if (value.int_val < 0) {
        return Status::InvalidArgument("expected a non-negative integer, got " +
                                        std::to_string(value.int_val));
    }
    return static_cast<std::uint64_t>(value.int_val);
}

std::string FormatValue(std::uint32_t type_val, const parser::AstValue& value) {
    switch (value.type) {
        case parser::ValueType::kInt:
            // **The one place the column's type is consulted** (TY06). A
            // date and a timestamp are integers everywhere else in the
            // engine, and this is the boundary where they stop being one.
            //
            // Guarded on the value being an integer as well as on the
            // column's type, so a caller that passes a `type_val` not
            // matching the value it holds gets the value rendered rather
            // than a nonsense date - the same fall-through discipline
            // CompareValues uses for an operand that does not fit its
            // column.
            if (type_val == catalog::kTypeValDate) {
                return FormatDate(static_cast<std::int32_t>(value.int_val));
            }
            if (type_val == catalog::kTypeValTimestamp) {
                return FormatTimestamp(value.int_val);
            }
            return !value.raw_int_text.empty() ? value.raw_int_text : std::to_string(value.int_val);
        case parser::ValueType::kStr:
            return value.str_val;
        // The scale is part of the value's meaning, so `12.30` and not
        // `12.3` (docs/spec/types.md §3.3). This is the one kind that
        // carries enough to render itself, so it ignores `type_val`
        // entirely - a decimal read out of a column and a decimal folded
        // by SUM render the same way with no caller having to know which
        // it is holding.
        case parser::ValueType::kDecimal:
            return FormatDecimal(value.int_val, value.scale);
        case parser::ValueType::kDecimalWide:
            // Carries its own scale like the narrow kind, so it too
            // ignores `type_val` and renders itself.
            return FormatDecimalWide(Int128FromHalves(value.dec_hi, value.int_val), value.scale);
        // Rendered as written, sigil restored. Only a plan printed from a
        // declared pattern's body reaches this - no row ever holds one - and
        // printing `$flag` is what makes such a plan readable back against
        // the declaration it came from.
        case parser::ValueType::kParam:
            return "$" + value.param_name();
        case parser::ValueType::kNull:
        default:
            return "NULL";
    }
}

bool CompareValues(std::uint32_t type_val, const parser::AstValue& lhs,
                   const parser::AstValue& rhs, parser::CompareOp op) {
    // IS NULL / IS NOT NULL are answered from the lhs alone - the rhs is
    // unused by construction (ast.hpp) - and before the collapse below,
    // which would otherwise answer false for exactly the rows they exist
    // to find.
    if (op == parser::CompareOp::kIsNull) return lhs.type == parser::ValueType::kNull;
    if (op == parser::CompareOp::kIsNotNull) return lhs.type != parser::ValueType::kNull;
    // Three-valued logic's collapse for a boolean caller (null.md §4):
    // a comparison with a NULL operand is unknown, and WHERE keeps only
    // true - so every relational op with a NULL side is a non-match. The
    // sort path does not come through here (its comparator orders NULLs
    // largest, D3); aggregates skip NULLs before comparing.
    if (lhs.type == parser::ValueType::kNull || rhs.type == parser::ValueType::kNull) {
        return false;
    }
    if (type_val == kTypeValUint64) {
        // Unsigned, because int_val is signed and cannot represent the
        // upper half of the range - comparing it would order large ids
        // below small ones.
        //
        // **Through ValueAsUint64, not through raw_int_text.** That field
        // carries the value only when int_val cannot (a value above
        // INT64_MAX) and is empty otherwise, which is precisely the rule
        // ValueAsUint64 exists to be the single owner of. Reading the text
        // directly here parsed an empty string for every ordinary value and
        // failed, so *any* comparison with a uint64 operand at or below
        // INT64_MAX answered false: `WHERE big = 5` returned no rows, and
        // MIN over a uint64 column could not descend past INT64_MAX. The
        // header's warning about this caller was already written; this is
        // that caller.
        auto a = ValueAsUint64(lhs);
        auto b = ValueAsUint64(rhs);
        // A negative operand is not a uint64 and so is a non-match, which
        // is the answer every other type mismatch here gets.
        if (!a.ok() || !b.ok()) return false;
        return ApplyCompare(a.value(), b.value(), op);
    }
    // ---- DECIMAL (docs/spec/types.md §3.1, TY05) -----------------------
    //
    // Unscaled integers, compared directly - which is only valid because
    // **the scales are equal**, and they are equal because the compiler
    // made them so: a string literal is parsed to the column's scale at
    // compile (`CoerceLiteralTo`), and a column-column residual whose two
    // scales differ is refused there rather than reaching this line.
    //
    // So a disagreement here is not a case to handle, it is a compiler bug
    // - and the answer is a non-match rather than a rescale, because
    // rescaling would either drop digits or invent them, and doing it
    // per-row inside a predicate is the worst possible place to decide it.
    if (lhs.type == parser::ValueType::kDecimal || rhs.type == parser::ValueType::kDecimal) {
        if (lhs.type != rhs.type || lhs.scale != rhs.scale) return false;
        return ApplyCompare(lhs.int_val, rhs.int_val, op);
    }
    // The wide decimal: the same equal-kind, equal-scale contract - and
    // since a narrow and a wide column can never share a (p, s), a
    // cross-width operand pair is a kind mismatch here by construction,
    // answered as the non-match every other mismatch gets.
    if (lhs.type == parser::ValueType::kDecimalWide ||
        rhs.type == parser::ValueType::kDecimalWide) {
        if (lhs.type != rhs.type || lhs.scale != rhs.scale) return false;
        return ApplyCompare(Int128FromHalves(lhs.dec_hi, lhs.int_val),
                            Int128FromHalves(rhs.dec_hi, rhs.int_val), op);
    }
    // DATE and TIMESTAMP arrive here as the integers they are - epoch days
    // and epoch microseconds - so they need no arm of their own. That is
    // the whole reason TY5 reused kInt for them instead of giving each a
    // ValueType: an ordering on the encoded integer *is* the ordering on
    // the value, for both.
    if (lhs.type == parser::ValueType::kInt && rhs.type == parser::ValueType::kInt) {
        return ApplyCompare(lhs.int_val, rhs.int_val, op);
    }
    if (lhs.type == parser::ValueType::kStr && rhs.type == parser::ValueType::kStr) {
        return ApplyCompare<std::string_view>(lhs.str_val, rhs.str_val, op);
    }
    return false;  // incompatible value kinds
}

int OrderKey::Compare(const OrderKey& rhs) const noexcept {
    // D3: NULL orders above every value of its column, and two NULLs tie
    // (the sort's `seq` key makes the order total). Checked before the
    // str/num split because a NULL key carries neither.
    if (is_null || rhs.is_null) {
        if (is_null == rhs.is_null) return 0;
        return is_null ? 1 : -1;
    }
    // A string key and a numeric one never meet: both come from one column,
    // whose type decided which arm `OrderKeyOf` took. The check is here
    // because the alternative is comparing a string's `num` (always 0)
    // against a real one and calling the result an order.
    if (is_str != rhs.is_str) return is_str ? 1 : -1;
    if (is_str) {
        const int cmp = str.compare(rhs.str);
        return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    }
    if (num < rhs.num) return -1;
    return rhs.num < num ? 1 : 0;
}

StatusOr<OrderKey> OrderKeyOf(std::uint32_t type_val, const parser::AstValue& value) {
    OrderKey key;
    if (value.type == parser::ValueType::kNull) {
        // D3: a NULL is a legal sort operand and orders largest - ASC last,
        // and the ordinary descending flip puts it first.
        key.is_null = true;
        return key;
    }
    if (type_val == kTypeValUint64) {
        // Through ValueAsUint64 and not through int_val, for the reason
        // CompareValues spells out above: int_val cannot hold the upper half
        // of the range, and ordering it directly puts large ids below small
        // ones. Int128 then holds the unsigned value exactly.
        auto u = ValueAsUint64(value);
        if (!u.ok()) return u.status();
        key.num = static_cast<Int128>(u.value());
        return key;
    }
    switch (value.type) {
        case parser::ValueType::kInt:
        case parser::ValueType::kDecimal:
            // A decimal's unscaled integer orders the value directly because
            // every value of one column shares that column's scale - (p, s)
            // is a schema constant, so no rescale can be needed here.
            key.num = static_cast<Int128>(value.int_val);
            return key;
        case parser::ValueType::kDecimalWide:
            key.num = Int128FromHalves(value.dec_hi, value.int_val);
            return key;
        case parser::ValueType::kStr:
            key.is_str = true;
            key.str = value.str_val;
            return key;
        default:
            return Status::Corruption("cannot order a value of this kind");
    }
}

}  // namespace kds::exec
