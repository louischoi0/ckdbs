#pragma once

#include "kds/base/status.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/catalog/schema.hpp"

// What a foreign key declaration has to satisfy before it may be recorded
// (docs/spec/foreign-keys.md §1, milestone FK-M1).
//
// ---- Why these are free functions and not Catalog methods ---------------
//
// Two doors ask the same questions at different moments, and both have to
// get the same answers:
//
//   - `CREATE TABLE ... REFERENCES parent` checks **before** the relation
//     is created, so a refusable declaration fails with nothing written.
//     There is no DROP TABLE to undo a half-made one, and a constraint is
//     not an accelerator - unlike a Cabin, which CREATE TABLE downgrades to
//     a warning when it cannot be built, a foreign key that silently did
//     not happen is a table that says REFERENCES and enforces nothing.
//   - `Catalog::CreateForeignKey()` checks **again**, because it is the one
//     door every foreign key comes through and a check living only in the
//     DDL layer is a check the next caller forgets. Same argument
//     `Catalog::CreateCabin()` makes about `NO CABIN`.
//
// Sharing them here is what keeps the pre-check and the door from drifting
// into two different definitions of a legal foreign key.
//
// ---- The split is by what each half needs to exist ----------------------
//
// The first function's questions are answerable from the parent relation
// and the child *column* alone, which is exactly what CREATE TABLE has in
// hand before the child relation exists. The second needs both relations,
// so it can only run once the child does.

namespace kds::catalog {

// Everything decidable from the parent and the child column alone.
//
// Fails with:
//   InvalidArgument  `child_column_pos == 0` - the Keystone pk is the
//                    child's identity, not a field of it (invariant 11), so
//                    a reference stored *in* it would make one row's
//                    identity a statement about another row; or a column
//                    whose declared type cannot hold a 40-bit Keystone id,
//                    which is a reference that can never be written.
//   Unsupported      a **heap** parent. F1 puts the reference on the
//                    parent's pk, and a heap relation has no pk index: a
//                    point lookup on one is a chain scan
//                    (CommandDispatcher::LocateByPk returns kScan), so
//                    every child INSERT would scan the parent - the whole
//                    relation when the parent is missing, which is the case
//                    the check exists to catch. Refusing keeps the
//                    constraint's cost a descent; relaxing it later is
//                    additive and needs no format change.
Status CheckForeignKeyDeclaration(const TableAccess& parent, const SysColumnRow& child_column,
                                  std::uint16_t child_column_pos);

// The colocation half (F5): parent and child must be owned by the same
// core. Fails with Unsupported naming both cores.
//
// v1 rejects a cross-core foreign key outright rather than growing a slow
// path for it, because both checks are reads injected into a writing
// statement and a write already binds to one home core (crosscore.md CC3).
// The FK graph becomes an input to placement policy - a decision for
// `AssignOwnerCore()`, which is why nothing here tries to *place* anything.
Status CheckForeignKeyColocation(const TableAccess& parent, const TableAccess& child);

}  // namespace kds::catalog
