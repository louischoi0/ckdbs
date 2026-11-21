#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/storage/btree/btree.hpp"
#include "kds/storage/heap/rowid.hpp"
#include "kds/storage/insert_placement.hpp"
#include "kds/storage/page_store.hpp"

// **A heap relation's rows** (DN, `docs/spec/heap-and-tuple.md` §3.1b): a
// chain of `kHeapData` pages from `desc_page_id`, linked through each page's
// `next_page_id`, its rows in no key order, each at a rowid
// (`heap::EncodeRowid`) that never changes while the row lives (an `UPDATE`
// is in place, invariant 13). A key access is the Keystone index's descent
// and the rowid's one page; a walk is the chain in page order, which is the
// btree's leaf walk over these pages (`btree::BtreeVisit` and its siblings
// take a data page as a leaf), and a key-ordered descent refuses one.
//
// **A rowid is resolved, never trusted** (DN-R3): every fetch by rowid
// checks the slot holds a tuple whose Keystone is the one the caller is
// after, and answers `NotFound` otherwise - so an entry that outlived its
// slot (DN-R2's defect) is a miss, never another row.
//
// Concurrency: the functions take page latches through the store, one data
// page at a time but for a growth, which holds the new page and then the
// head (a new page is reachable by no one else, so the order cannot cycle).
// `HeapSpace` is the instance's, under its own mutex; nothing it holds is
// authoritative, and every hint it gives is verified under the page's
// exclusive latch before a row is placed by it.

namespace kds::exec {

// The instance's placement hints for every heap relation (DN-Q7 (a)): the
// page each core fills, and the slots the slot release found free. Memory
// only - a restart forgets them, a slot release finds the free slots again,
// and a core with no page fills the head's chain from the head.
class HeapSpace {
public:
    PageId CurrentPage(std::uint64_t oid, std::uint32_t core) const;
    void SetCurrentPage(std::uint64_t oid, std::uint32_t core, PageId page);
    std::optional<heap::Rowid> TakeFree(std::uint64_t oid);
    void AddFree(std::uint64_t oid, heap::Rowid rowid);
    std::size_t FreeCount(std::uint64_t oid) const;
    void Forget(std::uint64_t oid);

private:
    struct Relation {
        std::unordered_map<std::uint32_t, PageId> current;
        std::vector<heap::Rowid> free;
    };
    mutable std::mutex mu_;
    std::unordered_map<std::uint64_t, Relation> relations_;
};

// The heap's Keystone index (`kIndexFlagKeystone`), or nullptr on any other
// relation. Every heap has exactly one.
const catalog::TableAccess::IndexRef* KeystoneIndexOf(const catalog::TableAccess& access);

// The row at `rowid`, held under `mode`, checked to be `tuple_key`'s: a
// `kHeapData` page of this relation, a slot in range holding a tuple - live
// or delete-marked - whose Keystone is `tuple_key`. `NotFound` for any
// other slot (DN-R3); `Corruption` for a page that is not this heap's.
StatusOr<btree::Location> FetchHeapRow(storage::PageStore& store,
                                       const catalog::TableAccess& access,
                                       std::uint64_t rowid, std::uint64_t tuple_key,
                                       storage::PageAccess mode = storage::PageAccess::kRead);

// **Where a relation's row keyed `tuple_key` is** - every relation's
// `btree::BtreeLookup`: a btree's descent, or a heap's Keystone index
// probe and the rowid's page. The answer is authoritative for a hit and a
// miss alike (K3), a delete-marked row included; `NotFound` is absence.
StatusOr<btree::Location> LocateRow(storage::PageStore& store,
                                    const catalog::TableAccess& access, std::uint64_t tuple_key,
                                    storage::PageAccess mode = storage::PageAccess::kRead);

// The rowids the Keystone index names for `tuple_key`, in index order:
// what `LocateRow` resolves, for a caller that resolves them itself.
StatusOr<std::vector<std::uint64_t>> KeystoneRowids(storage::PageStore& store,
                                                    const catalog::TableAccess& access,
                                                    std::uint64_t tuple_key);

// **A heap row's placement** (DN-Q7 (a), DN-R10): a free slot the slot
// release left, then the page this core fills, then a new page linked on
// after the head - each taken only where its `min_key` admits `tuple_key`
// (invariant 3). A new page's `min_key` is `tuple_key` (DN-Q11 (a)).
//
// The answer holds the page the row is on, exclusive, in `held[0]`, and
// on a growth the head after it (`held[1]`), whose link now names the new
// page; the caller logs under those holds and releases them. **A growth is
// logged as a split is** (`placed.restructured()`, the new page and the
// head as `placed.changes()`): one BTREE_SPLIT imaging both, the row
// already on the new page, so the page and the link that reaches it replay
// whole or not at all. Otherwise the row is one HEAP_INSERT at its slot.
// `payload` is the encoded tuple, its Keystone `tuple_key`.
struct HeapPlacement {
    storage::InsertPlacement placed;
    // The row took a freed slot (its HEAP_INSERT replays through
    // `PageView::ReuseSlot`); a take-back retires it, never frees it, since
    // an index may already hold an entry naming it.
    bool reused = false;
};
StatusOr<HeapPlacement> HeapInsert(storage::PageStore& store, const catalog::TableAccess& access,
                                   HeapSpace& space, std::uint32_t core, std::uint64_t tuple_key,
                                   std::span<const std::byte> payload, std::uint64_t trx_id);

}  // namespace kds::exec
