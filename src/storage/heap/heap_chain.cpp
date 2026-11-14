#include "kds/storage/heap/heap_chain.hpp"

#include <cstring>
#include <string>

#include "kds/storage/keystone.hpp"

namespace kds::heap {

StatusOr<std::uint32_t> ChainLength(storage::PageStore& store, PageId head) {
    PageId current = head;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = storage::CheckPageWalkBudget(steps, head, "heap chain"); !s.ok()) return s;

        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();

        const PageId next = PageView(bytes.value().bytes()).next_page_id();
        if (next == kInvalidPageId) return steps + 1;
        current = next;
    }
}

namespace {

// The chain's last page, held exclusive **as** the last page (BB-R7).
//
// Each page is taken exclusive while its link is read, and the walk stops
// only at a page whose link is still invalid under that hold - so the page
// handed back is the tail and stays the tail until released: a growth must
// write the tail's link, which needs this hold. Without it a page another
// core linked on in between would be overwritten by a second link and its
// rows orphaned (fixed in BB-S4).
struct HeldTail {
    PageId id = kInvalidPageId;
    storage::PageRef page;
};

StatusOr<HeldTail> WalkToHeldTail(storage::PageStore& store, PageId from, PageId head) {
    PageId current = from;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = storage::CheckPageWalkBudget(steps, head, "heap chain"); !s.ok()) return s;
        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();
        const PageId next = PageView(bytes.value().bytes()).next_page_id();
        if (next == kInvalidPageId) return HeldTail{current, std::move(bytes.value())};
        current = next;
    }
}

// Invariant 3, enforced at the one door tuples come through. Below the
// tail's min_key there is no page in this chain that may legally hold the
// tuple: earlier pages are closed (their ids are all below this one's
// min_key by construction) and this one is barred by the invariant.
Status RefuseBelowTail(PageView& tail, PageId tail_id, std::uint64_t id) {
    if (id >= tail.min_key()) return Status::OK();
    return Status::OutOfRange("id " + std::to_string(id) + " is below page " +
                              std::to_string(tail_id) + "'s min_key " +
                              std::to_string(tail.min_key()) +
                              "; the relation's id sequence has gone backwards");
}

// Everything an insert does once it holds the tail: invariant 3, the
// duplicate check, and the slot or the growth. Every door below ends here.
StatusOr<ChainInsertResult> PlaceOnTail(storage::PageStore& store, HeldTail tail,
                                        std::uint64_t id, std::span<const std::byte> payload,
                                        std::uint64_t trx_id, std::uint64_t owner_oid) {
    PageView page(tail.page.bytes());
    if (Status s = RefuseBelowTail(page, tail.id, id); !s.ok()) return s;

    // O(1) pages, and complete: see the header's ordering property. A
    // delete-marked tuple still holds its key - the key is free only once
    // the slot is physically retired.
    const std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = page.ReadTuple(i);
        if (!tuple.ok()) continue;  // retired or out-of-range slot

        auto existing = KeystoneIdOfPayload(tuple.value().payload);
        if (!existing.ok()) return existing.status();
        if (existing.value() == id) {
            return Status::AlreadyExists("duplicate tuple key " + std::to_string(id) +
                                          " already present at page " +
                                          std::to_string(tail.id) + " slot " +
                                          std::to_string(i));
        }
    }

    auto slot = page.InsertTuple(payload, trx_id);
    if (slot.ok()) {
        return ChainInsertResult{tail.id, slot.value(), /*grew_chain=*/false,
                                 /*linked_from=*/kInvalidPageId, std::move(tail.page), id};
    }
    if (slot.status().code() != StatusCode::kOutOfSpace) {
        return slot.status();  // a real failure, not a full page
    }

    // The tail is full: grow. min_key of the new page is this tuple's id -
    // the smallest id it can ever hold, since ids only increase from here.
    // This is not a split: nothing is moved off the old page, and its
    // min_key is untouched (invariant 2).
    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [new_id, new_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> new_bytes = new_bytes_ref.bytes();

    auto new_page = PageView::CreateEmpty(new_bytes, id, owner_oid);
    if (!new_page.ok()) return new_page.status();

    auto new_slot = new_page.value().InsertTuple(payload, trx_id);
    if (!new_slot.ok()) {
        // A tuple no empty page can hold. The page it was written into is
        // left allocated and empty rather than freed - a free needs BF-R4's
        // replay gate - and an empty page is harmless where a dangling link
        // would not be. It is not linked in below, so nothing reaches it,
        // and no reclaim walk reaches an unlinked page, so it is a stated
        // leak (`drop-table.md` DT1).
        return new_slot.status();
    }

    // Linked last, after the tuple is in the new page: the link is what
    // makes the page reachable, so publishing it earlier would expose an
    // empty page as the tail and let a concurrent walker see a chain whose
    // end holds nothing. Same ordering rule the free map follows in
    // DevicePageStore::FlushPages. Written through the tail's own hold: the
    // frame is pinned, and a pinned frame is never moved or evicted.
    page.set_next_page_id(new_id);

    return ChainInsertResult{new_id, new_slot.value(), /*grew_chain=*/true,
                             /*linked_from=*/tail.id, std::move(tail.page), id};
}

}  // namespace

StatusOr<PageId> ChainTail(storage::PageStore& store, PageId head) {
    auto tail = WalkToHeldTail(store, head, head);
    if (!tail.ok()) return tail.status();
    return tail.value().id;
}

StatusOr<ChainInsertResult> ChainInsert(storage::PageStore& store, PageId head, std::uint64_t id,
                                        std::span<const std::byte> payload,
                                        std::uint64_t trx_id, std::uint64_t owner_oid) {
    if (Status s = RequirePayloadCarries(payload, id); !s.ok()) return s;
    auto tail = WalkToHeldTail(store, head, head);
    if (!tail.ok()) return tail.status();
    return PlaceOnTail(store, std::move(tail.value()), id, payload, trx_id, owner_oid);
}

namespace {

// One page of the walk ChainVisit makes: `fn` per slot of exactly `page_id`,
// and the page the walk continues at - kInvalidPageId when `fn` stopped it
// or the chain ends here.
StatusOr<PageId> ChainVisitOnePage(
    storage::PageStore& store, PageId page_id, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, PageView&, std::uint16_t)>& fn) {
    // Peak pins held by this call: 1, dropped on return - the between-pages
    // suspension property the header promises.
    auto fetched = store.Fetch(page_id, access);
    if (!fetched.ok()) return fetched.status();
    storage::PageRef page_ref = std::move(fetched.value());
    PageView page(page_ref.bytes());

    const std::uint16_t n = page.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        // Liveness is re-tested by the callback through ReadTuple();
        // skipping here as well would mean two reads of every slot.
        auto outcome = storage::ResolveVisit(fn(page_id, page, i), "ChainVisit");
        if (!outcome.ok()) return outcome.status();
        // A successful early exit: the caller has what it came for, and
        // the rest of the chain is not fetched. Distinct from an error
        // precisely so the caller need not tell them apart.
        if (outcome.value() == storage::VisitControl::kStop) return kInvalidPageId;
    }

    return page.next_page_id();
}

}  // namespace

Status ChainVisit(
    storage::PageStore& store, PageId head, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, PageView&, std::uint16_t)>& fn) {
    PageId current = head;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = storage::CheckPageWalkBudget(steps, head, "heap chain"); !s.ok()) return s;
        // A bad `current` (an invalid head included) fails inside the
        // fetch, exactly as the inlined loop did.
        auto next = ChainVisitOnePage(store, current, access, fn);
        if (!next.ok()) return next.status();
        if (next.value() == kInvalidPageId) return Status::OK();
        current = next.value();
    }
}

}  // namespace kds::heap
