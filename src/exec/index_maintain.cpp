#include "kds/exec/index_maintain.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "kds/exec/index_key.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/storage/anchor_page.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/insert_placement.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"
#include "kds/txn/visibility.hpp"

namespace kds::exec {

namespace {

// The value for schema column `col`, coerced into its column's storage form.
//
// The coercion is the whole reason this is a function rather than an index
// into `values`: `values` holds literals as written on an INSERT, so a DATE
// column's value is still the string, while the key encoding compares epoch
// integers. `CoerceLiteralToColumn` (row_codec.hpp) says why it is the only
// coercion path.
StatusOr<parser::AstValue> ValueFor(const catalog::TableAccess& access,
                                    std::span<const parser::AstValue> values,
                                    std::uint16_t first_col_pos, std::uint16_t col) {
    if (col < first_col_pos) {
        return Status::InvalidArgument("index maintenance has no value for column " +
                                       std::to_string(col));
    }
    const std::size_t at = static_cast<std::size_t>(col - first_col_pos);
    if (at >= values.size()) {
        return Status::InvalidArgument("index maintenance has no value for column " +
                                       std::to_string(col));
    }
    parser::AstValue value = values[at];
    if (Status s = CoerceLiteralToColumn(access.schema.columns[col], value); !s.ok()) {
        return s.WithContext("index key column '" +
                             std::string(catalog::NameView(access.schema.columns[col].name)) +
                             "'");
    }
    return value;
}

// Whether an UPDATE moved `col`. `previous` is the decoded row before the
// write, so it is already in storage form and the comparison is against the
// coerced new value - against the raw literal a decoded date never compares
// equal to the string it was written as, and the check silently never fires.
bool Moved(const catalog::TableAccess& access, std::span<const parser::AstValue> previous,
           std::uint16_t col, const parser::AstValue& now) {
    if (col >= previous.size()) return true;  // nothing to compare against
    return !CompareValues(access.schema.columns[col].type_val, previous[col], now,
                          parser::CompareOp::kEq);
}

index::IndexLayout LayoutOf(const catalog::TableAccess::IndexRef& ix) {
    index::IndexLayout layout;
    layout.key_width = ix.key_width;
    layout.covered_width =
        static_cast<std::uint16_t>(ix.entry_width - ix.key_width - index::kIndexTupleKeyWidth);
    return layout;
}

// Encodes `ix`'s key from `values` into `out` (`key_width` bytes), and
// answers whether the write moved a key column - always, for an INSERT's
// empty `previous`. `coerced`, when given, receives each key column's value
// in its column's storage form, in declared order: what a unique check
// compares a resolved row against.
StatusOr<bool> EncodeKeyInto(const catalog::TableAccess& access,
                             const catalog::TableAccess::IndexRef& ix,
                             std::span<const parser::AstValue> values,
                             std::uint16_t first_col_pos,
                             std::span<const parser::AstValue> previous, std::span<std::byte> out,
                             std::vector<parser::AstValue>* coerced) {
    bool moved = previous.empty();
    std::size_t at = 0;
    for (std::uint16_t col : ix.keys()) {
        auto value = ValueFor(access, values, first_col_pos, col);
        if (!value.ok()) return value.status();

        auto width = IndexKeyColumnWidth(access.schema.columns[col]);
        if (!width.ok()) return width.status();
        if (at + width.value() > out.size()) {
            return Status::Corruption(
                "index '" + std::to_string(ix.index_oid) + "' declares key_width " +
                std::to_string(ix.key_width) +
                " but its columns encode to more; the catalog row and the schema disagree");
        }
        if (Status s = EncodeIndexKeyColumn(access.schema.columns[col], value.value(),
                                            out.subspan(at, width.value()));
            !s.ok()) {
            return s;
        }
        at += width.value();

        if (!moved) moved = Moved(access, previous, col, value.value());
        if (coerced != nullptr) coerced->push_back(std::move(value.value()));
    }
    if (at != out.size()) {
        return Status::Corruption("index '" + std::to_string(ix.index_oid) +
                                  "' key encoded to " + std::to_string(at) +
                                  " bytes, but its catalog row declares " +
                                  std::to_string(ix.key_width));
    }
    return moved;
}

}  // namespace

StatusOr<PageId> SlotRoot(storage::PageStore& store, PageId anchor, std::uint64_t index_oid) {
    auto bytes = store.GetForRead(anchor);
    if (!bytes.ok()) return bytes.status();
    auto root = storage::AnchorIndexRoot(bytes.value().bytes(), index_oid);
    if (!root.ok()) return root.status();
    if (root.value() == kInvalidPageId) {
        return Status::Corruption("index oid " + std::to_string(index_oid) +
                                  " has no slot in its relation's anchor page " +
                                  std::to_string(anchor));
    }
    return root.value();
}

StatusOr<PageId> AppendIndexEntry(storage::PageStore& store,
                                  const catalog::TableAccess& access,
                                  const catalog::TableAccess::IndexRef& ix,
                                  std::span<const parser::AstValue> values,
                                  std::uint16_t first_col_pos, std::span<const std::byte> row,
                                  std::uint64_t tuple_key,
                                  std::span<const parser::AstValue> previous,
                                  const IndexWriteLog* log, catalog::Catalog* publish) {
    const bool is_update = !previous.empty();
    const index::IndexLayout layout = LayoutOf(ix);

    // Uninitialized on purpose: every byte of the key is written by the
    // encoder, which errors if the widths disagree, and zeroing 4 KB per
    // index per row to satisfy a habit is not free.
    std::array<std::byte, index::kMaxIndexEntryWidth> key_buf;
    std::span<std::byte> key(key_buf.data(), layout.key_width);

    // One pass: encode the key and decide whether the write touched it.
    // Encoding unconditionally wastes a stack memcpy on the untouched
    // path and costs no allocation; testing first would coerce every
    // value twice on the path that does append.
    auto key_moved = EncodeKeyInto(access, ix, values, first_col_pos, previous, key,
                                   /*coerced=*/nullptr);
    if (!key_moved.ok()) return key_moved.status();
    bool touched = key_moved.value();

    // A covered column moving matters too: the entry carries its value,
    // and the read path filters on it, so an entry with the old value
    // would drop a row that now matches.
    std::array<std::byte, index::kMaxIndexEntryWidth> covered_buf;
    std::span<std::byte> covered(covered_buf.data(), layout.covered_width);
    std::size_t covered_at = 0;
    for (std::uint16_t col : ix.covered()) {
        auto width = catalog::RowLayout::ColumnWidth(access.schema.columns[col],
                                                      access.layout.inline_cell_width);
        if (!width.ok()) return width.status();

        const std::size_t offset = access.layout.offsets[col];
        if (offset + width.value() > row.size() ||
            covered_at + width.value() > covered.size()) {
            return Status::Corruption("index '" + std::to_string(ix.index_oid) +
                                      "' covered column " + std::to_string(col) +
                                      " does not fit the row it was written from");
        }
        // Verbatim from the encoded tuple - byte-identical to the page
        // by construction, spill pointer included.
        std::memcpy(covered.data() + covered_at, row.data() + offset, width.value());
        covered_at += width.value();

        if (is_update && !touched) {
            auto value = ValueFor(access, values, first_col_pos, col);
            if (!value.ok()) return value.status();
            touched = Moved(access, previous, col, value.value());
        }
    }

    // **§2's rule: an UPDATE that moved nothing this index cares about
    // appends nothing.** Correct either way by IX1's superset rule, and
    // unbounded the other way - one entry per write, forever.
    if (!touched) return kInvalidPageId;

    const PageId anchor =
        log != nullptr || publish != nullptr ? access.anchor_page_id : kInvalidPageId;
    PageId root = ix.root_page_id;
    auto placed = index::IndexInsert(store, root, layout, key, tuple_key, covered, ix.index_oid,
                                     anchor);
    // CI-Q11 (b): a root another core grew past this one's memo - an online
    // build's backfill, or a writer - refuses the descent with nothing
    // written; the slot names the tree's root now.
    for (int retry = 0; anchor != kInvalidPageId && !placed.ok() &&
                        placed.status().code() == StatusCode::kTxnConflict &&
                        retry < storage::kMaxDescentRestarts;
         ++retry) {
        auto current = SlotRoot(store, anchor, ix.index_oid);
        if (!current.ok() || current.value() == root) break;
        root = current.value();
        placed = index::IndexInsert(store, root, layout, key, tuple_key, covered, ix.index_oid,
                                    anchor);
    }
    if (!placed.ok()) {
        // Failed shut. An index missing an entry is a row lost to every
        // later probe, so unlike the Cabin's hook there is nothing to
        // absorb this with.
        return placed.status().WithContext("maintaining index " +
                                           std::to_string(ix.index_oid));
    }

    // A byte-identical entry was already there, so no page changed and there
    // is nothing to log (index_tree.hpp).
    if (log != nullptr && !placed.value().already_present) {
        IndexWrite write;
        write.page_id = placed.value().page_id;
        write.slot = placed.value().slot;
        // The entry as the tree composed it - tuple key and covered bytes included -
        // read at its slot **under the hold the tree handed out** (AT-S21),
        // so the slot is still this entry's and the record and the page
        // cannot disagree about what was written.
        auto page = store.GetForRead(placed.value().page_id);
        if (!page.ok()) return page.status();
        index::IndexLeafView leaf(
            std::span<std::byte, kPageSize>(page.value().bytes().data(), kPageSize));
        auto stored = leaf.Entry(placed.value().slot);
        if (!stored.ok()) return stored.status();
        write.entry.assign(stored.value().begin(), stored.value().end());
        page.value().Release();
        for (const index::IndexChange& change : placed.value().changes()) {
            write.restructured.push_back(change.page_id);
        }
        write.new_root = placed.value().new_root;
        write.rel_oid = access.oid;
        write.index_oid = ix.index_oid;
        write.anchor_page_id = access.anchor_page_id;
        write.anchor_pin = &placed.value().anchor;
        write.held = placed.value().held;
        if (Status s = (*log)(write); !s.ok()) return s;
        // The log publishes a root it carries, so there is none to report.
        return kInvalidPageId;
    }
    if (publish == nullptr || placed.value().new_root == kInvalidPageId) {
        return placed.value().new_root;  // the holds drop here, after the log
    }
    // A split grew the tree with no log to carry the root, so it is
    // republished here, under the divide's holds and its pin on the anchor.
    // The catalog updates the cached entry **in place**, so `access`, and
    // `ix` inside it, stay valid for the next index and for the caller.
    if (Status s = publish->PublishRoot(access.oid, ix.index_oid, placed.value().new_root,
                                        access.anchor_page_id);
        !s.ok()) {
        return s.WithContext("republishing the root of index " +
                             std::to_string(ix.index_oid));
    }
    return kInvalidPageId;
}

// ---- Unique indexes (BX) --------------------------------------------------

StatusOr<std::optional<UniqueKey>> EncodeUniqueKey(const catalog::TableAccess& access,
                                                   const catalog::TableAccess::IndexRef& ix,
                                                   std::span<const parser::AstValue> values,
                                                   std::uint16_t first_col_pos,
                                                   std::span<const parser::AstValue> previous) {
    UniqueKey out;
    out.encoded.resize(ix.key_width);
    out.values.reserve(ix.nkeys);
    auto moved =
        EncodeKeyInto(access, ix, values, first_col_pos, previous, out.encoded, &out.values);
    if (!moved.ok()) return moved.status();
    if (!moved.value()) return std::optional<UniqueKey>{};
    return std::optional<UniqueKey>{std::move(out)};
}

txn::LockKey UniqueKeyUnit(const catalog::TableAccess& access,
                           const catalog::TableAccess::IndexRef& ix, const UniqueKey& key) {
    // FNV-1a over the index oid and the encoded key. Not a persisted format:
    // the unit lives and dies in memory, and the lock table mixes `lo` again
    // when it picks a partition.
    std::uint64_t h = 0xCBF29CE484222325ULL;
    const auto feed = [&h](std::uint8_t b) {
        h ^= b;
        h *= 0x100000001B3ULL;
    };
    for (int i = 0; i < 8; ++i) {
        feed(static_cast<std::uint8_t>(std::uint64_t{ix.index_oid} >> (8 * i)));
    }
    for (std::byte b : key.encoded) feed(static_cast<std::uint8_t>(b));
    return txn::LockKey::IndexKey(access.oid, h);
}

StatusOr<UniqueVerdict> CheckUniqueKey(storage::PageStore& store,
                                       const catalog::TableAccess& access,
                                       const catalog::TableAccess::IndexRef& ix,
                                       const UniqueKey& key, const txn::ReadView& check_view,
                                       std::uint64_t self) {
    const index::IndexLayout layout = LayoutOf(ix);

    // Phase 1: the tuple keys the run names, distinct. The seek is `(key, 0)`
    // - the tuple key is the sort key's tail (IX4b) - and the walk stops at
    // the first larger key. Collected before any is resolved, so no
    // clustered page is fetched under an index leaf's span (`parser-v2.md`
    // I15's R1).
    std::vector<std::byte> low(layout.sort_key_width(), std::byte{0x00});
    std::memcpy(low.data(), key.encoded.data(), key.encoded.size());
    auto first = index::IndexSeekLeaf(store, ix.root_page_id, layout, low);
    if (!first.ok()) return first.status();
    std::vector<std::uint64_t> named;
    Status walked = index::IndexVisitFrom(
        store, first.value(), layout, storage::PageAccess::kRead,
        [&](PageId, index::IndexLeafView& leaf,
            std::uint16_t at) -> StatusOr<storage::VisitControl> {
            auto entry = leaf.Entry(at);
            if (!entry.ok()) return entry.status();
            const int order =
                std::memcmp(entry.value().data(), key.encoded.data(), key.encoded.size());
            if (order < 0) return storage::VisitControl::kContinue;
            if (order > 0) return storage::VisitControl::kStop;
            named.push_back(index::GetIndexTupleKey(entry.value().subspan(ix.key_width)));
            return storage::VisitControl::kContinue;
        });
    if (!walked.ok()) return walked;
    std::sort(named.begin(), named.end());
    named.erase(std::unique(named.begin(), named.end()), named.end());

    // Phase 2: each named row at latest state, its key columns compared in
    // full - the encoded key matched at most a prefix (IX6).
    std::uint64_t mask = 0;
    for (std::uint16_t col : ix.keys()) {
        mask = col < 64 ? mask | (std::uint64_t{1} << col) : kAllColumnsMask;
    }
    std::vector<parser::AstValue> row(access.schema.columns.size());
    std::vector<std::byte> payload;
    std::vector<PendingSpill> spills;
    for (std::uint64_t tuple_key : named) {
        if (tuple_key == self) continue;
        auto at = btree::BtreeLookup(store, access.desc_page_id, tuple_key);
        if (!at.ok()) {
            // Rolled back, purged, or never placed: it holds nothing.
            if (at.status().code() == StatusCode::kNotFound) continue;
            return at.status();
        }
        auto version = heap::PageView(at.value().leaf.bytes()).ReadTuple(at.value().slot);
        if (!version.ok()) return version.status();
        const txn::CheckVerdict verdict = txn::CheckVisibility(check_view, version.value());
        if (verdict == txn::CheckVerdict::kAbsent) continue;
        if (verdict == txn::CheckVerdict::kBusy) {
            return UniqueVerdict{UniqueVerdict::Kind::kBusy, tuple_key, version.value().trx_id};
        }
        // Copied out and the leaf released before the decode: a spilled key
        // value is a var-heap fetch, never taken under a live span.
        payload.assign(version.value().payload.begin(), version.value().payload.end());
        at.value().leaf.Release();
        if (Status s = DecodeAndResolve(store, access.schema, access.layout, payload, row, mask,
                                        spills);
            !s.ok()) {
            return s;
        }
        bool equal = true;
        for (std::size_t i = 0; i < ix.nkeys && equal; ++i) {
            const std::uint16_t col = ix.key_cols[i];
            equal = CompareValues(access.schema.columns[col].type_val, row[col], key.values[i],
                                  parser::CompareOp::kEq);
        }
        if (equal) return UniqueVerdict{UniqueVerdict::Kind::kHeld, tuple_key, 0};
    }
    return UniqueVerdict{};
}

std::string UniqueKeyText(const catalog::TableAccess& access,
                          const catalog::TableAccess::IndexRef& ix, const UniqueKey& key) {
    std::string columns;
    std::string values;
    for (std::size_t i = 0; i < ix.nkeys; ++i) {
        const catalog::SysColumnRow& col = access.schema.columns[ix.key_cols[i]];
        if (i > 0) {
            columns += ", ";
            values += ", ";
        }
        columns += catalog::NameView(col.name);
        values += FormatValue(col.type_val, key.values[i]);
    }
    return "unique index '" + ix.name + "' on (" + columns + "): (" + values + ")";
}

Status UniqueViolation(const catalog::TableAccess& access,
                       const catalog::TableAccess::IndexRef& ix, const UniqueKey& key,
                       std::uint64_t holder, std::uint32_t byte) {
    return Status::AlreadyExists(UniqueKeyText(access, ix, key) +
                                 " is already held by tuple key " + std::to_string(holder) +
                                 " (byte " + std::to_string(byte) + ")");
}

Status MaintainIndexes(catalog::Catalog& catalog, storage::PageStore& store,
                       const catalog::TableAccess& access,
                       std::span<const parser::AstValue> values, std::uint16_t first_col_pos,
                       std::span<const std::byte> row, std::uint64_t tuple_key,
                       std::span<const parser::AstValue> previous,
                       const IndexWriteLog* log) {
    // The one test a relation with no index pays.
    if (access.indexes.empty()) return Status::OK();

    for (const catalog::TableAccess::IndexRef& ix : access.indexes) {
        // With a log the root rides in the split's record
        // (`IndexWrite::new_root`); without one it is published inside.
        auto moved = AppendIndexEntry(store, access, ix, values, first_col_pos, row, tuple_key,
                                      previous, log, &catalog);
        if (!moved.ok()) return moved.status();
    }
    return Status::OK();
}

}  // namespace kds::exec
