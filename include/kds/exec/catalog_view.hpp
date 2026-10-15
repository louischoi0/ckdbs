#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"

// `SELECT * FROM sys.tables` and friends: the catalog, readable.
//
// ---- Why these are views and not relations ------------------------------
//
// A catalog row is not a row-codec tuple. `SysTableRow::Encode()` memcpy's
// its fields at fixed offsets (catalog/rows.hpp): no Keystone word, `oid`
// is a plain 8-byte value at offset 0, `name` a fixed 64-byte array. A user
// tuple is `[Keystone word][packed body]` with the tuple key carried *only* in the
// word (invariant 11) and varchar length-prefixed. The two formats have
// nothing in common, so attaching a `sys.columns` schema to a catalog table
// would not make `DecodeRow` read it - it would misread every field.
//
// So the rows are produced here, through the same typed readers the rest of
// the engine uses (`ListTables`, `GetSysTableRow`, `ListPatterns`), and
// handed on as already-decoded values. That costs one thing and buys
// another:
//
//   costs   a view is materialized, not walked. It is not a relation to
//           the step VM, so it cannot be joined against or appear in a
//           subquery - the executor reads relations page by page and a
//           view has no pages. Refused explicitly rather than silently
//           producing nothing.
//   buys    no on-disk format change, so every existing data file still
//           reads; and no second copy of the catalog's layout, which is
//           the thing that would rot the first time a row gained a field.
//
// A WHERE clause still applies: the values are ordinary `AstValue`s by the
// time they get here, so the same evaluator filters them as filters a real
// relation.

namespace kds::exec {

// The schema qualifier these views live under. Deliberately the only one
// that resolves - `public.t` is not accepted, because user tables are one
// flat space and pretending otherwise would be a namespace mechanism that
// does not exist.
inline constexpr std::string_view kCatalogSchema = "sys";

// One materialized catalog view: the column names a reply's header needs,
// and the rows under them.
struct CatalogView {
    std::vector<std::string> column_names;
    // Each column's type, beside its name: `uint64` for every integer a view
    // holds and `varchar` for every string. Declared rather than read off a
    // row, so a WHERE literal is judged against the column before any row is
    // read (docs/spec/types.md §3.1a) and an empty view refuses what a full
    // one does.
    std::vector<std::uint32_t> column_types;
    std::vector<std::vector<parser::AstValue>> rows;
};

}  // namespace kds::exec
