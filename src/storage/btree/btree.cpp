#include "kds/storage/btree/btree.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "kds/storage/heap/heap_chain.hpp"  // kMaxChainPages: one cycle guard, not two
#include "kds/storage/keystone.hpp"
#include "kds/storage/page_header.hpp"

namespace kds::btree {

namespace {

// The Keystone id of the tuple at `slot`, or NotFound if that slot holds
// no tuple (out of range or retired). `nr_slots` is hoisted by the caller,
// so a search over a page reads the page header once rather than once per
// probe. Corruption propagates: a payload too short to hold a Keystone
// word is damage, not a miss.
StatusOr<std::uint64_t> SlotKeystoneId(heap::PageView& leaf, std::uint16_t slot,
                                        std::uint16_t nr_slots) {
    auto payload = leaf.PayloadAt(slot, nr_slots);
    if (!payload.ok()) return payload.status();
    return KeystoneIdOfPayload(payload.value());
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
    // The leaf, **held**: the descent's pin rides in the struct, so the
    // bytes stay valid for exactly as long as the Descent does. This used
    // to be a bare span with the pin dropped at DescendTo's return - the
    // one Shape C the stopped review never reached, found by finishing its
    // checklist: any fault between the descent and the caller's read (a
    // split's CreateNew is the everyday case) could walk the leaf's usage
    // down and reclaim it mid-operation. Peak pins (MG03): 1, this ref.
    storage::PageRef leaf;
};

// A descent's leaf bytes back at their true fixed extent. The span is
// dynamic only because Descent needs an unset state; anything that reaches
// here came from a store fetch and is exactly one page long.
std::span<std::byte, kPageSize> AsPage(std::span<std::byte> bytes) {
    return std::span<std::byte, kPageSize>(bytes.data(), kPageSize);
}

// Follows child pointers for `key` from `root`, recording the path.
//
// Internal nodes are never modified by a descent, so they are always
// fetched read-only. `leaf_for_write` says whether the *leaf* is about to
// be mutated: an insert takes it for write so the frame is marked dirty, a
// lookup does not, which is what keeps a read-only statement from
// scheduling a write-back of everything it read (page_store.hpp).
StatusOr<Descent> DescendTo(storage::PageStore& store, PageId root, std::uint64_t key,
                            bool leaf_for_write) {
    Descent d;
    PageId current = root;
    for (;;) {
        if (d.depth >= storage::kMaxBtreeDepth) {
            return Status::Corruption("btree descent from page " + std::to_string(root) +
                                      " exceeded " + std::to_string(storage::kMaxBtreeDepth) +
                                      " levels; the child pointers are cyclic or corrupt");
        }
        d.path[d.depth] = current;

        auto bytes = store.GetForRead(current);
        if (!bytes.ok()) return bytes.status();
        if (IsLeafPage(bytes.value().bytes())) {
            if (!leaf_for_write) {
                d.leaf = std::move(bytes.value());
                return d;
            }
            // Re-fetch for write: the frame is already resident, so this
            // is a hash lookup that flips the dirty flag, and it happens
            // only on the insert path where a WAL append dwarfs it.
            auto writable = store.Get(current);
            if (!writable.ok()) return writable.status();
            d.leaf = std::move(writable.value());
            return d;
        }

        if (Status s = RequireType(bytes.value().bytes(), current, PageType::kBtreeInternal); !s.ok()) {
            return s;
        }
        current = InternalView(bytes.value().bytes()).ChildFor(key);
        ++d.depth;
    }
}

// Highest live Keystone id in a leaf, or 0 if it holds none. Used to
// establish that a splitting insert appends rather than divides.
StatusOr<std::uint64_t> MaxLiveId(heap::PageView& leaf) {
    std::uint64_t max_id = 0;
    const std::uint16_t n = leaf.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto id = SlotKeystoneId(leaf, i, n);
        if (id.status().code() == StatusCode::kNotFound) continue;  // retired slot
        if (!id.ok()) return id.status();
        if (id.value() > max_id) max_id = id.value();
    }
    return max_id;
}

// Where `id` sits among a leaf's slots, or NotFound.
//
// Two passes, and the second is what makes the first safe to attempt.
//
// Slots in a leaf are in ascending key order whenever ids were issued
// monotonically: a descent routes each new id to the one leaf whose range
// covers it, appends land past every key already there, and dead slots keep
// their position so retirement does not disturb the order.
//
// **A caller-supplied id breaks that** (docs/spec/heap-and-tuple.md section 4.1):
// a caller-supplied id may sort anywhere, so it can be appended into a slot
// below its neighbours, and SplitLeafAndInsert redistributes by key rather
// than by slot position. Which is why the fallback below is not decoration:
// the binary search is an optimization for the ordered case, and the linear
// pass is what makes the answer correct in every case. An unsorted leaf
// costs a wasted log2(n) probes and still returns the right answer.
//
// Dead slots are the one wrinkle: they carry no key, so a probe can land
// on a hole. Stepping to the nearest live slot inside the window keeps the
// search going; a window with no live slot at all is a miss, which the
// linear pass then confirms or corrects.
StatusOr<std::uint16_t> FindSlotForId(heap::PageView& leaf, std::uint64_t id,
                                       std::uint16_t nr_slots) {
    std::uint16_t lo = 0;
    std::uint16_t hi = nr_slots;  // exclusive
    while (lo < hi) {
        const std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);

        // Nearest live slot at or after mid, within the window.
        std::uint16_t probe = mid;
        StatusOr<std::uint64_t> key = Status::NotFound("");
        for (; probe < hi; ++probe) {
            key = SlotKeystoneId(leaf, probe, nr_slots);
            if (key.ok()) break;
            if (key.status().code() != StatusCode::kNotFound) return key.status();
        }
        if (probe >= hi) {
            // Every slot in [mid, hi) is dead; the live ones, if any, are
            // below mid.
            hi = mid;
            continue;
        }

        if (key.value() == id) return probe;
        if (key.value() < id) {
            lo = static_cast<std::uint16_t>(probe + 1);
        } else {
            hi = mid;
        }
    }

    for (std::uint16_t i = 0; i < nr_slots; ++i) {
        auto key = SlotKeystoneId(leaf, i, nr_slots);
        if (key.status().code() == StatusCode::kNotFound) continue;
        if (!key.ok()) return key.status();
        if (key.value() == id) return i;
    }
    return Status::NotFound("no such key in leaf");
}

// Which of the two ways a full internal node can grow applies: true when
// `sep` sorts above every separator the node holds, so the cheap right-split
// that moves no entries is sound. False means the entries have to be
// divided, because promoting an interior separator by that path would strand
// every subtree above it.
StatusOr<bool> SeparatorAboveEveryEntry(const InternalView& parent, std::uint64_t sep) {
    if (parent.entry_count() == 0) return true;
    auto highest = parent.Entry(static_cast<std::uint16_t>(parent.entry_count() - 1));
    if (!highest.ok()) return highest.status();
    return sep >= highest.value().sep_key;
}

// ---- Dividing a full internal node (workplan-key-mode.md PK09) -----------
//
// The leaf division's shape, one level up, and simpler: an internal entry is
// a fixed `(sep_key, child)` pair, so there is no payload to copy and no
// MVCC state to preserve - only the entry array to cut.
//
// **The median separator moves up rather than being copied**, which is the
// one place this differs from a leaf. In a leaf both halves keep their keys
// and the new page's low key is duplicated as the parent's separator; in an
// internal node the median's *child* becomes the new node's leftmost child,
// so the key itself has no remaining home here and belongs to the parent.
// Copying it instead would route every key at exactly that value into a
// subtree that no longer holds it.
//
// Returns what the caller must promote one level further.
struct InternalDivision {
    std::uint64_t sep = 0;             // the median, moved up
    PageId child = kInvalidPageId;     // the node now holding everything above it
};

StatusOr<InternalDivision> DivideInternalNode(storage::PageStore& store, PageId node_id,
                                               std::uint64_t sep, PageId child,
                                               storage::InsertPlacement& out,
                                               std::uint64_t owner_oid) {
    std::vector<BtreeInternalEntryFields> entries;
    std::uint16_t level = 0;
    PageId leftmost = kInvalidPageId;
    {
        auto bytes = store.Get(node_id);
        if (!bytes.ok()) return bytes.status();
        InternalView node(bytes.value().bytes());
        level = node.level();
        leftmost = node.leftmost_child();
        const std::uint16_t n = node.entry_count();
        entries.reserve(static_cast<std::size_t>(n) + 1);
        for (std::uint16_t i = 0; i < n; ++i) {
            auto e = node.Entry(i);
            if (!e.ok()) return e.status();
            entries.push_back(e.value());
        }
    }

    // The incoming entry, placed in sort order. Done here rather than through
    // InsertEntry because the node is full - the array only exists in this
    // vector until both halves are written back.
    auto at = std::lower_bound(
        entries.begin(), entries.end(), sep,
        [](const BtreeInternalEntryFields& e, std::uint64_t key) { return e.sep_key < key; });
    if (at != entries.end() && at->sep_key == sep) {
        // InsertEntry's rule, and for its reason: two subtrees sharing a low
        // key makes the descent's choice between them arbitrary and one of
        // them permanently unreachable.
        return Status::AlreadyExists("separator key " + std::to_string(sep) +
                                      " is already present in internal node " +
                                      std::to_string(node_id));
    }
    entries.insert(at, BtreeInternalEntryFields{sep, child});

    const std::size_t m = entries.size() / 2;
    const BtreeInternalEntryFields median = entries[m];

    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [right_id, right_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> right_bytes = right_bytes_ref.bytes();

    auto right = InternalView::CreateEmpty(right_bytes, level, median.child, owner_oid);
    if (!right.ok()) return right.status();
    for (std::size_t k = m + 1; k < entries.size(); ++k) {
        if (Status s = right.value().InsertEntry(entries[k].sep_key, entries[k].child); !s.ok()) {
            return s;
        }
    }

    // The old node is rebuilt, not edited: there is no API to drop the upper
    // half in place, and reformatting is what the leaf division does for the
    // same reason. `leftmost` goes back unchanged, so every key below the
    // median still routes exactly where it did.
    //
    // Re-fetched because CreateNew() above may have handed out a new frame.
    auto again = store.Get(node_id);
    if (!again.ok()) return again.status();
    // The rebuild keeps the page's *own* stamp, not the caller's: a
    // pre-§2a page carries 0, and §2a's no-backfill rule requires it to
    // stay 0 — upgrading it here would quietly make an unreclaimable page
    // reclaimable. On a stamped page the two are equal, so this costs
    // nothing.
    const std::uint64_t old_owner = storage::GetOwnerOid(AsPage(again.value().bytes()));
    auto rebuilt =
        InternalView::CreateEmpty(AsPage(again.value().bytes()), level, leftmost, old_owner);
    if (!rebuilt.ok()) return rebuilt.status();
    for (std::size_t k = 0; k < m; ++k) {
        if (Status s = rebuilt.value().InsertEntry(entries[k].sep_key, entries[k].child);
            !s.ok()) {
            return s;
        }
    }

    out.Record(right_id, /*is_new_page=*/false, 0);
    out.Record(node_id, /*is_new_page=*/false, 0);
    return InternalDivision{median.sep_key, right_id};
}

// ---- Propagate a new subtree's separator up the descent path -------------
//
// Shared by both ways a leaf grows: the append-split above and the true
// division in SplitLeafAndInsert. `sep` is the new subtree's low key and
// `child` the page that holds it; `out` carries whatever the caller has
// already recorded, and comes back with the ancestors appended.
// Peak pins (MG03): 3, and not cumulative across levels - both locals are
// per-iteration, so a level's pins drop before the next is fetched. The
// append path holds 2 (the parent and the created node); the *divide* path
// holds 3, because DivideInternalNode runs with `parent_bytes` still held
// and itself holds its created node and its re-fetch of the same node
// together. Stacked under SplitLeafAndInsert's 2 - which are alive across
// the tail call below - the whole grow path holds at most 5, under
// DevicePageStore::kPinCeiling with three frames of slack.
StatusOr<storage::InsertPlacement> PromoteSeparator(storage::PageStore& store,
                                                     const Descent& descent, std::uint64_t sep,
                                                     PageId child,
                                                     storage::InsertPlacement out,
                                                     std::uint64_t owner_oid) {
    std::uint16_t old_root_level = 0;  // the root is a leaf unless proven otherwise

    for (int d = static_cast<int>(descent.depth) - 1; d >= 0; --d) {
        const PageId parent_id = descent.path[static_cast<std::uint16_t>(d)];
        auto parent_bytes = store.Get(parent_id);
        if (!parent_bytes.ok()) return parent_bytes.status();
        InternalView parent(parent_bytes.value().bytes());

        if (!parent.IsFull()) {
            if (Status s = parent.InsertEntry(sep, child); !s.ok()) return s;
            out.Record(parent_id, /*is_new_page=*/false, 0);
            return out;  // absorbed; the tree did not grow
        }

        const std::uint16_t level = parent.level();

        // A full node, and two ways to grow it.
        //
        // When `sep` sorts above every separator here, a right-split with no
        // movement is enough: a new node whose only child is `child` needs
        // no entries at all, and `sep` is promoted another level. That is the
        // append case a monotonic id sequence produces exclusively, and it
        // stays because it is correct and costs nothing.
        //
        // Anything else has to divide the node's entries, which only a
        // caller-supplied id can require (§4.1). Promoting an interior
        // separator by the cheap path would strand every subtree above it -
        // silent data loss, not a wrong answer someone would notice - so the
        // two are told apart rather than assumed.
        auto appends = SeparatorAboveEveryEntry(parent, sep);
        if (!appends.ok()) return appends.status();

        if (!appends.value()) {
            auto divided = DivideInternalNode(store, parent_id, sep, child, out, owner_oid);
            if (!divided.ok()) return divided.status();
            sep = divided.value().sep;
            child = divided.value().child;
            old_root_level = level;
            continue;
        }

        auto created_node = store.CreateNew();
        if (!created_node.ok()) return created_node.status();
        auto& [new_node_id, new_node_bytes_ref] = created_node.value();
        const std::span<std::byte, kPageSize> new_node_bytes = new_node_bytes_ref.bytes();

        auto new_node = InternalView::CreateEmpty(new_node_bytes, level, child, owner_oid);
        if (!new_node.ok()) return new_node.status();

        out.Record(new_node_id, /*is_new_page=*/false, 0);
        child = new_node_id;
        old_root_level = level;
    }

    // The split reached past the root: grow a level. The old root becomes
    // the new root's leftmost child, so no key changes page.
    const PageId old_root = descent.path[0];
    auto created_root = store.CreateNew();
    if (!created_root.ok()) return created_root.status();
    auto& [new_root_id, new_root_bytes_ref] = created_root.value();
    const std::span<std::byte, kPageSize> new_root_bytes = new_root_bytes_ref.bytes();

    auto new_root = InternalView::CreateEmpty(new_root_bytes,
                                               static_cast<std::uint16_t>(old_root_level + 1),
                                               old_root, owner_oid);
    if (!new_root.ok()) return new_root.status();
    if (Status s = new_root.value().InsertEntry(sep, child); !s.ok()) return s;

    out.Record(new_root_id, /*is_new_page=*/false, 0);
    out.new_root = new_root_id;
    return out;
}

// ---- Dividing a full leaf (docs/spec/heap-and-tuple.md section 4.1) ------------
//
// Reached only when a caller-supplied id sorts *inside* a full leaf. A
// monotonic sequence never gets here: it always appends past the leaf's
// highest key, which BtreeInsert handles without moving a byte.
//
// **Why this does not violate invariant 2 or 3.** The old leaf keeps its
// `min_key` untouched - the division moves the *upper* half out, and
// everything that stays is still at or above the low bound it was created
// with. The new leaf's `min_key` is the split key, which is by construction
// the smallest id moved into it. So both pages satisfy "no tuple below this
// page's min_key" and neither page's min_key is ever rewritten. Dividing a
// page's contents was refused before this existed because nothing needed
// it, not because it could not be done inside the invariants.
//
// **What moving a tuple costs.** Its (page_id, slot) changes, which is a
// relayout in everything but name, so the old leaf's `relayout_epoch` is
// bumped: every Waystone trail entry and Cabin hint pointing into it
// becomes untrusted at once (section 3.1a's pairing rule). Secondary
// indexes need nothing - an index entry's sort key is `key || pk`
// (index_page.hpp), never a location - and the undo chain is likewise
// addressed by `undo_ptr`, not by where the version sits.
//
// The delete mark travels with the tuple. A delete-marked version carries
// its deleter's `trx_id` and must arrive still marked; re-inserting the
// payload alone would resurrect a row some snapshot has already been told
// is gone.
StatusOr<storage::InsertPlacement> SplitLeafAndInsert(storage::PageStore& store,
                                                       const Descent& descent, PageId leaf_id,
                                                       std::uint64_t id,
                                                       std::span<const std::byte> payload,
                                                       std::uint64_t trx_id,
                                                       std::uint64_t owner_oid) {
    // Every live version on the page, copied out whole before anything is
    // written. Two reasons it is a copy and not a set of slot indices: a
    // Tuple's `payload` is a view into the page, and the page is about to be
    // reformatted under it; and the division chooses its boundary from the
    // *keys*, since a leaf fed descending ids is not in slot order and
    // splitting at slot n/2 would divide it at an arbitrary key.
    struct Version {
        std::uint64_t key;
        std::uint64_t trx_id;
        std::uint64_t undo_ptr;
        bool deleted;
        std::vector<std::byte> bytes;
    };
    std::vector<Version> live;
    std::uint64_t old_min_key = 0;
    PageId old_next = kInvalidPageId;
    std::uint64_t old_epoch = 0;

    {
        heap::PageView leaf(descent.leaf.bytes());
        old_min_key = leaf.min_key();
        old_next = leaf.next_page_id();
        old_epoch = leaf.RelayoutEpoch();

        const std::uint16_t n = leaf.slot_count();
        live.reserve(n);
        for (std::uint16_t i = 0; i < n; ++i) {
            auto key = SlotKeystoneId(leaf, i, n);
            if (key.status().code() == StatusCode::kNotFound) continue;  // retired slot
            if (!key.ok()) return key.status();
            auto tuple = leaf.ReadTuple(i);
            if (!tuple.ok()) return tuple.status();
            live.push_back({key.value(), tuple.value().trx_id, tuple.value().undo_ptr,
                            tuple.value().deleted,
                            std::vector<std::byte>(tuple.value().payload.begin(),
                                                   tuple.value().payload.end())});
        }
    }

    if (live.size() < 2) {
        // Nothing to divide: a single live tuple filling a whole page means
        // the row is near page-sized, and no boundary makes room for a
        // second. Reported as the space failure it is rather than producing
        // an empty leaf the descent can route to and never satisfy.
        return Status::OutOfSpace("leaf " + std::to_string(leaf_id) +
                                  " is full with fewer than two live tuples; the row is too "
                                  "large for a leaf to hold two of");
    }

    std::sort(live.begin(), live.end(),
              [](const Version& a, const Version& b) { return a.key < b.key; });

    // The median key opens the new leaf: everything from `split_at` on moves,
    // everything before it stays. Both halves are non-empty because
    // `live.size() >= 2`.
    const std::size_t split_at = live.size() / 2;
    const std::uint64_t split_key = live[split_at].key;

    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [new_leaf_id, new_leaf_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> new_leaf_bytes = new_leaf_bytes_ref.bytes();

    auto new_leaf = heap::PageView::CreateEmptyAs(new_leaf_bytes, /*min_key=*/split_key,
                                                   PageType::kBtreeLeaf, owner_oid);
    if (!new_leaf.ok()) return new_leaf.status();

    for (std::size_t k = split_at; k < live.size(); ++k) {
        auto slot = new_leaf.value().InsertTuple(live[k].bytes, live[k].trx_id, live[k].undo_ptr);
        if (!slot.ok()) return slot.status();
        // The delete mark travels with the version. Re-inserting the payload
        // alone would resurrect a row some snapshot has already been told is
        // gone.
        if (live[k].deleted) {
            if (Status s = new_leaf.value().DeleteMark(slot.value(), live[k].trx_id); !s.ok()) {
                return s;
            }
        }
    }

    // ---- The old leaf is rebuilt, not edited in place ---------------------
    //
    // `RetireSlot` marks a slot dead; it does not give the bytes back, since
    // reclamation is a purge pass's job (heap_page.hpp). Retiring the moved
    // half would therefore leave this page exactly as full as it was, and the
    // division would make room for nothing - which is the whole point of it.
    // So the page is reformatted and the staying half written back.
    //
    // Re-fetched rather than reusing the descent's span: CreateNew() above
    // may have handed out a new frame, and page stores are free to move
    // theirs.
    auto old_bytes = store.Get(leaf_id);
    if (!old_bytes.ok()) return old_bytes.status();

    // `min_key` goes back **unchanged**, which is invariant 2 and the reason
    // a division is legal at all: the low bound a reader may have pruned by
    // without a latch does not move. Everything staying is at or above it
    // already, so invariant 3 holds on both sides.
    // The page's *own* stamp, not the caller's — a pre-§2a page must keep
    // its 0; the internal-node rebuild above states the full argument.
    const std::uint64_t old_owner = storage::GetOwnerOid(AsPage(old_bytes.value().bytes()));
    auto rebuilt = heap::PageView::CreateEmptyAs(AsPage(old_bytes.value().bytes()), old_min_key,
                                                  PageType::kBtreeLeaf, old_owner);
    if (!rebuilt.ok()) return rebuilt.status();

    for (std::size_t k = 0; k < split_at; ++k) {
        auto slot = rebuilt.value().InsertTuple(live[k].bytes, live[k].trx_id, live[k].undo_ptr);
        if (!slot.ok()) return slot.status();
        if (live[k].deleted) {
            if (Status s = rebuilt.value().DeleteMark(slot.value(), live[k].trx_id); !s.ok()) {
                return s;
            }
        }
    }

    // The sibling link: the new leaf takes the old one's right neighbour,
    // then the old one points at the new. Reformatting zeroed the link, so
    // both ends are restored here rather than one being left as it was.
    new_leaf.value().set_next_page_id(old_next);
    rebuilt.value().set_next_page_id(new_leaf_id);

    // Every tuple on this page changed slot, and half of them changed page
    // (section 3.1a). Reformatting zeroed the counter, so it is restored to
    // one *past* what it was rather than bumped from zero - an epoch that
    // went backwards would let a trail entry recorded at the old value
    // compare equal again, which is the one thing this field exists to stop.
    storage::SetRelayoutEpoch(AsPage(old_bytes.value().bytes()), old_epoch + 1);

    storage::InsertPlacement out;

    // The incoming tuple goes to whichever half now covers it - the routing a
    // fresh descent would make, decided here because both pages are in hand.
    const bool to_new = id >= split_key;
    auto slot = to_new ? new_leaf.value().InsertTuple(payload, trx_id)
                       : rebuilt.value().InsertTuple(payload, trx_id);
    if (!slot.ok()) return slot.status();
    out.page_id = to_new ? new_leaf_id : leaf_id;
    out.slot = slot.value();

    // ---- Both pages are logged as full images, neither as "new" ----------
    //
    // `is_new_page` is not "this page did not exist"; it means "a PAGE_INIT
    // is enough, because the HEAP_INSERT that follows describes the only
    // tuple on it" (command_dispatcher.cpp's LogInsert). Neither page here
    // satisfies that. The new leaf receives the *moved* half, which no
    // record describes; and the incoming tuple may land in the rebuilt old
    // leaf instead, so the new leaf is not even guaranteed to be the page
    // the HEAP_INSERT names. Redo would reconstruct an empty new leaf and
    // lose every version this division moved.
    //
    // A full page image is self-contained - header, min_key, page type and
    // all - so it reconstructs a page that never existed just as well as one
    // that changed, which is exactly why a newly created *internal* node
    // takes this arm too.
    out.Record(new_leaf_id, /*is_new_page=*/false, 0);
    out.Record(leaf_id, /*is_new_page=*/false, 0);

    return PromoteSeparator(store, descent, split_key, new_leaf_id, std::move(out), owner_oid);
}

}  // namespace

Status FormatRoot(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid) {
    auto leaf = heap::PageView::CreateEmptyAs(page, /*min_key=*/0, PageType::kBtreeLeaf,
                                              owner_oid);
    if (!leaf.ok()) return leaf.status();
    return Status::OK();
}

StatusOr<storage::InsertPlacement> BtreeInsert(storage::PageStore& store, PageId root,
                                                std::uint64_t id,
                                                std::span<const std::byte> payload,
                                                std::uint64_t trx_id,
                                                std::uint64_t owner_oid) {
    // Same cross-check ChainInsert makes, for the same reason: two
    // disagreeing copies of a tuple's identity is the kind of defect that
    // stays silent for months.
    auto encoded_id = KeystoneIdOfPayload(payload);
    if (!encoded_id.ok()) return encoded_id.status();
    if (encoded_id.value() != id) {
        return Status::Corruption("tuple's Keystone id " + std::to_string(encoded_id.value()) +
                                  " does not match the id being inserted (" + std::to_string(id) +
                                  ")");
    }

    auto descent = DescendTo(store, root, id, /*leaf_for_write=*/true);
    if (!descent.ok()) return descent.status();
    const PageId leaf_id = descent.value().path[descent.value().depth];
    heap::PageView leaf(descent.value().leaf.bytes());

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
    //
    // Linear rather than FindSlotForId(): this check expects to find
    // nothing, and a miss is exactly the case where the binary search pays
    // its probes and then falls through to this scan anyway.
    {
        const std::uint16_t n = leaf.slot_count();
        for (std::uint16_t i = 0; i < n; ++i) {
            auto existing = SlotKeystoneId(leaf, i, n);
            if (existing.status().code() == StatusCode::kNotFound) continue;
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

    // ---- The leaf is full ------------------------------------------------
    //
    // Two shapes. When `id` sorts above everything in the leaf the growth is
    // an *append*: a fresh leaf, nothing moved, which is what a monotonic id
    // sequence produces and is handled below. When `id` sorts inside the
    // leaf - which only a caller-supplied, possibly descending id can do
    // (docs/spec/heap-and-tuple.md section 4.1) - the leaf must genuinely divide,
    // which is SplitLeaf's job.
    auto max_id = MaxLiveId(leaf);
    if (!max_id.ok()) return max_id.status();
    if (id < max_id.value()) {
        return SplitLeafAndInsert(store, descent.value(), leaf_id, id, payload, trx_id,
                                  owner_oid);
    }

    // The leaf this one is being spliced in *front of*, read before anything
    // is created because the descent's span is in hand here and the splice
    // below has to hand it on. While ids were monotonic the leaf reached by
    // an append-split was always the rightmost one, so this was always
    // kInvalidPageId and CreateEmptyAs's default happened to be right. A
    // caller-supplied id (docs/spec/heap-and-tuple.md section 4.1) reaches a full
    // leaf in the *middle* of the chain - `id` above everything in it, still
    // below the next leaf's min_key - and dropping the link there truncates
    // the chain: every leaf past the splice vanishes from every sequential
    // scan while still answering a descent.
    const PageId right_sibling = leaf.next_page_id();

    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [new_leaf_id, new_leaf_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> new_leaf_bytes = new_leaf_bytes_ref.bytes();

    auto new_leaf = heap::PageView::CreateEmptyAs(new_leaf_bytes, /*min_key=*/id,
                                                   PageType::kBtreeLeaf, owner_oid);
    if (!new_leaf.ok()) return new_leaf.status();
    // Safe to publish before the tuple is in: nothing reaches this page
    // until the old leaf's link is repointed at it, below.
    new_leaf.value().set_next_page_id(right_sibling);

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
    heap::PageView(leaf_again.value().bytes()).set_next_page_id(new_leaf_id);

    // Redo order: the new leaf's PAGE_INIT (which the HEAP_INSERT then
    // fills), then the old leaf's image carrying the link that reaches it,
    // then the ancestors. The images are self-contained, so the order among
    // them is not load-bearing; what matters is that all of them precede
    // the HEAP_INSERT the caller emits for the tuple.
    out.Record(new_leaf_id, /*is_new_page=*/true, /*min_key=*/id);
    out.Record(leaf_id, /*is_new_page=*/false, 0);

    // `sep` is the new subtree's low key, which is exactly the new leaf's
    // min_key - the same number, never a separately derived boundary
    // (btree_page.hpp's routing rule).
    return PromoteSeparator(store, descent.value(), /*sep=*/id, /*child=*/new_leaf_id,
                            std::move(out), owner_oid);
}

StatusOr<Location> BtreeLookup(storage::PageStore& store, PageId root, std::uint64_t id) {
    auto descent = DescendTo(store, root, id, /*leaf_for_write=*/false);
    if (!descent.ok()) return descent.status();
    const PageId leaf_id = descent.value().path[descent.value().depth];
    heap::PageView leaf(descent.value().leaf.bytes());

    auto slot = FindSlotForId(leaf, id, leaf.slot_count());
    if (slot.ok()) return Location{leaf_id, slot.value()};
    if (slot.status().code() != StatusCode::kNotFound) return slot.status();
    return Status::NotFound("no tuple with primary key " + std::to_string(id) + " in leaf " +
                            std::to_string(leaf_id));
}

StatusOr<PageId> BtreeSeekLeaf(storage::PageStore& store, PageId root, std::uint64_t id) {
    auto descent = DescendTo(store, root, id, /*leaf_for_write=*/false);
    if (!descent.ok()) return descent.status();
    return descent.value().path[descent.value().depth];
}

// Leftmost leaf, for an ordered scan's starting point. Public (btree.hpp)
// since P4d-3, for callers that own the page loop themselves.
StatusOr<PageId> BtreeLeftmostLeaf(storage::PageStore& store, PageId root) {
    PageId current = root;
    for (std::uint16_t level = 0;; ++level) {
        if (level >= storage::kMaxBtreeDepth) {
            return Status::Corruption("btree from page " + std::to_string(root) + " exceeded " +
                                      std::to_string(storage::kMaxBtreeDepth) + " levels");
        }
        auto bytes = store.GetForRead(current);
        if (!bytes.ok()) return bytes.status();
        if (IsLeafPage(bytes.value().bytes())) return current;

        if (Status s = RequireType(bytes.value().bytes(), current, PageType::kBtreeInternal); !s.ok()) {
            return s;
        }
        current = InternalView(bytes.value().bytes()).leftmost_child();
    }
}

Status BtreeVisit(
    storage::PageStore& store, PageId root, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn) {
    auto first = BtreeLeftmostLeaf(store, root);
    if (!first.ok()) return first.status();
    return BtreeVisitFrom(store, first.value(), access, fn);
}

Status BtreeVisitFrom(
    storage::PageStore& store, PageId first_leaf, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn) {
    PageId current = first_leaf;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = storage::CheckPageWalkBudget(steps, first_leaf, "btree leaf chain");
            !s.ok()) {
            return s;
        }
        // A bad `current` (an invalid first_leaf included) fails inside
        // the fetch - the same shape as ChainVisit, comment included.
        auto next = BtreeVisitLeafPage(store, current, access, fn);
        if (!next.ok()) return next.status();
        if (next.value() == kInvalidPageId) return Status::OK();
        current = next.value();
    }
}

StatusOr<PageId> BtreeVisitLeafPage(
    storage::PageStore& store, PageId leaf_id, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, heap::PageView&, std::uint16_t)>&
        fn) {
    auto bytes = access == storage::PageAccess::kWrite ? store.Get(leaf_id)
                                                       : store.GetForRead(leaf_id);
    if (!bytes.ok()) return bytes.status();
    if (Status s = RequireType(bytes.value().bytes(), leaf_id, PageType::kBtreeLeaf); !s.ok()) {
        return s;
    }
    heap::PageView leaf(bytes.value().bytes());

    const std::uint16_t n = leaf.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        // Liveness is re-tested by the callback through ReadTuple();
        // skipping here as well would mean two reads of every slot.
        auto outcome = storage::ResolveVisit(fn(leaf_id, leaf, i), "BtreeVisit");
        if (!outcome.ok()) return outcome.status();
        // Successful early exit, as in ChainVisit: the leaves to the
        // right of this one are never fetched.
        if (outcome.value() == storage::VisitControl::kStop) return kInvalidPageId;
    }

    return leaf.next_page_id();
}

StatusOr<std::uint16_t> BtreeHeight(storage::PageStore& store, PageId root) {
    auto bytes = store.GetForRead(root);
    if (!bytes.ok()) return bytes.status();
    if (IsLeafPage(bytes.value().bytes())) return std::uint16_t{1};
    if (Status s = RequireType(bytes.value().bytes(), root, PageType::kBtreeInternal); !s.ok()) return s;

    const std::uint16_t level = InternalView(bytes.value().bytes()).level();
    if (level == 0 || level >= storage::kMaxBtreeDepth) {
        return Status::Corruption("btree root " + std::to_string(root) + " reports level " +
                                  std::to_string(level));
    }
    return static_cast<std::uint16_t>(level + 1);
}

StatusOr<std::uint32_t> BtreeLeafCount(storage::PageStore& store, PageId root) {
    std::uint32_t leaves = 0;
    auto first = BtreeLeftmostLeaf(store, root);
    if (!first.ok()) return first.status();

    PageId current = first.value();
    for (;;) {
        if (leaves >= heap::kMaxChainPages) {
            return Status::Corruption("btree leaf chain from page " + std::to_string(root) +
                                      " exceeds " + std::to_string(heap::kMaxChainPages) +
                                      " pages; the sibling links are cyclic or corrupt");
        }
        auto bytes = store.GetForRead(current);
        if (!bytes.ok()) return bytes.status();
        ++leaves;

        const PageId next = heap::PageView(bytes.value().bytes()).next_page_id();
        if (next == kInvalidPageId) return leaves;
        current = next;
    }
}

}  // namespace kds::btree
