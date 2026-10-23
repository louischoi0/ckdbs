#include "kds/exec/index_ddl.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "kds/catalog/schema.hpp"
#include "kds/exec/index_key.hpp"
#include "kds/exec/index_maintain.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/heap_chain.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/keystone.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/insert_placement.hpp"
#include "kds/storage/log_page_image.hpp"
#include "kds/storage/visit.hpp"
#include "kds/storage/page_header.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/txn/manager.hpp"
#include "kds/txn/read_view.hpp"
#include "kds/txn/undo_log.hpp"

namespace kds::exec {
namespace {

// Resolution is `Schema::FindColumn`'s, every column name's: ignoring case,
// two or more matches refused as ambiguous (docs/spec/parser-v2.md).
StatusOr<std::uint16_t> ResolveColumn(const catalog::TableAccess& access,
                                       const parser::IndexColumnRef& col,
                                       const std::string& table_name) {
    auto found = access.schema.FindColumn(col.name, col.byte_offset);
    if (!found.ok()) return found.status();
    if (const catalog::SysColumnRow* column = found.value(); column != nullptr) {
        // D2 (docs/spec/null.md): no nullable column enters an index in
        // v1, key or covered - an entry has no NULL encoding. Refused
        // here because this is the layer that still holds the byte;
        // Catalog::CheckIndexDef re-checks as the catalog's own defense,
        // the same one-implementation-two-doors split the pre-check
        // below describes.
        if (!column->notnull) {
            return Status::NotImplemented(
                "column '" + col.name + "' is nullable and cannot be in an index "
                "(docs/spec/null.md D2; declare it NOT NULL or leave it unindexed) (byte " +
                std::to_string(col.byte_offset) + ")");
        }
        return static_cast<std::uint16_t>(column - access.schema.columns.data());
    }
    return Status::NotFound("relation '" + table_name + "' has no column '" + col.name +
                            "' (byte " + std::to_string(col.byte_offset) + ")");
}

// One version of one row, copied out of its page.
//
// Copied rather than referenced, and that is the whole shape of the backfill:
// appending an entry fetches pages (a descent, and a `CreateNew` on a split),
// and `parser-v2.md` I15's R1 forbids a page fetch while a span into another
// page is live. So the walk runs in two phases **per leaf** - copy the page's
// tuples out, drop the span, then append - which bounds the memory at one
// page rather than at one relation.
struct StagedRow {
    std::uint64_t tuple_key = 0;
    std::uint64_t trx_id = 0;  // the current version's writer (a delete-mark's deleter)
    std::uint64_t undo_ptr = 0;
    bool deleted = false;
    // Online (CI-R7): `trx_id` was in flight when the row was copied, read
    // under the leaf's hold. A rollback takes its version back only under an
    // exclusive hold and retires the id only after, so a writer whose
    // version the copy holds and which is in flight then may yet lose.
    bool head_in_flight = false;
    std::vector<std::byte> payload;
};

// What one build's backfill appends with, the same for every leaf.
struct Backfill {
    storage::PageStore& store;
    catalog::Catalog& catalog;
    const catalog::TableAccess& access;
    const txn::TransactionManager* txns;
    txn::HolderProgress::Scope* progress;
    // Online (CI): appends log through `log` - or, with none, publish a root
    // they grew in place - from the root the slot names (CI-R6), and the walk
    // ends at the first leaf past `high_water` (§3.5 A2).
    bool online = false;
    const IndexWriteLog* log = nullptr;
    std::uint64_t high_water = 0;
};

// BX-R7: a unique index's build refuses a key two live rows hold. Asked of
// each row's latest version once its entries are in, so of every pair the
// later row finds the earlier one in the run. Under the build's relation `X`
// (AT-S5e) no other writer is undecided - each decided before its `IX` was
// released - so every version on a page is committed or the build's own
// transaction's, and an everything-visible view is the latest state.
// `values` is the row's latest version, decoded by its append.
Status CheckBuiltKeyUnique(storage::PageStore& store, const catalog::TableAccess& access,
                           const catalog::TableAccess::IndexRef& ix, const StagedRow& row,
                           std::span<const parser::AstValue> values) {
    if (row.deleted) return Status::OK();
    auto key = EncodeUniqueKey(access, ix, values, /*first_col_pos=*/0);
    if (!key.ok()) return key.status();
    auto verdict = CheckUniqueKey(store, access, ix, *key.value(), txn::ReadView::Everything(),
                                  row.tuple_key);
    if (!verdict.ok()) return verdict.status();
    if (verdict.value().kind == UniqueVerdict::Kind::kFree) return Status::OK();
    return Status::AlreadyExists(UniqueKeyText(access, ix, *key.value()) +
                                 " is held by both tuple key " +
                                 std::to_string(verdict.value().tuple_key) + " and tuple key " +
                                 std::to_string(row.tuple_key));
}

// The pages of a built or half-built tree, pre-order from `root`: every
// ancestor before its descendants.
//
// **Guarded, because a failed build frees what this returns.** Every page of
// the tree carries the index's oid from birth (`PrepareIndexDef` pre-issues
// it, page.md §2a) and is an index page; one that is not, or one reached
// twice, is not this tree's, and the walk refuses `Corruption` rather than
// hand another relation's page - or one freed and reissued in between - to
// `FreePage`. The visited set also bounds a cycle.
StatusOr<std::vector<PageId>> CollectTreePages(storage::PageStore& store, PageId root,
                                               catalog::Oid owner) {
    std::vector<PageId> pages;
    std::unordered_set<PageId> seen;
    std::vector<PageId> pending{root};
    while (!pending.empty()) {
        const PageId id = pending.back();
        pending.pop_back();
        if (!seen.insert(id).second) {
            return Status::Corruption("index oid " + std::to_string(owner) + ": page " +
                                      std::to_string(id) + " is reached twice");
        }
        auto bytes = store.GetForRead(id);
        if (!bytes.ok()) return bytes.status();
        const std::uint8_t type = storage::RawPageType(bytes.value().bytes());
        const bool internal = type == static_cast<std::uint8_t>(PageType::kIndexInternal);
        if ((!internal && type != static_cast<std::uint8_t>(PageType::kIndexLeaf)) ||
            storage::GetOwnerOid(bytes.value().bytes()) != owner) {
            return Status::Corruption("index oid " + std::to_string(owner) + ": page " +
                                      std::to_string(id) + " is not one of its pages");
        }
        pages.push_back(id);
        if (!internal) continue;
        index::IndexInternalView node(bytes.value().bytes());
        pending.push_back(node.leftmost_child());
        for (std::uint16_t i = 0; i < node.entry_count(); ++i) {
            auto child = node.Child(i);
            if (!child.ok()) return child.status();
            pending.push_back(child.value());
        }
    }
    return pages;
}

// RV3's record of the whole built tree: one full page image per page,
// descendants first. Order among the images is irrelevant - each describes
// a whole page and redo's FPI arm creates the page it names - and every
// image is stamped so the WAL-before-data gate holds the pages until the
// images are durable. A null `wal` no-ops (tests, bootstrap).
Status LogBuiltTree(wal::WalManager* wal, storage::PageStore& store, std::uint64_t trx_id,
                    PageId root, catalog::Oid owner, txn::HolderProgress::Scope* progress) {
    if (wal == nullptr) return Status::OK();
    auto pages = CollectTreePages(store, root, owner);
    if (!pages.ok()) return pages.status();
    for (auto page = pages.value().rbegin(); page != pages.value().rend(); ++page) {
        if (Status s = storage::LogFullPageImage(wal, store, trx_id, *page); !s.ok()) return s;
        if (progress != nullptr) progress->Advance();
    }
    return Status::OK();
}

// Gives back the pages of a tree a failed build filled (BX-Q6 (b),
// `instructions/v3.2.0/workorder-bx-unique-index.md`). `FreePage`'s
// precondition holds by the build's order: the tree is logged only after the
// backfill succeeds (`LogBuiltTree`), and named only after that (the
// `sys.indexes` row, its anchor slot), so no record that names a page can
// be replayed and no reader can reach one.
//
// **Best-effort, and a leak where it falls short** - as every failed build
// leaked before: a tree the guarded walk refuses frees nothing; a page a
// failed divide left unlinked is not reached; a frame a writeback holds
// (`kDeferred`) keeps its page; and a store that cannot free frees none. No
// map sync follows: a crash before the map is next written leaves the bits
// set, which is the leak there was.
void FreeUnbuiltTree(storage::PageStore& store, PageId root, catalog::Oid owner) {
    auto pages = CollectTreePages(store, root, owner);
    if (!pages.ok()) return;
    for (const PageId id : pages.value()) {
        auto freed = store.FreePage(id);
        if (!freed.ok() && freed.status().code() == StatusCode::kNotImplemented) return;
    }
}

// Appends one version's entry, decoding the key columns out of `payload`,
// and returns the decoded version for a unique build's check.
//
// Through `AppendIndexEntry`, the same function the write hook loops over -
// so a backfilled entry and a written one cannot disagree about what an entry
// is. `previous` is empty, which is what makes every version append rather
// than only the ones that moved.
StatusOr<std::vector<parser::AstValue>> AppendVersion(const Backfill& cx,
                                                      catalog::TableAccess::IndexRef& ix,
                                                      std::uint64_t tuple_key,
                                                      std::span<const std::byte> payload) {
    storage::PageStore& store = cx.store;
    const catalog::TableAccess& access = cx.access;
    std::vector<PendingSpill> spills;
    auto row = DecodeRow(access.schema, access.layout, payload, &spills);
    if (!row.ok()) return row.status();
    // Safe here and only here: no span into a heap page is live, because the
    // caller copied the payload out and released it.
    if (Status s = ResolveSpills(store, spills, row.value()); !s.ok()) return s;

    if (cx.online) {
        // CI-R6: one root, the slot's. Writers grow the tree too, and each
        // growth is published to the slot under the divide's holds, so the
        // slot read here names the root of every page any append can reach;
        // a root grown between this read and the descent is retried from the
        // slot by `AppendIndexEntry` (CI-Q11 (b)).
        auto root = SlotRoot(store, access.anchor_page_id, ix.index_oid);
        if (!root.ok()) return root.status();
        ix.root_page_id = root.value();
    }
    // Online the append logs, or with no log publishes its grown root in
    // place, and reports none. Fenced the root is held in the caller's own
    // IndexRef rather than published: the index has no sys.indexes row yet,
    // which is the point of building it before it is visible.
    auto moved = AppendIndexEntry(store, access, ix, row.value(), /*first_col_pos=*/0, payload,
                                  tuple_key, /*previous=*/{}, cx.log,
                                  cx.online && cx.log == nullptr ? &cx.catalog : nullptr);
    if (!moved.ok()) return moved.status();
    if (moved.value() != kInvalidPageId) ix.root_page_id = moved.value();
    return std::move(row.value());
}

// The row `tuple_key` as it now stands, copied out of its leaf under that
// leaf's hold and released (I15's R1), or none: retired, rolled back,
// purged. Descends from the clustered root as this core's catalog now
// names it, since a growth moves it and the build's own `access` is the
// generation it pinned before H3.
StatusOr<std::optional<StagedRow>> ReadRowAgain(const Backfill& cx, std::uint64_t tuple_key) {
    auto access = cx.catalog.InitTableAccess(cx.access.oid);
    if (!access.ok()) return access.status();
    auto at = btree::BtreeLookup(cx.store, access.value()->desc_page_id, tuple_key);
    if (!at.ok()) {
        if (at.status().code() == StatusCode::kNotFound) return std::optional<StagedRow>{};
        return at.status();
    }
    heap::PageView page(at.value().leaf.bytes());
    auto tuple = page.ReadTuple(at.value().slot);
    if (tuple.status().code() == StatusCode::kNotFound) return std::optional<StagedRow>{};
    if (!tuple.ok()) return tuple.status();
    StagedRow row;
    row.tuple_key = tuple_key;
    row.trx_id = tuple.value().trx_id;
    row.undo_ptr = tuple.value().undo_ptr;
    row.deleted = tuple.value().deleted;
    row.head_in_flight = cx.txns != nullptr && cx.txns->IsInFlight(row.trx_id);
    row.payload.assign(tuple.value().payload.begin(), tuple.value().payload.end());
    return std::optional<StagedRow>(std::move(row));
}

// One staged row: its version, then every older one a reader can reach, and
// for a unique build the key check (spec §10a).
//
// **Every version a reader can reach, and no further.** A version whose
// superseding writer is resolved for every reader (`ResolvedForEveryReader`)
// is invisible to every live and future view, so a reader stops above it and
// the undo purge may already have recycled its record: the pointer to it is
// stale, and what it names may be another row's version. The walk stops where
// the oldest reader would, before reading that pointer. `txns` null - a
// dispatcher with no transaction manager, where no snapshot exists and undo is
// never recycled - walks to the chain's end.
//
// **CI-R7, online: a head writer in flight at the copy is never trusted
// resolved later.** `ResolvedForEveryReader` answers true for a transaction
// that rolled back once the floor passes it - an answer about a version still
// on a page, and a rolled-back writer's version is not, though the copy holds
// it. So the walk never stops at that writer; it reads the writer's undo,
// which is the version a rollback restores. Where that read fails and the
// writer has decided since, the row is read again as it now stands and
// indexed instead, at most `storage::kMaxDescentRestarts` times.
Status IndexStagedRow(const Backfill& cx, catalog::TableAccess::IndexRef& ix,
                      txn::UndoLog& undo, StagedRow row) {
    const auto unreachable_below = [&cx](std::uint64_t superseder) {
        return cx.txns != nullptr && cx.txns->ResolvedForEveryReader(superseder);
    };
    for (int reread = 0;; ++reread) {
        if (cx.progress != nullptr) cx.progress->Advance();
        auto latest = AppendVersion(cx, ix, row.tuple_key, row.payload);
        if (!latest.ok()) return latest.status();

        const auto trusted = [&](std::uint64_t superseder) {
            if (row.head_in_flight && superseder == row.trx_id) return false;
            return unreachable_below(superseder);
        };
        // The writer that superseded the version the walk reads next. A
        // row whose current writer every reader sees contributes its
        // current version alone.
        std::uint64_t superseder = row.trx_id;
        bool stale = false;
        // A failed append leaves the walk by its Status and fails the
        // build: the index must not be published without that version's
        // entry or any older one of the row, or an old snapshot reaches
        // its row through no entry (spec §10a). Kept apart from a failed
        // read, which the superseder's resolution may excuse.
        Status appended = Status::OK();
        if (!trusted(superseder)) {
            Status walked = undo.Walk(
                row.undo_ptr,
                [&](const txn::UndoVersion& version) -> StatusOr<storage::VisitControl> {
                    // Judged after the walk, as a failed read is.
                    if (version.tuple_key != row.tuple_key) {
                        return Status::Corruption(
                            "undo chain of the row with tuple key " +
                            std::to_string(row.tuple_key) +
                            " reached a version of tuple key " +
                            std::to_string(version.tuple_key));
                    }
                    // A delete-mark's image is empty because a mark
                    // changes no tuple bytes - the version it supersedes
                    // is the one already appended, so there is nothing
                    // new here.
                    if (!version.image.empty()) {
                        auto older = AppendVersion(cx, ix, row.tuple_key, version.image);
                        if (!older.ok()) {
                            appended = older.status().WithContext(
                                "indexing an older version of the row with tuple key " +
                                std::to_string(row.tuple_key));
                            return appended;
                        }
                    }
                    superseder = version.prior_trx_id;  // wrote this version
                    return trusted(superseder) ? storage::VisitControl::kStop
                                               : storage::VisitControl::kContinue;
                });
            if (!appended.ok()) return appended;
            if (!walked.ok()) {
                if (row.head_in_flight && superseder == row.trx_id &&
                    !cx.txns->IsInFlight(row.trx_id)) {
                    stale = true;
                } else if (!unreachable_below(superseder)) {
                    return walked;
                }
            }
        }
        if (!stale) {
            if (ix.unique) {
                if (Status s = CheckBuiltKeyUnique(cx.store, cx.access, ix, row, latest.value());
                    !s.ok()) {
                    return s;
                }
            }
            return Status::OK();
        }
        if (reread == storage::kMaxDescentRestarts) {
            return Status::TxnConflict(
                "the row with tuple key " + std::to_string(row.tuple_key) +
                " was taken back by a rollback under the index build " +
                std::to_string(reread + 1) + " times; the build can be run again");
        }
        auto again = ReadRowAgain(cx, row.tuple_key);
        if (!again.ok()) return again.status();
        if (!again.value().has_value()) return Status::OK();  // gone: nothing to index
        row = std::move(*again.value());
    }
}

// Indexes one leaf's rows into `ix`'s tree, and returns the leaf after it
// (spec §10a).
//
// **Every version, not only the current one.** A reader holding an older
// snapshot that probes the index must find *its* version through it. Every version
// of a logical tuple shares one tuple key, so the walk is bounded by the undo chain
// and the entries are the ordinary shape. `IndexInsert` deduplicates a
// byte-identical entry, so a version that did not move the key costs nothing
// and needs no distinctness tracking here.
//
// A **delete-marked** row is walked like any other: it is gone for newer
// readers and still there for older ones, which is exactly the case the undo
// chain exists for.
//
// `ix` is the caller's, so its `root_page_id` names the root the tree has
// grown to even when the backfill fails - which is what a failed build frees
// from (BX-Q6 (b)). Online it is read from the slot per append (CI-R6).
//
// **Online, the walk ends past the high-water key** (§3.5 A2): a leaf whose
// `min_key` is above it holds only rows written after the drain, by writers
// that maintain the index; without the bound a backfill slower than the
// writers' appends at the tail would chase them.
StatusOr<PageId> BackfillLeaf(const Backfill& cx, catalog::TableAccess::IndexRef& ix,
                              PageId leaf) {
    // Reads only, so a locally-built log is correct: Read/Walk resolve a
    // pointer against pages and carry no allocation state. A writer refused
    // by the superseder test can be resolved between the check and the read,
    // and another core's purge can then recycle its record: a read that
    // fails, or returns another row's version, is asked again after the walk,
    // and once its superseder is resolved the walk just ends there.
    txn::UndoLog undo(cx.store);

    // Per leaf, and per row below: what keeps this build's waiters inside
    // the fault net, however wide a leaf's rows make it.
    if (cx.progress != nullptr) cx.progress->Advance();

    // ---- Phase 1: copy out, with no page fetch under the span -------
    std::vector<StagedRow> staged;
    PageId next = kInvalidPageId;
    {
        auto bytes = cx.store.GetForRead(leaf);
        if (!bytes.ok()) return bytes.status();
        heap::PageView page(bytes.value().bytes());
        if (cx.online) {
            // Implied by the `X` before CI; under `IS`, said: the cursor is
            // still on this relation's chain.
            if (storage::GetOwnerOid(bytes.value().bytes()) != cx.access.oid) {
                return Status::Corruption("index build: leaf " + std::to_string(leaf) +
                                          " is not relation oid " +
                                          std::to_string(cx.access.oid) + "'s");
            }
            if (page.min_key() > cx.high_water) return kInvalidPageId;
        }
        const std::uint16_t n = page.slot_count();
        staged.reserve(n);
        for (std::uint16_t i = 0; i < n; ++i) {
            auto tuple = page.ReadTuple(i);
            // NotFound is a retired slot: physically gone, no version any
            // reader can reach, so nothing to index.
            if (tuple.status().code() == StatusCode::kNotFound) continue;
            if (!tuple.ok()) return tuple.status();

            auto tuple_key = kds::KeystoneIdOfPayload(tuple.value().payload);
            if (!tuple_key.ok()) return tuple_key.status();

            StagedRow row;
            row.tuple_key = tuple_key.value();
            row.trx_id = tuple.value().trx_id;
            row.undo_ptr = tuple.value().undo_ptr;
            row.deleted = tuple.value().deleted;
            row.head_in_flight =
                cx.online && cx.txns != nullptr && cx.txns->IsInFlight(row.trx_id);
            row.payload.assign(tuple.value().payload.begin(), tuple.value().payload.end());
            staged.push_back(std::move(row));
        }
        next = page.next_page_id();
    }

    // ---- Phase 2: append, with nothing live -------------------------
    for (StagedRow& row : staged) {
        if (Status s = IndexStagedRow(cx, ix, undo, std::move(row)); !s.ok()) return s;
    }
    return next;
}

// The highest tuple key the relation holds now, or the last leaf's
// `min_key` when that leaf is empty: the leaf the key above every other
// sorts to, then on along its chain to the end, since a divide may have
// linked leaves after it since the descent.
StatusOr<std::uint64_t> HighWaterKey(catalog::Catalog& catalog, storage::PageStore& store,
                                     catalog::Oid rel_oid) {
    auto access = catalog.InitTableAccess(rel_oid);
    if (!access.ok()) return access.status();
    auto leaf = btree::BtreeSeekLeaf(store, access.value()->desc_page_id, kMaxKeystoneId);
    if (!leaf.ok()) return leaf.status();
    PageId at = leaf.value();
    std::uint64_t high = 0;
    for (std::uint32_t walked = 0; at != kInvalidPageId; ++walked) {
        if (Status s = storage::CheckPageWalkBudget(walked, leaf.value(), "relation leaf chain");
            !s.ok()) {
            return s;
        }
        auto bytes = store.GetForRead(at);
        if (!bytes.ok()) return bytes.status();
        heap::PageView page(bytes.value().bytes());
        high = std::max(high, page.min_key());
        for (std::uint16_t i = 0; i < page.slot_count(); ++i) {
            auto tuple = page.ReadTuple(i);
            if (tuple.status().code() == StatusCode::kNotFound) continue;
            if (!tuple.ok()) return tuple.status();
            auto key = kds::KeystoneIdOfPayload(tuple.value().payload);
            if (!key.ok()) return key.status();
            high = std::max(high, key.value());
        }
        at = page.next_page_id();
    }
    return high;
}

}  // namespace

StatusOr<catalog::Catalog::IndexDef> PrepareIndexDef(catalog::Catalog& catalog,
                                                     const parser::IndexStmt& stmt,
                                                     catalog::Oid default_namespace,
                                                     const txn::ReadView* view,
                                                     std::uint64_t trx_id) {
    auto oid = catalog.ResolveRelation(stmt.schema, stmt.table_name, default_namespace,
                                       stmt.table_byte_offset, view);
    if (!oid.ok()) return oid.status();
    auto access = catalog.InitTableAccess(oid.value());
    if (!access.ok()) return access.status();

    catalog::Catalog::IndexDef def;
    def.table_oid = oid.value();
    def.name = stmt.index_name;

    // The key's width, from the encoder that will produce it. Asking
    // `exec::IndexKeyWidth` rather than re-deriving here is what keeps the
    // stored constant and the bytes from ever disagreeing.
    std::vector<catalog::SysColumnRow> key_cols;
    for (const parser::IndexColumnRef& col : stmt.key_columns) {
        auto pos = ResolveColumn(*access.value(), col, stmt.table_name);
        if (!pos.ok()) return pos.status();
        def.key_cols.push_back(pos.value());
        key_cols.push_back(access.value()->schema.columns[pos.value()]);
    }
    auto key_width = IndexKeyWidth(key_cols);
    if (!key_width.ok()) {
        return key_width.status().WithContext("index '" + stmt.index_name + "'");
    }

    // A covered column is stored as its **inline cell bytes verbatim**, so
    // its width is the row layout's - tag included, which is what lets a
    // spilled value's pointer ride along and be resolved from the base row
    // exactly as it would have been.
    std::uint32_t covered_width = 0;
    for (const parser::IndexColumnRef& col : stmt.covered_columns) {
        auto pos = ResolveColumn(*access.value(), col, stmt.table_name);
        if (!pos.ok()) return pos.status();
        def.covered_cols.push_back(pos.value());
        auto width = catalog::RowLayout::ColumnWidth(access.value()->schema.columns[pos.value()],
                                                      access.value()->layout.inline_cell_width);
        if (!width.ok()) {
            return width.status().WithContext("covered column '" + col.name + "'");
        }
        covered_width += width.value();
    }

    index::IndexLayout layout;
    layout.key_width = static_cast<std::uint16_t>(key_width.value());
    layout.covered_width = static_cast<std::uint16_t>(covered_width);
    // Where a wide declaration is refused: by arithmetic, at declaration,
    // rather than by an insert that fails much later.
    if (Status s = index::CheckIndexLayout(layout); !s.ok()) {
        return s.WithContext("index '" + stmt.index_name + "'");
    }
    def.key_width = layout.key_width;
    if (stmt.unique) def.flags |= catalog::kIndexFlagUnique;
    def.entry_width = static_cast<std::uint16_t>(layout.leaf_entry_width());

    // Every refusal, before a page is walked. `Catalog::CreateIndex` runs
    // the same check again at the write - one implementation, two callers
    // - so this buys the *position* of the failure and nothing else: a
    // refusal by name rather than as a page-type error from inside the
    // build.
    if (Status s = catalog.CheckIndexDef(def, trx_id);
        !s.ok()) {
        return s;
    }

    // The oid, issued before any page exists so the root and every page
    // the backfill splits off carry it from birth (page.md §2a). Burned
    // by a build that then fails, which the row-id sequence permits.
    auto pre_oid = catalog.AllocateRowId(catalog::kSysIndexesTable);
    if (!pre_oid.ok()) return pre_oid.status();
    def.index_oid = pre_oid.value();
    return def;
}

namespace {

// The index `stmt` names under `view`, on `relation` only (`DropIndex`'s
// contract): the name was resolved once to take the `X` and is resolved again
// here, and between the two another transaction may have dropped the index
// and created one by its name on another relation (BY-R8).
StatusOr<catalog::SysIndexRow> FindNamedIndex(catalog::Catalog& catalog,
                                              const parser::IndexStmt& stmt,
                                              std::optional<catalog::Oid> relation,
                                              const txn::ReadView* view) {
    auto row = catalog.FindIndexByName(stmt.index_name, view);
    if (!relation.has_value() || !row.ok()) {
        return Status::NotFound("no index named '" + stmt.index_name + "' (byte " +
                                std::to_string(stmt.byte_offset) + ")");
    }
    if (row.value().table_oid != *relation) {
        return Status::TxnConflict("index '" + stmt.index_name +
                                   "' moved to another relation while the statement waited (byte " +
                                   std::to_string(stmt.byte_offset) + ")");
    }
    return row;
}

Status DropNamedIndex(catalog::Catalog& catalog, const parser::IndexStmt& stmt,
                      catalog::Oid index_oid, std::uint64_t trx_id,
                      catalog::CatalogRowChange* change) {
    Status s = catalog.DropIndex(index_oid, trx_id, change);
    // The byte belongs to the statement, and only this layer has it.
    // `WithContext` would prefix instead of append and would bury the
    // position mid-sentence, so the one refusal that names the user's
    // object gets it spelled out the way `FindNamedIndex` does - through
    // `WithMessage`, which keeps whatever code arrived rather than naming
    // one here. Naming one is how a re-wrap answers "could a later release
    // lift this?" with its own guess.
    if (s.ok() || (s.code() != StatusCode::kUnsupported &&
                   s.code() != StatusCode::kNotImplemented)) {
        return s;
    }
    return s.WithMessage("index '" + stmt.index_name + "': " + s.message() + " (byte " +
                         std::to_string(stmt.byte_offset) + ")");
}

}  // namespace

IndexBuild::IndexBuild(catalog::Catalog& catalog, storage::PageStore& store,
                       parser::IndexStmt stmt, catalog::Catalog::IndexDef def,
                       std::uint64_t trx_id, const txn::TransactionManager* txns,
                       txn::HolderProgress::Scope* progress, bool online, wal::WalManager* wal)
    : catalog_(catalog),
      store_(store),
      stmt_(std::move(stmt)),
      def_(std::move(def)),
      trx_id_(trx_id),
      txns_(txns),
      progress_(progress),
      online_(online),
      wal_(wal) {
    // Before `Begin` reads `access_` out of the memo: H3's own word move
    // drops the generation it is in, and the walk reads it for the build's
    // life (§3.5 A4).
    if (online_) pin_.emplace(catalog_);
}

IndexBuild::~IndexBuild() {
    // A build never finished - its statement's frame destroyed at a
    // suspension - gives its unlogged tree back as a failed one does. A
    // logged one is the reclaimer's, through its slot (CI-R4).
    if (!finished_ && !tree_logged_ && tree_.root_page_id != kInvalidPageId) {
        FreeUnbuiltTree(store_, tree_.root_page_id, def_.index_oid);
    }
}

StatusOr<std::unique_ptr<IndexBuild>> IndexBuild::ForCreate(
    catalog::Catalog& catalog, storage::PageStore& store, const parser::IndexStmt& stmt,
    catalog::Oid default_namespace, const txn::ReadView* view, std::uint64_t trx_id,
    const txn::TransactionManager* txns, txn::HolderProgress::Scope* progress,
    std::optional<catalog::Oid> relation, bool online, wal::WalManager* wal) {
    auto prepared = PrepareIndexDef(catalog, stmt, default_namespace, view, trx_id);
    if (!prepared.ok()) return prepared.status();
    // §3.5 A10: the name is resolved again here, and between the caller's
    // borrow and this lookup the relation can have been dropped and another
    // created under its name, which the borrow fences nothing of.
    if (relation.has_value() && prepared.value().table_oid != *relation) {
        return Status::TxnConflict("relation '" + stmt.table_name +
                                   "' was dropped and created again while the statement "
                                   "waited (byte " +
                                   std::to_string(stmt.table_byte_offset) + ")");
    }
    std::unique_ptr<IndexBuild> build(new IndexBuild(catalog, store, stmt,
                                                     std::move(prepared.value()), trx_id, txns,
                                                     progress, online, wal));
    if (Status s = build->Begin(); !s.ok()) return build->Refusal(s);
    return build;
}

StatusOr<std::unique_ptr<IndexBuild>> IndexBuild::ForReindex(
    catalog::Catalog& catalog, storage::PageStore& store, const parser::IndexStmt& stmt,
    std::optional<catalog::Oid> relation, const txn::ReadView* view, std::uint64_t trx_id,
    catalog::CatalogRowChange* dropped, const txn::TransactionManager* txns,
    txn::HolderProgress::Scope* progress) {
    auto old = FindNamedIndex(catalog, stmt, relation, view);
    if (!old.ok()) return old.status();
    const catalog::SysIndexRow& row = old.value();

    // **The definition is the row's own positions, never names** (B2): a
    // definition re-spelled by column names could resolve to other columns
    // than the index was built on, should a rename land between - none can
    // since `RENAME COLUMN` takes the relation `X` (`alter.md` AL4a), and
    // the positions keep it so as defense in depth. Widths and flags are the row's too - stored constants a column's
    // type cannot move under (types are not alterable, `alter.md` AL11).
    catalog::Catalog::IndexDef def;
    def.table_oid = row.table_oid;
    def.name = stmt.index_name;
    def.key_width = row.key_width;
    def.entry_width = row.entry_width;
    def.flags = row.flags;
    def.key_cols.assign(row.key_cols.begin(), row.key_cols.begin() + row.nkeys);
    def.covered_cols.assign(row.covered_cols.begin(), row.covered_cols.begin() + row.ncovered);

    // The old row first: its delete-mark is this transaction's, which is what
    // lets the new row take the name (BY-R8).
    if (Status s = DropNamedIndex(catalog, stmt, row.index_oid, trx_id, dropped); !s.ok()) {
        return s;
    }
    if (Status s = catalog.CheckIndexDef(def, trx_id); !s.ok()) return s;
    // Issued before any page exists, as `PrepareIndexDef` issues it (page.md
    // §2a).
    auto new_oid = catalog.AllocateRowId(catalog::kSysIndexesTable);
    if (!new_oid.ok()) return new_oid.status();
    def.index_oid = new_oid.value();
    std::unique_ptr<IndexBuild> build(
        new IndexBuild(catalog, store, stmt, std::move(def), trx_id, txns, progress));
    build->replaced_oid_ = row.index_oid;
    if (Status s = build->Begin(); !s.ok()) return build->Refusal(s);
    return build;
}

Status IndexBuild::Begin() {
    auto access = catalog_.InitTableAccess(def_.table_oid);
    if (!access.ok()) return access.status();
    access_ = access.value();

    // The layout the definition's widths encode: `PrepareIndexDef` stored
    // `entry_width` as `leaf_entry_width()`, so the covered width is what
    // is left after the key and the tuple key. Re-derived rather than carried, and
    // guarded; `FormatRoot` re-checks the layout as a whole.
    if (def_.entry_width < def_.key_width + index::kIndexTupleKeyWidth) {
        return Status::InvalidArgument("index oid " + std::to_string(def_.index_oid) +
                                       ": entry width " + std::to_string(def_.entry_width) +
                                       " is narrower than its key and tuple key");
    }
    // The same guard for the column counts, and here it is memory safety
    // rather than a message: `IndexRef`'s arrays are fixed at the caps, and
    // the assembly below writes one element per declared column.
    // `CheckIndexDef` refuses over-cap and empty upstream; this guard does
    // not rely on it.
    if (def_.key_cols.empty() || def_.key_cols.size() > catalog::kMaxIndexKeyColumns ||
        def_.covered_cols.size() > catalog::kMaxIndexCoveredColumns) {
        return Status::InvalidArgument(
            "index oid " + std::to_string(def_.index_oid) + ": " +
            std::to_string(def_.key_cols.size()) + " key and " +
            std::to_string(def_.covered_cols.size()) +
            " covered columns is not a shape an index entry has");
    }
    index::IndexLayout layout;
    layout.key_width = def_.key_width;
    layout.covered_width = static_cast<std::uint16_t>(def_.entry_width - def_.key_width -
                                                      index::kIndexTupleKeyWidth);

    // The root, scoped so its pin drops before the walk fetches pages.
    {
        auto created = store_.CreateNew();
        if (!created.ok()) return created.status();
        auto& [id, bytes] = created.value();
        if (Status s = index::FormatRoot(bytes.bytes(), layout, def_.index_oid); !s.ok()) {
            return s;
        }
        tree_.root_page_id = id;
    }

    tree_.index_oid = def_.index_oid;  // pre-issued; the backfill stamps pages with it
    tree_.key_width = def_.key_width;
    tree_.entry_width = def_.entry_width;
    tree_.nkeys = static_cast<std::uint8_t>(def_.key_cols.size());
    tree_.ncovered = static_cast<std::uint8_t>(def_.covered_cols.size());
    tree_.unique = (def_.flags & catalog::kIndexFlagUnique) != 0;
    tree_.name = def_.name;
    for (std::size_t i = 0; i < def_.key_cols.size(); ++i) tree_.key_cols[i] = def_.key_cols[i];
    for (std::size_t i = 0; i < def_.covered_cols.size(); ++i) {
        tree_.covered_cols[i] = def_.covered_cols[i];
    }

    // **H2, online**: the root's image, before H3 lets a writer log an
    // entry into it - so redo never meets an `INDEX_INSERT` for a page it
    // has no image of (CI-R3). From here the tree is logged, and from H3
    // writers append to it, so only the reclaimer frees it (CI-R4). A
    // refusal between here and H3 - the cursor's start below - leaks the root
    // page, as a crash between H2 and H3 does: no slot names it yet.
    if (online_) {
        if (Status s = storage::LogFullPageImage(wal_, store_, trx_id_, tree_.root_page_id);
            !s.ok()) {
            return s.WithContext("logging the index root");
        }
        tree_logged_ = true;
    }

    // The leftmost leaf's id is permanent - a growth makes the old root the
    // new root's leftmost child and a divide keeps a leaf's id and lower
    // half - so a cursor started here reaches every leaf under `IS` too.
    return cursor_.Start(store_, access_->desc_page_id);
}

bool IndexBuild::SeedOwedSlot() {
    // **The tree is logged and nothing may name it** (BY-S2's review): a
    // refusal before the slot went down - a name another core took after
    // `PrepareIndexDef` asked - would leave a tree no slot names, which no
    // mount's owed-slot scan finds (BY-R1). So the slot is seeded here,
    // best-effort, and the tree is owed from it. A slot already down is
    // left as it is; a seed that fails leaves the leak there was.
    auto table = catalog_.GetSysTableRow(def_.table_oid);
    if (!table.ok() || table.value().anchor_page_id == kInvalidPageId) return false;
    const PageId anchor = table.value().anchor_page_id;
    if (SlotRoot(store_, anchor, def_.index_oid).ok()) return true;
    (void)catalog_.WriteAnchorRoot(anchor, def_.table_oid, def_.index_oid, tree_.root_page_id,
                                   trx_id_);
    return SlotRoot(store_, anchor, def_.index_oid).ok();
}

Status IndexBuild::Announce(catalog::CatalogRowRef* written, bool* owed) {
    *owed = false;
    def_.root_page_id = tree_.root_page_id;
    def_.flags = static_cast<std::uint8_t>(def_.flags | catalog::kIndexFlagBuilding);
    auto index_oid = catalog_.CreateIndex(def_, trx_id_, written);
    if (!index_oid.ok()) {
        *owed = SeedOwedSlot();
        return Refusal(index_oid.status());
    }
    *owed = true;
    return Status::OK();
}

Status IndexBuild::Step(std::uint32_t leaves, const IndexWriteLog* log) {
    if (online_ && !high_water_.has_value()) {
        // Taken at the first slice, which runs after the drain: every row
        // above it was written by a writer that maintains the index.
        auto high = HighWaterKey(catalog_, store_, access_->oid);
        if (!high.ok()) return high.status();
        high_water_ = high.value();
    }
    const Backfill cx{store_,  catalog_, *access_, txns_,
                      progress_, online_, log,      high_water_.value_or(0)};
    return cursor_.Step(leaves, [&](PageId leaf) { return BackfillLeaf(cx, tree_, leaf); });
}

Status IndexBuild::Refusal(const Status& s) const {
    // A duplicate the unique build found (BX-R7) is the statement's
    // refusal, so it carries the index name's byte. It is the build's
    // only `AlreadyExists`: no two entries share a sort key, so no divide
    // is refused one (index.md IX4b).
    if (s.code() == StatusCode::kAlreadyExists) {
        return s.WithMessage(s.message() + " (byte " + std::to_string(stmt_.byte_offset) + ")");
    }
    return s.WithContext("building index '" + stmt_.index_name + "'");
}

StatusOr<IndexDdlResult> IndexBuild::Finish(const Status& ran, catalog::CatalogRowRef* written,
                                            wal::WalManager* wal) {
    finished_ = true;
    if (online_) {
        // The row, the slot and every entry are down already; a refusal is
        // answered as it is, and the tree is owed through its slot from H3.
        if (!ran.ok()) return Refusal(ran);
        // R (CI-Q7 (a)): the flag cleared in place, logged; from the schema
        // word this moves, every core's compile may choose the index.
        if (Status s = catalog_.ClearIndexBuilding(def_.index_oid, trx_id_); !s.ok()) {
            return Refusal(s.WithContext("marking the index ready"));
        }
        IndexDdlResult out;
        out.index_oid = def_.index_oid;
        out.rel_oid = def_.table_oid;
        auto root = SlotRoot(store_, access_->anchor_page_id, def_.index_oid);
        out.root_page_id = root.ok() ? root.value() : tree_.root_page_id;
        out.key_width = def_.key_width;
        out.entry_width = def_.entry_width;
        out.warnings = IndexCreationWarnings(catalog_, def_, stmt_.key_columns[0].name);
        return out;
    }
    if (!ran.ok()) {
        // Nothing has logged the tree yet and nothing names it, so its pages
        // go back now rather than leaking for the volume's life (BX-Q6 (b)).
        FreeUnbuiltTree(store_, tree_.root_page_id, def_.index_oid);
        return Refusal(ran);
    }

    // RV3: the built tree travels as full page images, **before** the
    // sys.indexes row that publishes it - a crash after the commit must
    // find a tree the recovered catalog row can probe, and the backfill
    // logged nothing per entry. An FPI both creates and fills a page at
    // redo, so no PAGE_INIT is needed; the cost is tree bytes per CREATE
    // INDEX, on a statement that is already O(relation).
    if (Status s =
            LogBuiltTree(wal, store_, trx_id_, tree_.root_page_id, def_.index_oid, progress_);
        !s.ok()) {
        return Refusal(s.WithContext("logging the built tree"));
    }
    def_.root_page_id = tree_.root_page_id;

    auto index_oid = catalog_.CreateIndex(def_, trx_id_, written);
    if (!index_oid.ok()) {
        (void)SeedOwedSlot();
        return index_oid.status();
    }

    IndexDdlResult out;
    out.index_oid = index_oid.value();
    out.rel_oid = def_.table_oid;
    out.root_page_id = def_.root_page_id;
    out.key_width = def_.key_width;
    out.entry_width = def_.entry_width;
    out.replaced_oid = replaced_oid_;
    // Collected after the write, because none of them is a reason not to
    // write. A rebuild names no column to warn about.
    if (!stmt_.reindex) {
        out.warnings = IndexCreationWarnings(catalog_, def_, stmt_.key_columns[0].name);
    }
    return out;
}

std::vector<std::string> IndexCreationWarnings(catalog::Catalog& catalog,
                                               const catalog::Catalog::IndexDef& def,
                                               std::string_view key_column) {
    std::vector<std::string> warnings;
    // An index is complete for every value where a Cabin is authoritative
    // only for observed ones, so the Cabin becomes dead weight - but
    // dropping it is the operator's call, not the engine's.
    if (catalog.FindCabinOnColumn(def.table_oid, def.key_cols[0]).ok()) {
        warnings.push_back("column '" + std::string(key_column) +
                           "' already carries a cabin; this index supersedes it for every "
                           "value, not just observed ones, and the cabin's write hook and "
                           "memory now buy nothing (DROP CABIN to reclaim them)");
    }
    return warnings;
}

StatusOr<catalog::Oid> DropIndex(catalog::Catalog& catalog, const parser::IndexStmt& stmt,
                                  std::optional<catalog::Oid> relation,
                                  const txn::ReadView* view, std::uint64_t trx_id,
                                  catalog::CatalogRowChange* change) {
    auto row = FindNamedIndex(catalog, stmt, relation, view);
    if (!row.ok()) return row.status();
    if (Status s = DropNamedIndex(catalog, stmt, row.value().index_oid, trx_id, change);
        !s.ok()) {
        return s;
    }
    return row.value().index_oid;
}

}  // namespace kds::exec
