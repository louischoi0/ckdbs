#pragma once

#include <string_view>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/catalog/rows.hpp"

// In-memory schema, built from the sys.columns rows for a given rel_id,
// plus the per-relation access handles the cache hands out.

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

// A pattern as the cache holds it (docs/waystone-concpets.md section 4):
// everything about a `sys.patterns` row that DDL alone can change.
//
// **The omissions are the point.** `use_count` and `last_seen` are not
// here, and neither may ever be added: they change on every execution,
// which is not DDL, so a cached copy of them would be stale the moment it
// was taken and there is no invalidation that could fix it. This is
// exactly why TableAccess carries no `next_id` (catalog_cache.hpp), and
// for exactly the same reason - a fact that moves without DDL is not a
// cacheable fact. A caller that wants heat reads the row from the page
// through Catalog::GetSysPatternRow().
//
// What is here divides in two. The identity - `oid`, `pattern_id`,
// `fingerprint_version`, `stmt_class` - is written once at registration
// and never changes. The location - `waystone_root`, `dir_depth` - changes
// only when the directory deepens, through the single writer
// Catalog::SetPatternWaystoneRoot(), which updates this entry in place so
// the cache stays coherent without a global invalidation.
struct PatternAccess {
    Oid oid = 0;
    std::uint64_t pattern_id = 0;
    std::uint32_t fingerprint_version = 0;
    PageId waystone_root = kInvalidPageId;
    std::uint8_t stmt_class = 0;
    std::uint8_t dir_depth = 0;

    // Same rule as SysPatternRow's: depth is the authority, never the
    // root. Restated rather than shared because a caller holding a
    // PatternAccess has no row to pass to HasWaystoneDirectory().
    bool has_waystone_directory() const noexcept { return dir_depth >= 1; }
};

}  // namespace kds::catalog
