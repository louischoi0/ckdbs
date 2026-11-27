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
#include "kds/storage/index/index_page.hpp"
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
    // The leaf and the parents the divide secured, still held
    // (`IndexInsertResult::held`), alive for the same call: a split's images
    // are read through them rather than through a second pin. A copy kept
    // past it is empty.
    std::span<const storage::PageRef> held;
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
//
// `rowid` is the row's `heap::EncodeRowid` word on a heap (DN, IX1) - every
// entry of a heap's index carries it - and ignored on a btree relation.
//
// `appended`, when given, is set where the write put a new entry into some
// index, and left alone where it put none - it moved nothing an index
// carries, or the entry it would append was already there (an UPDATE back
// to a value the row held, whose entry no purge has taken). A row is owed
// one entry an index, so for an UPDATE "an entry was appended" is exactly
// "the tree holds one dead entry more", which is what the unasked purge
// counts (DW): the append that finds its entry there kills the row's
// current one and brings the earlier one back, and the dead are as many as
// before.
Status MaintainIndexes(catalog::Catalog& catalog, storage::PageStore& store,
                       const catalog::TableAccess& access,
                       std::span<const parser::AstValue> values, std::uint16_t first_col_pos,
                       std::span<const std::byte> row, std::uint64_t tuple_key,
                       std::span<const parser::AstValue> previous = {},
                       const IndexWriteLog* log = nullptr, std::uint64_t rowid = 0,
                       bool* appended = nullptr);

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
//
// **A refusal from a root another core grew past is retried from the slot**
// (CI-Q11 (b), `instructions/v3.2.0/workorder-ci-online-index-build.md`):
// with `log` or `publish`, an insert refused `TxnConflict` - which wrote
// nothing - reads the index's anchor slot, and when it names another root
// asks again from there, a few times at most. A moved slot proves a level
// grew, and the descent from the slot's root is the current tree's.
StatusOr<PageId> AppendIndexEntry(storage::PageStore& store,
                                  const catalog::TableAccess& access,
                                  const catalog::TableAccess::IndexRef& ix,
                                  std::span<const parser::AstValue> values,
                                  std::uint16_t first_col_pos, std::span<const std::byte> row,
                                  std::uint64_t tuple_key,
                                  std::span<const parser::AstValue> previous = {},
                                  const IndexWriteLog* log = nullptr,
                                  catalog::Catalog* publish = nullptr,
                                  std::uint64_t rowid = 0, bool* appended = nullptr);

// `ix`'s layout: the widths its pages are checked against.
inline index::IndexLayout IndexLayoutOf(const catalog::TableAccess::IndexRef& ix) {
    index::IndexLayout layout;
    layout.key_width = ix.key_width;
    layout.rowid_width = ix.rowid ? static_cast<std::uint16_t>(index::kIndexRowidWidth) : 0;
    layout.covered_width = static_cast<std::uint16_t>(ix.entry_width - ix.key_width -
                                                      index::kIndexTupleKeyWidth -
                                                      layout.rowid_width);
    return layout;
}

// The whole entry `key || tuple key || covered` that `AppendIndexEntry`
// composes for one version - `values` its decoded columns in schema order,
// `row` its encoded tuple - without touching the tree: what an index entry
// purge's judge compares the tree's entries with (DD-R1).
StatusOr<std::vector<std::byte>> EncodeIndexEntry(const catalog::TableAccess& access,
                                                  const catalog::TableAccess::IndexRef& ix,
                                                  std::span<const parser::AstValue> values,
                                                  std::span<const std::byte> row,
                                                  std::uint64_t tuple_key,
                                                  std::uint64_t rowid = 0);

// Whether the tree's entry `stored` serves the version that is owed `owed`
// (DD-R1 as DD-S0 found it): the same key, tuple key and covered bytes, but
// for a spilled `varchar` covered cell, where two spilled cells of one
// length are one value - the write path judges a move by value and appends
// nothing for a value assigned again, whose cell then names a new spill.
// Conservative: it may keep an entry no version needs, never the reverse.
bool SameOwedEntry(const catalog::TableAccess& access, const catalog::TableAccess::IndexRef& ix,
                   std::span<const std::byte> stored, std::span<const std::byte> owed);

// The root `index_oid`'s slot in the relation's anchor page `anchor` names,
// read under a shared hold of that page alone. A live index with no slot is
// `Corruption`, as `Catalog::InitTableAccess` refuses it (BY-R10).
StatusOr<PageId> SlotRoot(storage::PageStore& store, PageId anchor, std::uint64_t index_oid);

// The tuple keys `ix`'s run for `encoded_key` names, ascending and distinct:
// every entry whose first `encoded_key.size()` bytes are the key's. A
// superset of the rows holding the key, as an index's entries always are
// (IX1, IX2) - a row that moved off the key, was deleted or rolled back,
// and with a string key a row sharing only its indexed prefix (IX6) - so a
// caller resolves each and tests the row it finds. `entries_read`, where
// given, is how many entries the run holds, before the tuple keys are made
// distinct. **Called holding no page**, and it returns holding none.
StatusOr<std::vector<std::uint64_t>> IndexRunTupleKeys(storage::PageStore& store,
                                                       const catalog::TableAccess::IndexRef& ix,
                                                       std::span<const std::byte> encoded_key,
                                                       std::uint64_t* entries_read = nullptr);

// **The tuple keys of `ix`'s entries between two bounds** (DU): every entry
// at or above `low` and at or below `high`, each a whole entry of the
// index's sort key width as an `IndexProbe`'s bounds are, **appended to
// `named` in entry order, neither sorted nor made distinct** - a caller
// that reads several ranges into one vector (a key set's, one a value)
// sorts and dedupes once. A superset of the rows whose keys lie between
// the bounds, as `IndexRunTupleKeys`' run is of its key's. **The read stops
// once `named` holds more than `limit` keys**, so a caller that will not
// use a longer collection pays for no more of it, and tells by `named`'s
// size. **Called holding no page**, and it returns holding none.
Status AppendIndexRangeTupleKeys(storage::PageStore& store,
                                 const catalog::TableAccess::IndexRef& ix,
                                 std::span<const std::byte> low, std::span<const std::byte> high,
                                 std::vector<std::uint64_t>& named,
                                 std::size_t limit = static_cast<std::size_t>(-1));

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

// Whether `a` and `b` are one key of `ix`: every key column's value equal,
// by the engine's own comparison. **Never `encoded` equality**, which on a
// string column is a prefix test (`index_key.hpp`, IX6): two values sharing
// their first `kIndexStringKeyBytes` bytes encode alike and are two keys
// (BX-R1).
bool SameUniqueKey(const catalog::TableAccess& access, const catalog::TableAccess::IndexRef& ix,
                   const UniqueKey& a, const UniqueKey& b);

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
