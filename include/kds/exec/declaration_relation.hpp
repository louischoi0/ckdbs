#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::wal {
class WalManager;
}

// The two row-codec **declaration relations**, `sys.assertions` and
// `sys.transitions` (`Catalog::DeclarationSchema`): one shape, one reader.
// Each row is a constraint's declaration verbatim plus four fixed fields;
// the parsed form is recovered by re-parsing `source_text`
// (`assertion.md` AS10's model), so nothing here knows what a declaration
// says. `assertion_catalog.hpp` and `transition_catalog.hpp` are the typed
// faces.
//
// ---- Why this is not in catalog/ ----------------------------------------
//
// These relations are stored in ordinary user tuple format - a Keystone
// word, tagged cells, a var-heap for values too long to inline - because a
// declaration's text is neither fixed-width nor small. Reading such a row
// needs `exec::DecodeRowInto`, and `exec/` depends on `catalog/`.
//
// ---- Two rules every function below honours ------------------------------
//
// **Decode before descending** (docs/spec/parser-v2.md I15 rule R1). A
// var-heap fetch must never happen while a heap-page span is live, so a
// scan decodes each row inside the walk and resolves the spilled cells
// *after* the walk has released every span. That is why no reader can stop
// early on a match: the name it would match on may still be an unresolved
// pointer while the walk runs.
//
// **Deletion is physical.** `DeleteDeclaration` retires the slot rather
// than delete-marking it, because catalog reads have no snapshot to filter
// a mark against - a marked row would still be found by name, and DROP
// followed by CREATE of the same name would collide with a row nobody can
// see.
//
// Concurrency: the readers take no latch of their own; `InsertDeclaration`
// checks the name and inserts under one exclusive hold of the relation's
// root page (AT-S17), the order root, then var-heap, never the reverse.
// Writes are logged at `kBootstrapXid` / `kNoTxnId`: a declaration commits
// at its statement, never with a session's transaction.

namespace kds::exec {

// One row, fully resolved unless the scan was asked not to resolve.
struct DeclarationRow {
    // The relation's own Keystone id, which is the declaration's id.
    std::uint64_t id = 0;
    // The relation the declaration constrains.
    catalog::Oid target_oid = 0;
    // The relation's own fixed column: sys.assertions' `cabin_root`,
    // sys.transitions' `column_no`.
    std::int64_t aux = 0;
    // Reserved; every writer passes 0.
    std::uint32_t flags = 0;
    std::string name;
    std::string source_text;
};

// Every row of `rel_oid`, in chain order. `resolve_spills = false` leaves
// `name` and `source_text` empty and **touches no var-heap page** - the
// fixed columns are inline in the heap tuple.
StatusOr<std::vector<DeclarationRow>> ScanDeclarations(catalog::Catalog& catalog,
                                                       storage::PageStore& store,
                                                       catalog::Oid rel_oid,
                                                       bool resolve_spills = true);

// The row named `name`, case-insensitively - an object name, like every
// other in this engine - or nullopt.
StatusOr<std::optional<DeclarationRow>> FindDeclarationByName(catalog::Catalog& catalog,
                                                              storage::PageStore& store,
                                                              catalog::Oid rel_oid,
                                                              std::string_view name);

// The rows constraining `target_oid`, in chain order.
StatusOr<std::vector<DeclarationRow>> DeclarationsOnRelation(catalog::Catalog& catalog,
                                                             storage::PageStore& store,
                                                             catalog::Oid rel_oid,
                                                             catalog::Oid target_oid);

// Records `row` (its `id` from the caller's `Catalog::AllocateRowId`).
// `kind` names the declaration in refusals ("assertion", "transition").
// `AlreadyExists` when the name is taken - checked under the root page's
// exclusive hold, so no other core publishes the name in between;
// `NotImplemented` when `source_text` is longer than one var-heap value.
Status InsertDeclaration(catalog::Catalog& catalog, storage::PageStore& store,
                         wal::WalManager* wal, catalog::Oid rel_oid, std::string_view kind,
                         const DeclarationRow& row);

// Retires the row named `name` (`SLOT_RETIRE`, no transaction) and returns
// it. `NotFound` if there is none. The var-heap bytes its text occupied are
// not released; the next mount's sweep collects them (`varheap_sweep.hpp`).
StatusOr<DeclarationRow> DeleteDeclaration(catalog::Catalog& catalog, storage::PageStore& store,
                                           wal::WalManager* wal, catalog::Oid rel_oid,
                                           std::string_view kind, std::string_view name);

}  // namespace kds::exec
