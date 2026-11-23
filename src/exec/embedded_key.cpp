#include "kds/exec/embedded_key.hpp"

#include <array>
#include <cstddef>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/index_key.hpp"
#include "kds/storage/keystone.hpp"

namespace kds::exec {

namespace {

// One column's code and its width in bits: the index key encoding's payload
// read as a big-endian unsigned. The encoder is the one authority for what
// a value of the column's type orders as (IX5), and for refusing a value
// the column cannot hold.
struct ColumnCode {
    std::uint64_t code = 0;
    std::uint32_t bits = 0;
};

StatusOr<ColumnCode> CodeOf(const catalog::SysColumnRow& col, const parser::AstValue& value) {
    const std::optional<std::uint32_t> bits = EmbeddedKeyColumnBits(col.type_val);
    if (!bits.has_value()) {
        return Status::Corruption("embedded key column '" +
                                  std::string(catalog::NameView(col.name)) +
                                  "' is of a type that never embeds");
    }
    // The widest embeddable payload is 4 bytes; the discriminator is one more.
    std::array<std::byte, kIndexKeyDiscriminatorSize + 4> buf{};
    const std::size_t width = kIndexKeyDiscriminatorSize + *bits / 8;
    if (Status s = EncodeIndexKeyColumn(col, value, std::span<std::byte>(buf).first(width));
        !s.ok()) {
        return s;
    }
    ColumnCode out;
    out.bits = *bits;
    for (std::size_t i = kIndexKeyDiscriminatorSize; i < width; ++i) {
        out.code = (out.code << 8) | static_cast<std::uint64_t>(buf[i]);
    }
    return out;
}

}  // namespace

std::optional<std::uint32_t> EmbeddedKeyColumnBits(std::uint32_t type_val) {
    switch (type_val) {
        case catalog::kTypeValInt8:
        case catalog::kTypeValBool:
            return 8;
        case catalog::kTypeValInt16:
            return 16;
        case catalog::kTypeValInt32:
        case catalog::kTypeValDate:
            return 32;
        default:
            return std::nullopt;
    }
}

bool KeyEmbeds(std::span<const catalog::SysColumnRow> key_cols) {
    if (key_cols.empty()) return false;
    std::uint32_t total = 0;
    for (const catalog::SysColumnRow& col : key_cols) {
        const std::optional<std::uint32_t> bits = EmbeddedKeyColumnBits(col.type_val);
        if (!bits.has_value()) return false;
        total += *bits;
    }
    return total <= kEmbeddedKeyMaxBits;
}

StatusOr<std::uint64_t> EmbeddedKeyId(const catalog::Schema& schema,
                                      std::span<const parser::AstValue> key_values) {
    if (key_values.size() != schema.embedded_key.size() || key_values.empty()) {
        return Status::InvalidArgument("an embedded key takes one value per key column");
    }
    std::uint64_t code = 0;
    for (std::size_t i = 0; i < key_values.size(); ++i) {
        auto column = CodeOf(schema.columns[schema.embedded_key[i]], key_values[i]);
        if (!column.ok()) return column.status();
        code = (code << column.value().bits) | column.value().code;
    }
    // id = code + 1, and the largest id is 2^48 - 1 (`kMaxKeystoneId`).
    if (code >= kMaxKeystoneId) {
        return Status::OutOfRange(
            "the largest value of every primary key column together is the one key this "
            "relation cannot hold: its key fills the Keystone id's 48 bits, and id 0 is no id");
    }
    return code + 1;
}

StatusOr<std::optional<EmbeddedKeyWindow>> EmbeddedKeyWindowOf(
    const catalog::Schema& schema, std::span<const parser::AstValue> prefix,
    const parser::AstValue* low, const parser::AstValue* high) {
    const std::vector<std::uint16_t>& key = schema.embedded_key;
    const bool ranged = low != nullptr && high != nullptr;
    if ((low == nullptr) != (high == nullptr) ||
        prefix.size() + (ranged ? 1 : 0) > key.size() || (prefix.empty() && !ranged)) {
        return Status::InvalidArgument(
            "an embedded key's window is its leading columns' values and an inclusive range "
            "on the next");
    }
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        auto column = CodeOf(schema.columns[key[i]], prefix[i]);
        if (!column.ok()) return column.status();
        lo = (lo << column.value().bits) | column.value().code;
    }
    hi = lo;
    std::size_t bound = prefix.size();
    if (ranged) {
        auto from = CodeOf(schema.columns[key[bound]], *low);
        auto to = CodeOf(schema.columns[key[bound]], *high);
        if (!from.ok()) return from.status();
        if (!to.ok()) return to.status();
        if (from.value().code > to.value().code) return std::optional<EmbeddedKeyWindow>{};
        lo = (lo << from.value().bits) | from.value().code;
        hi = (hi << to.value().bits) | to.value().code;
        ++bound;
    }
    // Every column after the bound: its least code below, its greatest above.
    for (; bound < key.size(); ++bound) {
        const std::uint32_t bits = *EmbeddedKeyColumnBits(schema.columns[key[bound]].type_val);
        lo <<= bits;
        hi = (hi << bits) | ((std::uint64_t{1} << bits) - 1);
    }
    // id = code + 1; the one code past the id space names no row.
    EmbeddedKeyWindow window;
    window.lo = lo + 1;
    window.hi = hi >= kMaxKeystoneId ? kMaxKeystoneId : hi + 1;
    if (window.lo > window.hi) return std::optional<EmbeddedKeyWindow>{};
    return std::optional<EmbeddedKeyWindow>{window};
}

}  // namespace kds::exec
