#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/parser/ast.hpp"

// Encodes/decodes one heap tuple payload to/from a row of parsed SQL
// values (parser::AstValue), positionally aligned with a
// catalog::Schema's columns (which BuildSchemaFromColumns() now returns
// sorted by `pos` - see catalog.cpp). This is the missing piece the
// project's not-yet-ported type registry was blocking (see ast.hpp's and
// command_dispatcher.hpp's file comments): rather than wait on that
// subsystem, this resolves against whatever Catalog::ResolveTypeByName()
// can already look up in sys.types (well_known.hpp's kTypeVal* tags).
//
// Scope, deliberately narrow:
//   - No NULLs. AstValue{type=kNull} is rejected by EncodeRow(); every
//     column must have a value on INSERT. Revisit once a null-bitmap
//     format is decided (would touch both EncodeRow and DecodeRow).
//   - kTypeValFloat/kTypeValDecimal columns can be declared (CREATE
//     TABLE) but not populated: EncodeRow() rejects them, because their
//     on-disk numeric encoding is exactly the kind of open, type-system-
//     dependent decision CLAUDE.md's KWP `DECIMAL` wire-encoding item
//     already flags as unresolved - picking one silently here would be
//     the same mistake in a different subsystem. Also, the SQL lexer has
//     no floating-point literal token yet (only int/string/NULL), so
//     there is no way to spell a float/decimal literal to insert anyway.
//   - Fixed-width columns (int8/16/32/64, uint64, bool, char) are packed
//     back-to-back with no padding, each field written byte-by-byte
//     (rules.md #5: no bitfields, no reinterpret_cast of the buffer).
//     varchar is the only variable-length column: a little-endian
//     uint16 byte length prefix followed by that many raw bytes.
//   - uint64 columns round-trip through AstValue::raw_int_text (the
//     literal's original digit text, preserved by the lexer/parser
//     specifically for this - see ast.hpp) rather than int_val, since
//     int_val is a signed int64_t and cannot represent the upper half of
//     the unsigned 64-bit range. DecodeRow() sets raw_int_text on the
//     values it produces for the same reason - callers formatting a
//     decoded uint64 column should read raw_int_text, not int_val.

namespace kds::exec {

// Rejects a schema that cannot carry a Keystone word: no columns at all,
// or a first column whose declared type is not an integer one. Exposed so
// CREATE TABLE can refuse such a table at definition time rather than at
// the first INSERT.
Status CheckKeystoneColumn(const catalog::Schema& schema);

// Encodes one row's values into a tuple payload.
//
// The payload is `[Keystone word][columns 1..n-1]`: the first schema
// column IS the primary key and is carried by the Keystone word's id
// field, so `values` supplies only the columns after it. `id` is the
// system-generated key (catalog::Catalog::AllocateRowId) - the pk is
// never encoded into the body as well, since two copies of one value is
// how the two come to disagree.
//
// Fails with InvalidArgument if the schema has no usable Keystone column,
// values.size() != schema.columns.size() - 1, `id` exceeds the Keystone
// word's 40-bit range, any value is NULL, any int value doesn't fit its
// column's width, a string value doesn't fit a fixed-width `char` column,
// a varchar value exceeds the uint16 length-prefix's range, or the row
// touches a float/decimal column (see file comment).
StatusOr<std::vector<std::byte>> EncodeRow(const catalog::Schema& schema, std::uint64_t id,
                                            const std::vector<parser::AstValue>& values);

// Reads just the primary key out of a tuple payload, without decoding the
// body. This is what a duplicate-key scan compares, and it is why the pk
// lives in a fixed-offset word: finding it costs no schema walk.
StatusOr<std::uint64_t> RowKeystoneId(std::span<const std::byte> payload);

// Decodes a tuple payload back into one value per `schema.columns` entry,
// pk first, in the same order EncodeRow() wrote them. Fails with
// Corruption if `payload` is shorter than the schema requires (e.g. a
// truncated varchar length prefix) - a heap tuple whose data_len was
// produced by EncodeRow() for this same schema should never trigger this.
StatusOr<std::vector<parser::AstValue>> DecodeRow(const catalog::Schema& schema,
                                                   std::span<const std::byte> payload);

// Renders one decoded value for display in a SELECT response line.
// Prefers AstValue::raw_int_text when set (exact literal/uint64 text),
// falling back to int_val otherwise.
std::string FormatValue(const parser::AstValue& value);

// Evaluates an AND-combined WHERE clause (empty = always matches) against
// one decoded row. A condition naming an unknown column, or comparing
// across incompatible value kinds (e.g. a string literal against an int
// column), never matches - same "no match" outcome SQL's NULL-comparison
// semantics would give here in the absence of real NULL support.
bool MatchesWhere(const catalog::Schema& schema, const std::vector<parser::AstValue>& row,
                   const std::vector<parser::Condition>& where);

}  // namespace kds::exec
