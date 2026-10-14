#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"

namespace kds::wal {
class WalManager;
}

// `CREATE CABIN` / `DROP CABIN`: the checks and the catalog writes behind
// them (docs/spec/cabin.md §10, docs/cabin-workplan.md CB03).
//
// ---- The error / warning line --------------------------------------------
//
// The same line `index_ddl.hpp` draws: an
// **error** is a declaration that could never do what it says - a Cabin on
// the tuple key, on a column the relation does not have, on a column that
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
    std::uint16_t col_pos = 0;  // the column shape's; 0 for the other two

    // The key function as an operator would write it: the column's name,
    // `DATE(ts)`, or the predicate.
    std::string key;

    // One line per check that passed but has something to say. Never a
    // reason the statement failed - a failure is a Status.
    std::vector<std::string> warnings;
};

// Resolves the statement's names, runs the checks, and writes the
// `sys.cabins` row.
//
// Fails with NotFound for an unknown relation or column, and with
// InvalidArgument for the tuple key column or a duplicate - the catalog owns those
// last two refusals (`Catalog::CreateCabin`) and this passes them through
// rather than restating them, so there is one answer to "why not" and not
// two that can drift.
//
// An expression or predicate Cabin (AQ-S1) is compiled first
// (`exec/cabin_expr.hpp`, whose refusals are its), and its text written to
// sys.cabin_exprs through `Catalog::CreateExprCabin`'s callback; `store` and
// `wal` are that relation's.
StatusOr<CabinDdlResult> CreateCabin(catalog::Catalog& catalog, storage::PageStore& store,
                                     wal::WalManager* wal, const parser::CabinStmt& stmt);

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
// An expression or predicate Cabin is found by its compiled `F`, and its
// sys.cabin_exprs row goes with it.
StatusOr<std::uint64_t> DropCabin(catalog::Catalog& catalog, storage::PageStore& store,
                                  wal::WalManager* wal, const parser::CabinStmt& stmt);

// The declaration loader's Cabin half (`Catalog::SetDeclarationLoader`):
// every `access.expr_cabins` entry's `F`, recompiled from its sys.cabin_exprs
// text against the relation's current schema. `Corruption` for a Cabin with
// no text, a text that no longer decodes, or one whose `expr_id` is not the
// row's - each was written only after it compiled.
Status LoadExprCabins(catalog::Catalog& catalog, storage::PageStore& store,
                      catalog::TableAccess& access);

// Whether an expression or predicate Cabin of `access`'s relation, **of any
// status**, reads column `col_pos` - `DROP COLUMN`'s restriction (BW-R8).
// Decided from the stored texts rather than `access.expr_cabins`, which holds
// serving Cabins only: a demoted one still names its inputs by position.
StatusOr<bool> AnyCabinKeyReads(catalog::Catalog& catalog, storage::PageStore& store,
                                const catalog::TableAccess& access, std::uint16_t col_pos);

// Removes every sys.cabin_exprs text naming `rel_oid` - after `DROP TABLE`
// has dropped its Cabins' rows, or a failed create left one no row names.
Status ForgetCabinTexts(catalog::Catalog& catalog, storage::PageStore& store,
                        wal::WalManager* wal, catalog::Oid rel_oid);

}  // namespace kds::exec
