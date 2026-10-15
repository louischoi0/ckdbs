#include "kds/exec/row_codec.hpp"

#include "kds/exec/type_literals.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
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

// Whether a column of this type takes an integer's `int_val` as the value
// itself - the types for which a literal past int64, whose `int_val` is the
// lexer's wrap, names nothing the column holds. A uint64 reads the digits
// instead, and every other type refuses an integer by kind. **One list for
// the encoder and the coercion**, so what stores and what compares cannot
// come to disagree about a wrapped literal.
bool TakesIntValAsValue(std::uint32_t type_val) noexcept {
    switch (type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        case kTypeValBool:
        case kTypeValDate:
        case kTypeValTimestamp:
            return true;
        default:
            return false;
    }
}

void StoreLe64(std::byte* out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
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
    // **The name is built only where an error is built**: a per-column
    // std::string was most of the per-column cost (workplan AP02).
    const auto NameOf = [&col] { return std::string(catalog::NameView(col.name)); };

    if (val.type == parser::ValueType::kNull) {
        // Unreachable: EncodeRow's driver intercepts NULL before the cell
        // is written (the bitmap is the authority, null.md §3), so a
        // NULL arriving here bypassed it - a caller bug, not a user error.
        return Status::Corruption("column '" + NameOf() +
                                  "': a NULL reached the cell encoder; EncodeRow's driver owns "
                                  "NULL handling");
    }
    // A `$param` has no value to encode. Unreachable since declared
    // patterns were withdrawn (ast.hpp), and refused by name rather than
    // left to fall through to "expects an integer", which would send
    // whoever hit it looking for a type error that is not there.
    if (val.type == parser::ValueType::kParam) {
        return Status::Corruption("column '" + NameOf() + "': parameter '$" +
                                  val.param_name() + "' has no bound value to store");
    }

    // `int_val` is the lexer's wrap, not the value (`TakesIntValAsValue`):
    // stored, it would be another number. The type is asked first, so a
    // decoded uint64 above INT64_MAX does not have its digits parsed.
    if (TakesIntValAsValue(col.type_val) && parser::IntLiteralWrapped(val)) {
        return Status::OutOfRange("integer literal " + val.raw_int_text +
                                  " does not fit column '" + NameOf() + "'");
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
                return Status::InvalidArgument("value of " +
                                                std::to_string(val.str_val.size()) +
                                                " byte(s) is too long for column '" + NameOf() +
                                                "', declared char(" +
                                                std::to_string(cell.size()) + ")");
            }
            // **A NUL cannot round-trip through this type, so it is refused
            // rather than stored.** The decoder below reads back to the
            // first zero byte, which is what makes a short value in a
            // fixed cell readable at all; a value carrying its own NUL
            // would come back truncated, silently. Refusing here is the
            // truthfulness rule (CLAUDE.md): never accept a spelling and
            // store something other than what was written. A caller that
            // needs arbitrary bytes wants `varchar`, which carries a
            // length and does not interpret them.
            if (val.str_val.find('\0') != std::string::npos) {
                return Status::InvalidArgument(
                    "column '" + NameOf() +
                    "' is char, whose values are read back to the first NUL byte, and this "
                    "value contains one at offset " +
                    std::to_string(val.str_val.find('\0')) +
                    "; use varchar for values with embedded NULs");
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
            varheap::ChainAppendResult& grew = appended.value();

            AppendedSpill spill{grew.ptr, std::vector<std::byte>(bytes.begin(), bytes.end()),
                                grew.created_page_id, grew.linked_page_id};
            if (varheap.on_append != nullptr) {
                // Under `grew`'s holds, which drop at this scope's end.
                if (Status s = (*varheap.on_append)(spill); !s.ok()) return s;
            } else if (varheap.appended != nullptr) {
                varheap.appended->push_back(std::move(spill));
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

// The span of `payload` column `i` occupies: exactly its column's width
// (`RowLayout::cell_ends`). Never "to the next offset or the bitmap": the
// bitmap follows the last column (null.md §2.1) and, with slots, the slot
// region follows the last declared column (BW-R2); a char or tagged cell
// sized by a longer span would claim those bytes as its own.
std::span<const std::byte> CellOf(const catalog::RowLayout& layout,
                                   std::span<const std::byte> payload, std::size_t i) {
    return payload.subspan(layout.offsets[i], layout.cell_ends[i] - layout.offsets[i]);
}

std::span<std::byte> MutableCellOf(const catalog::RowLayout& layout, std::span<std::byte> payload,
                                    std::size_t i) {
    return payload.subspan(layout.offsets[i], layout.cell_ends[i] - layout.offsets[i]);
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
        case parser::CompareOp::kLike:  // text only, answered before this
            return false;
    }
    return false;
}

}  // namespace

// `LIKE` over bytes (BU-Q6): `%` matches any run, `_` any one byte, every
// other byte itself, case-sensitively. Iterative with one backtrack point -
// the last `%` - so a pattern of many `%`s costs O(value x pattern) at
// worst, never the exponential a recursive matcher reaches. A pattern `%` is
// tested before the literal arm: a `%` byte in the value must not consume a
// pattern `%` as itself (`'a%b' LIKE 'a%'`).
bool LikeMatch(std::string_view value, std::string_view pattern) {
    std::size_t v = 0, p = 0;
    std::size_t star = std::string_view::npos, star_v = 0;
    while (v < value.size()) {
        if (p < pattern.size() && pattern[p] == '%') {
            star = p++;
            star_v = v;
        } else if (p < pattern.size() && (pattern[p] == '_' || pattern[p] == value[v])) {
            ++v;
            ++p;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            v = ++star_v;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '%') ++p;
    return p == pattern.size();
}

namespace {

// A `char(N)` cell's value: its bytes back to the first NUL and no further -
// a char(N) cell is N bytes whatever the value's length, and the padding is
// not part of the value (which is why the encoder refuses a NUL in one).
// **The one reading of a char cell**: the decoder and the
// scan kernel both take it from here, so the value the kernel compares is
// the value the residual compares (BM-R1).
std::span<const std::byte> CharCellValue(std::span<const std::byte> cell) {
    if (cell.empty()) return cell;
    const void* nul = std::memchr(cell.data(), 0, cell.size());
    if (nul == nullptr) return cell;
    return cell.first(static_cast<std::size_t>(static_cast<const std::byte*>(nul) - cell.data()));
}

// Copies value bytes into a slot the caller already owns, reusing that
// slot's capacity: one memcpy, no allocation once the frame has settled.
//
// Move-assigning a built std::string is not an assign: it trades the
// slot's buffer for a fresh one, a malloc and free per string column per
// row past the 15-byte SSO (CIP/OPT-002-string-slot-assign).
void AssignBytes(std::string& out, std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        out.clear();
        return;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
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
    // Built only where an error is built - see EncodeOneValue above.
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
            // **Left empty on purpose**: `raw_int_text` carries only a value
            // `int_val` cannot hold, never one of a signed column, and
            // FormatValue() renders from `int_val` when it is empty.
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
            // and the only thing a re-encode can read back. to_chars into a
            // stack buffer, then assign, for the reason AssignBytes states:
            // 20 digits is past SSO, and move-assigning std::to_string's
            // result would trade the slot's buffer for a fresh malloc.
            if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                char buf[20];  // max uint64 is 20 digits; to_chars writes no NUL
                auto res = std::to_chars(buf, buf + sizeof(buf), v);
                out.raw_int_text.assign(buf, static_cast<std::size_t>(res.ptr - buf));
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
            out.type = parser::ValueType::kStr;
            AssignBytes(out.str_val, CharCellValue(cell));
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
            out.type = parser::ValueType::kStr;
            AssignBytes(out.str_val, decoded.value().bytes);
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
        // **Integers out, no strings, no formatting** (TY5): rendering is
        // the emission boundary's job, so a scan builds no text for rows it
        // rejects. No re-validation: EncodeOneValue is the only gate (TY7).
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

// ---- Literal coercion (types.md §3.1) -----------------------------------
//
// `WHERE price = '12.34'` against a `DECIMAL(10,2)` column compiles to the
// scaled integer 1234, parsed by the same routines `EncodeOneValue` calls,
// so a literal that stores and one that compares cannot disagree. Per-row
// evaluation stays an int64 comparison, and a literal that does not parse
// (`'2026-02-30'`) is a positioned compile error, never an empty answer.
Status CoerceLiteralToColumn(const catalog::SysColumnRow& col, parser::AstValue& val) {
    // A parameter is never evaluated - nothing constructs one since
    // declared patterns were withdrawn (ast.hpp) - and NULL matches nothing
    // whatever its type. Neither has a value to coerce.
    if (val.type == parser::ValueType::kParam || val.type == parser::ValueType::kNull) {
        return Status::OK();
    }

    // An integer literal wider than int64 **wrapped in the lexer**, which
    // has always been the token's documented behaviour - `int_val` cannot
    // be trusted for a range question, only `raw_int_text` can (token.hpp's
    // digits() note). A type that reads `int_val` as the value refuses a
    // wrapped one by the encoder's list (`TakesIntValAsValue`), or
    // `= 36893488147419103232` coerces as the 0 it wrapped to and matches
    // rows the client never named. A uint64 reads the digits (its arm
    // below), the wide-decimal arm reads them too, and the narrow one
    // refuses them.
    if (TakesIntValAsValue(col.type_val) && parser::IntLiteralWrapped(val)) {
        return Status::OutOfRange("integer literal " + val.raw_int_text +
                                  " is outside the range of an int64");
    }

    switch (col.type_val) {
        case catalog::kTypeValDate: {
            if (val.type == parser::ValueType::kInt) {
                // Already an epoch day. Accepted, and range-checked to the
                // same bounds the encoder applies - the two sides of the
                // engine must agree on which integers are dates.
                if (val.int_val < kMinEpochDay || val.int_val > kMaxEpochDay) {
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
                if (val.int_val < kMinEpochMicros || val.int_val > kMaxEpochMicros) {
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
            // **Already in storage form: accept it and change nothing**, so
            // the coercion is idempotent: a write hook re-coerces a decoded
            // row (an UPDATE carries one), and a refusal here once silently
            // dropped a Cabin on a decimal column (workplan IX06).
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
            if (parser::IntLiteralWrapped(val)) {
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

        // **The kind the encode takes, or a refusal** (types.md §3.1a).
        // These arms compare the value as written, so a literal of another
        // kind reached CompareValues as a kind mismatch and answered false
        // per row: `WHERE a = '20'` over an integer column returned no rows
        // and `DELETE` reported `DELETED 0`. No conversion is made - the
        // statement is refused for its spelling. An integer outside a narrow
        // column's width stays legal: compared by value, it is simply true
        // or false, as it is under the SQL standard.
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        case kTypeValUint64:
            if (val.type != parser::ValueType::kInt) {
                return Status::InvalidArgument(
                    "expects an integer literal - unquoted, with no fraction");
            }
            // The same rule over the unsigned range, which the digits carry
            // (`ValueAsUint64`): a negative literal or one past 2^64 - 1
            // names nothing the column holds. Compared, it answered false
            // under every operator - `!=` and `>` included - and no row.
            if (col.type_val == kTypeValUint64 && !ValueAsUint64(val).ok()) {
                return Status::OutOfRange("integer literal " + val.raw_int_text +
                                          " is outside the range of a uint64");
            }
            return Status::OK();
        case kTypeValBool:
            if (val.type != parser::ValueType::kInt || (val.int_val != 0 && val.int_val != 1)) {
                return Status::InvalidArgument("expects 0 or 1 (no boolean literal in this grammar)");
            }
            return Status::OK();
        case kTypeValChar:
        case kTypeValVarchar:
            if (val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument("expects a string literal, '...'");
            }
            return Status::OK();

        default:
            return Status::OK();
    }
}

Status CheckIntegerLiteralFits(const catalog::SysColumnRow& col, const parser::AstValue& value) {
    switch (col.type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        case kTypeValUint64: {
            std::array<std::byte, 8> cell{};
            return EncodeOneValue(col, value, cell, VarHeapSink{});
        }
        default:
            return Status::InvalidArgument("column type " + std::to_string(col.type_val) +
                                           " is not an integer type");
    }
}

TypeFamily TypeFamilyOf(std::uint32_t type_val) {
    switch (type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        case kTypeValUint64: return TypeFamily::kInt;
        case kTypeValDecimal:
        case catalog::kTypeValDecimalWide: return TypeFamily::kDec;
        case kTypeValChar:
        case kTypeValVarchar: return TypeFamily::kText;
        case kTypeValDate: return TypeFamily::kDate;
        case kTypeValTimestamp: return TypeFamily::kTimestamp;
        case kTypeValBool: return TypeFamily::kBool;
        default: return TypeFamily::kOther;
    }
}

namespace {

// One column of a row being encoded into `out`, whose cell for it is zero:
// a NULL sets the bitmap and leaves the cell's deterministic zeros (or the
// kNull filler of a tagged cell), anything else is `EncodeOneValue`'s.
Status EncodeColumn(const catalog::Schema& schema, const catalog::RowLayout& layout,
                    std::size_t i, const parser::AstValue& value, std::span<std::byte> out,
                    const VarHeapSink& varheap) {
    if (value.type == parser::ValueType::kNull) {
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
                std::to_string(value.byte_offset) + ")");
        }
        catalog::SetNullBit(out, layout, i);
        if (schema.columns[i].type_val == kTypeValVarchar) {
            return storage::EncodeNullCell(MutableCellOf(layout, out, i));
        }
        return Status::OK();
    }
    return EncodeOneValue(schema.columns[i], value, MutableCellOf(layout, out, i), varheap);
}

// The checks and the Keystone word every encode starts from.
Status StartRow(const catalog::Schema& schema, const catalog::RowLayout& layout,
                std::uint64_t id, std::span<std::byte> out) {
    if (Status s = catalog::CheckKeystoneColumn(schema); !s.ok()) return s;
    if (Status s = CheckLayoutMatches(schema, layout); !s.ok()) return s;
    // The Keystone word leads every tuple (heap-and-tuple.md section 4).
    // The tuple key column is therefore *not* encoded into the body: storing it
    // twice is how the two copies come to disagree.
    auto word = Keystone::Encode(id, /*flags=*/0, /*reserved=*/0);
    if (!word.ok()) return word.status();
    StoreLe64(out.data(), word.value());
    return Status::OK();
}

}  // namespace

StatusOr<std::vector<std::byte>> EncodeRow(const catalog::Schema& schema,
                                            const catalog::RowLayout& layout, std::uint64_t id,
                                            const std::vector<parser::AstValue>& values,
                                            const VarHeapSink& varheap) {
    // One buffer of exactly the schema constant, zero-filled: every byte of
    // a tuple is written by this function, including the padding inside a
    // cell that a short value does not fill (invariant 13).
    std::vector<std::byte> out(layout.row_size, std::byte{0});
    if (Status s = StartRow(schema, layout, id, out); !s.ok()) return s;
    catalog::SetVacantSlotBits(out, layout);
    const std::size_t expected = schema.columns.size() - 1;
    if (values.size() != expected) {
        return Status::InvalidArgument("expected " + std::to_string(expected) +
                                        " value(s) after the tuple key, got " +
                                        std::to_string(values.size()));
    }
    // BW-R6's gate, read once per call: an unaltered relation's loop asks a
    // local, never the layout, per column.
    const bool has_dropped = layout.has_dropped();
    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        // A dropped column's cell stays zero with its bit clear (BW-R6): the
        // caller's placeholder there is not a value.
        if (has_dropped && schema.columns[i].dropped()) continue;
        if (Status s = EncodeColumn(schema, layout, i, values[i - 1], out, varheap); !s.ok()) {
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

StatusOr<std::vector<std::byte>> EncodeAssigned(const catalog::Schema& schema,
                                                 const catalog::RowLayout& layout,
                                                 std::uint64_t id,
                                                 std::span<const std::byte> before,
                                                 std::span<const AssignedCell> cells,
                                                 const VarHeapSink& varheap) {
    // Checked, not assumed: a disagreeing length is Corruption (invariant
    // 13), and the copy below would otherwise carry it into the new version.
    if (before.size() != layout.row_size) {
        return Status::Corruption("the row being updated is " + std::to_string(before.size()) +
                                   " bytes for a relation whose row size is " +
                                   std::to_string(layout.row_size));
    }
    std::vector<std::byte> out(before.begin(), before.end());
    if (Status s = StartRow(schema, layout, id, out); !s.ok()) return s;
    for (const AssignedCell& cell : cells) {
        if (cell.column == 0 || cell.column >= schema.columns.size()) {
            return Status::Corruption("an assignment names column " +
                                       std::to_string(cell.column) + " of a relation with " +
                                       std::to_string(schema.columns.size()) + " column(s)");
        }
        // The cell and its bit as a fresh encode finds them: zero and clear.
        auto bytes = MutableCellOf(layout, out, cell.column);
        std::fill(bytes.begin(), bytes.end(), std::byte{0});
        catalog::ClearNullBit(out, layout, cell.column);
        if (Status s = EncodeColumn(schema, layout, cell.column, *cell.value, out, varheap);
            !s.ok()) {
            return s;
        }
    }
    return out;
}

namespace {

// **The predicate first, the Status only on failure.** `Status` carries a
// `std::string` by value, so a Status-returning check constructs and
// destroys one even when it passes - and a `kAll` decode asks this per row.
// `CheckDecodeShape` asks it first too and words a message only on failure,
// so the shape is stated here alone (workplan AP02).
bool DecodeShapeHolds(const catalog::Schema& schema, const catalog::RowLayout& layout,
                      std::span<const parser::AstValue> out) {
    return !schema.columns.empty() && catalog::IsIntegerTypeVal(schema.columns.front().type_val) &&
           layout.offsets.size() == schema.columns.size() && layout.row_size != 0 &&
           out.size() == schema.columns.size();
}

// The shared front half of both decoders: everything that has to be true
// before a single cell is read. Under `kPayloadOnly` the caller has passed
// `CheckDecodeShape` for this schema, layout and span already, and only the
// row's own check runs.
Status CheckDecodeInputs(const catalog::Schema& schema, const catalog::RowLayout& layout,
                         std::span<const std::byte> payload,
                         std::span<const parser::AstValue> out, DecodeChecks checks) {
    if (checks == DecodeChecks::kAll && !DecodeShapeHolds(schema, layout, out)) {
        return CheckDecodeShape(schema, layout, out);
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

}  // namespace

Status CheckDecodeShape(const catalog::Schema& schema, const catalog::RowLayout& layout,
                        std::span<const parser::AstValue> out) {
    if (DecodeShapeHolds(schema, layout, out)) return Status::OK();
    if (Status s = catalog::CheckKeystoneColumn(schema); !s.ok()) return s;
    if (Status s = CheckLayoutMatches(schema, layout); !s.ok()) return s;
    return Status::InvalidArgument("decode target has " + std::to_string(out.size()) +
                                    " slot(s) for a schema of " +
                                    std::to_string(schema.columns.size()) + " column(s)");
}

namespace {

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
    auto id = KeystoneIdOfPayload(payload);
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
                         std::uint64_t columns, std::vector<PendingSpill>* spills,
                         DecodeChecks checks) {
    if (spills != nullptr) spills->clear();
    if (Status s = CheckDecodeInputs(schema, layout, payload, out, checks); !s.ok()) return s;

    // **Iterate the mask's set bits, not the relation's columns.** The mask
    // cannot name a column past 63, which is why the compiler answers
    // kAllColumns for a relation that wide (step_compiler section 4).
    // NU8's zero-cost gate: every relation with no nullable column skips
    // the bitmap read entirely - null_bitmap_bytes is on the cache line the
    // decoders already read row_size from, where null_bits is a separate
    // heap allocation.
    const bool has_nulls = layout.null_bitmap_bytes != 0;
    const bool has_dropped = layout.has_dropped();  // BW-R6 gate, read once per call like has_nulls

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
        if ((has_nulls && catalog::NullBitIsSet(payload, layout, i)) ||
            (has_dropped && schema.columns[i].dropped())) {
            // The bitmap decides here exactly as in DecodeRowInto below.
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
                     std::vector<PendingSpill>* spills, DecodeChecks checks) {
    if (spills != nullptr) spills->clear();
    // The same four checks DecodeColumnsInto makes, through the same
    // predicate-first function, so no Status is built per row and the two
    // cannot drift (CIP/OPT-004-decoderowinto-preconditions).
    if (Status s = CheckDecodeInputs(schema, layout, payload, out, checks); !s.ok()) return s;

    auto id = KeystoneIdOfPayload(payload);
    if (!id.ok()) return id.status();

    // The tuple key is not in the body: it lives in the Keystone word, which is
    // why the loop below starts at column 1.
    out[0].type = parser::ValueType::kInt;
    out[0].int_val = static_cast<std::int64_t>(id.value());
    // Same rule as every other integer: a Keystone id fits int_val exactly, so
    // the text would be a string conversion per row for nothing.
    out[0].raw_int_text.clear();
    out[0].str_val.clear();

    // The same zero-cost gates as DecodeColumnsInto.
    const bool has_nulls = layout.null_bitmap_bytes != 0;
    const bool has_dropped = layout.has_dropped();  // BW-R6 gate, read once per call like has_nulls

    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        // A dropped column reads NULL and its cell is never interpreted
        // (BW-R6): no statement can name it, and its bytes need not be a
        // value its type would accept.
        if ((has_nulls && catalog::NullBitIsSet(payload, layout, i)) ||
            (has_dropped && schema.columns[i].dropped())) {
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

        out[spill.column].type = parser::ValueType::kStr;
        AssignBytes(out[spill.column].str_val, bytes.value());
        out[spill.column].raw_int_text.clear();
    }
    return Status::OK();
}

Status DecodeAndResolve(storage::PageStore& store, const catalog::Schema& schema,
                        const catalog::RowLayout& layout, std::span<const std::byte> payload,
                        std::span<parser::AstValue> out, std::uint64_t columns,
                        std::vector<PendingSpill>& scratch, DecodeChecks checks) {
    if (columns == kAllColumnsMask) {
        if (Status s = DecodeRowInto(schema, layout, payload, out, &scratch, checks); !s.ok()) {
            return s;
        }
    } else if (Status s =
                   DecodeColumnsInto(schema, layout, payload, out, columns, &scratch, checks);
               !s.ok()) {
        return s;
    }
    if (scratch.empty()) return Status::OK();
    return ResolveSpills(store, scratch, out);
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
        // Rendered as written, sigil restored. No row ever holds one and
        // nothing constructs one since declared patterns were withdrawn
        // (ast.hpp); the arm stays so a plan render can never print a
        // placeholder as `NULL`, which is what the default below would do.
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
    // ---- Integers, of any width and signedness, by value (types.md §3.1a)
    //
    // A uint64 above INT64_MAX is the one integer `int_val` cannot hold: it
    // is carried by its digits (ValueAsUint64's rule) and `int_val` holds it
    // wrapped, negative. So such an operand is compared through Int128 and
    // every other pair stays an int64 comparison. Whichever side is the
    // uint64 - the left column's type is the only one this function is
    // told - the answer is the mathematical one: a uint64 column compared
    // with a signed column's -1 is greater, not a non-match.
    if (lhs.type == parser::ValueType::kInt && rhs.type == parser::ValueType::kInt) {
        // Judged by the digits, not by the sign: a literal past 2^64 wraps
        // to a small non-negative `int_val` (`18446744073709551617` is 1),
        // and a sign test would compare it as that. A text shorter than 19
        // characters cannot wrap, so the common pair costs a length test.
        if (!parser::IntLiteralWrapped(lhs) && !parser::IntLiteralWrapped(rhs)) {
            return ApplyCompare(lhs.int_val, rhs.int_val, op);
        }
        const auto exact = [](const parser::AstValue& v) -> std::optional<Int128> {
            if (!parser::IntLiteralWrapped(v)) return static_cast<Int128>(v.int_val);
            auto u = ParseUint64Text(v.raw_int_text);  // refuses a '-' and digits past 2^64 - 1
            if (!u.ok()) return std::nullopt;
            return static_cast<Int128>(u.value());
        };
        const auto a = exact(lhs);
        const auto b = exact(rhs);
        // Digits outside [-2^63, 2^64 - 1] name no integer any column holds,
        // and every WHERE - a catalog view's included - refuses such a
        // literal before a row is read; this is a guard, not an answer.
        if (!a.has_value() || !b.has_value()) return false;
        return ApplyCompare(*a, *b, op);
    }
    // A uint64 operand that is not an integer is a mismatch the compiler
    // refuses; it must not reach the decimal or string arms below.
    if (type_val == kTypeValUint64) return false;
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
    // DATE and TIMESTAMP arrive as the integers they are - epoch days and
    // epoch microseconds - and took the integer arm above: that is the whole
    // reason TY5 reused kInt for them. A date column is never compared with
    // a timestamp one; the compiler refuses the pair (types.md §3.1a).
    if (lhs.type == parser::ValueType::kStr && rhs.type == parser::ValueType::kStr) {
        if (op == parser::CompareOp::kLike) return LikeMatch(lhs.str_val, rhs.str_val);
        return ApplyCompare<std::string_view>(lhs.str_val, rhs.str_val, op);
    }
    return false;  // incompatible value kinds
}

std::optional<ScanKernelTerm> CompileScanKernelTerm(const catalog::Schema& schema,
                                                    const catalog::RowLayout& layout,
                                                    std::uint16_t column, parser::CompareOp op,
                                                    const parser::AstValue& literal) {
    if (op == parser::CompareOp::kIsNull || op == parser::CompareOp::kIsNotNull ||
        op == parser::CompareOp::kLike) {
        return {};
    }
    if (column >= schema.columns.size()) return {};
    const catalog::SysColumnRow& col = schema.columns[column];
    ScanKernelTerm term;
    term.op = op;
    term.column = column;
    term.lit = literal.int_val;

    if (column == 0) {
        // The Keystone id, compared as the int64 the decoder makes of it. A
        // `uint64` tuple key compares through `ValueAsUint64` and is left to the
        // residual; the id fits 48 bits, so taking it in later is a matter
        // of the literal's range alone.
        if (col.type_val == kTypeValUint64 || literal.type != parser::ValueType::kInt) return {};
        term.kind = ScanKernelTerm::Kind::kKeystone;
        return term;
    }
    switch (col.type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        case kTypeValDate:
        case kTypeValTimestamp:
        case kTypeValBool:
            if (literal.type != parser::ValueType::kInt) return {};
            term.kind = ScanKernelTerm::Kind::kInt;
            term.width = static_cast<std::uint8_t>(
                col.type_val == kTypeValDate        ? 4
                : col.type_val == kTypeValTimestamp ? 8
                : col.type_val == kTypeValBool      ? 1
                                                    : IntWidthFor(col.type_val));
            term.sign_extend = col.type_val != kTypeValBool;
            return term;
        case kTypeValDecimal:
            if (literal.type != parser::ValueType::kDecimal ||
                literal.scale != catalog::DecimalScaleOf(col.len)) {
                return {};
            }
            term.kind = ScanKernelTerm::Kind::kInt;
            term.width = 8;
            return term;
        case catalog::kTypeValDecimalWide:
            if (literal.type != parser::ValueType::kDecimalWide ||
                literal.scale != catalog::DecimalScaleOf(col.len)) {
                return {};
            }
            term.kind = ScanKernelTerm::Kind::kWide;
            term.lit_hi = literal.dec_hi;
            return term;
        case kTypeValChar:
        case kTypeValVarchar:
            if (literal.type != parser::ValueType::kStr) return {};
            term.kind = col.type_val == kTypeValChar ? ScanKernelTerm::Kind::kChar
                                                     : ScanKernelTerm::Kind::kVarchar;
            term.text = literal.str_val;
            return term;
        case kTypeValUint64: {
            if (literal.type != parser::ValueType::kInt) return {};
            auto u = ValueAsUint64(literal);
            if (!u.ok()) return {};
            term.kind = ScanKernelTerm::Kind::kUint64;
            term.ulit = u.value();
            return term;
        }
        default:
            return {};
    }
}

bool ScanKernelRejects(const ScanKernel& kernel, const catalog::RowLayout& layout,
                       std::span<const std::byte> payload) {
    for (const ScanKernelTerm& term : kernel) {
        // A NOT NULL column has no bit, and `NullBitIsSet` answers false.
        if (catalog::NullBitIsSet(payload, layout, term.column)) continue;
        bool keep = true;
        switch (term.kind) {
            case ScanKernelTerm::Kind::kKeystone: {
                auto id = KeystoneIdOfPayload(payload);
                if (!id.ok()) continue;  // the residual's decode answers it
                keep = ApplyCompare(static_cast<std::int64_t>(id.value()), term.lit, term.op);
                break;
            }
            case ScanKernelTerm::Kind::kInt: {
                const std::uint64_t raw = GetLE(CellOf(layout, payload, term.column), term.width);
                const std::int64_t v = term.sign_extend ? SignExtend(raw, term.width)
                                                        : static_cast<std::int64_t>(raw);
                keep = ApplyCompare(v, term.lit, term.op);
                break;
            }
            case ScanKernelTerm::Kind::kUint64:
                keep = ApplyCompare(GetLE(CellOf(layout, payload, term.column), 8), term.ulit,
                                    term.op);
                break;
            case ScanKernelTerm::Kind::kWide: {
                const auto cell = CellOf(layout, payload, term.column);
                const auto lo = static_cast<std::int64_t>(GetLE(cell.subspan(0, 8), 8));
                const auto hi = static_cast<std::int64_t>(GetLE(cell.subspan(8, 8), 8));
                keep = ApplyCompare(Int128FromHalves(hi, lo),
                                    Int128FromHalves(term.lit_hi, term.lit), term.op);
                break;
            }
            case ScanKernelTerm::Kind::kChar: {
                const auto value = CharCellValue(CellOf(layout, payload, term.column));
                keep = ApplyCompare<std::string_view>(
                    std::string_view(reinterpret_cast<const char*>(value.data()), value.size()),
                    term.text, term.op);
                break;
            }
            case ScanKernelTerm::Kind::kVarchar: {
                // Only an inline value the cell codec vouches for; anything
                // else - spilled, NULL-tagged, refused - is the residual's.
                auto cell = storage::DecodeCell(CellOf(layout, payload, term.column));
                if (!cell.ok() || cell.value().tag != storage::CellTag::kInline) continue;
                const auto bytes = cell.value().bytes;
                keep = ApplyCompare<std::string_view>(
                    std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                    term.text, term.op);
                break;
            }
        }
        if (!keep) return true;
    }
    return false;
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
