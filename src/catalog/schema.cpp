#include "kds/catalog/schema.hpp"

#include <algorithm>
#include <string>

#include "kds/catalog/well_known.hpp"
#include "kds/storage/tagged_cell.hpp"  // varchar(N)'s bounds are the cell width's

namespace kds::catalog {

namespace {

// The refusal of a written name that resolves to more than one live column
// (`matches`: positions, two or more), naming each match with its position.
Status AmbiguousColumnName(const Schema& schema, std::string_view written,
                           const std::vector<std::uint16_t>& matches, std::uint32_t byte_offset) {
    std::string listed;
    for (std::size_t i = 0; i < matches.size(); ++i) {
        if (i > 0) listed += i + 1 == matches.size() ? " and " : ", ";
        listed += "'" + std::string(NameView(schema.columns[matches[i]].name)) + "' (position " +
                  std::to_string(matches[i]) + ")";
    }
    return Status::InvalidArgument("column name '" + std::string(written) +
                                   "' is ambiguous: it matches " + listed +
                                   " ignoring case (byte " + std::to_string(byte_offset) + ")");
}

}  // namespace

StatusOr<const SysColumnRow*> Schema::FindColumn(std::string_view name,
                                                std::uint32_t byte_offset) const {
    const std::vector<std::uint16_t> matches = MatchColumns(name);
    if (matches.empty()) return static_cast<const SysColumnRow*>(nullptr);
    if (matches.size() > 1) return AmbiguousColumnName(*this, name, matches, byte_offset);
    return &columns[matches.front()];
}

std::vector<std::uint16_t> Schema::MatchColumns(std::string_view name) const {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (!columns[i].dropped() && ColumnNamesCollide(NameView(columns[i].name), name)) {
            out.push_back(static_cast<std::uint16_t>(i));
        }
    }
    return out;
}

// ASCII only, and not `std::tolower`, whose answer is the locale's: the
// lexer admits no other identifier byte, and whether two names are one must
// not move with the process's locale.
bool ColumnNamesCollide(std::string_view a, std::string_view b) noexcept {
    const auto fold = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) {
               return fold(x) == fold(y);
           });
}

Status CheckColumnNamesUnique(const Schema& schema) {
    for (std::size_t i = 0; i < schema.columns.size(); ++i) {
        if (schema.columns[i].dropped()) continue;
        const std::string_view later = NameView(schema.columns[i].name);
        for (std::size_t j = 0; j < i; ++j) {
            if (schema.columns[j].dropped()) continue;
            const std::string_view earlier = NameView(schema.columns[j].name);
            if (ColumnNamesCollide(earlier, later)) {
                return Status::InvalidArgument("column '" + std::string(later) +
                                               "' repeats column '" + std::string(earlier) +
                                               "' (names are unique ignoring case)");
            }
        }
    }
    return Status::OK();
}

bool IsIntegerTypeVal(std::uint32_t type_val) noexcept {
    switch (type_val) {
        case kTypeValInt8:
        case kTypeValInt16:
        case kTypeValInt32:
        case kTypeValInt64:
        case kTypeValUint64: return true;
        default: return false;
    }
}

bool SchemaCanSpill(const Schema& schema) noexcept {
    for (const auto& col : schema.columns) {
        // varchar is the only tagged-cell type today. `char` is fixed by
        // declaration and every numeric type by its width, so neither can
        // ever exceed its cell.
        if (col.type_val == kTypeValVarchar) return true;
    }
    return false;
}

Status CheckDeclarableColumnTypes(const Schema& schema) {
    for (const auto& col : schema.columns) {
        // A declared varchar cell width, checked at the catalog's own door
        // for the reason the decimal check below is here: the dispatcher
        // bounds it and the parser does not, so a schema built by neither -
        // a tool, a test, a future caller - is exactly what this is for.
        // **And nothing else catches it**: `RowLayout::Build` accepts an
        // 8-byte varchar cell happily, and the relation then refuses every
        // INSERT with a message about a cell width nobody can trace to a
        // declaration.
        //
        // `char` needs no arm here, deliberately: `Build` runs on the same
        // schema seven lines after this function and already refuses a
        // zero-width column, so a second refusal would be a third wording
        // of one condition (the phase-A review's S-1).
        //
        // A varchar's `len` of 0 is not unset: it is "the instance's
        // width", which every pre-v2.5.0 column carries, so only a
        // *declared* width is checked.
        if (col.type_val == kTypeValVarchar && col.len != 0) {
            if (Status s = storage::CheckInlineCellWidth(col.len); !s.ok()) {
                return s.WithContext("column '" + std::string(NameView(col.name)) + "'");
            }
        }
        // A decimal with no scale stored has values with no defined
        // meaning. The parser refuses a bare `decimal` and the dispatcher
        // packs the pair, so reaching here with an unset one means the
        // schema was built by neither - which is exactly the case a check
        // at the catalog's own door is for.
        if ((col.type_val == kTypeValDecimal || col.type_val == kTypeValDecimalWide) &&
            DecimalPrecisionOf(col.len) == 0) {
            return Status::InvalidArgument(
                "column '" + std::string(NameView(col.name)) +
                "' is decimal with no precision recorded (docs/spec/types.md TY2)");
        }
    }
    return Status::OK();
}

Status CheckKeystoneColumn(const Schema& schema) {
    if (schema.columns.empty()) {
        return Status::InvalidArgument(
            "relation has no columns; the first column is the mandatory Keystone tuple key");
    }
    if (!IsIntegerTypeVal(schema.columns.front().type_val)) {
        return Status::InvalidArgument("tuple key column '" +
                                        std::string(NameView(schema.columns.front().name)) +
                                        "' must be an integer type - it carries the Keystone id");
    }
    return Status::OK();
}

}  // namespace kds::catalog
