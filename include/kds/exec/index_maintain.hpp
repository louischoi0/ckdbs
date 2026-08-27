#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"

// The secondary-index write hook (docs/spec/index.md §2, workplan IX06):
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
// `cabin.md` §5's statement carried over intact: an older snapshot may
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
// any other statement failure does (`docs/spec/txn.md`: failure atomicity is per
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
// inserted after a value was observed (`docs/spec/types.md` §3.1).
//
// **Covered columns come from `row`**, the encoded tuple, sliced at the
// layout's offsets. They are stored as their inline cell bytes verbatim, so
// taking them from the payload rather than re-encoding makes them byte-
// identical to what is on the page by construction - spill pointer included,
// which is what lets a spilled covered value resolve from the base row
// exactly as it would have.

namespace kds::exec {

// One index mutation, reported back so the caller can log it.
//
// **Reported, not logged here.** `exec/` has no WAL, exactly as
// `storage/btree/btree.cpp` has none - "an insert reports every page it
// created or restructured and the caller emits the records" is the division
// this follows. The bytes are *copied* for `AppendedSpill`'s reason: the
// caller writes the record after the append returns, by which time the stack
// buffer the entry was encoded into is gone.
//
// **`restructured` empty is the common case and means "log an INDEX_INSERT".**
// Non-empty means the append split something, and then the caller logs a
// full page image of each named page and **no** INDEX_INSERT - the images are
// taken after the entry is in, so emitting both would apply it twice
// (wal/record.hpp).
//
// Collected only when there is a log to write to: a null sink costs the write
// path nothing, which is what keeps the unlogged path the code it always was.
struct IndexWrite {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    std::vector<std::byte> entry;
    std::vector<PageId> restructured;
};

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
                       std::span<const parser::AstValue> previous = {},
                       std::vector<IndexWrite>* logged = nullptr);

// The same append, for **one** index and with no catalog.
//
// It exists because the backfill (IX09) builds a tree that has no
// `sys.indexes` row yet, so it cannot go through `MaintainIndexes` - and
// writing the append twice is how the backfill and the write path come to
// disagree about what an entry is. `MaintainIndexes` is a loop over this.
//
// Returns the index's **new root** when a split grew the tree, and
// `kInvalidPageId` otherwise - which also covers "the write touched nothing
// this index cares about", since both mean there is nothing to republish.
// Reporting rather than republishing is what lets the backfill hold its root
// in a local while the index is still unpublished.
StatusOr<PageId> AppendIndexEntry(storage::PageStore& store,
                                  const catalog::TableAccess& access,
                                  const catalog::TableAccess::IndexRef& ix,
                                  std::span<const parser::AstValue> values,
                                  std::uint16_t first_col_pos, std::span<const std::byte> row,
                                  std::uint64_t pk,
                                  std::span<const parser::AstValue> previous = {},
                                  std::vector<IndexWrite>* logged = nullptr);

}  // namespace kds::exec
