#include "kds/storage/btree/btree.hpp"

#include <cstring>
#include <string>

#include "kds/storage/heap/heap_chain.hpp"  // kMaxChainPages: one cycle guard, not two
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::btree {

namespace {

// Little-endian load of the leading Keystone word, and the id in it. Kept
// local for the reason heap_chain.cpp keeps its own copy: it is three
// lines, and an explicit shift/mask read is what rules.md #5 asks of
// anything that came off a page.
std::uint64_t LoadLe64(const std::byte* in) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(in[i]));
    }
    return v;
}

StatusOr<std::uint64_t> PayloadKeystoneId(std::span<const std::byte> payload) {
    if (payload.size() < kKeystoneWordSize) {
        return Status::Corruption("tuple payload is shorter than its Keystone word");
    }
    return Keystone::Decode(LoadLe64(payload.data())).id;
}

// A page's type as the tree understands it. Anything that is neither an
// internal node nor a leaf means the tree is not what the caller thinks it
// is - a heap-clustered relation's root reached through here, or a
// corrupted child pointer - and is reported rather than parsed.
Status RequireType(std::span<const std::byte, kPageSize> page, PageId page_id, PageType want) {
    const std::uint8_t raw = storage::RawPageType(page);
    if (raw == static_cast<std::uint8_t>(want)) return Status::OK();
    return Status::Corruption("page " + std::to_string(page_id) + " has page_type " +
                              std::to_string(raw) + ", expected " +
                              std::to_string(static_cast<std::uint8_t>(want)));
}

bool IsLeafPage(std::span<const std::byte, kPageSize> page) {
    return storage::RawPageType(page) == static_cast<std::uint8_t>(PageType::kBtreeLeaf);
}

// The root-to-leaf path an insert descended, so a split can walk back up.
// path[0] is the root; path[depth] is the leaf.
struct Descent {
    std::array<PageId, storage::kMaxBtreeDepth> path{};
    std::uint16_t depth = 0;  // index of the leaf within `path`
};

// Follows child pointers for `key` from `root`, recording the path.
StatusOr<Descent> DescendTo(storage::PageStore& store, PageId root, std::uint64_t key) {
    Descent d;
    PageId current = root;
    for (;;) {
        if (d.depth >= storage::kMaxBtreeDepth) {
            return Status::Corruption("btree descent from page " + std::to_string(root) +
                                      " exceeded " + std::to_string(storage::kMaxBtreeDepth) +
                                      " levels; the child pointers are cyclic or corrupt");
        }
        d.path[d.depth] = current;

        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();
        if (IsLeafPage(bytes.value())) return d;

        if (Status s = RequireType(bytes.value(), current, PageType::kBtreeInternal); !s.ok()) {
            return s;
        }
        current = InternalView(bytes.value()).ChildFor(key);
        ++d.depth;
    }
}

// Leftmost leaf, for an ordered scan's starting point.
StatusOr<PageId> LeftmostLeaf(storage::PageStore& store, PageId root) {
    PageId current = root;
    for (std::uint16_t level = 0;; ++level) {
        if (level >= storage::kMaxBtreeDepth) {
            return Status::Corruption("btree from page " + std::to_string(root) + " exceeded " +
                                      std::to_string(storage::kMaxBtreeDepth) + " levels");
        }
        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();
        if (IsLeafPage(bytes.value())) return current;

        if (Status s = RequireType(bytes.value(), current, PageType::kBtreeInternal); !s.ok()) {
            return s;
        }
        current = InternalView(bytes.value()).leftmost_child();
    }
}

// Highest live Keystone id in a leaf, or 0 if it holds none. Used to
// establish that a splitting insert appends rather than divides.
StatusOr<std::uint64_t> MaxLiveId(heap::PageView& leaf) {
    std::uint64_t max_id = 0;
    const std::uint16_t n = leaf.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = leaf.ReadTuple(i);
        if (!tuple.ok()) continue;  // retired or out-of-range slot
        auto id = PayloadKeystoneId(tuple.value().payload);
        if (!id.ok()) return id.status();
        if (id.value() > max_id) max_id = id.value();
    }
    return max_id;
}

}  // namespace

Status FormatRoot(std::span<std::byte, kPageSize> page) {
    auto leaf = heap::PageView::CreateEmptyAs(page, /*min_key=*/0, PageType::kBtreeLeaf);
    if (!leaf.ok()) return leaf.status();
    return Status::OK();
}

StatusOr<storage::InsertPlacement> BtreeInsert(storage::PageStore& store, PageId root,
                                                std::uint64_t id,
                                                std::span<const std::byte> payload,
                                                std::uint64_t trx_id) {
    // Same cross-check ChainInsert makes, for the same reason: two
    // disagreeing copies of a tuple's identity is the kind of defect that
    // stays silent for months.
    auto encoded_id = PayloadKeystoneId(payload);
    if (!encoded_id.ok()) return encoded_id.status();
    if (encoded_id.value() != id) {
        return Status::Corruption("tuple's Keystone id " + std::to_string(encoded_id.value()) +
                                  " does not match the id being inserted (" + std::to_string(id) +
                                  ")");
    }

    auto descent = DescendTo(store, root, id);
    if (!descent.ok()) return descent.status();
    const PageId leaf_id = descent.value().path[descent.value().depth];

    auto leaf_bytes = store.Get(leaf_id);
    if (!leaf_bytes.ok()) return leaf_bytes.status();
    heap::PageView leaf(leaf_bytes.value());

    // Invariant 3, enforced at the one door tuples come through - and the
    // descent already guarantees no other leaf may hold this id, so being
    // below this leaf's low key means the id sequence went backwards.
    if (id < leaf.min_key()) {
        return Status::OutOfRange("id " + std::to_string(id) + " is below leaf " +
                                  std::to_string(leaf_id) + "'s min_key " +
                                  std::to_string(leaf.min_key()) +
                                  "; the relation's id sequence has gone backwards");
    }

    // Complete, unlike the heap chain's tail-only check: the descent is
    // exact, so the leaf it landed on is the only page that may hold `id`.
    // Still a sanity check on the id sequence rather than a uniqueness
    // index - a delete-marked tuple holds its key until the slot is
    // physically retired.
    {
        const std::uint16_t n = leaf.slot_count();
        for (std::uint16_t i = 0; i < n; ++i) {
            auto tuple = leaf.ReadTuple(i);
            if (!tuple.ok()) continue;
            auto existing = PayloadKeystoneId(tuple.value().payload);
            if (!existing.ok()) return existing.status();
            if (existing.value() == id) {
                return Status::AlreadyExists("duplicate primary key " + std::to_string(id) +
                                              " already present at page " +
                                              std::to_string(leaf_id) + " slot " +
                                              std::to_string(i));
            }
        }
    }

    storage::InsertPlacement out;

    if (auto slot = leaf.InsertTuple(payload, trx_id); slot.ok()) {
        out.page_id = leaf_id;
        out.slot = slot.value();
        return out;  // the common case: no structural change at all
    } else if (slot.status().code() != StatusCode::kOutOfSpace) {
        return slot.status();  // a real failure, not a full leaf
    }

    // ---- The leaf is full: right-split, moving nothing -------------------
    //
    // Only legal when `id` sorts above everything already in the leaf, so
    // the "split" is an append of a fresh leaf rather than a division of
    // this one's contents. Anything else needs the split policy CLAUDE.md
    // leaves open, and is refused rather than guessed.
    auto max_id = MaxLiveId(leaf);
    if (!max_id.ok()) return max_id.status();
    if (id < max_id.value()) {
        return Status::OutOfSpace(
            "leaf " + std::to_string(leaf_id) + " is full and id " + std::to_string(id) +
            " sorts below its highest key " + std::to_string(max_id.value()) +
            "; dividing a full page's contents needs the heap page split policy, "
            "which is an open design decision");
    }

    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto [new_leaf_id, new_leaf_bytes] = created.value();

    auto new_leaf = heap::PageView::CreateEmptyAs(new_leaf_bytes, /*min_key=*/id,
                                                   PageType::kBtreeLeaf);
    if (!new_leaf.ok()) return new_leaf.status();

    auto new_slot = new_leaf.value().InsertTuple(payload, trx_id);
    if (!new_slot.ok()) {
        // A tuple no empty leaf can hold. The page stays allocated and
        // unlinked rather than freed - the store has no free-page path yet
        // (page.md's SpaceManager), and an unreachable empty page is
        // harmless where a dangling link would not be.
        return new_slot.status();
    }
    out.page_id = new_leaf_id;
    out.slot = new_slot.value();

    // Sibling link last, after the tuple is in: the link is what makes the
    // leaf reachable to a scan, so publishing it earlier would expose an
    // empty leaf. Re-fetched because CreateNew() may have handed out a new
    // frame (today's stores do not move frames; a buffer pool with eviction
    // will).
    auto leaf_again = store.Get(leaf_id);
    if (!leaf_again.ok()) return leaf_again.status();
    heap::PageView(leaf_again.value()).set_next_page_id(new_leaf_id);

    // Redo order: the new leaf's PAGE_INIT (which the HEAP_INSERT then
    // fills), then the old leaf's image carrying the link that reaches it,
    // then the ancestors. The images are self-contained, so the order among
    // them is not load-bearing; what matters is that all of them precede
    // the HEAP_INSERT the caller emits for the tuple.
    out.Record(new_leaf_id, /*is_new_page=*/true, /*min_key=*/id);
    out.Record(leaf_id, /*is_new_page=*/false, 0);

    // ---- Propagate the separator up -------------------------------------
    //
    // `sep` is the new subtree's low key, which is exactly the new leaf's
    // min_key - the same number, never a separately derived boundary
    // (btree_page.hpp's routing rule).
    std::uint64_t sep = id;
    PageId child = new_leaf_id;
    std::uint16_t old_root_level = 0;  // the root is a leaf unless proven otherwise

    for (int d = static_cast<int>(descent.value().depth) - 1; d >= 0; --d) {
        const PageId parent_id = descent.value().path[static_cast<std::uint16_t>(d)];
        auto parent_bytes = store.Get(parent_id);
        if (!parent_bytes.ok()) return parent_bytes.status();
        InternalView parent(parent_bytes.value());

        if (!parent.IsFull()) {
            if (Status s = parent.InsertEntry(sep, child); !s.ok()) return s;
            out.Record(parent_id, /*is_new_page=*/false, 0);
            return out;  // absorbed; the tree did not grow
        }

        // Full internal node, same right-split with no movement: every key
        // reachable through `child` is >= sep, and every separator already
        // in `parent` is < sep (the descent chose `parent` for `sep`, and
        // the leaf under it was the rightmost). So a new node whose only
        // child is `child` needs no entries, and `sep` is what gets
        // promoted another level.
        const std::uint16_t level = parent.level();
        auto created_node = store.CreateNew();
        if (!created_node.ok()) return created_node.status();
        auto [new_node_id, new_node_bytes] = created_node.value();

        auto new_node = InternalView::CreateEmpty(new_node_bytes, level, child);
        if (!new_node.ok()) return new_node.status();

        out.Record(new_node_id, /*is_new_page=*/false, 0);
        child = new_node_id;
        old_root_level = level;
    }

    // The split reached past the root: grow a level. The old root becomes
    // the new root's leftmost child, so no key changes page.
    const PageId old_root = descent.value().path[0];
    auto created_root = store.CreateNew();
    if (!created_root.ok()) return created_root.status();
    auto [new_root_id, new_root_bytes] = created_root.value();

    auto new_root = InternalView::CreateEmpty(new_root_bytes,
                                               static_cast<std::uint16_t>(old_root_level + 1),
                                               old_root);
    if (!new_root.ok()) return new_root.status();
    if (Status s = new_root.value().InsertEntry(sep, child); !s.ok()) return s;

    out.Record(new_root_id, /*is_new_page=*/false, 0);
    out.new_root = new_root_id;
    return out;
}

StatusOr<Location> BtreeLookup(storage::PageStore& store, PageId root, std::uint64_t id) {
    auto descent = DescendTo(store, root, id);
    if (!descent.ok()) return descent.status();
    const PageId leaf_id = descent.value().path[descent.value().depth];

    auto bytes = store.Get(leaf_id);
    if (!bytes.ok()) return bytes.status();
    heap::PageView leaf(bytes.value());

    // Tuples within a leaf are unordered - the "semi-sorted" property of
    // heap-and-tuple.md section 3 carries over unchanged - so this is a
    // linear scan of one page, bounded by how many tuples fit in 8 KB.
    const std::uint16_t n = leaf.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = leaf.ReadTuple(i);
        if (!tuple.ok()) continue;  // retired or out-of-range slot
        auto existing = PayloadKeystoneId(tuple.value().payload);
        if (!existing.ok()) return existing.status();
        if (existing.value() == id) return Location{leaf_id, i};
    }
    return Status::NotFound("no tuple with primary key " + std::to_string(id) + " in leaf " +
                            std::to_string(leaf_id));
}

Status BtreeVisit(storage::PageStore& store, PageId root,
                  const std::function<Status(PageId, heap::PageView&, std::uint16_t)>& fn) {
    auto first = LeftmostLeaf(store, root);
    if (!first.ok()) return first.status();

    PageId current = first.value();
    for (std::uint32_t steps = 0;; ++steps) {
        // Same cycle guard the heap chain applies to next_page_id, for the
        // same reason: a cyclic sibling link would otherwise be an infinite
        // loop inside a request.
        if (steps >= heap::kMaxChainPages) {
            return Status::Corruption("btree leaf chain from page " + std::to_string(root) +
                                      " exceeds " + std::to_string(heap::kMaxChainPages) +
                                      " pages; the sibling links are cyclic or corrupt");
        }

        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();
        if (Status s = RequireType(bytes.value(), current, PageType::kBtreeLeaf); !s.ok()) {
            return s;
        }
        heap::PageView leaf(bytes.value());

        const std::uint16_t n = leaf.slot_count();
        for (std::uint16_t i = 0; i < n; ++i) {
            // Liveness is re-tested by the callback through ReadTuple();
            // skipping here as well would mean two reads of every slot.
            if (Status s = fn(current, leaf, i); !s.ok()) return s;
        }

        const PageId next = leaf.next_page_id();
        if (next == kInvalidPageId) return Status::OK();
        current = next;
    }
}

StatusOr<std::uint16_t> BtreeHeight(storage::PageStore& store, PageId root) {
    auto bytes = store.Get(root);
    if (!bytes.ok()) return bytes.status();
    if (IsLeafPage(bytes.value())) return std::uint16_t{1};
    if (Status s = RequireType(bytes.value(), root, PageType::kBtreeInternal); !s.ok()) return s;

    const std::uint16_t level = InternalView(bytes.value()).level();
    if (level == 0 || level >= storage::kMaxBtreeDepth) {
        return Status::Corruption("btree root " + std::to_string(root) + " reports level " +
                                  std::to_string(level));
    }
    return static_cast<std::uint16_t>(level + 1);
}

StatusOr<std::uint32_t> BtreeLeafCount(storage::PageStore& store, PageId root) {
    std::uint32_t leaves = 0;
    auto first = LeftmostLeaf(store, root);
    if (!first.ok()) return first.status();

    PageId current = first.value();
    for (;;) {
        if (leaves >= heap::kMaxChainPages) {
            return Status::Corruption("btree leaf chain from page " + std::to_string(root) +
                                      " exceeds " + std::to_string(heap::kMaxChainPages) +
                                      " pages; the sibling links are cyclic or corrupt");
        }
        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();
        ++leaves;

        const PageId next = heap::PageView(bytes.value()).next_page_id();
        if (next == kInvalidPageId) return leaves;
        current = next;
    }
}

}  // namespace kds::btree
