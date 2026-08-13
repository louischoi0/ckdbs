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
    // pointer against pages and carry no allocation state.
    txn::UndoLog undo(store);

    auto first = btree::BtreeSeekLeaf(store, access.desc_page_id, 0);
    if (!first.ok()) return first.status();

    PageId leaf = first.value();
    for (std::uint32_t leaves = 0; leaf != kInvalidPageId; ++leaves) {
        if (leaves >= heap::kMaxChainPages) {
            return Status::Corruption("relation leaf chain exceeds " +
                                      std::to_string(heap::kMaxChainPages) + " pages");
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

StatusOr<IndexDdlResult> CreateIndex(catalog::Catalog& catalog, storage::PageStore& store,
                                     const parser::IndexStmt& stmt) {
    auto oid = catalog.FindTableOidByName(stmt.table_name);
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

    // The root, allocated and formatted before the row that names it. The
    // reverse order would leave a catalog row pointing at a page that does
    // not exist, which is a worse failure than a page nothing points at.
    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [root_id, root_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> root_bytes = root_bytes_ref.bytes();
    if (Status s = index::FormatRoot(root_bytes, layout); !s.ok()) return s;
    def.root_page_id = root_id;

    // Every refusal, before a page is walked. `Catalog::CreateIndex` runs
    // the same check again at the write below - one implementation, two
    // callers - so this buys the *position* of the failure and nothing else:
    // a heap relation refused by name rather than as a page-type error from
    // inside the build.
    if (Status s = catalog.CheckIndexDef(def); !s.ok()) return s;

    // ---- Built before it is published (spec §10a) -----------------------
    //
    // The backfill runs *before* the sys.indexes row exists, so an index is
    // published complete or not at all - a failure here leaves an
    // unreachable tree and no catalog row, where the reverse order would
    // leave a declared index missing rows, which is a wrong answer with a
    // right answer's shape.
    //
    // Nothing can observe the half-built tree in between: DDL is one
    // statement on one cooperative thread, so there is no reader to race.
    // A split during the build moves the root, which is why the final root
    // comes back from here rather than being the page allocated above.
    catalog::TableAccess::IndexRef building;
    building.index_oid = 0;  // unpublished; nothing reads this field here
    building.root_page_id = root_id;
    building.key_width = def.key_width;
    building.entry_width = def.entry_width;
    building.nkeys = static_cast<std::uint8_t>(def.key_cols.size());
    building.ncovered = static_cast<std::uint8_t>(def.covered_cols.size());
    for (std::size_t i = 0; i < def.key_cols.size(); ++i) building.key_cols[i] = def.key_cols[i];
    for (std::size_t i = 0; i < def.covered_cols.size(); ++i) {
        building.covered_cols[i] = def.covered_cols[i];
    }

    auto final_root = Backfill(store, *access.value(), building);
    if (!final_root.ok()) {
        return final_root.status().WithContext("building index '" + stmt.index_name + "'");
    }
    def.root_page_id = final_root.value();

    // The heap-relation, pk-column, duplicate-column, over-cap, UNIQUE and
    // duplicate-name refusals all live in the catalog and are deliberately
    // not restated here: two answers to "why not" is how one of them ends up
    // wrong.
    auto index_oid = catalog.CreateIndex(def);
    if (!index_oid.ok()) return index_oid.status();

    IndexDdlResult out;
    out.index_oid = index_oid.value();
    out.rel_oid = def.table_oid;
    out.root_page_id = def.root_page_id;
    out.key_width = def.key_width;
    out.entry_width = def.entry_width;

    // Collected after the write, because none of them is a reason not to
    // write. An index is complete for every value where a Cabin is
    // authoritative only for observed ones, so the Cabin becomes dead weight
    // - but dropping it is the operator's call, not the engine's.
    if (catalog.FindCabinOnColumn(def.table_oid, def.key_cols[0]).ok()) {
        out.warnings.push_back("column '" + stmt.key_columns[0].name +
                               "' already carries a cabin; this index supersedes it for every "
                               "value, not just observed ones, and the cabin's write hook and "
                               "memory now buy nothing (DROP CABIN to reclaim them)");
    }
    return out;
}

StatusOr<catalog::Oid> DropIndex(catalog::Catalog& catalog, const parser::IndexStmt& stmt) {
    auto row = catalog.FindIndexByName(stmt.index_name);
    if (!row.ok()) {
        return Status::NotFound("no index named '" + stmt.index_name + "' (byte " +
                                std::to_string(stmt.byte_offset) + ")");
    }
    if (Status s = catalog.DropIndex(row.value().index_oid); !s.ok()) return s;
    return row.value().index_oid;
}

}  // namespace kds::exec
