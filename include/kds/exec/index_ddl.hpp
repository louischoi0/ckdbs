#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"

// `CREATE INDEX` / `DROP INDEX`: the checks, the root page, and the catalog
// write behind them (docs/feat-index.md §10, workplan IX05).
//
// It sits here rather than in `catalog/` for one reason: computing an
// index's `key_width` needs the key encoding (`exec/index_key.hpp`) and
// formatting its root needs the page format (`storage/index/`), neither of
// which the catalog may know about. `Catalog::CreateIndex` takes the widths
// already computed and writes the row.
//
// ---- The error / warning line --------------------------------------------
//
// The same line `cabin_ddl.hpp` and `pattern_ddl.hpp` draw. An **error** is a
// declaration that could never do what it says - an index on a heap relation,
// on the primary key, on a column the relation has not got, on a type with no
// order. A **warning** is one that works and will disappoint.
//
// ---- What CREATE INDEX does *not* do yet ---------------------------------
//
// **It refuses a relation that has ever held a row.** Not a limitation to
// route around: IX06 (the write hook) and IX09 (the backfill) are not built,
// so an index over existing rows would be empty and complete-looking - and
// once the read path lands it would answer "no rows" authoritatively for
// every value. The refusal is what keeps that impossible until the backfill
// exists, and IX09 is what lifts it.
//
// The test is "has this relation ever issued a Keystone id", not "does it
// have live rows", and the conservative direction is the correct one: a
// relation whose rows were all deleted still has versions reachable through
// the undo chain, which is exactly what IX09's backfill has to walk.

namespace kds::exec {

// What a successful `CREATE INDEX` did.
struct IndexDdlResult {
    catalog::Oid index_oid = 0;
    catalog::Oid rel_oid = 0;
    PageId root_page_id = kInvalidPageId;
    std::uint16_t key_width = 0;
    std::uint16_t entry_width = 0;

    // One line per check that passed but has something to say. Never a
    // reason the statement failed - a failure is a Status.
    std::vector<std::string> warnings;
};

// Resolves the statement's names, computes the index's widths, allocates and
// formats its root page, and writes the `sys.indexes` row.
//
// The page is allocated **before** the row, so the row can never name a page
// that does not exist. A catalog write that then fails leaks an unreachable
// page, which is the bargain every allocation in this engine strikes while
// there is no free-page path.
//
// Fails with NotFound for an unknown relation or column, Unsupported for a
// key column whose type has no index encoding, and whatever
// `Catalog::CreateIndex` answers for the rest - passed through rather than
// restated, so there is one answer to "why not" and not two that can drift.
// `trx_id` / `written` make the DDL transactional
// (workplan-ddl-transactional.md DT5): the catalog row is stamped with
// the creating transaction and its address reported, so a rollback can
// retire it. Defaulted to the autocommit path.
StatusOr<IndexDdlResult> CreateIndex(catalog::Catalog& catalog, storage::PageStore& store,
                                     const parser::IndexStmt& stmt,
                                     std::uint64_t trx_id = catalog::kBootstrapXid,
                                     catalog::CatalogRowRef* written = nullptr,
                                     const txn::ReadView* view = nullptr);

// Removes the index named by `stmt` and returns its `index_oid`.
//
// **Frees no page.** Nothing frees a page in this engine, so a dropped index
// leaks its tree exactly as a dropped Cabin leaks its sets and a superseded
// var-heap value leaks its bytes.
// As above, but a drop **delete-marks** its `sys.indexes` row rather than
// retiring it when transactional, so a rollback clears the mark. Unlike
// `DROP TABLE` this is isolated too - there is no in-place retype here
// (spec-ddl-transactional.md §5a).
StatusOr<catalog::Oid> DropIndex(catalog::Catalog& catalog, const parser::IndexStmt& stmt,
                                  std::uint64_t trx_id = catalog::kBootstrapXid,
                                  catalog::CatalogRowChange* change = nullptr);

}  // namespace kds::exec
