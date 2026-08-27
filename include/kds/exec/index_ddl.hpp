#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"

// `CREATE INDEX` / `DROP INDEX`: the checks, the tree, and the catalog
// write behind them (docs/spec/index.md §10, workplan IX05).
//
// It sits here rather than in `catalog/` for one reason: computing an
// index's `key_width` needs the key encoding (`exec/index_key.hpp`) and
// formatting its root needs the page format (`storage/index/`), neither of
// which the catalog may know about. `Catalog::CreateIndex` takes the widths
// already computed and writes the row.
//
// ---- Three halves (workplan-peer-writer.md §7c, PW1c-6b-1) ---------------
//
// `CreateIndex` is `PrepareIndexDef`, then `BuildIndexTree`, then
// `Catalog::CreateIndex` - three because a peer-owned relation's index is
// built by the core that owns its pages: core 0 prepares the definition and
// publishes the row, the owner builds the tree from its own lease, and the
// definition crosses the ring between them as the plain `IndexDef`.
//
// ---- The error / warning line --------------------------------------------
//
// The same line `cabin_ddl.hpp` and `pattern_ddl.hpp` draw. An **error** is a
// declaration that could never do what it says - an index on a heap relation,
// on the primary key, on a column the relation has not got, on a type with no
// order. A **warning** is one that works and will disappoint.

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

// The definition half: the relation resolved under `view` (spec §5's rule -
// an index must not be built against a relation the caller cannot see),
// every column resolved and refused by name if it cannot be indexed, the
// widths computed by the encoders that will produce them, every catalog
// refusal answered (`CheckIndexDef`), and the index oid issued. Touches no
// page *of the relation* - only catalog pages, the oid bump among them - so
// it is safe on the core that owns the catalog and none of the relation's;
// `root_page_id` comes back `kInvalidPageId`, the build's to fill.
//
// The oid is issued here, before any page exists, so the root and every
// page a split creates carry their owner from birth (page.md §2a) - and
// after the checks, so a refused declaration burns none. One burned by a
// build that then fails is never reissued, which the row-id sequence
// permits by rule.
//
// Fails with NotFound for an unknown relation or column, Unsupported for a
// key column whose type has no index encoding, and whatever `CheckIndexDef`
// answers for the rest - passed through rather than restated, so there is
// one answer to "why not" and not two that can drift. `seed` goes to that
// check: `kByOwner` is how core 0 prepares a definition for a relation
// another core owns, whose anchor it must not seed.
StatusOr<catalog::Catalog::IndexDef> PrepareIndexDef(
    catalog::Catalog& catalog, const parser::IndexStmt& stmt, const txn::ReadView* view = nullptr,
    catalog::Catalog::AnchorSeed seed = catalog::Catalog::AnchorSeed::kHere);

// The page half: the root allocated from `store` and formatted, the tree
// backfilled over everything `access` already holds (spec §10a - every
// version, so a reader on an older snapshot finds its row through it), and
// the whole tree logged as full page images under `trx_id` (RV3), so a
// committed row survives a crash with a tree it can probe. Returns the root
// the build ended at, which a split may have moved off the page allocated
// first.
//
// Publishes nothing. The caller writes the `sys.indexes` row naming this
// root, or leaves an unreachable tree and no row: a failure here is the
// safe direction, where a row over a partial tree would be a wrong answer
// with a right answer's shape. Nothing can observe the half-built tree in
// between - DDL is one statement on one cooperative thread.
// `wal` null = unlogged, the pre-RV3 path every socket-free test runs.
StatusOr<PageId> BuildIndexTree(storage::PageStore& store, const catalog::TableAccess& access,
                                const catalog::Catalog::IndexDef& def, std::uint64_t trx_id,
                                wal::WalManager* wal);

// What a successful CREATE INDEX has to say besides its row (the
// `IndexDdlResult::warnings` lines), for both arms of the statement -
// the local one through `CreateIndex`, the owner-built one from the
// dispatcher's phase 2. Today one line: the key column already carries a
// Cabin. `key_column` is the user's spelling, for the message.
std::vector<std::string> IndexCreationWarnings(catalog::Catalog& catalog,
                                               const catalog::Catalog::IndexDef& def,
                                               std::string_view key_column);

// The three halves back to back, for a relation whose pages this core owns.
//
// `trx_id` / `written` make the DDL transactional
// (workplan-ddl-transactional.md DT5): the catalog row is stamped with
// the creating transaction and its address reported, so a rollback can
// retire it. Defaulted to the autocommit path. A failure after the tree is
// built leaks it, which is the bargain every allocation in this engine
// strikes while there is no free-page path.
StatusOr<IndexDdlResult> CreateIndex(catalog::Catalog& catalog, storage::PageStore& store,
                                     const parser::IndexStmt& stmt,
                                     std::uint64_t trx_id = catalog::kBootstrapXid,
                                     catalog::CatalogRowRef* written = nullptr,
                                     const txn::ReadView* view = nullptr,
                                     wal::WalManager* wal = nullptr);

// Removes the index named by `stmt` and returns its `index_oid`.
//
// **Frees no page.** Nothing frees a page in this engine, so a dropped index
// leaks its tree exactly as a dropped Cabin leaks its sets and a superseded
// var-heap value leaks its bytes.
// As above, but a drop **delete-marks** its `sys.indexes` row rather than
// retiring it when transactional, so a rollback clears the mark. Unlike
// `DROP TABLE` this is isolated too - there is no in-place retype here
// (ddl-transactional.md §5a).
StatusOr<catalog::Oid> DropIndex(catalog::Catalog& catalog, const parser::IndexStmt& stmt,
                                  std::uint64_t trx_id = catalog::kBootstrapXid,
                                  catalog::CatalogRowChange* change = nullptr);

}  // namespace kds::exec
