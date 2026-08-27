#include "kds/wire/row_codec.hpp"

#include <cstring>
#include <limits>

#include "kds/catalog/well_known.hpp"

namespace kds::wire {
namespace {

using catalog::kTypeValBool;
using catalog::kTypeValChar;
using catalog::kTypeValDate;
using catalog::kTypeValDecimal;
using catalog::kTypeValInt16;
using catalog::kTypeValInt32;
using catalog::kTypeValInt64;
using catalog::kTypeValInt8;
using catalog::kTypeValTimestamp;
using catalog::kTypeValUint64;
using catalog::kTypeValVarchar;

// Little-endian append, explicit shift/mask - the same discipline every
// persisted format in this codebase follows, and for the same reason: this
// one crosses a socket to a client that may not share this machine's byte
// order (protocol.md D5).
template <typename T>
void PutLE(std::vector<std::byte>& out, T value, int width) {
    auto u = static_cast<std::uint64_t>(value);
    for (int i = 0; i < width; ++i) {
        out.push_back(static_cast<std::byte>((u >> (8 * i)) & 0xFF));
    }
}

std::uint64_t LoadLE(std::span<const std::byte> bytes) noexcept {
    std::uint64_t u = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        u |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[i])) << (8 * i);
    }
    return u;
}

void PutBytes(std::vector<std::byte>& out, std::string_view s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    out.insert(out.end(), p, p + s.size());
}

// Appends `{len i32, bytes}` for a value that is present.
void PutField(std::vector<std::byte>& out, std::string_view bytes) {
    PutLE(out, static_cast<std::uint32_t>(bytes.size()), 4);
    PutBytes(out, bytes);
}

Status ExpectInt(const parser::AstValue& value, const char* type_name) {
    if (value.type == parser::ValueType::kParam) {
        return Status::Unsupported(
            "wire row codec: parameter '$" + value.param_name() +
            "' has no value to encode; a declaration is not an execution");
    }
    if (value.type != parser::ValueType::kInt) {
        return Status::InvalidArgument(std::string("wire row codec: ") + type_name +
                                       " column did not receive an integer");
    }
    return Status::OK();
}

// Parses the preserved digit text of a uint64. Falls back to int_val when
// the text is absent, which is what a value built by the engine rather than
// parsed from a literal carries.
std::uint64_t Uint64Of(const parser::AstValue& value) {
    if (value.raw_int_text.empty()) return static_cast<std::uint64_t>(value.int_val);
    std::uint64_t u = 0;
    for (char c : value.raw_int_text) {
        if (c < '0' || c > '9') return static_cast<std::uint64_t>(value.int_val);
        u = u * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return u;
}

// Where the u16 row count lives in a batch payload.
constexpr std::size_t kRowCountOffset = 0;
constexpr std::size_t kBatchHeaderSize = 2;

}  // namespace

std::int16_t WireTypeLen(std::uint32_t type_val) noexcept {
    switch (type_val) {
        case kTypeValInt8:
        case kTypeValBool: return 1;
        case kTypeValInt16: return 2;
        case kTypeValInt32:
        // A date is its epoch-day int32, on the wire as in the tuple.
        case kTypeValDate: return 4;
        case kTypeValInt64:
        case kTypeValUint64:
        // Epoch microseconds, and the unscaled decimal - both the int64
        // storage holds (well_known.hpp's TY1/TY9 table); the decimal's
        // scale is in the description's type_mod, not in the value.
        case kTypeValTimestamp:
        case kTypeValDecimal: return 8;
        // The wide decimal: the int128 unscaled value, 16 LE bytes - the
        // "own type_oid and 16-byte width" §6's DECIMAL decision reserved.
        case catalog::kTypeValDecimalWide: return 16;
        // char and varchar are both variable **on the wire**, whatever they
        // occupy in a tuple: a char column's stored width is a schema fact
        // and its value's length is not.
        default: return -1;
    }
}

std::vector<FieldDescription> DescribeSchema(const catalog::Schema& schema) {
    std::vector<FieldDescription> fields;
    fields.reserve(schema.columns.size());
    for (const auto& col : schema.columns) {
        FieldDescription f;
        f.name = std::string(catalog::NameView(col.name));
        f.type_oid = col.type_val;
        f.type_len = WireTypeLen(col.type_val);
        // The catalog's packed (p, s) word, carried as-is: one packing,
        // two readers (TY02's helpers), zero for every other type. Not
        // col.len unconditionally - a char column's len is a storage width,
        // which the header says this description deliberately does not leak.
        if (col.type_val == kTypeValDecimal || col.type_val == catalog::kTypeValDecimalWide) {
            f.type_mod = col.len;
        }
        // Column 0 is the Keystone id on every user relation (invariant 11,
        // protocol.md §6), and it is the one field a client can rely on
        // without reading the schema.
        if (col.pos == 0) f.flags |= kFieldFlagKeystone;
        fields.push_back(std::move(f));
    }
    return fields;
}

void EncodeRowDescription(const std::vector<FieldDescription>& fields,
                          std::vector<std::byte>& out) {
    PutLE(out, static_cast<std::uint32_t>(fields.size()), 2);
    for (const FieldDescription& f : fields) {
        PutLE(out, static_cast<std::uint32_t>(f.name.size()), 2);
        PutBytes(out, f.name);
        PutLE(out, f.type_oid, 4);
        PutLE(out, static_cast<std::uint16_t>(f.type_len), 2);
        PutLE(out, f.flags, 2);
        PutLE(out, f.type_mod, 4);
    }
}

Status EncodeValue(const catalog::SysColumnRow& col, const parser::AstValue& value,
                   std::vector<std::byte>& out) {
    const std::uint32_t type_val = col.type_val;
    if (value.type == parser::ValueType::kNull) {
        // -1, the one NULL convention (protocol.md §6). Decided before the
        // engine could store a NULL, which is why NULL storage landing
        // (null.md) was no wire break: a stored NULL ships as the
        // length this format always reserved for it.
        PutLE(out, static_cast<std::uint32_t>(0xFFFFFFFFu), 4);
        return Status::OK();
    }

    switch (type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        // A date and a timestamp *are* integers (types.md TY5): the
        // decoder hands them over as kInt, and WireTypeLen already knows
        // their widths, so the int arm is their arm.
        case kTypeValDate:
        case kTypeValTimestamp: {
            if (Status s = ExpectInt(value, "an integer"); !s.ok()) return s;
            const std::int16_t width = WireTypeLen(type_val);
            PutLE(out, static_cast<std::uint32_t>(width), 4);
            PutLE(out, value.int_val, width);
            return Status::OK();
        }
        case kTypeValDecimal: {
            if (value.type == parser::ValueType::kParam) {
                return Status::Unsupported(
                    "wire row codec: parameter '$" + value.param_name() +
                    "' has no value to encode; a declaration is not an execution");
            }
            if (value.type != parser::ValueType::kDecimal) {
                return Status::InvalidArgument(
                    "wire row codec: decimal column did not receive a decimal value");
            }
            // The description declared this column's scale; an unscaled
            // integer under any other scale is a different number wearing
            // the right width, so a disagreement is refused, never
            // rescaled - the same rule the storage codec applies (TY04).
            const std::uint8_t scale = catalog::DecimalScaleOf(col.len);
            if (value.scale != scale) {
                return Status::InvalidArgument(
                    "wire row codec: decimal value at scale " + std::to_string(value.scale) +
                    " for a column of scale " + std::to_string(scale));
            }
            PutLE(out, static_cast<std::uint32_t>(8), 4);
            PutLE(out, value.int_val, 8);
            return Status::OK();
        }
        case catalog::kTypeValDecimalWide: {
            if (value.type == parser::ValueType::kParam) {
                return Status::Unsupported(
                    "wire row codec: parameter '$" + value.param_name() +
                    "' has no value to encode; a declaration is not an execution");
            }
            if (value.type != parser::ValueType::kDecimalWide) {
                return Status::InvalidArgument(
                    "wire row codec: wide decimal column did not receive a wide decimal value");
            }
            const std::uint8_t scale = catalog::DecimalScaleOf(col.len);
            if (value.scale != scale) {
                return Status::InvalidArgument(
                    "wire row codec: decimal value at scale " + std::to_string(value.scale) +
                    " for a column of scale " + std::to_string(scale));
            }
            // Both halves, low first - 16 LE bytes, the value the tuple
            // holds, with the scale in the description's type_mod exactly
            // as the 8-byte type carries it.
            PutLE(out, static_cast<std::uint32_t>(16), 4);
            PutLE(out, value.int_val, 8);
            PutLE(out, value.dec_hi, 8);
            return Status::OK();
        }
        case kTypeValUint64: {
            if (Status s = ExpectInt(value, "a uint64"); !s.ok()) return s;
            PutLE(out, static_cast<std::uint32_t>(8), 4);
            PutLE(out, Uint64Of(value), 8);
            return Status::OK();
        }
        case kTypeValBool: {
            if (Status s = ExpectInt(value, "a bool"); !s.ok()) return s;
            if (value.int_val != 0 && value.int_val != 1) {
                return Status::InvalidArgument("wire row codec: bool column value is not 0 or 1");
            }
            PutLE(out, static_cast<std::uint32_t>(1), 4);
            PutLE(out, value.int_val, 1);
            return Status::OK();
        }
        case kTypeValChar:
        case kTypeValVarchar: {
            if (value.type == parser::ValueType::kParam) {
                return Status::Unsupported(
                    "wire row codec: parameter '$" + value.param_name() +
                    "' has no value to encode; a declaration is not an execution");
            }
            if (value.type != parser::ValueType::kStr) {
                return Status::InvalidArgument(
                    "wire row codec: text column did not receive a string");
            }
            PutField(out, value.str_val);
            return Status::OK();
        }
        default:
            // Only float and unknown type_vals land here now. float is not
            // storable - refused at CREATE TABLE, encoding unsettled - so a
            // value of one arriving here is a defect upstream, not a gap.
            return Status::Unsupported("wire row codec: no wire encoding for type " +
                                       std::to_string(type_val));
    }
}

RowBatchWriter::RowBatchWriter(std::size_t reserve_bytes) {
    if (reserve_bytes > 0) buffer_.reserve(reserve_bytes);
    // The row count is patched in by Finish(); reserving its bytes up front
    // is what lets rows append straight into the same buffer.
    buffer_.resize(kBatchHeaderSize, std::byte{0});
}

bool RowBatchWriter::full() const noexcept {
    return row_count_ == std::numeric_limits<std::uint16_t>::max();
}

Status RowBatchWriter::AppendRow(const catalog::Schema& schema,
                                 std::span<const parser::AstValue> values) {
    if (values.size() != schema.columns.size()) {
        return Status::InvalidArgument(
            "wire row codec: row has " + std::to_string(values.size()) + " values but the schema has " +
            std::to_string(schema.columns.size()) + " columns");
    }
    if (full()) {
        return Status::ResourceExhausted(
            "wire row codec: a batch holds at most 65535 rows; start another");
    }

    // Encoded into the live buffer, then rolled back on failure: a
    // half-encoded row would leave the batch unparseable, and the caller's
    // natural response to an error is to report it and keep the rows it
    // already had.
    const std::size_t mark = buffer_.size();
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (Status s = EncodeValue(schema.columns[i], values[i], buffer_); !s.ok()) {
            buffer_.resize(mark);
            return s;
        }
    }
    ++row_count_;
    return Status::OK();
}

std::vector<std::byte> RowBatchWriter::Finish() {
    buffer_[kRowCountOffset] = static_cast<std::byte>(row_count_ & 0xFF);
    buffer_[kRowCountOffset + 1] = static_cast<std::byte>((row_count_ >> 8) & 0xFF);

    std::vector<std::byte> done;
    done.swap(buffer_);
    buffer_.resize(kBatchHeaderSize, std::byte{0});
    row_count_ = 0;
    return done;
}

StatusOr<std::vector<FieldDescription>> DecodeRowDescription(std::span<const std::byte> payload) {
    std::size_t at = 0;
    auto need = [&](std::size_t n) { return at + n <= payload.size(); };

    if (!need(2)) return Status::Corruption("wire row description: truncated field count");
    const auto count = static_cast<std::uint16_t>(LoadLE(payload.subspan(at, 2)));
    at += 2;

    std::vector<FieldDescription> fields;
    fields.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        if (!need(2)) return Status::Corruption("wire row description: truncated name length");
        const auto name_len = static_cast<std::uint16_t>(LoadLE(payload.subspan(at, 2)));
        at += 2;
        if (!need(name_len)) return Status::Corruption("wire row description: truncated name");

        FieldDescription f;
        f.name.assign(reinterpret_cast<const char*>(payload.data() + at), name_len);
        at += name_len;

        if (!need(12)) return Status::Corruption("wire row description: truncated field trailer");
        f.type_oid = static_cast<std::uint32_t>(LoadLE(payload.subspan(at, 4)));
        at += 4;
        f.type_len = static_cast<std::int16_t>(LoadLE(payload.subspan(at, 2)));
        at += 2;
        f.flags = static_cast<std::uint16_t>(LoadLE(payload.subspan(at, 2)));
        at += 2;
        f.type_mod = static_cast<std::uint32_t>(LoadLE(payload.subspan(at, 4)));
        at += 4;
        fields.push_back(std::move(f));
    }
    return fields;
}

StatusOr<std::vector<std::vector<DecodedField>>> DecodeRowBatch(std::span<const std::byte> payload,
                                                                std::size_t field_count) {
    if (payload.size() < kBatchHeaderSize) {
        return Status::Corruption("wire row batch: truncated row count");
    }
    const auto rows = static_cast<std::uint16_t>(LoadLE(payload.subspan(kRowCountOffset, 2)));
    std::size_t at = kBatchHeaderSize;

    std::vector<std::vector<DecodedField>> out;
    out.reserve(rows);
    for (std::uint16_t r = 0; r < rows; ++r) {
        std::vector<DecodedField> row;
        row.reserve(field_count);
        for (std::size_t f = 0; f < field_count; ++f) {
            if (at + 4 > payload.size()) {
                return Status::Corruption("wire row batch: truncated field length at row " +
                                          std::to_string(r));
            }
            const auto raw = static_cast<std::uint32_t>(LoadLE(payload.subspan(at, 4)));
            at += 4;
            if (raw == 0xFFFFFFFFu) {
                row.push_back(DecodedField{{}, /*is_null=*/true});
                continue;
            }
            if (at + raw > payload.size()) {
                return Status::Corruption("wire row batch: field of " + std::to_string(raw) +
                                          " bytes runs past the payload at row " +
                                          std::to_string(r));
            }
            row.push_back(DecodedField{payload.subspan(at, raw), false});
            at += raw;
        }
        out.push_back(std::move(row));
    }
    return out;
}

StatusOr<std::int64_t> DecodeInt(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > 8) {
        return Status::Corruption("wire row codec: integer field of " +
                                  std::to_string(bytes.size()) + " bytes");
    }
    const std::uint64_t u = LoadLE(bytes);
    // Sign-extend from the field's own width, which is what makes a
    // negative int32 read back as a negative int64 rather than as 4 billion.
    const int bits = static_cast<int>(bytes.size()) * 8;
    if (bits < 64 && (u >> (bits - 1)) & 1u) {
        return static_cast<std::int64_t>(u | (~0ULL << bits));
    }
    return static_cast<std::int64_t>(u);
}

StatusOr<std::uint64_t> DecodeUint64(std::span<const std::byte> bytes) {
    if (bytes.size() != 8) {
        return Status::Corruption("wire row codec: uint64 field of " +
                                  std::to_string(bytes.size()) + " bytes");
    }
    return LoadLE(bytes);
}

std::string_view DecodeText(std::span<const std::byte> bytes) {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

StatusOr<Int128> DecodeDecimalWide(std::span<const std::byte> bytes) {
    if (bytes.size() != 16) {
        return Status::Corruption("wire row codec: wide decimal field of " +
                                  std::to_string(bytes.size()) + " bytes");
    }
    const auto lo = static_cast<std::int64_t>(LoadLE(bytes.subspan(0, 8)));
    const auto hi = static_cast<std::int64_t>(LoadLE(bytes.subspan(8, 8)));
    return Int128FromHalves(hi, lo);
}

parser::AstValue FieldToValue(const catalog::SysColumnRow& col, const DecodedField& field) {
    parser::AstValue v;
    if (field.is_null) return v;  // kNull default
    auto le = [&](std::size_t n) {
        std::uint64_t out = 0;
        for (std::size_t i = 0; i < n && i < field.bytes.size(); ++i) {
            out |= static_cast<std::uint64_t>(field.bytes[i]) << (8 * i);
        }
        return out;
    };
    switch (col.type_val) {
        case catalog::kTypeValInt8:
            v.type = parser::ValueType::kInt;
            v.int_val = static_cast<std::int8_t>(le(1));
            break;
        case catalog::kTypeValInt16:
            v.type = parser::ValueType::kInt;
            v.int_val = static_cast<std::int16_t>(le(2));
            break;
        case catalog::kTypeValInt32:
        case catalog::kTypeValDate:
            v.type = parser::ValueType::kInt;
            v.int_val = static_cast<std::int32_t>(le(4));
            break;
        case catalog::kTypeValInt64:
        case catalog::kTypeValTimestamp:
            v.type = parser::ValueType::kInt;
            v.int_val = static_cast<std::int64_t>(le(8));
            break;
        case catalog::kTypeValUint64:
            v.type = parser::ValueType::kInt;
            v.int_val = static_cast<std::int64_t>(le(8));
            v.raw_int_text = std::to_string(le(8));
            break;
        case catalog::kTypeValBool:
            v.type = parser::ValueType::kInt;
            v.int_val = le(1) != 0 ? 1 : 0;
            break;
        case catalog::kTypeValDecimal:
            v.type = parser::ValueType::kDecimal;
            v.int_val = static_cast<std::int64_t>(le(8));
            v.scale = static_cast<std::uint8_t>(catalog::DecimalScaleOf(col.len));
            break;
        case catalog::kTypeValDecimalWide: {
            v.type = parser::ValueType::kDecimalWide;
            v.int_val = static_cast<std::int64_t>(le(8));
            std::uint64_t hi = 0;
            for (std::size_t i = 8; i < 16 && i < field.bytes.size(); ++i) {
                hi |= static_cast<std::uint64_t>(field.bytes[i]) << (8 * (i - 8));
            }
            v.dec_hi = static_cast<std::int64_t>(hi);
            v.scale = static_cast<std::uint8_t>(catalog::DecimalScaleOf(col.len));
            break;
        }
        default:  // varchar, char: the bytes are the text
            v.type = parser::ValueType::kStr;
            v.str_val.assign(reinterpret_cast<const char*>(field.bytes.data()),
                             field.bytes.size());
            break;
    }
    return v;
}

StatusOr<parser::AstValue> FieldToValueChecked(const catalog::SysColumnRow& col,
                                               const DecodedField& field) {
    if (!field.is_null) {
        const std::int16_t want = WireTypeLen(col.type_val);
        if (want >= 0 && field.bytes.size() != static_cast<std::size_t>(want)) {
            return Status::Corruption(
                "wire row codec: field for column type " + std::to_string(col.type_val) +
                " carries " + std::to_string(field.bytes.size()) + " bytes where " +
                std::to_string(want) + " are its width; a disagreeing length is never "
                "interpreted");
        }
    }
    return FieldToValue(col, field);
}

}  // namespace kds::wire
