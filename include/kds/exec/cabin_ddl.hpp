#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"

// `CREATE CABIN` / `DROP CABIN`: the checks and the catalog writes behind
// them (docs/spec/cabin.md §10, docs/cabin-workplan.md CB03).
//
// ---- The error / warning line --------------------------------------------
//
// The same line `pattern_ddl.hpp` draws, and for the same reason: an
// **error** is a declaration that could never do what it says - a Cabin on
// the primary key, on a column the relation does not have, on a column that
// already has one. A **warning** is a declaration that works and will
// disappoint - a Cabin nothing will ever probe, or one on a relation with no
// rows to observe. Refusing the second would be the engine overruling an
// operator about their own workload; staying silent about it would waste the
// one moment there is anything to say.
//
// ---- What creating a Cabin does *not* do ---------------------------------
//
// It observes nothing. A fresh Cabin is an empty directory, and the read
// path's miss branch is what fills it, value by value, out of scans that
// were going to happen anyway (spec §4). So `CREATE CABIN` cannot make a
// query faster on its own, and cannot make one wrong either - which is the
// property that lets it be ordinary DDL rather than a build.

namespace kds::exec {

// What a successful `CREATE CABIN` did.
struct CabinDdlResult {
    std::uint64_t cabin_id = 0;
    catalog::Oid rel_oid = 0;
    std::uint16_t col_pos = 0;

    // One line per check that passed but has something to say. Never a
    // reason the statement failed - a failure is a Status.
    std::vector<std::string> warnings;
};

// Resolves the statement's names, runs the checks, and writes the
// `sys.cabins` row.
//
// Fails with NotFound for an unknown relation or column, and with
// InvalidArgument for the pk column or a duplicate - the catalog owns those
// last two refusals (`Catalog::CreateCabin`) and this passes them through
// rather than restating them, so there is one answer to "why not" and not
// two that can drift.
StatusOr<CabinDdlResult> CreateCabin(catalog::Catalog& catalog, const parser::CabinStmt& stmt);

// Removes the Cabin on `(table, column)` and returns its `cabin_id`, so the
// caller can drop the observed sets that the catalog cannot see.
//
// **Dropping the sets is the caller's job and may be done late.** A set
// nothing consults is inert: the compiler stops emitting cabin probes for
// the column the moment the catalog row is gone, so a stale set costs memory
// until someone frees it and can never be reached by a reader.
//
// Fails with NotFound when the relation, the column, or the Cabin does not
// exist.
StatusOr<std::uint64_t> DropCabin(catalog::Catalog& catalog, const parser::CabinStmt& stmt);

}  // namespace kds::exec
