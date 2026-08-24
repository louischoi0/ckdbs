#include "kds/exec/index_maintain.hpp"

#include <array>
#include <cstring>
#include <string>

#include "kds/exec/index_key.hpp"
#include "kds/exec/row_codec.hpp"
#include "kds/storage/index/index_page.hpp"
#include "kds/storage/index/index_tree.hpp"

namespace kds::exec {

namespace {

// The value for schema column `col`, coerced into its column's storage form.
//
// The coercion is the whole reason this is a function rather than an index
// into `values`: `values` holds literals as written on an INSERT, so a DATE
// column's value is still the string, while the key encoding compares epoch
// integers. See the header for what a second coercion path cost the Cabin.
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

}  // namespace

StatusOr<PageId> AppendIndexEntry(storage::PageStore& store,
                                  const catalog::TableAccess& access,
                                  const catalog::TableAccess::IndexRef& ix,
                                  std::span<const parser::AstValue> values,
                                  std::uint16_t first_col_pos, std::span<const std::byte> row,
                                  std::uint64_t pk,
                                  std::span<const parser::AstValue> previous,
                                  std::vector<IndexWrite>* logged) {
    const bool is_update = !previous.empty();

    index::IndexLayout layout;
    layout.key_width = ix.key_width;
    layout.covered_width =
        static_cast<std::uint16_t>(ix.entry_width - ix.key_width - index::kIndexPkWidth);

    // Uninitialized on purpose: every byte of the key is written by the
    // encoder, which errors if the widths disagree, and zeroing 4 KB per
    // index per row to satisfy a habit is not free.
    std::array<std::byte, index::kMaxIndexEntryWidth> key_buf;
    std::span<std::byte> key(key_buf.data(), layout.key_width);

    // One pass: encode the key and decide whether the write touched it.
    // Encoding unconditionally wastes a stack memcpy on the untouched
    // path and costs no allocation; testing first would coerce every
    // value twice on the path that does append.
    bool touched = !is_update;
    std::size_t at = 0;
    for (std::uint16_t col : ix.keys()) {
        auto value = ValueFor(access, values, first_col_pos, col);
        if (!value.ok()) return value.status();

        auto width = IndexKeyColumnWidth(access.schema.columns[col]);
        if (!width.ok()) return width.status();
        if (at + width.value() > key.size()) {
            return Status::Corruption(
                "index '" + std::to_string(ix.index_oid) + "' declares key_width " +
                std::to_string(ix.key_width) +
                " but its columns encode to more; the catalog row and the schema disagree");
        }
        if (Status s = EncodeIndexKeyColumn(access.schema.columns[col], value.value(),
                                            key.subspan(at, width.value()));
            !s.ok()) {
            return s;
        }
        at += width.value();

        if (is_update && !touched) touched = Moved(access, previous, col, value.value());
    }
    if (at != key.size()) {
        return Status::Corruption("index '" + std::to_string(ix.index_oid) +
                                  "' key encoded to " + std::to_string(at) +
                                  " bytes, but its catalog row declares " +
                                  std::to_string(ix.key_width));
    }

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

    auto placed = index::IndexInsert(store, ix.root_page_id, layout, key, pk, covered,
                                     ix.index_oid);
    if (!placed.ok()) {
        // Failed shut. An index missing an entry is a row lost to every
        // later probe, so unlike the Cabin's hook there is nothing to
        // absorb this with.
        return placed.status().WithContext("maintaining index " +
                                           std::to_string(ix.index_oid));
    }

    // A byte-identical entry was already there, so no page changed and there
    // is nothing to log (index_tree.hpp).
    if (logged != nullptr && !placed.value().already_present) {
        IndexWrite write;
        write.page_id = placed.value().page_id;
        write.slot = placed.value().slot;
        write.entry.assign(key.begin(), key.end());
        // The pk and the covered bytes as the tree composed them - re-read
        // from the page rather than rebuilt here, so the record and the page
        // cannot disagree about what was written.
        auto page = store.GetForRead(placed.value().page_id);
        if (!page.ok()) return page.status();
        index::IndexLeafView leaf(std::span<std::byte, kPageSize>(page.value().bytes().data(), kPageSize));
        auto stored = leaf.Entry(placed.value().slot);
        if (!stored.ok()) return stored.status();
        write.entry.assign(stored.value().begin(), stored.value().end());
        for (const index::IndexChange& change : placed.value().changes()) {
            write.restructured.push_back(change.page_id);
        }
        logged->push_back(std::move(write));
    }
    return placed.value().new_root;
}

Status MaintainIndexes(catalog::Catalog& catalog, storage::PageStore& store,
                       const catalog::TableAccess& access,
                       std::span<const parser::AstValue> values, std::uint16_t first_col_pos,
                       std::span<const std::byte> row, std::uint64_t pk,
                       std::span<const parser::AstValue> previous,
                       std::vector<IndexWrite>* logged) {
    // The one test a relation with no index pays.
    if (access.indexes.empty()) return Status::OK();

    for (const catalog::TableAccess::IndexRef& ix : access.indexes) {
        auto moved = AppendIndexEntry(store, access, ix, values, first_col_pos, row, pk,
                                      previous, logged);
        if (!moved.ok()) return moved.status();
        if (moved.value() == kInvalidPageId) continue;

        // A split grew the tree, so this index's root moved. Republished
        // through the catalog, which updates the cached entry **in place** -
        // so `access`, and `ix` inside it, stay valid for the next iteration
        // and for the caller.
        if (Status s = catalog.UpdateIndexRoot(access.oid, ix.index_oid, moved.value(),
                                               access.anchor_page_id);
            !s.ok()) {
            return s.WithContext("republishing the root of index " +
                                 std::to_string(ix.index_oid));
        }
    }
    return Status::OK();
}

}  // namespace kds::exec
