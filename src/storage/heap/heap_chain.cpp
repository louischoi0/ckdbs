#include "kds/storage/heap/heap_chain.hpp"

#include <cstring>
#include <string>

#include "kds/storage/keystone.hpp"

namespace kds::heap {

namespace {

// Little-endian load of the leading Keystone word. Local rather than
// shared for the same reason row_codec.cpp keeps its own: it is three
// lines, and an explicit shift/mask read is what rules.md #5 asks for on
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

// One hop of a chain walk, with the cycle guard applied. `steps` is the
// number of hops already taken.
Status CheckHopBudget(std::uint32_t steps, PageId head) {
    if (steps < kMaxChainPages) return Status::OK();
    return Status::Corruption("heap chain from page " + std::to_string(head) + " exceeds " +
                              std::to_string(kMaxChainPages) +
                              " pages; the next_page_id links are cyclic or corrupt");
}

}  // namespace

StatusOr<PageId> ChainTail(storage::PageStore& store, PageId head) {
    PageId current = head;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = CheckHopBudget(steps, head); !s.ok()) return s;

        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();

        const PageId next = PageView(bytes.value()).next_page_id();
        if (next == kInvalidPageId) return current;
        current = next;
    }
}

StatusOr<std::uint32_t> ChainLength(storage::PageStore& store, PageId head) {
    PageId current = head;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = CheckHopBudget(steps, head); !s.ok()) return s;

        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();

        const PageId next = PageView(bytes.value()).next_page_id();
        if (next == kInvalidPageId) return steps + 1;
        current = next;
    }
}

StatusOr<ChainInsertResult> ChainInsert(storage::PageStore& store, PageId head, std::uint64_t id,
                                        std::span<const std::byte> payload,
                                        std::uint64_t trx_id) {
    // The caller passes `id` separately from the payload that encodes it;
    // disagreeing copies of a tuple's identity is the kind of thing that
    // is silent for months, so they are checked against each other once,
    // here, where both are in hand.
    auto encoded_id = PayloadKeystoneId(payload);
    if (!encoded_id.ok()) return encoded_id.status();
    if (encoded_id.value() != id) {
        return Status::Corruption("tuple's Keystone id " + std::to_string(encoded_id.value()) +
                                  " does not match the id being inserted (" +
                                  std::to_string(id) + ")");
    }

    auto tail_id = ChainTail(store, head);
    if (!tail_id.ok()) return tail_id.status();

    auto tail_bytes = store.Get(tail_id.value());
    if (!tail_bytes.ok()) return tail_bytes.status();
    PageView tail(tail_bytes.value());

    // Invariant 3, enforced at the one door tuples come through. Below the
    // tail's min_key there is no page in this chain that may legally hold
    // the tuple: earlier pages are closed (their ids are all below this
    // one's min_key by construction) and this one is barred by the
    // invariant. The id sequence has gone backwards.
    if (id < tail.min_key()) {
        return Status::OutOfRange("id " + std::to_string(id) + " is below page " +
                                  std::to_string(tail_id.value()) + "'s min_key " +
                                  std::to_string(tail.min_key()) +
                                  "; the relation's id sequence has gone backwards");
    }

    // O(1) pages, and complete: see the header's ordering property. A
    // delete-marked tuple still holds its key - the key is free only once
    // the slot is physically retired.
    const std::uint16_t n = tail.slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
        auto tuple = tail.ReadTuple(i);
        if (!tuple.ok()) continue;  // retired or out-of-range slot

        auto existing = PayloadKeystoneId(tuple.value().payload);
        if (!existing.ok()) return existing.status();
        if (existing.value() == id) {
            return Status::AlreadyExists("duplicate primary key " + std::to_string(id) +
                                          " already present at page " +
                                          std::to_string(tail_id.value()) + " slot " +
                                          std::to_string(i));
        }
    }

    auto slot = tail.InsertTuple(payload, trx_id);
    if (slot.ok()) {
        return ChainInsertResult{tail_id.value(), slot.value(), /*grew_chain=*/false,
                                 /*linked_from=*/kInvalidPageId};
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
    auto [new_id, new_bytes] = created.value();

    auto new_page = PageView::CreateEmpty(new_bytes, id);
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
    // DevicePageStore::FlushPages.
    //
    // Re-fetched rather than reusing `tail`: CreateNew() may have handed
    // out a new frame, and page stores are free to move their frames
    // (today's do not, tomorrow's buffer pool with eviction will).
    auto tail_again = store.Get(tail_id.value());
    if (!tail_again.ok()) return tail_again.status();
    PageView(tail_again.value()).set_next_page_id(new_id);

    return ChainInsertResult{new_id, new_slot.value(), /*grew_chain=*/true,
                             /*linked_from=*/tail_id.value()};
}

Status ChainVisit(
    storage::PageStore& store, PageId head, storage::PageAccess access,
    const std::function<StatusOr<storage::VisitControl>(PageId, PageView&, std::uint16_t)>& fn,
    storage::ScanFetcher* fetcher) {
    PageId current = head;
    for (std::uint32_t steps = 0;; ++steps) {
        if (Status s = CheckHopBudget(steps, head); !s.ok()) return s;

        // Ring mode is a read path only (spec-eviction §5): a writer's walk
        // takes the ordinary route, because the ring never bypasses the
        // dirty protocol. The visitor's per-page discipline is what makes
        // the ring's stricter lifetime safe: each page is finished before
        // the next fetch can rotate its frame away.
        auto bytes = access == storage::PageAccess::kWrite
                         ? store.Get(current)
                         : (fetcher != nullptr ? fetcher->Fetch(current)
                                               : store.GetForRead(current));
        if (!bytes.ok()) return bytes.status();
        PageView page(bytes.value());

        const std::uint16_t n = page.slot_count();
        for (std::uint16_t i = 0; i < n; ++i) {
            // Liveness is re-tested by the callback through ReadTuple();
            // skipping here as well would mean two reads of every slot.
            auto outcome = storage::ResolveVisit(fn(current, page, i), "ChainVisit");
            if (!outcome.ok()) return outcome.status();
            // A successful early exit: the caller has what it came for, and
            // the rest of the chain is not fetched. Distinct from an error
            // precisely so the caller need not tell them apart.
            if (outcome.value() == storage::VisitControl::kStop) return Status::OK();
        }

        const PageId next = page.next_page_id();
        if (next == kInvalidPageId) return Status::OK();
        current = next;
    }
}

}  // namespace kds::heap
