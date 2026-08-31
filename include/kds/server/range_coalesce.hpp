#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "kds/base/log.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/catalog.hpp"
#include "kds/catalog/oid.hpp"
#include "kds/storage/device_page_store.hpp"
#include "kds/wal/manager.hpp"

// AX — coalescing a split relation back to one range so an auxiliary DDL
// can build on it (`docs/spec/crosscore.md` §6c; ratification
// `instructions/v2.7.0/ratification-ax.md`; build order
// `instructions/v2.7.0/ax-coalesce-on-auxiliary-ddl.md`).
//
// `range_alloc.hpp` is the other end of the same axis: it opens a
// boundary, this closes every one of them. The two are deliberately
// separate files and not two halves of one, because they run on different
// cores for different reasons - opening runs on core 0 for a *peer's*
// lease refill, closing runs on core 0 for core 0's own DDL - and the
// only thing they share is `sys.ranges`, which the catalog owns.
//
// ---- What a merge is, in one paragraph -------------------------------
//
// The per-range chains are linked tail-to-head in `lo` order and no tuple
// moves. Ranges partition the id space, a heap range is its own chain
// (CC8), a range head carries `min_key = lo` (`CreateRangeEntryPage`), and
// ids ascend page by page inside a chain - so the concatenation is
// key-ordered exactly as a never-split relation's chain is, and invariants
// 2 and 3 hold without a check. The cost is therefore O(pages handed off),
// not O(rows), which is what §6c records as H-AX1's verdict.
//
// ---- The scope, which §6a's forward gates make narrow ------------------
//
// A split relation is a **heap** relation (D1 of
// `workplan-range-directory.md` is not taken, so a btree never splits) and
// carries no auxiliary at all: non-spilling, unindexed, un-cabined,
// FK-free, un-asserted. So a merge moves heap pages and only heap pages -
// no var-heap page, no index entry, no cabin entry set, no assertion
// registry. Nothing below has an arm for any of them, and that is the
// gates' doing rather than an omission.
//
// ---- Concurrency ------------------------------------------------------
//
// `PlanCoalesce` reads pages and writes nothing. `LinkSegments` writes,
// and must run **on the absorber's core**, inside one task, over pages the
// absorber already holds write rights on. Neither takes a lock: the
// relation is quiesced for the merge's duration (§6c step 0) and every
// core is single-threaded.

namespace kds::server {

// How many pages one acquisition call is handed. The sequence it drives
// (PL §9 rule 6: fault, acquisition record, restamp, flush) holds a frame
// per page for the length of the call, so a whole relation in one call
// would pin the relation. Not a tuning knob and deliberately not a config
// key - it bounds a working set, and the number that matters is the
// eviction budget, which is already a key (`buffer_pool_frames`).
inline constexpr std::size_t kAdmitChunkPages = 64;

// The page-level walk of one range's chain, bounded by the range: follow
// `next_page_id` from `head` and stop at the first page whose `min_key`
// reaches `hi`.
//
// **A page walk and not a `ChainVisit`, because an empty page is a page.**
// The visiting forms call their callback per *slot*, so a range whose head
// is still empty - which every range's head is at birth
// (`Catalog::CreateRangeEntryPage`) - would be named by nothing. A merge
// that missed it would leave a page owned by the departed core,
// unreachable from the absorber's rights and stamped by a stream that no
// longer routes to it.
//
// Appends to `out`; the caller clears. Fails Corruption if `head` itself
// is at or above `hi`, which is a directory and a chain that disagree.
Status CollectRangePages(storage::PageStore& store, PageId head, std::uint64_t hi,
                         std::vector<PageId>& out);

// One range as the merge sees it: where its chain starts and ends, whose
// core holds it, and every page in it.
struct CoalesceSegment {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::uint32_t owner_core = 0;
    PageId entry_page = kInvalidPageId;
    // The last page of this range's chain - where the *next* range's head
    // gets linked on. `entry_page` itself when the range is one page.
    PageId tail = kInvalidPageId;
    // Chain order, `entry_page` first. Includes empty pages: a range's
    // head is created empty and a page-less walk would never name it,
    // which is why this is a page walk and not a `ChainVisit`.
    std::vector<PageId> pages;
};

// The merge plan (AX1): which core absorbs, and every range in `lo` order.
struct CoalescePlan {
    catalog::Oid rel_oid = 0;
    // AX-D3: the core holding the most pages; on a tie the lowest
    // `core_id`. Determinism and test reproducibility are the tie rule's
    // whole ground, and it is a proposal accepted at ratification rather
    // than a measured choice.
    std::uint32_t absorber = 0;
    // Ascending by `lo`; `segments.front()` is the `lo = 0` range, whose
    // `entry_page` is `sys.tables.desc_page_id` and stays the merged
    // relation's chain head whatever the absorber turns out to be.
    std::vector<CoalesceSegment> segments;
    // Pages the absorber does not already hold - the merge's cost, and
    // what AX8 divides its microseconds by.
    std::uint64_t pages_to_move = 0;
    std::uint64_t pages_total = 0;
};

// AX1. Walks every range's chain and picks the absorber.
//
// **Precondition: the ranges are quiesced and flushed** (§6c steps 0-1).
// This walk reads the device through `store`, so a page a peer holds
// dirty and unflushed is invisible to it - and an unflushed *link* would
// end a chain early, undercounting a range and, worse, leaving its later
// pages out of the page list the handoff and the acquisition are built
// from. The caller owns that ordering; nothing here can check it.
//
// Runs on core 0, which holds no lease and may therefore fault any page
// (`DevicePageStore::MayFault`).
//
// Refuses `InvalidArgument` on a relation with fewer than two ranges -
// there is nothing to merge, and a caller that reached here without
// asking has a bug the merge would otherwise hide by doing nothing.
StatusOr<CoalescePlan> PlanCoalesce(catalog::Catalog& catalog, storage::PageStore& store,
                                    catalog::Oid rel_oid);

// §6c step 4, run **on the absorber**: acquire every page of every
// segment this core does not already hold, then link the chains
// tail-to-head in `lo` order.
//
// Acquisition is PL §9 rule 6's - a `PAGE_HANDOFF` naming this core,
// whose LSN the restamp writes into `page_lsn` - and it is what makes the
// move survive a restart: leases and grants are memory-resident, the
// stamp is the page's own durable statement of the same fact, and without
// it the departed owner re-claims the page at its next fault
// (`TryClaimByStamp`). The caller must have granted this core write
// rights first (§6c step 3); a page this core cannot write is a refusal
// here, never a silent skip.
//
// **Idempotent.** A page already stamped by this core is skipped, and a
// link already written is a `next_page_id` set to the value it holds. A
// merge interrupted by a crash is re-run rather than repaired, and this
// is what makes the re-run cost only what was not done.
Status LinkSegments(storage::DevicePageStore& store, wal::WalManager* wal,
                    const CoalescePlan& plan, std::uint32_t core_id, Logger* log = nullptr);

// Per-core coalesce accounting (AX6). Absent from `SHOW META` until the
// first caller exists, the absent-rather-than-zeroed rule.
struct CoalesceCounters {
    std::uint64_t runs = 0;
    std::uint64_t pages_moved = 0;
    std::uint64_t us_total = 0;
};

}  // namespace kds::server
