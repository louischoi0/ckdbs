#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/holder_progress.hpp"

namespace kds::txn {
class TransactionManager;
}

// `CREATE INDEX` / `DROP INDEX`: the checks, the tree, and the catalog
// write behind them (docs/spec/index.md §10, workplan IX05).
//
// It sits here rather than in `catalog/` for one reason: computing an
// index's `key_width` needs the key encoding (`exec/index_key.hpp`) and
// formatting its root needs the page format (`storage/index/`), neither of
// which the catalog may know about. `Catalog::CreateIndex` takes the widths
// already computed and writes the row.
//
// ---- Three halves ----------------------------------------------------------
//
// `CreateIndex` is `PrepareIndexDef`, then `BuildIndexTree`, then
// `Catalog::CreateIndex`, and is their one caller (the owner-built split
// that made them three retired at AT-S5e; history at cf889b63).
//
// ---- The error / warning line --------------------------------------------
//
// The same line `cabin_ddl.hpp` draws. An **error** is a
// declaration that could never do what it says - an index on the tuple
// key, on a column the relation has not got, on a type with no order. A **warning** is one that works and will disappoint.

namespace kds::exec {

// What a successful `CREATE INDEX` did.
struct IndexDdlResult {
    catalog::Oid index_oid = 0;
    catalog::Oid rel_oid = 0;
    PageId root_page_id = kInvalidPageId;
    std::uint16_t key_width = 0;
    std::uint16_t entry_width = 0;
    // `REINDEX` only: the dropped index's oid, whose tree the caller queues
    // at commit (BY-R2).
    catalog::Oid replaced_oid = 0;

    // One line per check that passed but has something to say. Never a
    // reason the statement failed - a failure is a Status.
    std::vector<std::string> warnings;
};

// The definition half: the relation resolved under `view` (spec §5's rule -
// an index must not be built against a relation the caller cannot see),
// every column resolved and refused by name if it cannot be indexed, the
// widths computed by the encoders that will produce them, every catalog
// refusal answered (`CheckIndexDef`), and the index oid issued. Touches no
// page *of the relation* - only catalog pages, the oid bump among them;
// `root_page_id` comes back `kInvalidPageId`, the
// build's to fill.
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
// one answer to "why not" and not two that can drift.
//
// The relation's name resolves as every statement's does: an unqualified one
// means `default_namespace` (`USE`, namespace.md NS5). `trx_id` is the DDL's
// own transaction: a name it delete-marked is free to it (BY-R8), which the
// view's own id - none, in autocommit - cannot say.
StatusOr<catalog::Catalog::IndexDef> PrepareIndexDef(catalog::Catalog& catalog,
                                                     const parser::IndexStmt& stmt,
                                                     catalog::Oid default_namespace,
                                                     const txn::ReadView* view,
                                                     std::uint64_t trx_id);

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
// between: no catalog row names it, and the caller holds the relation `X`
// (AT-S5e). `wal` null = unlogged (tests).
//
// The backfill indexes every version a live or future reader can reach: it
// follows a row's undo chain down to the first version `txns` resolves for
// every reader, never past it. `txns` null walks every chain to its end.
StatusOr<PageId> BuildIndexTree(storage::PageStore& store, const catalog::TableAccess& access,
                                const catalog::Catalog::IndexDef& def, std::uint64_t trx_id,
                                wal::WalManager* wal, const txn::TransactionManager* txns,
                                txn::HolderProgress::Scope* progress);

// What a successful CREATE INDEX has to say besides its row (the
// `IndexDdlResult::warnings` lines). Today one line: the key column
// already carries a Cabin. `key_column` is the user's spelling, for the
// message.
std::vector<std::string> IndexCreationWarnings(catalog::Catalog& catalog,
                                               const catalog::Catalog::IndexDef& def,
                                               std::string_view key_column);

// The three halves back to back.
//
// `trx_id` / `written` make the DDL transactional
// (workplan-ddl-transactional.md DT5): the catalog row is stamped with
// the creating transaction and its address reported, so a rollback can
// retire it. Defaulted to the autocommit path. A build that fails frees its
// unlogged tree (BX-S6); a failure after the tree is logged leaves it owed in
// its anchor slot, seeded if the catalog refused before writing it, and the
// next mount reclaims it (BY-R1, BY-R2).
StatusOr<IndexDdlResult> CreateIndex(catalog::Catalog& catalog, storage::PageStore& store,
                                     const parser::IndexStmt& stmt,
                                     catalog::Oid default_namespace, std::uint64_t trx_id,
                                     catalog::CatalogRowRef* written, const txn::ReadView* view,
                                     wal::WalManager* wal, const txn::TransactionManager* txns,
                                     txn::HolderProgress::Scope* progress);

// Removes the index named by `stmt` and returns its `index_oid`.
//
// **Frees no page at its own commit**: the anchor slot still names the tree,
// which the reclaimer frees once the commit is past the redo start and every
// older statement has ended (BY-R2), then removes the slot (BY-R5).
// As above, but a drop **delete-marks** its `sys.indexes` row rather than
// retiring it when transactional, so a rollback clears the mark. Unlike
// `DROP TABLE` this is isolated too - there is no in-place retype here
// (ddl-transactional.md §5a). The name is resolved under `view`, as
// `CreateIndex` resolves its relation: an index another transaction has not
// committed is not found.
//
// **Only an index on `relation`**, the relation the caller took the `X` on
// when it first resolved the name: no relation (the name did not resolve
// then) is `NotFound`, and an index the name now finds on another relation -
// dropped and created again by another transaction in between (BY-R8) - is
// `TxnConflict`, since writing it would be under no hold of this statement's.
StatusOr<catalog::Oid> DropIndex(catalog::Catalog& catalog, const parser::IndexStmt& stmt,
                                  std::optional<catalog::Oid> relation,
                                  const txn::ReadView* view, std::uint64_t trx_id,
                                  catalog::CatalogRowChange* change);

// **`REINDEX INDEX <name>`** (BY-R6,
// `instructions/v3.2.0/workorder-by-reindex-drop-index-reclaim.md`): the
// index named by `stmt` is dropped as `DROP INDEX` drops it, and built again
// through `CREATE INDEX`'s build - the row's own key and covered column
// positions, widths and unique flag, the same name, under a new `index_oid`
// (BY-Q2 (a)) - in one transaction, under the `X` the caller holds on
// `relation`, refused as `DropIndex` refuses off it. The backfill writes an
// entry for every version a reader can still reach (IX10a, `txns`), so a
// rebuild drops the surplus entries (IX2) of rows that left the relation and
// of versions no reader reaches. The result's
// `replaced_oid` is the dropped index's.
StatusOr<IndexDdlResult> ReindexIndex(catalog::Catalog& catalog, storage::PageStore& store,
                                      const parser::IndexStmt& stmt,
                                      std::optional<catalog::Oid> relation,
                                      const txn::ReadView* view, std::uint64_t trx_id,
                                      catalog::CatalogRowChange* dropped,
                                      catalog::CatalogRowRef* written, wal::WalManager* wal,
                                      const txn::TransactionManager* txns,
                                      txn::HolderProgress::Scope* progress);

}  // namespace kds::exec
