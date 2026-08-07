#pragma once

#include <cstdint>
#include <span>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"

// The secondary-index write hook (docs/feat-index.md §2, workplan IX06):
// one implementation, called from the same three doors `fk_check.hpp` uses.
//
// ---- Every maintenance action is an append (IX2) -------------------------
//
//   INSERT  one entry per index on the relation.
//   UPDATE  an entry for the **new** key, if the write touched a key or
//           covered column; the old entry is left alone.
//   DELETE  nothing at all.
//
// Removal is **incorrect** rather than merely unnecessary, and that is
// `feat-cabin.md` §5's statement carried over intact: an older snapshot may
// still match through the undo chain, so an entry naming a row whose current
// version no longer carries that key is exactly what a pre-update reader
// needs. The surplus is subtracted by the read path - MVCC visibility plus a
// re-check of the key predicate against the resolved version - which is why
// leaving it costs nothing but space.
//
// ---- Where an index is not a Cabin ---------------------------------------
//
// **A failed append fails the statement.** Un-observing is always legal for a
// Cabin (§1's corollary), so its hook can absorb any failure; an index has no
// such move, because an index missing an entry is not slower, it is *wrong*.
// Inside an explicit transaction the failure poisons the session exactly as
// any other statement failure does (`docs/txn.md`: failure atomicity is per
// transaction, not per statement).
//
// ---- The rule that decides whether the feature is usable -----------------
//
// An UPDATE that touches **no** key and no covered column of an index must
// not append to it. Appending anyway stays *correct* by IX1's superset rule
// and is unbounded: a workload updating a row's other columns would grow the
// index by an entry per write forever. Correct and useless is still a defect.
//
// ---- Two ways a value reaches this file, and why both --------------------
//
// **Key columns come from `values`**, coerced through
// `exec::CoerceLiteralToColumn` - the one path from a written literal to a
// value the engine keys on. A second coercion is how the Cabin came to key
// its writes on one form and its reads on another, silently losing every row
// inserted after a value was observed (`docs/spec-types.md` §3.1).
//
// **Covered columns come from `row`**, the encoded tuple, sliced at the
// layout's offsets. They are stored as their inline cell bytes verbatim, so
// taking them from the payload rather than re-encoding makes them byte-
// identical to what is on the page by construction - spill pointer included,
// which is what lets a spilled covered value resolve from the base row
// exactly as it would have.

namespace kds::exec {

// Appends this row's entries to every index on `access` that the write
// touched.
//
// `values` is positionally aligned with the schema from `first_col_pos` - 1
// for an INSERT, whose VALUES list supplies the columns after the pk, and 0
// for an UPDATE, which carries the whole decoded row. The same convention the
// Cabin hook uses, so the two call sites read alike.
//
// `previous` is the row before the write, empty on an INSERT. When it is
// non-empty this is an UPDATE and the touched-column rule above applies.
//
// `row` is the encoded tuple exactly as it was written to the page.
//
// **On return, `access` may be dangling - and is not.** A split republishes
// the index's root through `Catalog::UpdateIndexRoot`, which updates the
// cached entry **in place** rather than invalidating it, precisely so a
// caller holding the pointer across this call keeps a valid one and sees the
// new root. That is what makes calling this from inside a per-row lambda
// safe.
Status MaintainIndexes(catalog::Catalog& catalog, storage::PageStore& store,
                       const catalog::TableAccess& access,
                       std::span<const parser::AstValue> values, std::uint16_t first_col_pos,
                       std::span<const std::byte> row, std::uint64_t pk,
                       std::span<const parser::AstValue> previous = {});

}  // namespace kds::exec
