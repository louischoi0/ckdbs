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
// write the tail's link, which needs this hold. Until BB the walk held
// nothing at the end (`ChainTail`) and the caller took the page it named
// exclusive without asking again, so a page another core had linked on in
// between was overwritten by a second link and its rows orphaned (AT-S10b's
// review C2; its bug entry went with this fix, BB-S4).
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

// From the hint when one is offered (the header's argument: a former chain
// page is always a valid start, because next_page_id is write-once and pages
// never leave a chain), from the head otherwise - and from the head again if
// the hinted walk fails, so a damaged hint costs one retry and never an
// answer.
StatusOr<HeldTail> HoldTail(storage::PageStore& store, PageId head, const PageId* tail_hint) {
    const bool hinted = tail_hint != nullptr && *tail_hint != kInvalidPageId;
    auto held = WalkToHeldTail(store, hinted ? *tail_hint : head, head);
    if (!held.ok() && hinted) held = WalkToHeldTail(store, head, head);
    return held;
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
                                        std::uint64_t trx_id, std::uint64_t owner_oid,
                                        PageId* tail_hint) {
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
            return Status::AlreadyExists("duplicate primary key " + std::to_string(id) +
                                          " already present at page " +
                                          std::to_string(tail.id) + " slot " +
                                          std::to_string(i));
        }
    }

    auto slot = page.InsertTuple(payload, trx_id);
    if (slot.ok()) {
        if (tail_hint != nullptr) *tail_hint = tail.id;
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
        // left allocated and empty rather than freed: the store has no
        // free-page path yet (page.md's SpaceManager), and an empty linked
        // page is harmless where a dangling link would not be. It is not
        // linked in below, so nothing reaches it.
        return new_slot.status();
    }

    // Linked last, after the tuple is in the new page: the link is what
    // makes the page reachable, so publishing it earlier would expose an
    // empty page as the tail and let a concurrent walker see a chain whose
    // end holds nothing. Same ordering rule the free map follows in
    // DevicePageStore::FlushPages. Written through the tail's own hold: the
    // frame is pinned, and a pinned frame is never moved or evicted.
    page.set_next_page_id(new_id);

    if (tail_hint != nullptr) *tail_hint = new_id;
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
                                        std::uint64_t trx_id, std::uint64_t owner_oid,
                                        PageId* tail_hint) {
    if (Status s = RequirePayloadCarries(payload, id); !s.ok()) return s;
    auto tail = HoldTail(store, head, tail_hint);
    if (!tail.ok()) return tail.status();
    return PlaceOnTail(store, std::move(tail.value()), id, payload, trx_id, owner_oid, tail_hint);
}

StatusOr<ChainInsertResult> ChainInsertIssued(storage::PageStore& store, PageId head,
                                              const storage::IssueUnderHold& issue,
                                              std::uint64_t trx_id, std::uint64_t owner_oid,
                                              PageId* tail_hint) {
    auto tail = HoldTail(store, head, tail_hint);
    if (!tail.ok()) return tail.status();
    auto payload = issue();
    if (!payload.ok()) return payload.status();
    auto id = KeystoneIdOfPayload(payload.value());
    if (!id.ok()) return id.status();
    return PlaceOnTail(store, std::move(tail.value()), id.value(), payload.value(), trx_id,
                       owner_oid, tail_hint);
}

StatusOr<ChainInsertResult> ChainInsertNamed(storage::PageStore& store, PageId head,
                                             std::uint64_t id, std::span<const std::byte> payload,
                                             const storage::AdmitUnderHold& admit,
                                             std::uint64_t trx_id, std::uint64_t owner_oid,
                                             PageId* tail_hint) {
    if (Status s = RequirePayloadCarries(payload, id); !s.ok()) return s;
    auto tail = HoldTail(store, head, tail_hint);
    if (!tail.ok()) return tail.status();
    // Below the tail's min_key is below a placed id, so below the mark: the
    // chain's answer, with no read of page 7 (BB-R7), worded as BB-R12
    // worded it for every relation then and for the heap now (BD-Q4).
    PageView page(tail.value().page.bytes());
    if (id < page.min_key()) {
        return Status::OutOfRange(
            "primary key " + std::to_string(id) +
            " is below the relation's high-water mark: the chain's tail, page " +
            std::to_string(tail.value().id) + ", opens at " + std::to_string(page.min_key()) +
            ", an id already placed; a named key must sort above every key the relation has "
            "placed or issued");
    }
    // The mark before the duplicate check (BB-R12, kept for the heap by
    // BD-Q4): a key below the mark is refused `OutOfRange` whether or not the
    // tail holds it, so a heap never answers a named key `AlreadyExists`. A
    // btree admits first too, but never refuses there; its descent's
    // duplicate check answers (BD-R5).
    if (Status s = admit(id); !s.ok()) return s;
    return PlaceOnTail(store, std::move(tail.value()), id, payload, trx_id, owner_oid, tail_hint);
}

StatusOr<ChainAppendBatchResult> ChainAppendCarved(storage::PageStore& store, PageId head,
                                                   const CarveUnderHold& carve,
                                                   std::uint64_t trx_id, std::uint64_t owner_oid,
                                                   PageId* tail_hint) {
    auto tail = HoldTail(store, head, tail_hint);
    if (!tail.ok()) return tail.status();

    auto carved = carve();
    if (!carved.ok()) return carved.status();
    const std::span<const std::vector<std::byte>> payloads = carved.value();
    if (payloads.empty()) return Status::InvalidArgument("a carved fill of no rows");

    // Every payload's identity is checked against the contiguous range
    // before anything is placed - ChainInsert's id/payload agreement check,
    // once per row, ahead of the fill.
    auto first = KeystoneIdOfPayload(payloads.front());
    if (!first.ok()) return first.status();
    const std::uint64_t first_id = first.value();
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        auto encoded_id = KeystoneIdOfPayload(payloads[i]);
        if (!encoded_id.ok()) return encoded_id.status();
        if (encoded_id.value() != first_id + i) {
            return Status::Corruption("batch payload " + std::to_string(i) +
                                      " carries Keystone id " +
                                      std::to_string(encoded_id.value()) + ", expected " +
                                      std::to_string(first_id + i));
        }
    }

    ChainAppendBatchResult out;
    out.rows.reserve(payloads.size());

    PageId current = tail.value().id;
    storage::PageRef current_ref = std::move(tail.value().page);
    {
        PageView page(current_ref.bytes());
        if (Status s = RefuseBelowTail(page, current, first_id); !s.ok()) return s;
    }
    out.pages.push_back({current, /*is_new=*/false, kInvalidPageId});

    // Fills `page` from row `i` until it refuses - one fetch, many rows.
    std::size_t i = 0;
    const auto fill = [&](PageView& page, PageId page_id) -> Status {
        while (i < payloads.size()) {
            auto slot = page.InsertTuple(payloads[i], trx_id);
            if (!slot.ok()) {
                if (slot.status().code() != StatusCode::kOutOfSpace) return slot.status();
                break;
            }
            out.rows.push_back({page_id, slot.value()});
            ++i;
        }
        return Status::OK();
    };

    PageView page(current_ref.bytes());
    if (Status s = fill(page, current); !s.ok()) return s;
    while (i < payloads.size()) {

        // Grow, ChainInsert's order (BB-R7): the fresh page is held from its
        // creation through its fill, and linked only once it holds its rows.
        // Its min_key is the id that opens it - the sorted stream's exact
        // best case. The predecessor stays held until the link is written,
        // so no walker reaches past it to a page that is not yet filled.
        auto created = store.CreateNew();
        if (!created.ok()) return created.status();
        auto& [new_id, new_ref] = created.value();
        auto fresh = PageView::CreateEmpty(new_ref.bytes(), first_id + i, owner_oid);
        if (!fresh.ok()) return fresh.status();
        const std::size_t opened_at = i;
        if (Status s = fill(fresh.value(), new_id); !s.ok()) return s;
        if (i == opened_at) {
            // A row no empty page can hold: left allocated and unlinked, as
            // ChainInsert leaves one, so nothing reaches it.
            return Status::OutOfSpace("batch payload " + std::to_string(i) +
                                      " does not fit an empty page");
        }
        page.set_next_page_id(new_id);
        out.pages.push_back({new_id, /*is_new=*/true, current});
        current = new_id;
        current_ref = std::move(new_ref);  // the predecessor's hold ends here
        page = fresh.value();
    }

    if (tail_hint != nullptr) *tail_hint = current;
    return out;
}

Status ChainVisit(
    storage::PageStore& store, PageId head, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, PageView&, std::uint16_t)>& fn,
    storage::ScanFetcher* fetcher) {
    PageId current = head;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = storage::CheckPageWalkBudget(steps, head, "heap chain"); !s.ok()) return s;
        // A bad `current` (an invalid head included) fails inside the
        // fetch, exactly as the inlined loop did.
        auto next = ChainVisitOnePage(store, current, access, fn, fetcher);
        if (!next.ok()) return next.status();
        if (next.value() == kInvalidPageId) return Status::OK();
        current = next.value();
    }
}

StatusOr<PageId> ChainVisitOnePage(
    storage::PageStore& store, PageId page_id, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, PageView&, std::uint16_t)>& fn,
    storage::ScanFetcher* fetcher) {
    // Ring mode is a read path only (spec-eviction §5): a writer's walk
    // takes the ordinary route, because the ring never bypasses the
    // dirty protocol. The visitor's per-page discipline is what makes
    // the ring's stricter lifetime safe: each page is finished before
    // the next fetch can rotate its frame away.
    // Peak pins held by this call: 1. On the two `store` branches it is
    // dropped on return, which is the between-pages suspension property the
    // header promises. **On the ring branch the pin is the fetcher's and
    // outlives this call** (AM-R8): a ScanFetcher holds one pin, and
    // DevicePageStore's ring holds the shared page latch with it, on the
    // page its last Fetch returned, and drops both at its next Fetch or
    // when it is destroyed. The per-page discipline is the same - each page
    // is finished before the next fetch - but the suspension point between
    // pages is not, which is why the header promises it for the no-fetcher
    // forms only.
    storage::PageRef page_ref;
    std::byte* page_data = nullptr;
    if (access == storage::PageAccess::kWrite) {
        auto fetched = store.Get(page_id);
        if (!fetched.ok()) return fetched.status();
        page_ref = std::move(fetched.value());
        page_data = page_ref.bytes().data();
    } else if (fetcher != nullptr) {
        auto fetched = fetcher->Fetch(page_id);
        if (!fetched.ok()) return fetched.status();
        page_data = fetched.value().data();
    } else {
        auto fetched = store.GetForRead(page_id);
        if (!fetched.ok()) return fetched.status();
        page_ref = std::move(fetched.value());
        page_data = page_ref.bytes().data();
    }
    const std::span<std::byte, kPageSize> page_bytes(page_data, kPageSize);
    PageView page(page_bytes);

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

}  // namespace kds::heap
