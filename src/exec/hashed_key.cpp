#include "kds/exec/hashed_key.hpp"

#include <vector>

#include "kds/catalog/well_known.hpp"
#include "kds/exec/index_key.hpp"
#include "kds/exec/row_codec.hpp"

namespace kds::exec {

namespace {

constexpr std::byte kTagEncoded{1};
constexpr std::byte kTagText{2};

void AppendLength(std::vector<std::byte>& out, std::size_t length) {
    const auto n = static_cast<std::uint32_t>(length);
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((n >> shift) & 0xFF));
    }
}

bool IsText(std::uint32_t type_val) noexcept {
    return type_val == catalog::kTypeValVarchar || type_val == catalog::kTypeValChar;
}

}  // namespace

std::uint64_t Hash47(std::span<const std::byte> canonical) noexcept {
    // FNV-1a, 64-bit: offset basis and prime as published.
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const std::byte b : canonical) {
        h ^= static_cast<std::uint64_t>(b);
        h *= 0x100000001b3ull;
    }
    // MurmurHash3's fmix64: FNV alone leaves its low bits a weak function
    // of the last bytes, and the fold below reads all of them.
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return 1 + h % kHashedKeyHomes;
}

std::optional<std::uint64_t> HashedKeyHome(const catalog::Schema& schema,
                                           std::span<const std::uint16_t> key_cols,
                                           std::span<const parser::AstValue> values) {
    if (key_cols.empty() || key_cols.size() != values.size()) return std::nullopt;
    std::vector<std::byte> canonical;
    for (std::size_t i = 0; i < key_cols.size(); ++i) {
        if (key_cols[i] >= schema.columns.size()) return std::nullopt;
        const catalog::SysColumnRow& col = schema.columns[key_cols[i]];
        parser::AstValue value = values[i];
        if (value.type == parser::ValueType::kNull || value.type == parser::ValueType::kParam) {
            return std::nullopt;
        }
        if (!CoerceLiteralToColumn(col, value).ok()) return std::nullopt;
        if (IsText(col.type_val)) {
            if (value.type != parser::ValueType::kStr) return std::nullopt;
            canonical.push_back(kTagText);
            AppendLength(canonical, value.str_val.size());
            for (const char c : value.str_val) canonical.push_back(static_cast<std::byte>(c));
            continue;
        }
        auto width = IndexKeyColumnWidth(col);
        if (!width.ok()) return std::nullopt;
        canonical.push_back(kTagEncoded);
        AppendLength(canonical, width.value());
        const std::size_t at = canonical.size();
        canonical.resize(at + width.value());
        if (!EncodeIndexKeyColumn(col, value,
                                  std::span<std::byte>(canonical).subspan(at, width.value()))
                 .ok()) {
            return std::nullopt;
        }
    }
    return Hash47(canonical);
}

}  // namespace kds::exec
