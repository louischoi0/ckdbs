#include "kds/exec/index_key.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "kds/base/int128.hpp"
#include "kds/exec/row_codec.hpp"  // ValueAsUint64

namespace kds::exec {

namespace {

using catalog::DecimalScaleOf;
using catalog::kTypeValBool;
using catalog::kTypeValChar;
using catalog::kTypeValDate;
using catalog::kTypeValDecimal;
using catalog::kTypeValDecimalWide;
using catalog::kTypeValFloat;
using catalog::kTypeValInt16;
using catalog::kTypeValInt32;
using catalog::kTypeValInt64;
using catalog::kTypeValInt8;
using catalog::kTypeValTimestamp;
using catalog::kTypeValUint64;
using catalog::kTypeValVarchar;

std::string NameOf(const catalog::SysColumnRow& col) {
    return std::string(catalog::NameView(col.name));
}

// Big-endian store of the low `width` bytes of `v`. Big-endian, unlike
// every other encoding in this engine, because these bytes are compared
// with memcmp and nothing else reads them - see the header.
void PutBe(std::span<std::byte> out, std::uint64_t v, std::uint32_t width) {
    for (std::uint32_t i = 0; i < width; ++i) {
        out[i] = static_cast<std::byte>((v >> (8 * (width - 1 - i))) & 0xFF);
    }
}

// All-ones over `width` bytes. Spelled out because `1 << 64` is undefined.
std::uint64_t WidthMask(std::uint32_t width) {
    return width >= 8 ? ~std::uint64_t{0} : ((std::uint64_t{1} << (8 * width)) - 1);
}

// The order-preserving unsigned image of a `width`-byte two's-complement
// signed value: flip the sign bit, then keep the low `width` bytes.
//
// The mask is not cosmetic. `v` arrives sign-extended to 64 bits, so for a
// narrow column the bits above `width` are all 1 for a negative value and
// would otherwise survive into nothing - masking after the XOR is what
// makes INT32_MIN encode to 0x00000000 rather than to a truncation of
// 0xFFFFFFFF00000000.
std::uint64_t SignFlipped(std::int64_t v, std::uint32_t width) {
    const std::uint64_t u = static_cast<std::uint64_t>(v);
    const std::uint64_t sign_bit = std::uint64_t{1} << (8 * width - 1);
    return (u ^ sign_bit) & WidthMask(width);
}

// Inclusive bounds of a `width`-byte signed integer, for the range check.
std::int64_t SignedMin(std::uint32_t width) {
    if (width >= 8) return std::numeric_limits<std::int64_t>::min();
    return -(std::int64_t{1} << (8 * width - 1));
}
std::int64_t SignedMax(std::uint32_t width) {
    if (width >= 8) return std::numeric_limits<std::int64_t>::max();
    return (std::int64_t{1} << (8 * width - 1)) - 1;
}

// A signed integer column of `width` bytes: int8/16/32/64, date and
// timestamp all land here, because all six *are* integers in this engine
// (docs/spec/types.md: a DATE is epoch days, a TIMESTAMP is epoch micros)
// and share one arm rather than one each.
Status EncodeSignedInt(const catalog::SysColumnRow& col, const parser::AstValue& value,
                       std::uint32_t width, std::span<std::byte> out) {
    if (value.type != parser::ValueType::kInt) {
        return Status::InvalidArgument(
            "index key column '" + NameOf(col) +
            "' expects an integer in its storage form; the value is not one. A written literal "
            "must pass through exec::CoerceLiteralToColumn first (docs/spec/types.md §3.1)");
    }
    if (value.int_val < SignedMin(width) || value.int_val > SignedMax(width)) {
        return Status::OutOfRange("index key column '" + NameOf(col) + "' holds " +
                                  std::to_string(width) + " bytes and cannot represent " +
                                  std::to_string(value.int_val));
    }
    PutBe(out, SignFlipped(value.int_val, width), width);
    return Status::OK();
}

// The string arm: raw bytes, truncated to `width` and zero-padded. Both
// collapses this admits are argued in the header; neither can lose a row,
// because the read path re-checks the predicate against the base row.
Status EncodeString(const catalog::SysColumnRow& col, const parser::AstValue& value,
                    std::uint32_t width, std::span<std::byte> out) {
    if (value.type != parser::ValueType::kStr) {
        return Status::InvalidArgument("index key column '" + NameOf(col) +
                                       "' expects a string value");
    }
    std::fill(out.begin(), out.end(), std::byte{0});
    const std::size_t n = std::min<std::size_t>(value.str_val.size(), width);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(value.str_val[i]));
    }
    return Status::OK();
}

}  // namespace

StatusOr<std::uint32_t> IndexKeyColumnWidth(const catalog::SysColumnRow& col) {
    std::uint32_t payload = 0;
    switch (col.type_val) {
        case kTypeValInt8: payload = 1; break;
        case kTypeValInt16: payload = 2; break;
        case kTypeValInt32: payload = 4; break;
        case kTypeValInt64: payload = 8; break;
        case kTypeValUint64: payload = 8; break;
        case kTypeValBool: payload = 1; break;
        case kTypeValDate: payload = 4; break;        // int32 epoch days
        case kTypeValTimestamp: payload = 8; break;   // int64 epoch micros
        case kTypeValDecimal: payload = 8; break;     // int64 unscaled
        case kTypeValDecimalWide: payload = 16; break;  // int128 unscaled
        // A `char(n)` is already fixed-width, so it needs no more of the
        // key than it has bytes; a `varchar` always spends the full prefix.
        case kTypeValChar:
            payload = std::min<std::uint32_t>(col.len, kIndexStringKeyBytes);
            if (payload == 0) {
                return Status::InvalidArgument("index key column '" + NameOf(col) +
                                               "' has zero declared width");
            }
            break;
        case kTypeValVarchar: payload = kIndexStringKeyBytes; break;
        case kTypeValFloat:
            return Status::Unsupported(
                "index key column '" + NameOf(col) +
                "' has type float, which this engine does not store (docs/spec/types.md TY1)");
        default:
            return Status::InvalidArgument("index key column '" + NameOf(col) +
                                           "' has an unrecognized type_val " +
                                           std::to_string(col.type_val));
    }
    return payload + kIndexKeyDiscriminatorSize;
}

StatusOr<std::uint32_t> IndexKeyWidth(std::span<const catalog::SysColumnRow> key_cols) {
    if (key_cols.empty()) {
        return Status::InvalidArgument("an index needs at least one key column");
    }
    std::uint32_t total = 0;
    for (const catalog::SysColumnRow& col : key_cols) {
        auto width = IndexKeyColumnWidth(col);
        if (!width.ok()) return width.status();
        total += width.value();
    }
    return total;
}

Status EncodeIndexKeyColumn(const catalog::SysColumnRow& col, const parser::AstValue& value,
                            std::span<std::byte> out) {
    auto want = IndexKeyColumnWidth(col);
    if (!want.ok()) return want.status();
    if (out.size() != want.value()) {
        return Status::InvalidArgument("index key column '" + NameOf(col) + "' encodes to " +
                                       std::to_string(want.value()) + " bytes, was given " +
                                       std::to_string(out.size()));
    }

    if (value.type == parser::ValueType::kNull) {
        // The discriminator byte exists for this, and writing it would
        // settle where NULLs sort - which is a decision, not an
        // implementation detail, and one nothing can exercise while the row
        // codec refuses to encode a NULL.
        return Status::Unsupported(
            "index key column '" + NameOf(col) +
            "' was handed a NULL; NULLs are not storable yet and their position in the key "
            "order (first or last) is undecided");
    }

    out[0] = static_cast<std::byte>(kIndexKeyPresent);
    std::span<std::byte> payload = out.subspan(kIndexKeyDiscriminatorSize);

    switch (col.type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        case kTypeValDate:
        case kTypeValTimestamp:
            return EncodeSignedInt(col, value, static_cast<std::uint32_t>(payload.size()),
                                   payload);

        case kTypeValBool: {
            // An unsigned domain of exactly {0, 1}, so no sign flip: the
            // natural byte already orders correctly, and flipping would be
            // a transformation with nothing to undo.
            if (value.type != parser::ValueType::kInt ||
                (value.int_val != 0 && value.int_val != 1)) {
                return Status::InvalidArgument("index key column '" + NameOf(col) +
                                               "' expects 0 or 1");
            }
            PutBe(payload, static_cast<std::uint64_t>(value.int_val), 1);
            return Status::OK();
        }

        case kTypeValUint64: {
            // The full unsigned range, so no flip and no int64 round trip:
            // a value above INT64_MAX rides in `raw_int_text` and
            // ValueAsUint64 is the one reader of that arrangement.
            if (value.type != parser::ValueType::kInt) {
                return Status::InvalidArgument("index key column '" + NameOf(col) +
                                               "' expects an integer");
            }
            auto u = ValueAsUint64(value);
            if (!u.ok()) return u.status().WithContext("index key column '" + NameOf(col) + "'");
            PutBe(payload, u.value(), 8);
            return Status::OK();
        }

        case kTypeValDecimal: {
            if (value.type != parser::ValueType::kDecimal) {
                return Status::InvalidArgument(
                    "index key column '" + NameOf(col) +
                    "' expects a decimal in its storage form (an unscaled integer carrying its "
                    "scale); coerce the literal first");
            }
            const std::uint8_t scale = DecimalScaleOf(col.len);
            if (value.scale != scale) {
                return Status::InvalidArgument("index key column '" + NameOf(col) +
                                               "' has scale " + std::to_string(scale) +
                                               " but the value carries scale " +
                                               std::to_string(value.scale) +
                                               "; this engine does not rescale");
            }
            // At one fixed scale the unscaled integers order exactly as the
            // decimals do, which is the property that made the type
            // expressible at all (docs/spec/types.md TY2).
            PutBe(payload, SignFlipped(value.int_val, 8), 8);
            return Status::OK();
        }

        case kTypeValDecimalWide: {
            if (value.type != parser::ValueType::kDecimalWide) {
                return Status::InvalidArgument("index key column '" + NameOf(col) +
                                               "' expects a wide decimal in its storage form");
            }
            const std::uint8_t scale = DecimalScaleOf(col.len);
            if (value.scale != scale) {
                return Status::InvalidArgument("index key column '" + NameOf(col) +
                                               "' has scale " + std::to_string(scale) +
                                               " but the value carries scale " +
                                               std::to_string(value.scale) +
                                               "; this engine does not rescale");
            }
            // High half first, sign-flipped; low half as-is. Big-endian
            // concatenation then compares the high half first, which is
            // what makes memcmp reproduce int128 order. Built from the two
            // halves the AstValue carries rather than from the builtin's
            // bytes - invariant 6's explicit encoding.
            PutBe(payload.subspan(0, 8), SignFlipped(value.dec_hi, 8), 8);
            PutBe(payload.subspan(8, 8), static_cast<std::uint64_t>(value.int_val), 8);
            return Status::OK();
        }

        case kTypeValChar:
        case kTypeValVarchar:
            return EncodeString(col, value, static_cast<std::uint32_t>(payload.size()), payload);

        default:
            // Unreachable: IndexKeyColumnWidth above already rejected every
            // type this switch does not name.
            return Status::InvalidArgument("index key column '" + NameOf(col) +
                                           "' has an unrecognized type_val " +
                                           std::to_string(col.type_val));
    }
}

Status EncodeIndexKey(std::span<const catalog::SysColumnRow> key_cols,
                      std::span<const parser::AstValue> values, std::span<std::byte> out) {
    if (values.size() > key_cols.size()) {
        return Status::InvalidArgument("index key has " + std::to_string(key_cols.size()) +
                                       " columns but " + std::to_string(values.size()) +
                                       " values were given");
    }

    std::size_t at = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto width = IndexKeyColumnWidth(key_cols[i]);
        if (!width.ok()) return width.status();
        if (at + width.value() > out.size()) {
            return Status::InvalidArgument(
                "index key buffer is " + std::to_string(out.size()) +
                " bytes, too small for the first " + std::to_string(values.size()) + " columns");
        }
        if (Status s = EncodeIndexKeyColumn(key_cols[i], values[i], out.subspan(at, width.value()));
            !s.ok()) {
            return s;
        }
        at += width.value();
    }

    if (at != out.size()) {
        return Status::InvalidArgument("index key encoded to " + std::to_string(at) +
                                       " bytes but the buffer is " + std::to_string(out.size()));
    }
    return Status::OK();
}

}  // namespace kds::exec
