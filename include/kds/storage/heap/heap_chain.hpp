#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "kds/base/status.hpp"
#include "kds/storage/heap/heap_page.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/visit.hpp"

// A relation's heap as a linked chain of pages, walked through the
// `next_page_id` link every heap page already reserves (heap_page.hpp's
// tail reservation). Until this file existed a relation *was* one page:
// the dispatcher wrote straight into `sys.tables.desc_page_id` and an
// INSERT failed with OutOfSpace once 8 KB of tuples had landed there.
//
// ---- What this does, and what it deliberately does not ------------------
//
// Growth here is **tail append**, never a split: when the last page in the
// chain has no room, a brand-new page is allocated, linked on, and given
// `min_key` = the id of the tuple that caused the growth. No existing
// page's contents are ever divided, and no `min_key` is ever chosen for a
// page that already holds tuples - which is what keeps the heap page
// **split policy** (an open decision in CLAUDE.md: how a full page's
// contents get divided and where the new boundary goes) genuinely open.
// When a split policy is decided it slots in beside this, and pages this
// file produced stay valid, because everything below rests only on the
// confirmed invariants:
//
//   - `min_key` is immutable after page creation (invariant 2).
//   - No tuple with `id < min_key(page)` ever lands in that page
//     (invariant 3) - enforced here, at the one place tuples enter.
//   - Ids are system-issued, unique and monotonically increasing per
//     relation (invariant 10, `Catalog::AllocateRowId`).
//
// ---- The ordering property that falls out ------------------------------
//
// Because ids only increase and every insert goes to the tail, each page's
// ids are entirely below the next page's `min_key`:
//
//   page A: [min_key(A) .. x]   page B: [min_key(B) .. y]   with x < min_key(B)
//
// so the chain is key-ordered page by page (tuples *within* a page stay
// unordered - that is the "semi-sorted" of heap-and-tuple.md section 3).
// Two useful consequences, both relied on below:
//
//   1. A duplicate of an id being inserted could only be in the tail page,
//      because every earlier page holds ids strictly below the tail's
//      `min_key`. So the duplicate check is O(1) pages, not O(chain) - the
//      difference between an O(n) and an O(n^2) bulk load. It is still a
//      sanity check on the id sequence, not a uniqueness index; that is
//      the B+ tree's job when it exists.
//   2. An id *below* the tail's `min_key` cannot be placed at all without
//      violating invariant 3, and means the id sequence has gone backwards
//      (a rolled-back or corrupted `sys.tables.next_id`). Refused loudly
//      rather than written somewhere it does not belong.
//
// ---- What is not here yet -----------------------------------------------
//
// No free-space reuse: a page that fills, then has rows deleted, is not
// revisited - the chain only ever grows at the tail. Reclaiming that space
// needs page compaction, which needs a transaction manager to know no
// snapshot still needs the bytes (heap_page.hpp's RetireSlot comment).
// Consequently a delete-heavy relation grows monotonically, which is worth
// knowing before pointing a benchmark at it.
//
// Concurrency: none of its own. Every function here takes and releases the
// page spans it needs through the PageStore; the caller holds whatever
// pin/latch discipline applies (CLAUDE.md's page-latch consistency model),
// exactly as with PageView.

namespace kds::heap {

// Bound on how many pages a walk will follow before declaring the chain
// corrupt. A cycle in the links would otherwise be an infinite loop inside
// a request. 2^20 pages is 8 GiB of heap for one relation - far past
// anything this engine handles today, and cheap to check once per hop.
inline constexpr std::uint32_t kMaxChainPages = 1u << 20;

struct ChainInsertResult {
    PageId page_id;          // where the tuple actually landed
    std::uint16_t slot;      // slot index within that page
    bool grew_chain = false; // true if this insert allocated a new tail page

    // The page whose `next_page_id` was repointed at `page_id`, or
    // kInvalidPageId when the chain did not grow. Reported because growth
    // mutates *two* pages, and a caller that logs the insert has to log
    // both - the new page's contents are worthless to redo if the link
    // that makes it reachable was never recorded.
    PageId linked_from = kInvalidPageId;
};

// Follows the chain from `head` and returns its last page id (the page
// whose next_page_id is kInvalidPageId). Fails with Corruption if the
// chain exceeds kMaxChainPages, or with whatever the store reports for a
// link that does not resolve.
StatusOr<PageId> ChainTail(storage::PageStore& store, PageId head);

// Number of pages in the chain, head included. Same failures as
// ChainTail(); for `SHOW`-style reporting and tests, not a hot path.
StatusOr<std::uint32_t> ChainLength(storage::PageStore& store, PageId head);

// Inserts `payload` (whose leading Keystone word must carry `id`) into the
// chain, extending it if the tail page is full.
//
// Fails with:
//   AlreadyExists  a live tuple in the tail page already carries `id`
//   OutOfRange     `id` is below the tail page's min_key (invariant 3) -
//                  the id sequence has gone backwards
//   Corruption     the payload is too short to hold a Keystone word, or
//                  its Keystone id does not match `id`
//   ...            whatever the store reports when a new page cannot be
//                  allocated (e.g. OutOfSpace), in which case nothing was
//                  written and the chain is unchanged
StatusOr<ChainInsertResult> ChainInsert(storage::PageStore& store, PageId head, std::uint64_t id,
                                        std::span<const std::byte> payload, std::uint64_t trx_id);

// Calls `fn` once per live slot of every page in the chain, in chain order
// (which is id order page-wise, per the ordering property above). The
// PageView handed to `fn` is mutable so a scan can overwrite in place -
// UPDATE's HOT path does exactly that.
//
// `fn` says what happens next (storage/visit.hpp): kContinue walks on,
// kStop ends the walk **successfully** - the return is Status::OK() and
// the pages after the stopping point are never fetched - and a non-ok
// Status stops it and is returned as-is. A visitor that returns
// Status::OK() instead of kContinue is refused with InvalidArgument rather
// than being guessed at.
//
// Retired and out-of-range slots are skipped, matching PageView::ReadTuple.
// Delete-marked tuples ARE visited: whether a reader may see one depends on
// its snapshot versus the deleter's trx_id, which is not this layer's call.
//
// `access` says which of those two `fn` actually is. kRead fetches pages
// through PageStore::GetForRead(), so a SELECT does not leave the whole
// relation dirty behind it; kWrite is required of any visitor that
// overwrites or delete-marks, and passing kRead from one of those loses
// the write.
Status ChainVisit(
    storage::PageStore& store, PageId head, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, PageView&, std::uint16_t)>& fn);

}  // namespace kds::heap
