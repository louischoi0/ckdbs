#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/exec/leaf_cursor.hpp"
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
// `IndexBuild` is `PrepareIndexDef` and the root, then the backfill a slice
// at a time, then the logged tree and `Catalog::CreateIndex` (the owner-built
// split retired at AT-S5e, history at cf889b63; the slices are A1 stage 2's,
// `sched.md` §3).
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

// **`CREATE INDEX` and `REINDEX INDEX`'s build, in slices** (A1 stage 2,
// `sched.md` §3): a head that answers every refusal and allocates the root,
// a backfill its caller drives `leaves` relation leaves at a time - yielding
// its core in between - and a tail that logs the tree and writes the row.
//
// **What it holds between two `Step`s**: the relation's `TableAccess` (the
// caller pins the catalog memo), the root the tree has grown to, and the
// next leaf to read - no page and no span. Resuming at a leaf id is safe
// because the caller holds the relation `X` (AT-S5e) from before the head to
// its decide: no writer can split, grow or retire a leaf of the relation
// meanwhile, and no reader can reach the unpublished tree.
//
// The backfill indexes every version a live or future reader can reach
// (spec §10a): it follows a row's undo chain down to the first version
// `txns` resolves for every reader, never past it. `txns` null walks every
// chain to its end.
//
// Publishes nothing before `Finish`. A build that fails or is destroyed
// before `Finish` frees its unlogged tree (BX-S6); a failure after the tree
// is logged leaves it owed in its anchor slot, seeded if the catalog refused
// before writing it, and the next mount reclaims it (BY-R1, BY-R2).
class IndexBuild {
public:
    // `CREATE INDEX`'s head: `PrepareIndexDef`, then the root.
    static StatusOr<std::unique_ptr<IndexBuild>> ForCreate(
        catalog::Catalog& catalog, storage::PageStore& store, const parser::IndexStmt& stmt,
        catalog::Oid default_namespace, const txn::ReadView* view, std::uint64_t trx_id,
        const txn::TransactionManager* txns, txn::HolderProgress::Scope* progress);

    // **`REINDEX INDEX <name>`'s head** (BY-R6,
    // `instructions/v3.2.0/workorder-by-reindex-drop-index-reclaim.md`): the
    // index named by `stmt` dropped as `DROP INDEX` drops it (`dropped`), and
    // a definition for `CREATE INDEX`'s build from the row's own key and
    // covered column positions, widths and unique flag, the same name, under a
    // new `index_oid` (BY-Q2 (a)) - in the caller's transaction, under the `X`
    // it holds on `relation`, refused as `DropIndex` refuses off it. The
    // backfill writes an entry for every version a reader can still reach
    // (IX10a), so a rebuild drops the surplus entries (IX2) of rows that left
    // the relation and of versions no reader reaches. `Finish`'s
    // `replaced_oid` is the dropped index's.
    static StatusOr<std::unique_ptr<IndexBuild>> ForReindex(
        catalog::Catalog& catalog, storage::PageStore& store, const parser::IndexStmt& stmt,
        std::optional<catalog::Oid> relation, const txn::ReadView* view, std::uint64_t trx_id,
        catalog::CatalogRowChange* dropped, const txn::TransactionManager* txns,
        txn::HolderProgress::Scope* progress);

    ~IndexBuild();
    IndexBuild(const IndexBuild&) = delete;
    IndexBuild& operator=(const IndexBuild&) = delete;

    // Walks up to `leaves` more of the relation's leaves. A refusal ends
    // the build: the caller hands it to `Finish`.
    Status Step(std::uint32_t leaves);

    // Whether the walk has reached the end of the leaf chain.
    bool walked() const noexcept { return cursor_.walked(); }

    // The statement the build answers for: the names and the byte its
    // reply and refusals carry.
    const parser::IndexStmt& stmt() const noexcept { return stmt_; }

    // The tail. `ran` is how the walk ended - a `Step`'s refusal, the
    // caller's cancel, or OK once `walked()`. OK logs the whole tree as full
    // page images under the build's transaction (RV3) - **before** the
    // `sys.indexes` row that publishes it, so a committed row survives a
    // crash with a tree it can probe - and writes the row; anything else
    // frees the tree and is returned with the statement's context. The row
    // is stamped with the build's transaction and its address reported in
    // `written`, so a rollback can retire it (DT5). `wal` null = unlogged
    // (tests). Called once.
    StatusOr<IndexDdlResult> Finish(const Status& ran, catalog::CatalogRowRef* written,
                                    wal::WalManager* wal);

private:
    IndexBuild(catalog::Catalog& catalog, storage::PageStore& store, parser::IndexStmt stmt,
               catalog::Catalog::IndexDef def, std::uint64_t trx_id,
               const txn::TransactionManager* txns, txn::HolderProgress::Scope* progress);

    // The root, and the leaf the walk starts at.
    Status Begin();

    // A refusal from the build, as the statement answers it.
    Status Refusal(const Status& s) const;

    catalog::Catalog& catalog_;
    storage::PageStore& store_;
    const parser::IndexStmt stmt_;
    catalog::Catalog::IndexDef def_;
    const std::uint64_t trx_id_;
    const txn::TransactionManager* const txns_;
    txn::HolderProgress::Scope* const progress_;
    const catalog::TableAccess* access_ = nullptr;
    // Its `root_page_id` names the root the tree has grown to even when the
    // backfill fails, which is what a failed build frees from (BX-Q6 (b)).
    catalog::TableAccess::IndexRef building_;
    LeafCursor cursor_;
    catalog::Oid replaced_oid_ = 0;
    bool finished_ = false;
};

// What a successful CREATE INDEX has to say besides its row (the
// `IndexDdlResult::warnings` lines). Today one line: the key column
// already carries a Cabin. `key_column` is the user's spelling, for the
// message.
std::vector<std::string> IndexCreationWarnings(catalog::Catalog& catalog,
                                               const catalog::Catalog::IndexDef& def,
                                               std::string_view key_column);

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

}  // namespace kds::exec
