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
    // down and reclaim it mid-operation. Peak pins (MG03): 1 here, and 2 for the
    // span of `LeafStillCoversKey`, which takes the right sibling shared
    // and drops it before the descent returns (AT-S5c).
    //
    // `BtreeLookup` hands this same ref out as `Location::leaf`: the latch
    // it carries is what keeps a divide from renumbering the slot the
    // caller reads (btree.hpp `Location`).
    storage::PageRef leaf;
};

// A descent's leaf bytes back at their true fixed extent. The span is
// dynamic only because Descent needs an unset state; anything that reaches
// here came from a store fetch and is exactly one page long.
std::span<std::byte, kPageSize> AsPage(std::span<std::byte> bytes) {
    return std::span<std::byte, kPageSize>(bytes.data(), kPageSize);
}

// **Does this leaf still hold the top of `key`'s range?** (AT-S5c.)
//
// A leaf covers `[min_key, right sibling's min_key)`. Both ends are
// immutable - `min_key` is invariant 2 and the sibling's is its own - so
// the interval changes in exactly one way: a page is spliced in on the
// right, which is what both split shapes do (`BtreeInsert`'s append and
// `SplitLeafAndInsert`'s divide). Asking the sibling is therefore the whole
// test, and it is asked of the *current* chain rather than compared against
// an earlier reading, because the descent may have been routed here before
// the split even began.
//
// **Only the upper end.** A key below `min_key` is not a race - the bound
// cannot move - it is a caller whose id sequence went backwards, and
// `BtreeInsert` reports that in its own words. Answering it here would turn
// a deterministic `OutOfRange` into a retryable conflict.
//
// **Free on the commonest shape.** An issued id lands on the rightmost leaf,
// which has no right sibling and no fetch to make. What pays is a descent
// that lands mid-chain - a named key below the relation's highest, placed
// where it sorts (BD-R2) - one resident-page read.
//
// The caller holds this leaf; the sibling is taken shared and released
// here. The order is always leaf-then-right-neighbour, which is the order a
// scan takes, so it closes no cycle: **nothing anywhere holds a page and
// asks for the one to its left**, and the only other multi-page hold in
// this file is bottom-up (a leaf, then its parents, in `SecureParents`).
// A split reads `next_page_id` and never fetches the page it names.
StatusOr<bool> LeafStillCoversKey(storage::PageStore& store, heap::PageView& leaf,
                                   std::uint64_t key) {
    const PageId right = leaf.next_page_id();
    if (right == kInvalidPageId) return true;  // the rightmost leaf holds every key above it
    auto bytes = store.GetForRead(right);
    if (!bytes.ok()) return bytes.status();
    // `min_key` lives in the heap page *body*, not the common header, so on
    // anything that is not a leaf those bytes are another view's fields -
    // an arbitrary number, which can read as "covered" and admit the write
    // this function exists to refuse. Every other fetch in this file checks
    // its type; this one was the exception (AT-S5c's review, C4).
    if (Status s = RequireType(bytes.value().bytes(), right, PageType::kBtreeLeaf); !s.ok()) {
        return s;
    }
    return key < heap::PageView(bytes.value().bytes()).min_key();
}

// A restart happens when another core split the leaf this descent was
// routed to, so the key belongs on a page that did not exist when the
// parent was read. **A restart makes progress rather than spinning**, and
// that is a property of the split rather than of the bound
// (`storage::kMaxDescentRestarts`): a splitter holds the old leaf exclusive
// across its own `PromoteSeparator`, so a re-descent blocks on that leaf and
// is granted it only once the parent carries the new separator.
using storage::kMaxDescentRestarts;

// Follows child pointers for `key` from `root`, recording the path.
//
// Internal nodes are never modified by a descent, so they are always
// fetched read-only. `leaf_for_write` says whether the *leaf* is about to
// be mutated: an insert takes it for write so the frame is marked dirty, a
// lookup does not, which is what keeps a read-only statement from
// scheduling a write-back of everything it read (page_store.hpp).
//
// ---- The window, and what closes it (AT-S5c) ----------------------------
//
// A write descent cannot hold what it found. The page latch is never
// upgraded (AM-S1, `page_latch.hpp`), so the leaf's shared hold has to go
// before the exclusive one is asked for, and in between the leaf is held by
// nobody. Until AT-S5 that window could not be entered: a relation's writes
// all ran on its owner core, the reactor serialised them, and nothing
// between the two lines suspends. AT-S5 made a write run where the session
// is, so two cores now reach one relation's leaves and the window is real.
//
// **And the window is wider than that re-fetch**, which is the correction
// this stage's own cell forced. There is no latch coupling here: an
// internal node is read, its child chosen, and the node released before the
// child is asked for. So the child's key range can shrink between the
// parent read and the leaf's arrival - before this descent has touched the
// leaf at all, leaving nothing to compare a later reading against. A first
// draft of this check captured the leaf's right link under the shared hold
// and compared it after the exclusive one; it closed the re-fetch window,
// and `btree_race_test.cpp` still failed.
//
// **So what is checked is coverage itself**, which is what a descent
// promises - `BtreeInsert` says it in its own words, *"the descent is
// exact, so the leaf it landed on is the only page that may hold `id`"* -
// asked of the chain as it stands: `LeafStillCoversKey` above. Once `Get`
// returns, the leaf is held **exclusive** for the life of the `Descent`
// (`PageRef` carries the latch, not only the pin) and every splice must
// write this leaf, so the answer cannot go stale under the caller that
// receives it.
StatusOr<Descent> DescendTo(storage::PageStore& store, PageId root, std::uint64_t key,
                            bool leaf_for_write) {
    for (int attempt = 0; attempt <= kMaxDescentRestarts; ++attempt) {
        Descent d;
        PageId current = root;
        // Every path out of this loop returns, except the one `break` below
        // - the chain moved under the window and the descent starts over.
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
                // Re-fetch for write: the frame is already resident, so
                // this is a hash lookup that flips the dirty flag, and it
                // happens only on the insert path where a WAL append
                // dwarfs it. The read handle is dropped **first**, because
                // asking for the latch exclusive while this task's own
                // share is live is the one self-deadlock in the tree's
                // access patterns, found by the census.
                bytes.value().Release();
                auto writable = store.Get(current);
                if (!writable.ok()) return writable.status();
                // Still a leaf. Nothing turns one into an internal node and
                // no page id is ever reused, so this is unreachable rather
                // than defensive - and it costs one byte's compare to read
                // as a diagnosis instead of as a misparsed slot directory.
                if (Status s = RequireType(writable.value().bytes(), current,
                                           PageType::kBtreeLeaf);
                    !s.ok()) {
                    return s;
                }
                heap::PageView relatched(writable.value().bytes());
                auto covers = LeafStillCoversKey(store, relatched, key);
                if (!covers.ok()) return covers.status();
                if (!covers.value()) {
                    break;  // a page was spliced in; `key` is past this leaf now
                }
                d.leaf = std::move(writable.value());
                return d;
            }

            if (Status s = RequireType(bytes.value().bytes(), current, PageType::kBtreeInternal);
                !s.ok()) {
                return s;
            }
            current = InternalView(bytes.value().bytes()).ChildFor(key);
            ++d.depth;
        }
    }
    // Retryable, because nothing about the statement is wrong: the tree
    // moved under it every time it looked, and the next attempt meets a
    // different tree.
    // **What this knows, rather than one guess at why.** Churn is one
    // cause; a **stale root** is the everyday other, and it does not look
    // different from here - a descent from a pre-growth root lands in what
    // is now the leftmost subtree every time, so a key outside that subtree
    // exhausts the bound without anything racing at all. Retryable is right
    // for both (the client's retry crosses a task boundary, where
    // `Revalidate()` drops the memo and the fill re-reads the anchor), so
    // the code does not change - the message stops naming a cause it cannot
    // tell (AT-S5c's review, C5).
    return Status::TxnConflict("btree descent for key " + std::to_string(key) + " from page " +
                               std::to_string(root) + " gave up after " +
                               std::to_string(kMaxDescentRestarts + 1) +
                               " attempts: each leaf it reached had already given the key away. "
                               "Either the chain is being split faster than a descent can cross "
                               "it, or this core is descending from a root that has since grown "
                               "a level");
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

// `held` is the node, held exclusive by the caller (`SecureParents`): a held
// frame is never a victim and its bytes do not move, so the one reference
// serves the read, the CreateNew() below and the rebuild.
StatusOr<InternalDivision> DivideInternalNode(storage::PageStore& store,
                                               const storage::PageRef& held, std::uint64_t sep,
                                               PageId child, storage::InsertPlacement& out,
                                               std::uint64_t owner_oid) {
    const PageId node_id = held.page_id();
    std::vector<BtreeInternalEntryFields> entries;
    std::uint16_t level = 0;
    PageId leftmost = kInvalidPageId;
    bool grown_over = false;
    {
        InternalView node(held.bytes());
        level = node.level();
        leftmost = node.leftmost_child();
        grown_over = node.grown_over();
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
    // The rebuild keeps the page's *own* stamp, not the caller's: a
    // pre-§2a page carries 0, and §2a's no-backfill rule requires it to
    // stay 0 — upgrading it here would quietly make an unreclaimable page
    // reclaimable. On a stamped page the two are equal, so this costs
    // nothing.
    const std::uint64_t old_owner = storage::GetOwnerOid(held.bytes());
    auto rebuilt = InternalView::CreateEmpty(held.bytes(), level, leftmost, old_owner);
    if (!rebuilt.ok()) return rebuilt.status();
    // An old root keeps its mark through the rebuild: a core whose memo
    // still names it is what the mark is read by (`SecureParents`).
    if (grown_over) rebuilt.value().MarkGrownOver();
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

// ---- The parents a split writes, found before it writes (AT-S16) ---------
//
// **The window.** A split puts its separator into the parents its descent
// recorded, and the descent held none of them: it releases each node
// before reading the child. Until AT-S5 nothing could run in between. Since
// then another core can divide a recorded parent first - which moves the
// upper half of its children, and possibly the one this split came from,
// to a new node - and a separator put into the recorded parent then lands
// in the half that no longer routes to its subtree. The append path is no
// exception: two appends serialise on the tail leaf, but a below-mark
// divide under the same parent does not wait for either.
//
// **The ruling** (Q1, 2026-09-26): re-validate the recorded parent, and
// re-descend for the real one on a miss. An internal node carries no
// bounds of its own, so the question is asked as routing - *does it still
// send the insert's key to the node below?* - which is exact because that
// node is held: every change to the range routed to it is a divide of it,
// and a divide of it needs the hold this insert has.
//
// **Every parent is found before anything is written.** The split goes
// only as far up as the nodes are full, so this climbs while they are,
// holding each parent exclusive, and stops at the first with room or at
// the root. A refusal here - a path no descent can revalidate - therefore
// leaves the tree exactly as it found it, instead of a divided node whose
// new half no parent routes to.
//
// **The root** is the one node with no parent to ask. A split that reaches
// past it grows a level, and marks it (`MarkGrownOver`, under the hold it
// already has): the mark is the only thing on the page that says it now
// has a parent. A core whose memo still names it - another core grew over
// it, published or not, and a memo drops at the next task boundary - finds
// the mark here and is refused before it writes, instead of growing a
// second root over it (the operator's word of 2026-09-28).
//
// **Nothing can deadlock on these holds.** They are taken bottom-up, a
// level at a time, and a re-descent reads only above everything this
// insert holds. No code holds a node and asks for one below it or to its
// left: a descent releases each node before the next, and `LeafStillCoversKey`
// reads rightward.
//
// **A restart makes progress rather than spinning**, by `DescendTo`'s
// argument one level up: a divider holds every node it writes until its
// insert returns, so a re-descent either blocks on the node it divided or
// reads it after its separator is in place. An attempt fails only when a
// divide on its route both starts and finishes between two of its reads.
// `storage::kMaxDescentRestarts` bounds the attempts, and what exhausts it
// in practice is a **stale path**: the root the caller named has been grown
// past, and no descent from it reaches the node that now holds the child.
// That refusal is retryable - the retry crosses a task boundary and reads
// the current root - and nothing was written.
struct Parents {
    // held[i] is the parent of the node one level below it - held[0] the
    // leaf's - taken exclusive and checked to route the key to that node.
    std::array<storage::PageRef, storage::kMaxBtreeDepth> held;
    std::uint16_t count = 0;
};

// The node one level above `path[below]` that routes `key` to it, held
// exclusive. The recorded one first - the case where nothing moved - and
// then descents from the named root, `path[0]`, which rewrite the path
// above `below`.
StatusOr<storage::PageRef> FetchParent(storage::PageStore& store,
                                       std::array<PageId, storage::kMaxBtreeDepth>& path,
                                       std::uint16_t below, std::uint64_t key) {
    const PageId child = path[below];
    const std::uint16_t at = static_cast<std::uint16_t>(below - 1);
    for (int attempt = 0; attempt <= kMaxDescentRestarts; ++attempt) {
        if (attempt > 0) {
            if (at == 0) break;  // the recorded parent *is* the named root
            PageId current = path[0];
            for (std::uint16_t i = 0; i < at; ++i) {
                auto bytes = store.GetForRead(current);
                if (!bytes.ok()) return bytes.status();
                if (Status s = RequireType(bytes.value().bytes(), current, PageType::kBtreeInternal);
                    !s.ok()) {
                    return s;
                }
                current = InternalView(bytes.value().bytes()).ChildFor(key);
                path[i + 1] = current;
            }
        }
        auto parent = store.Get(path[at]);
        if (!parent.ok()) return parent.status();
        if (Status s = RequireType(parent.value().bytes(), path[at], PageType::kBtreeInternal);
            !s.ok()) {
            return s;
        }
        if (InternalView(parent.value().bytes()).ChildFor(key) == child) {
            return std::move(parent.value());
        }
    }
    return Status::TxnConflict(
        "btree insert of key " + std::to_string(key) + " could not find the parent of page " +
        std::to_string(child) + ": the path its descent recorded from root " +
        std::to_string(path[0]) + " is stale - no node at that level routes the key to the page "
        "any more. Either that root has since been grown past, or the tree above the page is "
        "being divided faster than a descent can cross it. Nothing was written");
}

StatusOr<Parents> SecureParents(storage::PageStore& store, const Descent& descent,
                                std::uint64_t key) {
    Parents out;
    std::array<PageId, storage::kMaxBtreeDepth> path = descent.path;
    for (std::uint16_t below = descent.depth; below > 0; --below) {
        auto parent = FetchParent(store, path, below, key);
        if (!parent.ok()) return parent.status();
        const bool full = InternalView(parent.value().bytes()).IsFull();
        out.held[out.count++] = std::move(parent.value());
        if (!full) return out;
    }
    // Full all the way up: the split grows a level over the named root,
    // which is held - the leaf, or the last parent taken.
    const bool grown_over = out.count == 0
                                ? heap::PageView(descent.leaf.bytes()).grown_over()
                                : InternalView(out.held[out.count - 1].bytes()).grown_over();
    if (grown_over) {
        return Status::TxnConflict(
            "btree insert of key " + std::to_string(key) + " would grow a level over page " +
            std::to_string(descent.path[0]) + ", which is no longer the root: another core grew "
            "one over it after this core read it. Nothing was written");
    }
    return out;
}

// ---- Propagate a new subtree's separator up the secured parents ----------
//
// Shared by both ways a leaf grows: the append-split above and the true
// division in SplitLeafAndInsert. `sep` is the new subtree's low key and
// `child` the page that holds it; `out` carries whatever the caller has
// already recorded, and comes back with the ancestors appended. Every
// parent it writes is already held (`SecureParents`), and every one but the
// last is full by construction.
// Peak pins (MG03): the leaf and the `k` secured parents stay held
// throughout, and a level adds 1 - the node it creates, dropped before the
// next - on either path, since a divide works in the held parent's own
// bytes. Both split paths release their created leaf before the call, so a
// split holds at most `2 + k` (`SecureParents`' re-descent reads one page at
// a time on the same base). That is under DevicePageStore::kPinCeiling for
// k <= 6; dividing seven 678-way levels takes more leaves than a page id can
// name, and an index's narrower nodes are index_tree.cpp's to state.
StatusOr<storage::InsertPlacement> PromoteSeparator(storage::PageStore& store,
                                                     const Descent& descent, Parents& parents,
                                                     std::uint64_t sep, PageId child,
                                                     storage::InsertPlacement out,
                                                     std::uint64_t owner_oid) {
    std::uint16_t old_root_level = 0;  // the root is a leaf unless proven otherwise

    for (std::uint16_t i = 0; i < parents.count; ++i) {
        const PageId parent_id = parents.held[i].page_id();
        InternalView parent(parents.held[i].bytes());

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
        // Anything else has to divide the node's entries - any leaf split
        // whose separator sorts below the node's top one: a divide, or an
        // append split of a mid-chain leaf, both of which a named key placed
        // where it sorts reaches (BD-R2; heap-and-tuple.md section 4.1).
        // Promoting an interior separator by the cheap path would strand
        // every subtree above it - silent data loss, not a wrong answer
        // someone would notice - so the two are told apart rather than
        // assumed.
        auto appends = SeparatorAboveEveryEntry(parent, sep);
        if (!appends.ok()) return appends.status();

        if (!appends.value()) {
            auto divided =
                DivideInternalNode(store, parents.held[i], sep, child, out, owner_oid);
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
        // A right-split writes nothing into this node - unless it is the
        // root, which the growth below marks, and whose image is then owed.
        // (A divided node records itself; a leaf root's split path records
        // the leaf.)
        if (i + 1 == parents.count) out.Record(parent_id, /*is_new_page=*/false, 0);
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

    // And the old root is marked, under the hold `SecureParents` took, so
    // no core that still names it as the root grows a second one over it.
    // **Last, after everything that can fail**: a mark left by a growth
    // whose allocation failed would claim a parent that does not exist and
    // refuse every later growth of the tree.
    if (parents.count == 0) {
        heap::PageView(descent.leaf.bytes()).MarkGrownOver();
    } else {
        InternalView(parents.held[parents.count - 1].bytes()).MarkGrownOver();
    }
    out.Record(new_root_id, /*is_new_page=*/false, 0);
    out.new_root = new_root_id;
    return out;
}

// ---- Dividing a full leaf (docs/spec/heap-and-tuple.md section 4.1) ------------
//
// Reached when an id sorts *inside* a full leaf - below one of its keys
// (BD-R2, `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`). An
// id above every key the leaf holds takes the append split in
// `PlaceUnderHold`, which moves nothing.
//
// **Where it cuts** (BD-Q11 (a)). A leaf with a right sibling divides at the
// median key. The **rightmost** leaf divides at the insertion point instead:
// the incoming id opens the new leaf, the keys above it move there with it,
// and every key below it stays - so ids that arrive slightly out of issue
// order at `cores > 1` leave the left leaf full rather than half-empty for
// good, where nothing would refill it. The median takes over where the
// insertion point cannot serve: an id below every key (the new leaf would
// share the old one's low key) or a moving half no empty leaf holds.
//
// **Why this does not violate invariant 2 or 3.** The old leaf keeps its
// `min_key` untouched - the division moves the *upper* part out, and
// everything that stays is still at or above the low bound it was created
// with. The new leaf's `min_key` is the split key, which is by construction
// the smallest id moved into it. So both pages satisfy "no tuple below this
// page's min_key" and neither page's min_key is ever rewritten.
//
// **What moving a tuple costs.** Its (page_id, slot) changes, so the old
// leaf's `relayout_epoch` is bumped: every Waystone trail entry and Cabin
// hint pointing into it becomes untrusted at once (section 3.1a's pairing
// rule). Secondary indexes need nothing - an index entry's sort key is
// `key || pk` (index_page.hpp), never a location - and the undo chain is
// likewise addressed by `undo_ptr`, not by where the version sits.
//
// The delete mark travels with the tuple. A delete-marked version carries
// its deleter's `trx_id` and must arrive still marked; re-inserting the
// payload alone would resurrect a row some snapshot has already been told
// is gone. A retired slot carries no key and is dropped.
StatusOr<storage::InsertPlacement> SplitLeafAndInsert(storage::PageStore& store,
                                                       const Descent& descent, Parents& parents,
                                                       PageId leaf_id,
                                                       std::uint64_t id,
                                                       std::span<const std::byte> payload,
                                                       std::uint64_t trx_id,
                                                       std::uint64_t owner_oid) {
    // Every keyed version on the page, copied out whole before anything is
    // written: a Tuple's `payload` is a view into the page, and the page is
    // about to be reformatted under it. Slot order is key order (BD-R1), so
    // the incoming row is merged in where it sorts and the vector stays
    // sorted - no sort, and one cut.
    struct Version {
        std::uint64_t key;
        std::uint64_t trx_id;
        std::uint64_t undo_ptr;
        bool deleted;
        bool incoming;
        std::vector<std::byte> bytes;
    };
    std::vector<Version> merged;
    std::uint64_t old_min_key = 0;
    PageId old_next = kInvalidPageId;
    std::uint64_t old_epoch = 0;
    bool old_grown_over = false;

    {
        heap::PageView leaf(descent.leaf.bytes());
        old_min_key = leaf.min_key();
        old_next = leaf.next_page_id();
        old_epoch = leaf.RelayoutEpoch();
        old_grown_over = leaf.grown_over();

        const std::uint16_t n = leaf.slot_count();
        merged.reserve(static_cast<std::size_t>(n) + 1);
        for (std::uint16_t i = 0; i < n; ++i) {
            auto key = SlotKeystoneId(leaf, i, n);
            if (key.status().code() == StatusCode::kNotFound) continue;  // retired slot
            if (!key.ok()) return key.status();
            if (!merged.empty() && merged.back().key >= key.value()) {
                return Status::Corruption("leaf " + std::to_string(leaf_id) + " holds key " +
                                          std::to_string(key.value()) + " after key " +
                                          std::to_string(merged.back().key) +
                                          "; a leaf's keyed slots ascend (BD-R1)");
            }
            auto tuple = leaf.ReadTuple(i);
            if (!tuple.ok()) return tuple.status();
            merged.push_back({key.value(), tuple.value().trx_id, tuple.value().undo_ptr,
                              tuple.value().deleted, /*incoming=*/false,
                              std::vector<std::byte>(tuple.value().payload.begin(),
                                                     tuple.value().payload.end())});
        }
    }
    const std::size_t keyed = merged.size();
    const auto pos = std::lower_bound(merged.begin(), merged.end(), id,
                                      [](const Version& v, std::uint64_t k) { return v.key < k; });
    const std::size_t incoming_at = static_cast<std::size_t>(pos - merged.begin());
    merged.insert(pos, Version{id, trx_id, /*undo_ptr=*/0, /*deleted=*/false, /*incoming=*/true,
                               std::vector<std::byte>(payload.begin(), payload.end())});

    // Whether a run of versions fits an empty leaf. Always so for SQL's rows,
    // which are one size per relation (invariant 13) - the run is at most the
    // leaf's own keyed rows - and a guard for the storage contract, whose
    // callers may mix sizes.
    const auto fits_one_leaf = [&](std::size_t from) {
        std::size_t need = 0;
        for (std::size_t k = from; k < merged.size(); ++k) {
            need += merged[k].bytes.size() + heap::kTupleHeaderOnDiskSize + heap::kSlotOnDiskSize;
        }
        return need <= heap::kNextPageIdOffset - (heap::kHeapHeaderOffset + heap::kHeaderSize);
    };
    std::size_t cut = 0;
    if (old_next == kInvalidPageId && incoming_at > 0 && fits_one_leaf(incoming_at)) {
        cut = incoming_at;  // the rightmost leaf, at the insertion point
    } else {
        if (keyed < 2) {
            // Nothing to divide: a single keyed tuple filling a whole page
            // means the row is near page-sized, and no boundary makes room
            // for a second. Reported as the space failure it is rather than
            // producing an empty leaf the descent can route to and never
            // satisfy.
            return Status::OutOfSpace("leaf " + std::to_string(leaf_id) +
                                      " is full with fewer than two live tuples; the row is too "
                                      "large for a leaf to hold two of");
        }
        cut = merged.size() / 2;  // the median; both halves are non-empty
    }
    const std::uint64_t split_key = merged[cut].key;

    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [new_leaf_id, new_leaf_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> new_leaf_bytes = new_leaf_bytes_ref.bytes();

    auto new_leaf = heap::PageView::CreateEmptyAs(new_leaf_bytes, /*min_key=*/split_key,
                                                   PageType::kBtreeLeaf, owner_oid);
    if (!new_leaf.ok()) return new_leaf.status();

    // ---- The old leaf is rebuilt, not edited in place ---------------------
    //
    // `RetireSlot` marks a slot dead; it does not give the bytes back, since
    // reclamation is a purge pass's job (heap_page.hpp). Retiring the moved
    // half would therefore leave this page exactly as full as it was, and the
    // division would make room for nothing - which is the whole point of it.
    // So the page is reformatted and the staying half written back.
    //
    // Written through the descent's own hold: a held frame is never a
    // victim and its bytes do not move, so CreateNew() above cannot have
    // left them stale.
    const std::span<std::byte, kPageSize> old_bytes = AsPage(descent.leaf.bytes());

    // `min_key` goes back **unchanged**, which is invariant 2 and the reason
    // a division is legal at all: the low bound a reader may have pruned by
    // without a latch does not move. Everything staying is at or above it
    // already, so invariant 3 holds on both sides.
    // The page's *own* stamp, not the caller's — a pre-§2a page must keep
    // its 0; the internal-node rebuild above states the full argument.
    const std::uint64_t old_owner = storage::GetOwnerOid(old_bytes);
    auto rebuilt = heap::PageView::CreateEmptyAs(old_bytes, old_min_key,
                                                  PageType::kBtreeLeaf, old_owner);
    if (!rebuilt.ok()) return rebuilt.status();
    if (old_grown_over) rebuilt.value().MarkGrownOver();  // DivideInternalNode's reason

    storage::InsertPlacement out;

    // Each half in key order, appended - which is the order BD-R1 asks of
    // its slots.
    for (std::size_t k = 0; k < merged.size(); ++k) {
        const bool to_new = k >= cut;
        heap::PageView& half = to_new ? new_leaf.value() : rebuilt.value();
        auto slot = half.InsertTuple(merged[k].bytes, merged[k].trx_id, merged[k].undo_ptr);
        if (!slot.ok()) return slot.status();
        if (merged[k].deleted) {
            if (Status s = half.DeleteMark(slot.value(), merged[k].trx_id); !s.ok()) return s;
        }
        if (merged[k].incoming) {
            out.page_id = to_new ? new_leaf_id : leaf_id;
            out.slot = slot.value();
        }
    }

    // The sibling link: the new leaf takes the old one's right neighbour,
    // then the old one points at the new. Reformatting zeroed the link, so
    // both ends are restored here rather than one being left as it was.
    new_leaf.value().set_next_page_id(old_next);
    rebuilt.value().set_next_page_id(new_leaf_id);

    // Every tuple on this page changed slot, and some changed page (section
    // 3.1a). Reformatting zeroed the counter, so it is restored to one *past*
    // what it was rather than bumped from zero - an epoch that went backwards
    // would let a trail entry recorded at the old value compare equal again,
    // which is the one thing this field exists to stop.
    storage::SetRelayoutEpoch(old_bytes, old_epoch + 1);

    // Both pages are logged as images, in the split's one record
    // (`BTREE_SPLIT`, BD-R12), with the incoming row already in whichever
    // half took it - so no insert record follows (BD-R3 E2).
    out.Record(new_leaf_id, /*is_new_page=*/false, 0);
    out.Record(leaf_id, /*is_new_page=*/false, 0);

    // The new leaf is written; the descent's hold on the old one is what
    // keeps a descent from outrunning the promotion, so this pin goes now
    // rather than stacking under every level the promotion climbs.
    new_leaf_bytes_ref.Release();
    return PromoteSeparator(store, descent, parents, split_key, new_leaf_id, std::move(out),
                            owner_oid);
}

}  // namespace

// The one search (btree.hpp states the contract). A probe that lands on a
// retired slot steps to the nearest keyed slot inside the window; a window
// with none is empty of keys, which narrows it from above.
StatusOr<LeafPosition> SearchLeaf(heap::PageView& leaf, std::uint64_t id) {
    const std::uint16_t n = leaf.slot_count();
    // Invariant: every keyed slot below `lo` holds a key below `id`, and
    // every keyed slot at or above `hi` a key at or above it.
    std::uint16_t lo = 0;
    std::uint16_t hi = n;
    while (lo < hi) {
        const std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);
        std::uint16_t probe = mid;
        StatusOr<std::uint64_t> key = Status::NotFound("");
        for (; probe < hi; ++probe) {
            key = SlotKeystoneId(leaf, probe, n);
            if (key.ok()) break;
            if (key.status().code() != StatusCode::kNotFound) return key.status();
        }
        if (probe >= hi || key.value() >= id) {
            hi = mid;  // [mid, hi) holds no key, or its first key is at or above `id`
        } else {
            lo = static_cast<std::uint16_t>(probe + 1);
        }
    }
    // The first keyed slot at or after `lo`.
    for (std::uint16_t i = lo; i < n; ++i) {
        auto key = SlotKeystoneId(leaf, i, n);
        if (key.status().code() == StatusCode::kNotFound) continue;
        if (!key.ok()) return key.status();
        return LeafPosition{i, key.value() == id};
    }
    return LeafPosition{n, false};
}

Status FormatRoot(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid) {
    auto leaf = heap::PageView::CreateEmptyAs(page, /*min_key=*/0, PageType::kBtreeLeaf,
                                              owner_oid);
    if (!leaf.ok()) return leaf.status();
    return Status::OK();
}

Status DuplicateKey(std::uint64_t id, PageId leaf_id, std::uint16_t slot, bool deleted) {
    if (deleted) {
        return Status::AlreadyExists("duplicate primary key " + std::to_string(id) +
                                     ": a row with this key was deleted; a Keystone id is bound "
                                     "once (page " +
                                     std::to_string(leaf_id) + " slot " + std::to_string(slot) +
                                     ")");
    }
    return Status::AlreadyExists("duplicate primary key " + std::to_string(id) +
                                 " already present at page " + std::to_string(leaf_id) + " slot " +
                                 std::to_string(slot));
}

namespace {

// Everything an insert does once its descent holds the leaf: invariant 3,
// the duplicate check, and the placement or the split.
StatusOr<storage::InsertPlacement> PlaceUnderHold(storage::PageStore& store, Descent& descent,
                                                  std::uint64_t id,
                                                  std::span<const std::byte> payload,
                                                  std::uint64_t trx_id, std::uint64_t owner_oid);

}  // namespace

StatusOr<storage::InsertPlacement> BtreeInsert(storage::PageStore& store, PageId root,
                                                std::uint64_t id,
                                                std::span<const std::byte> payload,
                                                std::uint64_t trx_id,
                                                std::uint64_t owner_oid) {
    if (Status s = RequirePayloadCarries(payload, id); !s.ok()) return s;
    auto descent = DescendTo(store, root, id, /*leaf_for_write=*/true);
    if (!descent.ok()) return descent.status();
    return PlaceUnderHold(store, descent.value(), id, payload, trx_id, owner_oid);
}

namespace {

StatusOr<storage::InsertPlacement> PlaceUnderHold(storage::PageStore& store, Descent& descent,
                                                  std::uint64_t id,
                                                  std::span<const std::byte> payload,
                                                  std::uint64_t trx_id, std::uint64_t owner_oid) {
    const PageId leaf_id = descent.path[descent.depth];
    heap::PageView leaf(descent.leaf.bytes());

    // Invariant 3, enforced at the one door tuples come through - and the
    // descent already guarantees no other leaf may hold this id, so being
    // below this leaf's low key means the id sequence went backwards.
    if (id < leaf.min_key()) {
        return Status::OutOfRange("id " + std::to_string(id) + " is below leaf " +
                                  std::to_string(leaf_id) + "'s min_key " +
                                  std::to_string(leaf.min_key()) +
                                  "; the relation's id sequence has gone backwards");
    }

    // The one search (BD-R2): presence, the slot the key sorts to, and
    // whether that is past every key here. Complete, unlike the heap chain's
    // tail-only check: the descent is exact, so the leaf it landed on is the
    // only page that may hold `id`, and a delete-marked tuple holds its key
    // for the life of the relation (BD-R4).
    auto at = SearchLeaf(leaf, id);
    if (!at.ok()) return at.status();
    if (at.value().present) {
        auto held = leaf.ReadTuple(at.value().at);
        if (!held.ok()) return held.status();
        return DuplicateKey(id, leaf_id, at.value().at, held.value().deleted);
    }

    const bool appends = at.value().at == leaf.slot_count();

    storage::InsertPlacement out;

    if (auto slot = leaf.InsertTupleAt(at.value().at, payload, trx_id); slot.ok()) {
        out.page_id = leaf_id;
        out.slot = slot.value();
        out.held.push_back(std::move(descent.leaf));
        return out;  // the common case: no structural change at all
    } else if (slot.status().code() != StatusCode::kOutOfSpace) {
        return slot.status();  // a real failure, not a full leaf
    }

    // ---- The leaf is full ------------------------------------------------
    //
    // Two shapes. When `id` sorts above every key in the leaf the growth is
    // an *append*: a fresh leaf, nothing moved, which is what a monotonic id
    // sequence produces and is handled below. When `id` sorts below one of
    // its keys the leaf must divide, which is SplitLeafAndInsert's job.
    //
    // Either shape writes a separator into the parents, so they are found
    // and held first (`SecureParents`, AT-S16): a refusal there leaves the
    // tree as it was, with the tuple not inserted.
    auto parents = SecureParents(store, descent, id);
    if (!parents.ok()) return parents.status();
    // What the caller logs under (AT-S21): the leaf and every parent the
    // split writes, handed out still held once the split is done.
    auto hand_out = [&](storage::InsertPlacement& placed) {
        placed.held.push_back(std::move(descent.leaf));
        for (std::uint16_t i = 0; i < parents.value().count; ++i) {
            placed.held.push_back(std::move(parents.value().held[i]));
        }
    };

    if (!appends) {
        auto divided = SplitLeafAndInsert(store, descent, parents.value(), leaf_id, id,
                                          payload, trx_id, owner_oid);
        if (divided.ok()) hand_out(divided.value());
        return divided;
    }

    // The leaf this one is being spliced in *front of*, read before anything
    // is created because the descent's span is in hand here and the splice
    // below has to hand it on. An append split of the rightmost leaf has
    // none, and CreateEmptyAs's default would happen to be right; one of a
    // leaf in the *middle* of the chain - `id` above every key in it, still
    // below the next leaf's min_key - has one, and dropping the link there
    // truncates the chain: every leaf past the splice vanishes from every
    // sequential scan while still answering a descent.

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
    // Logged as an image in the split's one record (BTREE_SPLIT, BD-R12),
    // which carries the link mid-chain and the row in it: a PAGE_INIT plus
    // the row's insert could not carry the link (AT-S21's survey), and two
    // records could be cut apart.
    out.Record(new_leaf_id, /*is_new_page=*/false, /*min_key=*/id);

    // ---- The separator first, the sibling link last (H9) -----------------
    //
    // The two writes that publish a new leaf go to *different* structures -
    // the parent's separator makes it reachable to a descent, the old leaf's
    // link makes it reachable to a scan - and this path cannot make them one
    // write. So the order is chosen by which half-applied state the engine
    // can survive, and only one of them can be:
    //
    //   - **link written, separator not** is unsurvivable. The leaf is in
    //     the sibling chain and routed by nothing, so the *old* leaf still
    //     takes every id the new one holds. It is full, so the next such id
    //     appends yet another leaf and splices it in front - between the old
    //     leaf and the unrouted one - and the chain now descends: the sim
    //     read it as `min_key 78 does not exceed a predecessor page's max id
    //     183` (H9, seed 20260826003). Ascending leaves are what
    //     `heap-and-tuple.md` section 3.1b's page-wise ordering rests on.
    //   - **separator written, link not** leaves the created page allocated
    //     and unreachable to a scan, which is the residue this file already
    //     accepts twice above: harmless, because no tuple that any caller
    //     kept is in it - a failed promote fails the insert.
    //
    // `sep` is the new subtree's low key, which is exactly the new leaf's
    // min_key - the same number, never a separately derived boundary
    // (btree_page.hpp's routing rule). The new leaf's pin goes first, as
    // SplitLeafAndInsert's does, rather than stacking under every level.
    new_leaf_bytes_ref.Release();
    auto promoted = PromoteSeparator(store, descent, parents.value(), /*sep=*/id,
                                     /*child=*/new_leaf_id, std::move(out), owner_oid);
    if (!promoted.ok()) return promoted.status();

    // **Infallible, which is what makes the ordering above worth anything.**
    // Written through the descent's own handle rather than a fresh fetch,
    // and that handle is already everything this write needs: `DescendTo`
    // took the leaf with `Get` when `leaf_for_write` was set, so the frame
    // is pinned *and* its dirty flag is flipped, and the pin rides in
    // `Descent` exactly so the bytes stay valid for its lifetime (a pinned
    // frame is never a victim, and its bytes are a `unique_ptr` allocation
    // the frame map cannot move). The re-fetch that used to stand here
    // guarded against a frame move a held pin already forbids, duplicated a
    // write fetch already made, and could **fail** - which is how the engine
    // reached the state this ordering exists to prevent.
    leaf.set_next_page_id(new_leaf_id);

    // The new leaf, the ancestors, then the old leaf carrying the link - all
    // images in one BTREE_SPLIT, which redo applies whole or not at all. The
    // order among them was "not load-bearing" only while nothing could end
    // the log between them, which a run of separate records allowed
    // (BD-S1's `SortedLeafCrashTest.ALogCutInsideAnAppendSplit...`).
    promoted.value().Record(leaf_id, /*is_new_page=*/false, 0);
    hand_out(promoted.value());
    return promoted;
}

}  // namespace

// **A miss has to prove it is a miss** (AT-S5c).
//
// A read descent takes no window of its own - it holds what it finds - but
// it shares the write descent's other gap: there is no latch coupling, so
// the parent is released before the child is asked for, and a *divide* on
// another core moves the upper half of that child's keys to a new page in
// between. A lookup routed to the old leaf then finds nothing and answers
// `NotFound` for a row that exists, which is the one answer this file's
// header says a lookup may never give.
//
// **The check is on the miss and not on the hit**, and that is what keeps
// it off the hot path: a hit is authoritative however the chain has moved
// since - the row is *here* - so nothing needs asking. Only an empty-handed
// lookup owes a reason, and it pays one resident-page read to give one.
//
// **A write lookup owes none**: `DescendTo` asked coverage under the
// exclusive hold it returns, and every splice must write this leaf, so a
// miss there is already proved.
//
// **The leaf leaves held**: a hit is the descent's own hold, handed to the
// caller (btree.hpp `Location`).
StatusOr<Location> BtreeLookup(storage::PageStore& store, PageId root, std::uint64_t id,
                               storage::PageAccess access) {
    const bool for_write = access == storage::PageAccess::kWrite;
    for (int attempt = 0; attempt <= kMaxDescentRestarts; ++attempt) {
        auto descent = DescendTo(store, root, id, /*leaf_for_write=*/for_write);
        if (!descent.ok()) return descent.status();
        const PageId leaf_id = descent.value().path[descent.value().depth];
        heap::PageView leaf(descent.value().leaf.bytes());

        auto at = SearchLeaf(leaf, id);
        if (!at.ok()) return at.status();
        if (at.value().present) {
            return Location{leaf_id, at.value().at, std::move(descent.value().leaf)};
        }

        bool covered = for_write;
        if (!covered) {
            auto covers = LeafStillCoversKey(store, leaf, id);
            if (!covers.ok()) return covers.status();
            covered = covers.value();
        }
        if (covered) {
            return Status::NotFound("no tuple with primary key " + std::to_string(id) +
                                    " in leaf " + std::to_string(leaf_id));
        }
        // The key belongs past this leaf now, so the miss says nothing about
        // the row. Descend again; the splitter held this leaf across its own
        // `PromoteSeparator`, so the parent already carries the separator
        // that routes the second attempt correctly.
    }
    return Status::TxnConflict("lookup of primary key " + std::to_string(id) + " from page " +
                               std::to_string(root) + " gave up after " +
                               std::to_string(kMaxDescentRestarts + 1) +
                               " attempts: each leaf it reached had already given the key away. "
                               "Either the chain is being split faster than a descent can cross "
                               "it, or this core is descending from a root that has since grown "
                               "a level");
}

// **No coverage check here, and the reason is directional** (AT-S5c). A
// seek names where an ordered scan starts, and the scan walks *forward*
// from it along `next_page_id`. A split can only move keys to the right, so
// a seek outrun by one lands on the page to the **left** of where the key
// went - and the scan reaches that page next. Landing early costs a page;
// landing late would cost rows, and no split produces it.
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
    auto bytes = store.Fetch(leaf_id, access);
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
