#pragma once

#include <cstdint>
#include <span>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/txn/lock_table.hpp"
#include "kds/txn/read_view.hpp"

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
// value the engine keys on (`docs/spec/types.md` §3.1).
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
// Non-empty means the append split something, and then the caller logs one
// BTREE_SPLIT of the named pages and **no** INDEX_INSERT - the images are
// taken after the entry is in, so emitting both would apply it twice
// (wal/record.hpp). A root the split grew is the log's to publish
// (`new_root`): a sink that will not carry it in the split's record
// publishes it itself (`Catalog::PublishRoot`).
//
// Collected only when there is a log to write to: a null sink costs the write
// path nothing, which is what keeps the unlogged path the code it always was.
struct IndexWrite {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    std::vector<std::byte> entry;
    std::vector<PageId> restructured;
    // Set when the split grew the index a level: the log publishes the root
    // in the split's own record (`Catalog::StageAnchorRoot`), so the anchor
    // slot and the pages under it are replayed together or not at all.
    PageId new_root = kInvalidPageId;
    catalog::Oid rel_oid = 0;
    catalog::Oid index_oid = 0;
    PageId anchor_page_id = kInvalidPageId;
    // The divide's pin on that anchor (`IndexInsertResult::anchor`), alive
    // for the log call this write is handed to; a copy kept past it is null.
    storage::UnlatchedPin* anchor_pin = nullptr;
};

// What maintenance hands each write to, **while the tree still holds the
// pages it names** (AT-S21): the leaf, and on a divide the parents it
// wrote. A caller with a log appends and stamps here, from bytes read under
// that hold; the hold drops when the call returns. Logged after release,
// another core's insert could shift the slot and the record carry another
// row's entry (the AT-S21 ground).
using IndexWriteLog = std::function<Status(const IndexWrite&)>;

// Appends this row's entries to every index on `access` that the write
// touched.
//
// `values` is positionally aligned with the schema from `first_col_pos` - 1
// for an INSERT, whose VALUES list supplies the columns after the tuple key, and 0
// for an UPDATE, which carries the whole decoded row. The same convention the
// Cabin hook uses, so the two call sites read alike.
//
// `previous` is the row before the write, empty on an INSERT. When it is
// non-empty this is an UPDATE and the touched-column rule above applies.
//
// `row` is the encoded tuple exactly as it was written to the page.
//
// **On return, `access` may be dangling - and is not.** A split republishes
// the index's root through `Catalog::AdoptRoot`, which updates the
// cached entry **in place** rather than invalidating it, precisely so a
// caller holding the pointer across this call keeps a valid one and sees the
// new root. That is what makes calling this from inside a per-row lambda
// safe.
Status MaintainIndexes(catalog::Catalog& catalog, storage::PageStore& store,
                       const catalog::TableAccess& access,
                       std::span<const parser::AstValue> values, std::uint16_t first_col_pos,
                       std::span<const std::byte> row, std::uint64_t tuple_key,
                       std::span<const parser::AstValue> previous = {},
                       const IndexWriteLog* log = nullptr);

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
// in a local while the index is still unpublished. With `publish` and no
// `log`, a new root is republished here instead, through the catalog and
// while the divide still pins the anchor (`UnlatchedPin`), and none is
// reported; the backfill passes neither, and its growth pins nothing.
StatusOr<PageId> AppendIndexEntry(storage::PageStore& store,
                                  const catalog::TableAccess& access,
                                  const catalog::TableAccess::IndexRef& ix,
                                  std::span<const parser::AstValue> values,
                                  std::uint16_t first_col_pos, std::span<const std::byte> row,
                                  std::uint64_t tuple_key,
                                  std::span<const parser::AstValue> previous = {},
                                  const IndexWriteLog* log = nullptr,
                                  catalog::Catalog* publish = nullptr);

// ---- Unique indexes (BX, `instructions/v3.2.0/workorder-bx-unique-index.md`)
//
// A unique index refuses a write that would give two live rows one key
// value. Its entries are still a superset (IX1, IX2): a key's run names rows
// that moved off the key, were deleted, rolled back or purged, and with a
// string longer than `kIndexStringKeyBytes` rows sharing only the prefix.
// So the check walks the run, resolves every tuple key it names and
// compares **full values** at latest state (BX-R1, BX-R2), never the encoded
// key alone.
//
// **Called holding no page.** The check descends the clustered tree, and two
// user relation pages are unordered (`page.md` §6): a writer holding its own
// leaf while it descends to another could deadlock against a writer doing
// the reverse. An INSERT checks before its placement; an UPDATE stops its
// walk and checks with its span released (BX-R4).

// One key of one unique index, as a write would carry it: the encoded key
// the index orders by, and the values it was encoded from, coerced to their
// columns' storage forms, in declared index order.
struct UniqueKey {
    std::vector<std::byte> encoded;
    std::vector<parser::AstValue> values;
};

// `values`/`first_col_pos`/`previous` as `AppendIndexEntry` takes them.
// Empty when the write moved no key column of `ix` - never for an INSERT's
// empty `previous`: a covered column moving appends an entry but is not
// checked (BX-R5), so the gate is the key, not the append.
StatusOr<std::optional<UniqueKey>> EncodeUniqueKey(const catalog::TableAccess& access,
                                                   const catalog::TableAccess::IndexRef& ix,
                                                   std::span<const parser::AstValue> values,
                                                   std::uint16_t first_col_pos,
                                                   std::span<const parser::AstValue> previous = {});

// The lock unit a writer of `key` takes `X` from its check to its append
// (BX-R3, BX-Q2 (a)): the relation's oid, and a hash of the index's oid and
// the encoded key.
txn::LockKey UniqueKeyUnit(const catalog::TableAccess& access,
                           const catalog::TableAccess::IndexRef& ix, const UniqueKey& key);

struct UniqueVerdict {
    enum class Kind : std::uint8_t {
        kFree,  // no live row holds the key
        kHeld,  // `tuple_key`'s latest version is live and carries the key
        kBusy,  // `tuple_key` is being written by `writer`, undecided
    };
    Kind kind = Kind::kFree;
    std::uint64_t tuple_key = 0;
    std::uint64_t writer = 0;
};

// The latest-state check (BX-R1) under `check_view`, minted at check time
// with the writer's own id (`TransactionManager::MintCheckView`), so the
// writer's own pending rows count. `self` is the row being written - an
// UPDATE's own row, which holds its old key - and is skipped.
//
// **Busy is conservative**: any row the run names whose latest writer is in
// flight answers busy, whether or not its pending version carries the key -
// the committed version under it may, and latest state does not walk undo.
// A wait it did not need is the cost; a duplicate admitted is impossible.
StatusOr<UniqueVerdict> CheckUniqueKey(storage::PageStore& store,
                                       const catalog::TableAccess& access,
                                       const catalog::TableAccess::IndexRef& ix,
                                       const UniqueKey& key, const txn::ReadView& check_view,
                                       std::uint64_t self);

// "unique index '<name>' on (<columns>): (<values>)" - what a refusal names.
std::string UniqueKeyText(const catalog::TableAccess& access,
                          const catalog::TableAccess::IndexRef& ix, const UniqueKey& key);

// BX-R6's refusal: `AlreadyExists`, naming the index, its columns and values,
// the tuple key that holds them and `byte`, the written value's position.
Status UniqueViolation(const catalog::TableAccess& access,
                       const catalog::TableAccess::IndexRef& ix, const UniqueKey& key,
                       std::uint64_t holder, std::uint32_t byte);

}  // namespace kds::exec
