#include "kds/storage/index/index_tree.hpp"

#include <cstring>
#include <string>

#include "kds/storage/heap/heap_chain.hpp"  // kMaxChainPages: one cycle guard, not two
#include "kds/storage/page_header.hpp"

namespace kds::index {

namespace {

bool IsLeafPage(std::span<const std::byte, kPageSize> page) {
    return storage::RawPageType(page) == static_cast<std::uint8_t>(PageType::kIndexLeaf);
}

// A page's type as the tree understands it. Anything else means the caller
// is not holding the tree it thinks it is - a clustered-tree root reached
// through here, or a corrupted child pointer - and is reported rather than
// parsed.
Status RequireType(std::span<const std::byte, kPageSize> page, PageId page_id, PageType want) {
    const std::uint8_t raw = storage::RawPageType(page);
    if (raw == static_cast<std::uint8_t>(want)) return Status::OK();
    return Status::Corruption("index page " + std::to_string(page_id) + " has page_type " +
                              std::to_string(raw) + ", expected " +
                              std::to_string(static_cast<std::uint8_t>(want)));
}

// A fetched page back at its true fixed extent. The store hands out dynamic
// spans; anything reaching here is exactly one page long.
std::span<std::byte, kPageSize> AsPage(std::span<std::byte> bytes) {
    return std::span<std::byte, kPageSize>(bytes.data(), kPageSize);
}

// The root-to-leaf path an insert descended, so a split can walk back up.
// path[0] is the root; path[depth] is the leaf.
struct Descent {
    std::array<PageId, storage::kMaxBtreeDepth> path{};
    std::uint16_t depth = 0;
    // The leaf, held: the pin rides in the struct (btree.cpp's Descent
    // says why; the same Shape C lived here). Peak pins (MG03): 1.
    storage::PageRef leaf;
};

// Follows child pointers for `sort_key` from `root`, recording the path and
// validating every page's stored widths against `layout` on the way - the
// checked half of index_page.hpp's redundancy, applied at the one place
// every page of the tree passes through.
//
// Internal nodes are never modified by a descent, so they are fetched
// read-only; `leaf_for_write` says whether the *leaf* is about to be
// mutated, which is what keeps a read-only statement from scheduling a
// write-back of everything it read (page_store.hpp).
StatusOr<Descent> DescendTo(storage::PageStore& store, PageId root, const IndexLayout& layout,
                            std::span<const std::byte> sort_key, bool leaf_for_write) {
    Descent d;
    PageId current = root;
    for (;;) {
        if (d.depth >= storage::kMaxBtreeDepth) {
            return Status::Corruption("index descent from page " + std::to_string(root) +
                                      " exceeded " + std::to_string(storage::kMaxBtreeDepth) +
                                      " levels; the child pointers are cyclic or corrupt");
        }
        d.path[d.depth] = current;

        auto bytes = store.GetForRead(current);
        if (!bytes.ok()) return bytes.status();

        if (IsLeafPage(bytes.value().bytes())) {
            if (Status s = IndexLeafView(AsPage(bytes.value().bytes())).CheckAgainst(layout, current);
                !s.ok()) {
                return s;
            }
            if (!leaf_for_write) {
                d.leaf = std::move(bytes.value());
                return d;
            }
            // Re-fetch for write: the frame is already resident, so this is
            // a hash lookup that flips the dirty flag.
            auto writable = store.Get(current);
            if (!writable.ok()) return writable.status();
            d.leaf = std::move(writable.value());
            return d;
        }

        if (Status s = RequireType(bytes.value().bytes(), current, PageType::kIndexInternal); !s.ok()) {
            return s;
        }
        IndexInternalView node(AsPage(bytes.value().bytes()));
        if (Status s = node.CheckAgainst(layout, current); !s.ok()) return s;

        current = node.ChildFor(sort_key);
        if (current == kInvalidPageId) {
            return Status::Corruption("index node " + std::to_string(d.path[d.depth]) +
                                      " routed to an invalid child");
        }
        ++d.depth;
    }
}

StatusOr<PageId> LeftmostLeaf(storage::PageStore& store, PageId root, const IndexLayout& layout) {
    PageId current = root;
    for (std::uint16_t level = 0;; ++level) {
        if (level >= storage::kMaxBtreeDepth) {
            return Status::Corruption("index from page " + std::to_string(root) + " exceeded " +
                                      std::to_string(storage::kMaxBtreeDepth) + " levels");
        }
        auto bytes = store.GetForRead(current);
        if (!bytes.ok()) return bytes.status();
        if (IsLeafPage(bytes.value().bytes())) {
            if (Status s = IndexLeafView(AsPage(bytes.value().bytes())).CheckAgainst(layout, current);
                !s.ok()) {
                return s;
            }
            return current;
        }
        if (Status s = RequireType(bytes.value().bytes(), current, PageType::kIndexInternal); !s.ok()) {
            return s;
        }
        IndexInternalView node(AsPage(bytes.value().bytes()));
        if (Status s = node.CheckAgainst(layout, current); !s.ok()) return s;
        current = node.leftmost_child();
    }
}

// The byte-identical entry already in this leaf, if there is one.
//
// Complete for the leaf the descent lands on, which is where an exact
// duplicate always sorts: entries sharing a sort key are contiguous, and the
// descent for that sort key lands on the first leaf that can hold it. The
// scan is over exactly those neighbours.
StatusOr<bool> FindExactDuplicate(IndexLeafView& leaf, std::span<const std::byte> entry,
                                   std::size_t sort_key_len, std::uint16_t* at) {
    const std::uint16_t n = leaf.entry_count();
    for (std::uint16_t i = leaf.LowerBound(entry.subspan(0, sort_key_len)); i < n; ++i) {
        auto stored = leaf.Entry(i);
        if (!stored.ok()) return stored.status();
        if (std::memcmp(stored.value().data(), entry.data(), sort_key_len) != 0) break;
        if (std::memcmp(stored.value().data(), entry.data(), entry.size()) == 0) {
            *at = i;
            return true;
        }
    }
    return false;
}

}  // namespace

Status FormatRoot(std::span<std::byte, kPageSize> page, const IndexLayout& layout,
                  std::uint64_t owner_oid) {
    auto leaf = IndexLeafView::CreateEmpty(page, layout, owner_oid);
    if (!leaf.ok()) return leaf.status();
    return Status::OK();
}

StatusOr<IndexInsertResult> IndexInsert(storage::PageStore& store, PageId root,
                                        const IndexLayout& layout,
                                        std::span<const std::byte> key, std::uint64_t pk,
                                        std::span<const std::byte> covered,
                                        std::uint64_t owner_oid) {
    if (Status s = CheckIndexLayout(layout); !s.ok()) return s;
    if (key.size() != layout.key_width || covered.size() != layout.covered_width) {
        return Status::InvalidArgument(
            "index entry expects a " + std::to_string(layout.key_width) + "-byte key and " +
            std::to_string(layout.covered_width) + " covered bytes, was given " +
            std::to_string(key.size()) + " and " + std::to_string(covered.size()));
    }

    // The whole entry on the stack: bounded by kMaxIndexEntryWidth, which
    // CheckIndexLayout has already established.
    std::array<std::byte, kMaxIndexEntryWidth> entry_buf{};
    const std::size_t sort_key_len = layout.sort_key_width();
    const std::size_t entry_len = layout.leaf_entry_width();
    std::span<std::byte> entry(entry_buf.data(), entry_len);
    if (Status s = EncodeIndexSortKey(layout, key, pk, entry.subspan(0, sort_key_len)); !s.ok()) {
        return s;
    }
    if (!covered.empty()) {
        std::memcpy(entry.data() + sort_key_len, covered.data(), covered.size());
    }

    auto descent = DescendTo(store, root, layout, entry.subspan(0, sort_key_len),
                             /*leaf_for_write=*/true);
    if (!descent.ok()) return descent.status();
    const PageId leaf_id = descent.value().path[descent.value().depth];
    IndexLeafView leaf(descent.value().leaf.bytes());

    IndexInsertResult out;

    std::uint16_t dup_at = 0;
    auto duplicate = FindExactDuplicate(leaf, entry, sort_key_len, &dup_at);
    if (!duplicate.ok()) return duplicate.status();
    if (duplicate.value()) {
        out.page_id = leaf_id;
        out.slot = dup_at;
        out.already_present = true;
        return out;
    }

    if (auto slot = leaf.InsertEntry(entry); slot.ok()) {
        out.page_id = leaf_id;
        out.slot = slot.value();
        // Nothing recorded: the entry alone describes what changed, which is
        // what lets the caller log one small INDEX_INSERT instead of an 8 KB
        // page image. `changes()` is for pages no record type describes.
        return out;  // the common case: one page touched
    } else if (slot.status().code() != StatusCode::kOutOfSpace) {
        return slot.status();
    }

    // ---- The leaf is full: divide it ------------------------------------
    //
    // Unlike the clustered tree, which refuses this case, dividing is the
    // ordinary path here - see the header for why that decides nothing about
    // heap pages.
    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [new_leaf_id, new_leaf_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> new_leaf_bytes = new_leaf_bytes_ref.bytes();

    auto new_leaf = IndexLeafView::CreateEmpty(new_leaf_bytes, layout, owner_oid);
    if (!new_leaf.ok()) return new_leaf.status();

    // CreateNew() may have handed out a new frame, so the leaf is re-fetched
    // rather than reused (today's stores do not move frames; a buffer pool
    // with eviction will).
    auto leaf_again = store.Get(leaf_id);
    if (!leaf_again.ok()) return leaf_again.status();
    IndexLeafView left(AsPage(leaf_again.value().bytes()));

    if (Status s = left.SplitInto(new_leaf.value()); !s.ok()) return s;

    // The separator is the **first entry of the right sibling**, copied up:
    // its sort key stays in the leaf, unlike an internal node's median,
    // which leaves. index_page.hpp's SplitInto documents the difference.
    auto first_right = new_leaf.value().SortKey(0);
    if (!first_right.ok()) return first_right.status();
    std::array<std::byte, kMaxIndexEntryWidth> sep_buf{};
    std::span<std::byte> sep(sep_buf.data(), sort_key_len);
    std::memcpy(sep.data(), first_right.value().data(), sort_key_len);

    // Into whichever half the new entry belongs.
    const bool goes_right = std::memcmp(entry.data(), sep.data(), sort_key_len) >= 0;
    IndexLeafView& target = goes_right ? new_leaf.value() : left;
    auto slot = target.InsertEntry(entry);
    if (!slot.ok()) return slot.status();
    out.page_id = goes_right ? new_leaf_id : leaf_id;
    out.slot = slot.value();

    // Sibling link last, after the entries are in: the link is what makes
    // the new leaf reachable to a walk, so publishing it earlier would
    // expose a half-built page.
    new_leaf.value().set_right_sibling(left.right_sibling());
    left.set_right_sibling(new_leaf_id);

    out.Record(new_leaf_id, /*is_new_page=*/true);
    out.Record(leaf_id, /*is_new_page=*/false);

    // ---- Propagate the separator up --------------------------------------
    PageId child = new_leaf_id;
    std::uint16_t old_root_level = 0;  // the root is a leaf unless proven otherwise

    for (int d = static_cast<int>(descent.value().depth) - 1; d >= 0; --d) {
        const PageId parent_id = descent.value().path[static_cast<std::uint16_t>(d)];
        auto parent_bytes = store.Get(parent_id);
        if (!parent_bytes.ok()) return parent_bytes.status();
        IndexInternalView parent(AsPage(parent_bytes.value().bytes()));
        old_root_level = parent.level();

        if (!parent.IsFull(layout)) {
            if (Status s = parent.InsertEntry(layout, sep, child); !s.ok()) return s;
            out.Record(parent_id, /*is_new_page=*/false);
            return out;  // absorbed; the tree did not grow
        }

        // A full internal node divides too, and its median separator is
        // *pushed* up rather than copied - see IndexInternalView::SplitInto.
        //
        // `level` is read out before the allocation and `parent` is not
        // touched after it: CreateNew() may hand back a new frame, which
        // leaves every span taken earlier stale. Today's stores never move
        // one; a buffer pool with eviction will, and a view outliving its
        // fetch is the shape that would not fail until then.
        const std::uint16_t level = parent.level();

        auto created_node = store.CreateNew();
        if (!created_node.ok()) return created_node.status();
        auto& [new_node_id, new_node_bytes_ref] = created_node.value();
        const std::span<std::byte, kPageSize> new_node_bytes = new_node_bytes_ref.bytes();

        auto new_node = IndexInternalView::CreateEmpty(new_node_bytes, layout, level,
                                                        /*leftmost_child=*/kInvalidPageId,
                                                        owner_oid);
        if (!new_node.ok()) return new_node.status();

        auto parent_again = store.Get(parent_id);
        if (!parent_again.ok()) return parent_again.status();
        IndexInternalView left_node(AsPage(parent_again.value().bytes()));

        std::array<std::byte, kMaxIndexEntryWidth> up_buf{};
        std::span<std::byte> pushed(up_buf.data(), sort_key_len);
        if (Status s = left_node.SplitInto(new_node.value(), pushed); !s.ok()) return s;

        // The pending (sep -> child) belongs to whichever half now covers it.
        IndexInternalView& into =
            std::memcmp(sep.data(), pushed.data(), sort_key_len) < 0 ? left_node : new_node.value();
        if (Status s = into.InsertEntry(layout, sep, child); !s.ok()) return s;

        out.Record(new_node_id, /*is_new_page=*/true);
        out.Record(parent_id, /*is_new_page=*/false);

        std::memcpy(sep.data(), pushed.data(), sort_key_len);
        child = new_node_id;
    }

    // The split reached past the root: grow a level. The old root becomes the
    // new root's leftmost child, so no key changes page.
    const PageId old_root = descent.value().path[0];
    auto created_root = store.CreateNew();
    if (!created_root.ok()) return created_root.status();
    auto& [new_root_id, new_root_bytes_ref] = created_root.value();
    const std::span<std::byte, kPageSize> new_root_bytes = new_root_bytes_ref.bytes();

    auto new_root = IndexInternalView::CreateEmpty(
        new_root_bytes, layout, static_cast<std::uint16_t>(old_root_level + 1), old_root,
        owner_oid);
    if (!new_root.ok()) return new_root.status();
    if (Status s = new_root.value().InsertEntry(layout, sep, child); !s.ok()) return s;

    out.Record(new_root_id, /*is_new_page=*/true);
    out.new_root = new_root_id;
    return out;
}

StatusOr<PageId> IndexSeekLeaf(storage::PageStore& store, PageId root, const IndexLayout& layout,
                               std::span<const std::byte> sort_key) {
    if (sort_key.size() != layout.sort_key_width()) {
        return Status::InvalidArgument("index seek expects a " +
                                       std::to_string(layout.sort_key_width()) +
                                       "-byte sort key, was given " +
                                       std::to_string(sort_key.size()) +
                                       "; a partial key is zero-padded, never shortened");
    }
    auto descent = DescendTo(store, root, layout, sort_key, /*leaf_for_write=*/false);
    if (!descent.ok()) return descent.status();
    return descent.value().path[descent.value().depth];
}

Status IndexVisitFrom(
    storage::PageStore& store, PageId first_leaf, const IndexLayout& layout,
    storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, IndexLeafView&, std::uint16_t)>&
        fn) {
    PageId current = first_leaf;
    for (std::uint32_t steps = 0;; ++steps) {
        // The same cycle guard the heap chain applies to next_page_id: a
        // cyclic sibling link would otherwise be an infinite loop inside a
        // request.
        if (steps >= heap::kMaxChainPages) {
            return Status::Corruption("index leaf chain from page " +
                                      std::to_string(first_leaf) + " exceeds " +
                                      std::to_string(heap::kMaxChainPages) +
                                      " pages; the sibling links are cyclic or corrupt");
        }

        auto bytes = access == storage::PageAccess::kWrite ? store.Get(current)
                                                           : store.GetForRead(current);
        if (!bytes.ok()) return bytes.status();
        if (Status s = RequireType(bytes.value().bytes(), current, PageType::kIndexLeaf); !s.ok()) {
            return s;
        }
        IndexLeafView leaf(AsPage(bytes.value().bytes()));
        if (Status s = leaf.CheckAgainst(layout, current); !s.ok()) return s;

        const std::uint16_t n = leaf.entry_count();
        for (std::uint16_t i = 0; i < n; ++i) {
            auto outcome = storage::ResolveVisit(fn(current, leaf, i), "IndexVisit");
            if (!outcome.ok()) return outcome.status();
            if (outcome.value() == storage::VisitControl::kStop) return Status::OK();
        }

        const PageId next = leaf.right_sibling();
        if (next == kInvalidPageId) return Status::OK();
        current = next;
    }
}

Status IndexVisit(
    storage::PageStore& store, PageId root, const IndexLayout& layout,
    storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, IndexLeafView&, std::uint16_t)>&
        fn) {
    auto first = LeftmostLeaf(store, root, layout);
    if (!first.ok()) return first.status();
    return IndexVisitFrom(store, first.value(), layout, access, fn);
}

StatusOr<std::uint16_t> IndexHeight(storage::PageStore& store, PageId root,
                                    const IndexLayout& layout) {
    auto bytes = store.GetForRead(root);
    if (!bytes.ok()) return bytes.status();
    if (IsLeafPage(bytes.value().bytes())) return std::uint16_t{1};
    if (Status s = RequireType(bytes.value().bytes(), root, PageType::kIndexInternal); !s.ok()) return s;

    IndexInternalView node(AsPage(bytes.value().bytes()));
    if (Status s = node.CheckAgainst(layout, root); !s.ok()) return s;
    return static_cast<std::uint16_t>(node.level() + 1);
}

StatusOr<std::uint32_t> IndexLeafCount(storage::PageStore& store, PageId root,
                                        const IndexLayout& layout) {
    auto first = LeftmostLeaf(store, root, layout);
    if (!first.ok()) return first.status();

    std::uint32_t leaves = 0;
    PageId current = first.value();
    for (;;) {
        if (leaves >= heap::kMaxChainPages) {
            return Status::Corruption("index leaf chain from page " + std::to_string(root) +
                                      " exceeds " + std::to_string(heap::kMaxChainPages) +
                                      " pages; the sibling links are cyclic or corrupt");
        }
        auto bytes = store.GetForRead(current);
        if (!bytes.ok()) return bytes.status();
        ++leaves;

        const PageId next = IndexLeafView(AsPage(bytes.value().bytes())).right_sibling();
        if (next == kInvalidPageId) return leaves;
        current = next;
    }
}

StatusOr<std::uint64_t> IndexEntryCount(storage::PageStore& store, PageId root,
                                         const IndexLayout& layout) {
    std::uint64_t entries = 0;
    Status s = IndexVisit(store, root, layout, storage::PageAccess::kRead,
                          [&](PageId, IndexLeafView&, std::uint16_t) -> StatusOr<storage::VisitControl> {
                              ++entries;
                              return storage::VisitControl::kContinue;
                          });
    if (!s.ok()) return s;
    return entries;
}

}  // namespace kds::index
