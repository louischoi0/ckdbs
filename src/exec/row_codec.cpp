#include "kds/exec/row_codec.hpp"

#include <charconv>
#include <cstdint>
#include <string>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/keystone.hpp"

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

void PutLE(std::vector<std::byte>& out, std::uint64_t v, int width) {
    for (int i = 0; i < width; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
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

Status EncodeOneValue(const catalog::SysColumnRow& col, const parser::AstValue& val,
                       std::vector<std::byte>& out) {
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
            PutLE(out, static_cast<std::uint64_t>(val.int_val), width);
            return Status::OK();
        }
        case kTypeValUint64: {
            if (val.type != parser::ValueType::kInt) {
                return Status::InvalidArgument("column '" + col_name + "' expects an integer");
            }
            auto u = ParseUint64Text(val.raw_int_text);
            if (!u.ok()) return u.status();
            PutLE(out, u.value(), 8);
            return Status::OK();
        }
        case kTypeValBool: {
            if (val.type != parser::ValueType::kInt || (val.int_val != 0 && val.int_val != 1)) {
                return Status::InvalidArgument(
                    "column '" + col_name + "' expects 0 or 1 (no boolean literal in this grammar)");
            }
            PutLE(out, static_cast<std::uint64_t>(val.int_val), 1);
            return Status::OK();
        }
        case kTypeValChar: {
            if (val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument("column '" + col_name + "' expects a string");
            }
            if (val.str_val.size() > col.len) {
                return Status::InvalidArgument("value too long for column '" + col_name +
                                                "' (max " + std::to_string(col.len) + " bytes)");
            }
            for (char c : val.str_val) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
            for (std::size_t i = val.str_val.size(); i < col.len; ++i) out.push_back(std::byte{0});
            return Status::OK();
        }
        case kTypeValVarchar: {
            if (val.type != parser::ValueType::kStr) {
                return Status::InvalidArgument("column '" + col_name + "' expects a string");
            }
            if (val.str_val.size() > 0xFFFF) {
                return Status::InvalidArgument("value too long for varchar column '" + col_name + "'");
            }
            PutLE(out, val.str_val.size(), 2);
            for (char c : val.str_val) out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
            return Status::OK();
        }
        case kTypeValFloat:
        case kTypeValDecimal:
            return Status::InvalidArgument(
                "column '" + col_name +
                "' has type float/decimal - no on-disk encoding exists yet (open decision, see "
                "row_codec.hpp)");
        default:
            return Status::InvalidArgument("column '" + col_name + "' has an unrecognized type_val");
    }
}

StatusOr<std::size_t> DecodeOneValue(const catalog::SysColumnRow& col,
                                      std::span<const std::byte> payload, std::size_t offset,
                                      std::vector<parser::AstValue>& out) {
    std::string col_name(catalog::NameView(col.name));
    auto need = [&](std::size_t n) -> Status {
        if (offset + n > payload.size()) {
            return Status::Corruption("truncated tuple payload decoding column '" + col_name + "'");
        }
        return Status::OK();
    };

    switch (col.type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64: {
            int width = IntWidthFor(col.type_val);
            if (Status s = need(static_cast<std::size_t>(width)); !s.ok()) return s;
            std::int64_t v = SignExtend(GetLE(payload.subspan(offset, width), width), width);
            parser::AstValue av;
            av.type = parser::ValueType::kInt;
            av.int_val = v;
            av.raw_int_text = std::to_string(v);
            out.push_back(std::move(av));
            return offset + static_cast<std::size_t>(width);
        }
        case kTypeValUint64: {
            if (Status s = need(8); !s.ok()) return s;
            std::uint64_t v = GetLE(payload.subspan(offset, 8), 8);
            parser::AstValue av;
            av.type = parser::ValueType::kInt;
            av.int_val = static_cast<std::int64_t>(v);
            av.raw_int_text = std::to_string(v);
            out.push_back(std::move(av));
            return offset + 8;
        }
        case kTypeValBool: {
            if (Status s = need(1); !s.ok()) return s;
            std::uint64_t v = GetLE(payload.subspan(offset, 1), 1);
            parser::AstValue av;
            av.type = parser::ValueType::kInt;
            av.int_val = static_cast<std::int64_t>(v);
            av.raw_int_text = std::to_string(v);
            out.push_back(std::move(av));
            return offset + 1;
        }
        case kTypeValChar: {
            if (Status s = need(col.len); !s.ok()) return s;
            std::string s;
            for (std::uint32_t i = 0; i < col.len; ++i) {
                auto b = static_cast<unsigned char>(payload[offset + i]);
                if (b == 0) break;
                s.push_back(static_cast<char>(b));
            }
            parser::AstValue av;
            av.type = parser::ValueType::kStr;
            av.str_val = std::move(s);
            out.push_back(std::move(av));
            return offset + col.len;
        }
        case kTypeValVarchar: {
            if (Status s = need(2); !s.ok()) return s;
            std::uint64_t len = GetLE(payload.subspan(offset, 2), 2);
            offset += 2;
            if (Status s = need(len); !s.ok()) return s;
            std::string s(len, '\0');
            for (std::uint64_t i = 0; i < len; ++i) {
                s[i] = static_cast<char>(static_cast<unsigned char>(payload[offset + i]));
            }
            parser::AstValue av;
            av.type = parser::ValueType::kStr;
            av.str_val = std::move(s);
            out.push_back(std::move(av));
            return offset + len;
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

bool ConditionMatches(const catalog::Schema& schema, const std::vector<parser::AstValue>& row,
                       const parser::Condition& cond) {
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        if (catalog::NameView(schema.columns[i].name) != cond.col_name) continue;

        const parser::AstValue& lhs = row[i];
        const parser::AstValue& rhs = cond.val;
        if (lhs.type == parser::ValueType::kNull || rhs.type == parser::ValueType::kNull) {
            return false;  // no NULL support; NULL never matches (see file comment)
        }

        if (schema.columns[i].type_val == kTypeValUint64) {
            auto a = ParseUint64Text(lhs.raw_int_text);
            auto b = ParseUint64Text(rhs.raw_int_text);
            if (!a.ok() || !b.ok()) return false;
            return CompareUint(a.value(), b.value(), cond.op);
        }
        if (lhs.type == parser::ValueType::kInt && rhs.type == parser::ValueType::kInt) {
            return CompareInt(lhs.int_val, rhs.int_val, cond.op);
        }
        if (lhs.type == parser::ValueType::kStr && rhs.type == parser::ValueType::kStr) {
            return CompareStr(lhs.str_val, rhs.str_val, cond.op);
        }
        return false;  // incompatible value kinds
    }
    return false;  // unknown column name
}

}  // namespace

StatusOr<std::vector<std::byte>> EncodeRow(const catalog::Schema& schema, std::uint64_t id,
                                            const std::vector<parser::AstValue>& values) {
    if (Status s = catalog::CheckKeystoneColumn(schema); !s.ok()) return s;

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

    std::vector<std::byte> out;
    out.resize(kKeystoneWordSize);
    StoreLe64(out.data(), word.value());

    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        if (Status s = EncodeOneValue(schema.columns[i], values[i - 1], out); !s.ok()) return s;
    }
    return out;
}

StatusOr<std::uint64_t> RowKeystoneId(std::span<const std::byte> payload) {
    if (payload.size() < kKeystoneWordSize) {
        return Status::Corruption("tuple payload is shorter than its Keystone word");
    }
    return Keystone::Decode(LoadLe64(payload.data())).id;
}

StatusOr<std::vector<parser::AstValue>> DecodeRow(const catalog::Schema& schema,
                                                   std::span<const std::byte> payload) {
    if (Status s = catalog::CheckKeystoneColumn(schema); !s.ok()) return s;

    auto id = RowKeystoneId(payload);
    if (!id.ok()) return id.status();

    std::vector<parser::AstValue> out;
    out.reserve(schema.columns.size());

    parser::AstValue pk{};
    pk.type = parser::ValueType::kInt;
    pk.int_val = static_cast<std::int64_t>(id.value());
    pk.raw_int_text = std::to_string(id.value());
    out.push_back(std::move(pk));

    std::size_t offset = kKeystoneWordSize;
    for (std::size_t i = 1; i < schema.columns.size(); ++i) {
        auto next = DecodeOneValue(schema.columns[i], payload, offset, out);
        if (!next.ok()) return next.status();
        offset = next.value();
    }
    return out;
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

bool MatchesWhere(const catalog::Schema& schema, const std::vector<parser::AstValue>& row,
                   const std::vector<parser::Condition>& where) {
    for (const auto& cond : where) {
        if (!ConditionMatches(schema, row, cond)) return false;
    }
    return true;
}

}  // namespace kds::exec
