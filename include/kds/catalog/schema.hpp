#pragma once

#include <string_view>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/catalog/rows.hpp"

// In-memory schema (built from sys.columns rows for a given rel_id) and
// table access handle - ported from the legacy kernel engine's
// kds_schema_t/kds_table_access_t.

namespace kds::catalog {

struct Schema {
    std::vector<SysColumnRow> columns;

    const SysColumnRow* FindColumn(std::string_view name) const noexcept;
};

// True for the integer types a Keystone id can be declared as. Float,
// decimal, bool, char and varchar are not among them: none can represent a
// 40-bit id, and a pk that cannot hold its own value is a table no row can
// be written to.
bool IsIntegerTypeVal(std::uint32_t type_val) noexcept;

// Rejects a schema that cannot carry a Keystone word: no columns at all,
// or a first column whose declared type is not an integer one. Every
// relation's first column is its system-generated primary key
// (heap-and-tuple.md section 4), so this is a property of the schema
// itself, checked at CREATE TABLE rather than at the first INSERT.
Status CheckKeystoneColumn(const Schema& schema);

struct TableAccess {
    Oid namespace_oid;
    Oid oid;
    Schema schema;
    PageId desc_page_id;
    ClusteredType clustered_type;
};

}  // namespace kds::catalog
