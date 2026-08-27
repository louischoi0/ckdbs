#include "kds/exec/index_ddl.hpp"

#include <string>
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
#include "kds/storage/log_page_image.hpp"
#include "kds/wal/payload.hpp"
#include "kds/wal/record.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/txn/undo_log.hpp"

namespace kds::exec {
namespace {

// Resolution is exact-match, like `Schema::FindColumn` everywhere else: the
// catalog stores a column's name as it was declared, and case folding here
// alone would make `CREATE INDEX` accept a spelling no other statement does.
StatusOr<std::uint16_t> ResolveColumn(const catalog::TableAccess& access,
                                       const parser::IndexColumnRef& col,
                                       const std::string& table_name) {
    for (std::size_t i = 0; i < access.schema.columns.size(); ++i) {
        if (catalog::NameView(access.schema.columns[i].name) == col.name) {
            // D2 (docs/spec/null.md): no nullable column enters an index in
            // v1, key or covered - an entry has no NULL encoding. Refused
            // here because this is the layer that still holds the byte;
            // Catalog::CheckIndexDef re-checks as the catalog's own defense,
            // the same one-implementation-two-doors split the pre-check
            // below describes.
            if (!access.schema.columns[i].notnull) {
                return Status::Unsupported(
                    "column '" + col.name + "' is nullable and cannot be in an index "
                    "(docs/spec/null.md D2; declare it NOT NULL or leave it unindexed) (byte " +
                    std::to_string(col.byte_offset) + ")");
            }
            return static_cast<std::uint16_t>(i);
        }
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
    std::uint64_t pk = 0;
    std::uint64_t undo_ptr = 0;
    std::vector<std::byte> payload;
};

// RV3's record of the whole built tree: one full page image per page,
// depth-first from the root. Order among the images is irrelevant - each
// describes a whole page and redo's FPI arm creates the page it names -
// and every image is stamped so the WAL-before-data gate holds the pages
// until the images are durable. A null `wal` no-ops (tests, bootstrap).
Status LogBuiltTree(wal::WalManager* wal, storage::PageStore& store, std::uint64_t trx_id,
                    PageId page_id) {
    if (wal == nullptr) return Status::OK();

    // The children first, collected under a scope that drops this page's
    // pin before recursing - depth bounds the pin count either way, but a
    // copied id list keeps the shape obviously safe.
    std::vector<PageId> children;
    {
        auto bytes = store.GetForRead(page_id);
        if (!bytes.ok()) return bytes.status();
        if (storage::RawPageType(bytes.value().bytes()) ==
            static_cast<std::uint8_t>(PageType::kIndexInternal)) {
            index::IndexInternalView node(bytes.value().bytes());
            children.push_back(node.leftmost_child());
            for (std::uint16_t i = 0; i < node.entry_count(); ++i) {
                auto child = node.Child(i);
                if (!child.ok()) return child.status();
                children.push_back(child.value());
            }
        }
    }
    for (const PageId child : children) {
        if (Status s = LogBuiltTree(wal, store, trx_id, child); !s.ok()) return s;
    }

    return storage::LogFullPageImage(wal, store, trx_id, page_id);
}

// Appends one version's entry, decoding the key columns out of `payload`.
//
// Through `AppendIndexEntry`, the same function the write hook loops over -
// so a backfilled entry and a written one cannot disagree about what an entry
// is. `previous` is empty, which is what makes every version append rather
// than only the ones that moved.
Status AppendVersion(storage::PageStore& store, const catalog::TableAccess& access,
                     catalog::TableAccess::IndexRef& ix, std::uint64_t pk,
                     std::span<const std::byte> payload) {
    std::vector<PendingSpill> spills;
    auto row = DecodeRow(access.schema, access.layout, payload, &spills);
    if (!row.ok()) return row.status();
    // Safe here and only here: no span into a heap page is live, because the
    // caller copied the payload out and released it.
    if (Status s = ResolveSpills(store, spills, row.value()); !s.ok()) return s;

    auto moved = AppendIndexEntry(store, access, ix, row.value(), /*first_col_pos=*/0, payload,
                                  pk, /*previous=*/{});
    if (!moved.ok()) return moved.status();
    // The root is held in the caller's own IndexRef rather than published:
    // the index has no sys.indexes row yet, which is the point of building it
    // before it is visible.
    if (moved.value() != kInvalidPageId) ix.root_page_id = moved.value();
    return Status::OK();
}

// Builds `ix`'s tree over everything the relation already holds, and returns
// the root it ended at (spec §10a).
//
// **Every version, not only the current one.** DDL is not transactional, so
// the index becomes visible to every reader at once - including one holding
// an older snapshot, which must find *its* version through it. Every version
// of a logical tuple shares one pk, so the walk is bounded by the undo chain
// and the entries are the ordinary shape. `IndexInsert` deduplicates a
// byte-identical entry, so a version that did not move the key costs nothing
// and needs no distinctness tracking here.
//
// A **delete-marked** row is walked like any other: it is gone for newer
// readers and still there for older ones, which is exactly the case the undo
// chain exists for.
StatusOr<PageId> Backfill(storage::PageStore& store, const catalog::TableAccess& access,
                          catalog::TableAccess::IndexRef ix) {
    // Reads only, so a locally-built log is correct: Read/Walk resolve a
    // pointer against pages and carry no allocation state. And safe under
    // the undo purge for two reasons that must both stay true: this walk
    // appends nothing, so no growth can trigger a reclaim mid-walk on this
    // cooperative core; and the statement's own undo appends (the DDL undo
    // hook's) all happen after the backfill returns, inside
    // Catalog::CreateIndex.
    txn::UndoLog undo(store);

    auto first = btree::BtreeLeftmostLeaf(store, access.desc_page_id);
    if (!first.ok()) return first.status();

    PageId leaf = first.value();
    const PageId walk_origin = leaf;
    for (std::uint32_t leaves = 0; leaf != kInvalidPageId; ++leaves) {
        if (Status s = storage::CheckPageWalkBudget(leaves, walk_origin, "relation leaf chain");
            !s.ok()) {
            return s;
        }

        // ---- Phase 1: copy out, with no page fetch under the span -------
        std::vector<StagedRow> staged;
        PageId next = kInvalidPageId;
        {
            auto bytes = store.GetForRead(leaf);
            if (!bytes.ok()) return bytes.status();
            heap::PageView page(bytes.value().bytes());
            const std::uint16_t n = page.slot_count();
            staged.reserve(n);
            for (std::uint16_t i = 0; i < n; ++i) {
                auto tuple = page.ReadTuple(i);
                // NotFound is a retired slot: physically gone, no version any
                // reader can reach, so nothing to index.
                if (tuple.status().code() == StatusCode::kNotFound) continue;
                if (!tuple.ok()) return tuple.status();

                auto pk = kds::KeystoneIdOfPayload(tuple.value().payload);
                if (!pk.ok()) return pk.status();

                StagedRow row;
                row.pk = pk.value();
                row.undo_ptr = tuple.value().undo_ptr;
                row.payload.assign(tuple.value().payload.begin(), tuple.value().payload.end());
                staged.push_back(std::move(row));
            }
            next = page.next_page_id();
        }

        // ---- Phase 2: append, with nothing live -------------------------
        for (const StagedRow& row : staged) {
            if (Status s = AppendVersion(store, access, ix, row.pk, row.payload); !s.ok()) {
                return s;
            }

            Status walked = undo.Walk(
                row.undo_ptr, [&](const txn::UndoVersion& version) -> bool {
                    // A delete-mark's image is empty because a mark changes
                    // no tuple bytes - the version it supersedes is the one
                    // already appended, so there is nothing new here.
                    if (version.image.empty()) return true;
                    if (Status s = AppendVersion(store, access, ix, row.pk, version.image);
                        !s.ok()) {
                        return false;
                    }
                    return true;
                });
            if (!walked.ok()) return walked;
        }

        leaf = next;
    }
    return ix.root_page_id;
}

}  // namespace

StatusOr<catalog::Catalog::IndexDef> PrepareIndexDef(catalog::Catalog& catalog,
                                                     const parser::IndexStmt& stmt,
                                                     const txn::ReadView* view,
                                                     catalog::Catalog::AnchorSeed seed) {
    auto oid = catalog.FindTableOidByName(stmt.table_name, view);
    if (!oid.ok()) {
        return Status::NotFound("no relation named '" + stmt.table_name + "' (byte " +
                                std::to_string(stmt.table_byte_offset) + ")");
    }
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
    def.entry_width = static_cast<std::uint16_t>(layout.leaf_entry_width());

    // Every refusal, before a page is walked. `Catalog::CreateIndex` runs
    // the same check again at the write - one implementation, two callers
    // - so this buys the *position* of the failure and nothing else: a
    // heap relation refused by name rather than as a page-type error from
    // inside the build.
    if (Status s = catalog.CheckIndexDef(def, seed); !s.ok()) return s;

    // The oid, issued before any page exists so the root and every page
    // the backfill splits off carry it from birth (page.md §2a). Burned
    // by a build that then fails, which the row-id sequence permits.
    auto pre_oid = catalog.AllocateRowId(catalog::kSysIndexesTable);
    if (!pre_oid.ok()) return pre_oid.status();
    def.index_oid = pre_oid.value();
    return def;
}

StatusOr<PageId> BuildIndexTree(storage::PageStore& store, const catalog::TableAccess& access,
                                const catalog::Catalog::IndexDef& def, std::uint64_t trx_id,
                                wal::WalManager* wal) {
    // The layout the definition's widths encode: `PrepareIndexDef` stored
    // `entry_width` as `leaf_entry_width()`, so the covered width is what
    // is left after the key and the pk. Re-derived rather than carried, so
    // a definition crosses a ring as the plain `IndexDef` - and guarded,
    // because one that did is bytes this core did not compute.
    // `FormatRoot` re-checks the layout as a whole.
    if (def.entry_width < def.key_width + index::kIndexPkWidth) {
        return Status::InvalidArgument("index oid " + std::to_string(def.index_oid) +
                                       ": entry width " + std::to_string(def.entry_width) +
                                       " is narrower than its key and pk");
    }
    // The same guard for the column counts, and here it is memory safety
    // rather than a message: `IndexRef`'s arrays are fixed at the caps, and
    // the assembly below writes one element per declared column. A
    // definition this core computed came through `CheckIndexDef`, which
    // refuses over-cap and empty; one that crossed a ring did not.
    if (def.key_cols.empty() || def.key_cols.size() > catalog::kMaxIndexKeyColumns ||
        def.covered_cols.size() > catalog::kMaxIndexCoveredColumns) {
        return Status::InvalidArgument(
            "index oid " + std::to_string(def.index_oid) + ": " +
            std::to_string(def.key_cols.size()) + " key and " +
            std::to_string(def.covered_cols.size()) +
            " covered columns is not a shape an index entry has");
    }
    index::IndexLayout layout;
    layout.key_width = def.key_width;
    layout.covered_width =
        static_cast<std::uint16_t>(def.entry_width - def.key_width - index::kIndexPkWidth);

    // The root, scoped so its pin drops before the walk below fetches pages.
    PageId root_id = kInvalidPageId;
    {
        auto created = store.CreateNew();
        if (!created.ok()) return created.status();
        auto& [id, bytes] = created.value();
        if (Status s = index::FormatRoot(bytes.bytes(), layout, def.index_oid); !s.ok()) return s;
        root_id = id;
    }

    catalog::TableAccess::IndexRef building;
    building.index_oid = def.index_oid;  // pre-issued; the backfill stamps pages with it
    building.root_page_id = root_id;
    building.key_width = def.key_width;
    building.entry_width = def.entry_width;
    building.nkeys = static_cast<std::uint8_t>(def.key_cols.size());
    building.ncovered = static_cast<std::uint8_t>(def.covered_cols.size());
    for (std::size_t i = 0; i < def.key_cols.size(); ++i) building.key_cols[i] = def.key_cols[i];
    for (std::size_t i = 0; i < def.covered_cols.size(); ++i) {
        building.covered_cols[i] = def.covered_cols[i];
    }

    auto final_root = Backfill(store, access, building);
    if (!final_root.ok()) return final_root.status();

    // RV3: the built tree travels as full page images, **before** the
    // sys.indexes row that publishes it - a crash after the commit must
    // find a tree the recovered catalog row can probe, and the backfill
    // logged nothing per entry. An FPI both creates and fills a page at
    // redo, so no PAGE_INIT is needed; the cost is tree bytes per CREATE
    // INDEX, on a statement that is already O(relation).
    if (Status s = LogBuiltTree(wal, store, trx_id, final_root.value()); !s.ok()) {
        return s.WithContext("logging the built tree");
    }
    return final_root.value();
}

StatusOr<IndexDdlResult> CreateIndex(catalog::Catalog& catalog, storage::PageStore& store,
                                     const parser::IndexStmt& stmt, std::uint64_t trx_id,
                                     catalog::CatalogRowRef* written,
                                     const txn::ReadView* view, wal::WalManager* wal) {
    auto prepared = PrepareIndexDef(catalog, stmt, view);
    if (!prepared.ok()) return prepared.status();
    catalog::Catalog::IndexDef& def = prepared.value();

    auto access = catalog.InitTableAccess(def.table_oid);
    if (!access.ok()) return access.status();
    auto root = BuildIndexTree(store, *access.value(), def, trx_id, wal);
    if (!root.ok()) {
        return root.status().WithContext("building index '" + stmt.index_name + "'");
    }
    def.root_page_id = root.value();

    auto index_oid = catalog.CreateIndex(def, trx_id, written);
    if (!index_oid.ok()) return index_oid.status();

    IndexDdlResult out;
    out.index_oid = index_oid.value();
    out.rel_oid = def.table_oid;
    out.root_page_id = def.root_page_id;
    out.key_width = def.key_width;
    out.entry_width = def.entry_width;

    // Collected after the write, because none of them is a reason not to
    // write.
    out.warnings = IndexCreationWarnings(catalog, def, stmt.key_columns[0].name);
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
                                  std::uint64_t trx_id, catalog::CatalogRowChange* change) {
    auto row = catalog.FindIndexByName(stmt.index_name);
    if (!row.ok()) {
        return Status::NotFound("no index named '" + stmt.index_name + "' (byte " +
                                std::to_string(stmt.byte_offset) + ")");
    }
    if (Status s = catalog.DropIndex(row.value().index_oid, trx_id, change); !s.ok()) {
        // The byte belongs to the statement, and only this layer has it.
        // `WithContext` would prefix instead of append and would bury the
        // position mid-sentence, so the one refusal that names the user's
        // object gets it spelled out the way the sibling above does.
        if (s.code() == StatusCode::kUnsupported) {
            return Status::Unsupported("index '" + stmt.index_name + "': " + s.message() +
                                       " (byte " + std::to_string(stmt.byte_offset) + ")");
        }
        return s;
    }
    return row.value().index_oid;
}

}  // namespace kds::exec
