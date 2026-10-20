#pragma once

#include <string_view>

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
// ---- The split is by what each door has in hand ------------------------
//
// `CheckForeignKeyDeclaration`'s questions are answerable from the parent
// relation and the child *column* alone, which is what CREATE TABLE has
// before the child relation exists. A self-reference needs the child's
// identity, which CREATE TABLE has only as the name it is creating and the
// door as an oid, so each door detects it its own way and both refuse it
// through `RefuseSelfReference`, in one wording.

namespace kds::catalog {

// Everything decidable from the parent and the child column alone.
//
// Fails with:
//   InvalidArgument  `child_column_pos == 0` - the Keystone tuple key is the
//                    child's identity, not a field of it (invariant 11), so
//                    a reference stored *in* it would make one row's
//                    identity a statement about another row; or a column
//                    whose declared type cannot hold a Keystone id,
//                    which is a reference that can never be written.
//   Unsupported      a **heap** parent (BZ-R7 of
//                    `instructions/v3.2.0/workorder-bz-heap-hidden-key.md`):
//                    F1 puts the reference on the parent's tuple key, which
//                    a heap hides, so no child value could name a parent row.
//   InvalidArgument  a system relation, whose catalog chain has no tuple
//                    key descent.
Status CheckForeignKeyDeclaration(const TableAccess& parent, const SysColumnRow& child_column,
                                  std::uint16_t child_column_pos);

// A foreign key on the relation it references, `NotImplemented`
// (`docs/spec/foreign-keys.md` §1): nothing on the write path checks one.
// The forward check resolves every parent before any row is written, so a
// parent the same statement inserts would read absent, and a multi-row
// DELETE's reverse check would admit or refuse by the order its walk meets
// a parent and its child.
Status RefuseSelfReference(std::string_view child_column);


}  // namespace kds::catalog
