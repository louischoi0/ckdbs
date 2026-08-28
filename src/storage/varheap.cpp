#include "kds/storage/varheap.hpp"

#include <cstring>
#include <string>

// rules.md #2: all access to on-disk page bytes goes through field-wise
// memcpy helpers; reinterpret_cast of struct types onto page buffers is
// forbidden. Every read/write below copies one field at a time through an
// explicit byte offset, never the mirror structs directly - the same
// discipline heap_page.cpp follows.

namespace kds::varheap {

namespace {

// Bounds the walk in ChainAppend/ChainLength for the same reason
// heap_chain.cpp bounds its own: a cycle in the links would otherwise hang
// a request rather than fail it. Matched to that file's ceiling.
inline constexpr std::uint32_t kMaxChainPages = 1u << 20;

std::size_t SlotOffset(std::uint16_t idx) {
    return kVarHeapHeaderOffset + kHeaderSize + static_cast<std::size_t>(idx) * kSlotOnDiskSize;
}

VarHeapPageHeaderFields ReadHeader(std::span<const std::byte, kPageSize> page) {
    VarHeapPageHeaderFields h{};
    const std::byte* base = page.data() + kVarHeapHeaderOffset;
    std::memcpy(&h.flags, base + kHeaderFlagsOffset, sizeof(h.flags));
    std::memcpy(&h.nr_slots, base + kHeaderNrSlotsOffset, sizeof(h.nr_slots));
    std::memcpy(&h.lower, base + kHeaderLowerOffset, sizeof(h.lower));
    std::memcpy(&h.upper, base + kHeaderUpperOffset, sizeof(h.upper));
    return h;
}

void WriteHeader(std::span<std::byte, kPageSize> page, const VarHeapPageHeaderFields& h) {
    std::byte* base = page.data() + kVarHeapHeaderOffset;
    std::memcpy(base + kHeaderFlagsOffset, &h.flags, sizeof(h.flags));
    std::memcpy(base + kHeaderNrSlotsOffset, &h.nr_slots, sizeof(h.nr_slots));
    std::memcpy(base + kHeaderLowerOffset, &h.lower, sizeof(h.lower));
    std::memcpy(base + kHeaderUpperOffset, &h.upper, sizeof(h.upper));
}

VarHeapSlotFields ReadSlot(std::span<const std::byte, kPageSize> page, std::uint16_t idx) {
    VarHeapSlotFields s{};
    const std::byte* p = page.data() + SlotOffset(idx);
    std::memcpy(&s.offset, p + kSlotOffsetOffset, sizeof(s.offset));
    std::memcpy(&s.length, p + kSlotLengthOffset, sizeof(s.length));
    return s;
}

void WriteSlot(std::span<std::byte, kPageSize> page, std::uint16_t idx,
               const VarHeapSlotFields& s) {
    std::byte* p = page.data() + SlotOffset(idx);
    std::memcpy(p + kSlotOffsetOffset, &s.offset, sizeof(s.offset));
    std::memcpy(p + kSlotLengthOffset, &s.length, sizeof(s.length));
}

// A span of exactly one page, from a store that hands out one of the same
// size. Spelled out once because every entry point below needs it.
std::span<std::byte, kPageSize> Fixed(std::span<std::byte, kPageSize> page) { return page; }

}  // namespace

Status FormatPage(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid) {
    // Zeroes the page and writes the common header: page_type kVarHeap,
    // page_lsn 0, checksum 0 (stamped at flush time, page.md section 8).
    storage::FormatPage(page, PageType::kVarHeap, /*flags=*/0, owner_oid);

    VarHeapPageHeaderFields h{};
    h.flags = kHeaderFlagInitialized;
    h.nr_slots = 0;
    h.lower = static_cast<std::uint16_t>(kVarHeapHeaderOffset + kHeaderSize);
    // Stops short of kPageSize by sizeof(PageId): that tail is permanently
    // reserved for the chain link and never counted as free space.
    h.upper = static_cast<std::uint16_t>(kNextPageIdOffset);
    WriteHeader(page, h);

    // kInvalidPageId, not 0: 0 is a real page (the superblock).
    const PageId none = kInvalidPageId;
    std::memcpy(page.data() + kNextPageIdOffset, &none, sizeof(none));
    return Status::OK();
}

std::uint16_t PageSlotCount(std::span<const std::byte, kPageSize> page) {
    return ReadHeader(page).nr_slots;
}

std::uint16_t PageFreeSpace(std::span<const std::byte, kPageSize> page) {
    VarHeapPageHeaderFields h = ReadHeader(page);
    return h.upper > h.lower ? static_cast<std::uint16_t>(h.upper - h.lower) : 0;
}

PageId PageNextPageId(std::span<const std::byte, kPageSize> page) {
    PageId id;
    std::memcpy(&id, page.data() + kNextPageIdOffset, sizeof(id));
    return id;
}

StatusOr<std::uint16_t> PageAppend(std::span<std::byte, kPageSize> page,
                                    std::span<const std::byte> value) {
    if (value.size() > kMaxValueSize) {
        return Status::Unsupported("var-heap value of " + std::to_string(value.size()) +
                                    " bytes exceeds the " + std::to_string(kMaxValueSize) +
                                    " a page can hold; values spanning pages are not supported "
                                    "(docs/rules/rule-fixed-length-tuple.md section 9)");
    }

    VarHeapPageHeaderFields h = ReadHeader(page);
    const std::size_t needed = kSlotOnDiskSize + value.size();
    const std::size_t avail = h.upper > h.lower ? (h.upper - h.lower) : 0;
    if (avail < needed) {
        return Status::OutOfSpace("var-heap page has no room for this value");
    }

    const auto new_upper = static_cast<std::uint16_t>(h.upper - value.size());
    const std::uint16_t slot = h.nr_slots;

    if (!value.empty()) {
        std::memcpy(page.data() + new_upper, value.data(), value.size());
    }
    WriteSlot(page, slot, VarHeapSlotFields{new_upper, static_cast<std::uint16_t>(value.size())});

    h.nr_slots = static_cast<std::uint16_t>(h.nr_slots + 1);
    h.lower = static_cast<std::uint16_t>(h.lower + kSlotOnDiskSize);
    h.upper = new_upper;
    WriteHeader(page, h);
    return slot;
}

Status PageWriteAt(std::span<std::byte, kPageSize> page, std::uint16_t slot,
                   std::span<const std::byte> value) {
    if (value.size() > kMaxValueSize) {
        return Status::Unsupported("var-heap value of " + std::to_string(value.size()) +
                                    " bytes exceeds the " + std::to_string(kMaxValueSize) +
                                    " a page can hold");
    }
    // The same guard `PageRelease` carries, and for the same reason: a heap
    // page holds `nr_slots` at the body offset this file reads, so a
    // mis-addressed redo would pass every bound below and write a value
    // into a heap page's header. Redo's input is a log record, which a
    // crash can leave naming whatever it likes.
    if (storage::RawPageType(page) != static_cast<std::uint8_t>(PageType::kVarHeap)) {
        return Status::Corruption("var-heap write names page type " +
                                  std::to_string(storage::RawPageType(page)) +
                                  ", not a kVarHeap page; this record is not this page's");
    }

    VarHeapPageHeaderFields h = ReadHeader(page);

    if (slot > h.nr_slots) {
        return Status::Corruption("redo names var-heap slot " + std::to_string(slot) +
                                  " on a page holding " + std::to_string(h.nr_slots) +
                                  "; slots are dense, so this record is not this page's");
    }

    // Re-application. A var-heap value is immutable per version (invariant
    // 14), so the only legal second write is the same bytes - which makes
    // this a verified no-op rather than a rewrite.
    if (slot < h.nr_slots) {
        auto existing = PageRead(page, slot);
        // **Already released**, which is a no-op: the value this record
        // would restore is one a later record killed.
        //
        // Not reachable today - for redo to apply an append at L1 the page
        // must be older than L1, and a tombstone in that image would mean a
        // release at L2 > L1 was applied and stamped, so the page cannot be
        // older than L1 after all. The one crack is a release whose
        // `StampPageLsn` failed after the tombstone went in, and the answer
        // there is this same no-op.
        //
        // It is kept as **forward protection**, not as dead weight: page
        // recycling is exactly what stops a slot number from identifying
        // one value across a page's lifetime, and this is where a stale
        // append would land if it did.
        if (existing.status().code() == StatusCode::kNotFound) return Status::OK();
        if (!existing.ok()) {
            return existing.status();
        }
        if (existing.value().size() != value.size()) {
            return Status::Corruption("redo would change var-heap slot " + std::to_string(slot) +
                                      " from " + std::to_string(existing.value().size()) +
                                      " bytes to " + std::to_string(value.size()) +
                                      "; values are immutable per version");
        }
        return Status::OK();
    }

    const std::size_t needed = kSlotOnDiskSize + value.size();
    const std::size_t avail = h.upper > h.lower ? (h.upper - h.lower) : 0;
    if (avail < needed) {
        return Status::OutOfSpace("var-heap page has no room for this value");
    }

    const auto new_upper = static_cast<std::uint16_t>(h.upper - value.size());
    if (!value.empty()) {
        std::memcpy(page.data() + new_upper, value.data(), value.size());
    }
    WriteSlot(page, slot, VarHeapSlotFields{new_upper, static_cast<std::uint16_t>(value.size())});

    h.nr_slots = static_cast<std::uint16_t>(h.nr_slots + 1);
    h.lower = static_cast<std::uint16_t>(h.lower + kSlotOnDiskSize);
    h.upper = new_upper;
    WriteHeader(page, h);
    return Status::OK();
}

StatusOr<std::span<const std::byte>> PageRead(std::span<const std::byte, kPageSize> page,
                                               std::uint16_t slot) {
    VarHeapPageHeaderFields h = ReadHeader(page);
    if (slot >= h.nr_slots) {
        return Status::Corruption("var-heap slot " + std::to_string(slot) +
                                   " is out of range (page holds " + std::to_string(h.nr_slots) +
                                   ")");
    }

    VarHeapSlotFields s = ReadSlot(page, slot);
    // Released, and that is **NotFound rather than Corruption** - the one
    // distinction this whole status choice exists for. Under the lifetime
    // model no live traversal can reach a tombstone: a slot is released only
    // when the version owning it is dead, and a dead version is unreachable
    // by every live and future read. So arriving here is not damage, and a
    // checker walking a delete-marked row must be able to say "released, as
    // expected" instead of reporting a corruption it invented.
    if (s.offset == kReleasedSlotOffset) {
        return Status::NotFound("var-heap slot " + std::to_string(slot) +
                                 " was released; its value is dead");
    }
    // Never size a read from an extent the page disagrees with. A value is
    // committed data, so a slot pointing outside the page is corruption to
    // report, not a range to clamp.
    if (s.offset < h.upper || static_cast<std::size_t>(s.offset) + s.length > kNextPageIdOffset) {
        return Status::Corruption("var-heap slot " + std::to_string(slot) + " has extent [" +
                                   std::to_string(s.offset) + ", +" + std::to_string(s.length) +
                                   ") outside the page's value area");
    }
    return page.subspan(s.offset, s.length);
}

Status PageRelease(std::span<std::byte, kPageSize> page, std::uint16_t slot) {
    // **The page must be the record's** (redo's ANCHOR_UPDATE arm's stance,
    // src/wal/redo.cpp): a record that is not this page's is refused, never
    // applied. Not defensive politeness - a heap page carries `nr_slots` at
    // the *same* body offset this file reads (heap_page.hpp
    // kHeaderNrSlotsOffset == 2), so a mis-addressed release passes the
    // bound below and writes two zero bytes over `min_key` or a slot
    // pointer, breaking invariant 2 silently. The input is untrusted where
    // it matters most: `RecoveryUndo::Compensate` takes `target_page_id`
    // off an undo page a crash may have torn, and every other undo type is
    // saved from that by the pk identity check this one skips.
    if (storage::RawPageType(page) != static_cast<std::uint8_t>(PageType::kVarHeap)) {
        return Status::Corruption("var-heap release names page type " +
                                  std::to_string(storage::RawPageType(page)) +
                                  ", not a kVarHeap page; this record is not this page's");
    }

    VarHeapPageHeaderFields h = ReadHeader(page);
    if (slot >= h.nr_slots) {
        // A record naming a slot this page does not have is not this page's
        // record - the same reading PageWriteAt gives, and the same refusal.
        return Status::Corruption("var-heap release names slot " + std::to_string(slot) +
                                  " on a page holding " + std::to_string(h.nr_slots) +
                                  "; slots are dense, so this record is not this page's");
    }

    VarHeapSlotFields s = ReadSlot(page, slot);
    // Already released: silently OK, because a crash-restarted rollback
    // replays this and a compensation that failed the second time would
    // turn a recoverable crash into a refused mount.
    if (s.offset == kReleasedSlotOffset) return Status::OK();

    // The slot dies; the bytes do not move and are not overwritten.
    s.offset = kReleasedSlotOffset;
    WriteSlot(page, slot, s);
    return Status::OK();
}

std::uint16_t PageLiveSlots(std::span<const std::byte, kPageSize> page) {
    VarHeapPageHeaderFields h = ReadHeader(page);
    std::uint16_t live = 0;
    for (std::uint16_t i = 0; i < h.nr_slots; ++i) {
        if (ReadSlot(page, i).offset != kReleasedSlotOffset) ++live;
    }
    return live;
}

// ---- Chain ---------------------------------------------------------------

StatusOr<PageId> CreateChain(storage::PageStore& store, std::uint64_t owner_oid) {
    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [page_id, bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> bytes = bytes_ref.bytes();

    if (Status s = FormatPage(bytes, owner_oid); !s.ok()) return s;
    return page_id;
}

// Peak pins (MG03): 2 - the walk holds one page at a time (the GetForRead
// ref is reassigned per hop), and growth holds the old tail and the new
// page together for the link write.
StatusOr<ChainAppendResult> ChainAppend(storage::PageStore& store, PageId root,
                                        std::span<const std::byte> value,
                                        std::uint64_t owner_oid) {
    if (root == kInvalidPageId) {
        return Status::InvalidArgument(
            "this relation has no var-heap chain; it was created without a spillable column");
    }
    if (value.size() > kMaxValueSize) {
        return Status::Unsupported("var-heap value of " + std::to_string(value.size()) +
                                    " bytes exceeds the " + std::to_string(kMaxValueSize) +
                                    " a page can hold; values spanning pages are not supported "
                                    "(docs/rules/rule-fixed-length-tuple.md section 9)");
    }

    // Walk to the tail. Cheap in the shape that matters - a chain grows
    // only when a page fills - and bounded so a cycle fails rather than
    // hangs.
    PageId tail_id = root;
    for (std::uint32_t steps = 0;; ++steps) {
        if (steps >= kMaxChainPages) {
            return Status::Corruption("var-heap chain from page " + std::to_string(root) +
                                       " exceeds the maximum length; the links may form a cycle");
        }
        auto bytes = store.GetForRead(tail_id);
        if (!bytes.ok()) return bytes.status();
        const PageId next = PageNextPageId(bytes.value().bytes());
        if (next == kInvalidPageId) break;
        tail_id = next;
    }

    auto tail = store.Get(tail_id);
    if (!tail.ok()) return tail.status();

    auto slot = PageAppend(Fixed(tail.value().bytes()), value);
    if (slot.ok()) {
        // The common case, and the one that needs no structural record: an
        // existing page took the value, and the append record describes it
        // completely.
        return ChainAppendResult{VarHeapPtr{tail_id, slot.value()}, kInvalidPageId,
                                 kInvalidPageId};
    }
    if (slot.status().code() != StatusCode::kOutOfSpace) {
        return slot.status();  // a real failure, not a full page
    }

    // The tail is full: grow. Nothing is moved and no existing value's
    // pointer changes - values are immutable and never relocated, which is
    // the whole point of this class.
    auto created = store.CreateNew();
    if (!created.ok()) return created.status();
    auto& [new_id, new_bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> new_bytes = new_bytes_ref.bytes();
    if (Status s = FormatPage(new_bytes, owner_oid); !s.ok()) return s;

    auto new_slot = PageAppend(Fixed(new_bytes), value);
    if (!new_slot.ok()) {
        // A value no empty page can hold. The allocated page is left
        // unlinked rather than freed - the store has no free-page path yet
        // - so nothing reaches it. Same choice heap_chain.cpp makes.
        return new_slot.status();
    }

    // Linked last, after the value is in the new page: the link is what
    // makes the page reachable, so publishing it earlier would expose an
    // empty page as the tail. Re-fetched because CreateNew() may have
    // handed out a new frame.
    auto tail_again = store.Get(tail_id);
    if (!tail_again.ok()) return tail_again.status();
    std::memcpy(tail_again.value().bytes().data() + kNextPageIdOffset, &new_id, sizeof(new_id));

    // Both halves of the growth reported, because neither is described by the
    // append record the caller is about to write (ChainAppendResult).
    return ChainAppendResult{VarHeapPtr{new_id, new_slot.value()}, new_id, tail_id};
}

StatusOr<std::span<const std::byte>> Fetch(storage::PageStore& store, VarHeapPtr ptr,
                                           storage::PageRef& pin) {
    if (ptr.page_id == kInvalidPageId) {
        return Status::Corruption("var-heap pointer names the invalid page id");
    }
    auto bytes = store.GetForRead(ptr.page_id);
    if (!bytes.ok()) return bytes.status();

    // The page class is checked rather than assumed: a pointer is only as
    // good as the tuple it came out of, and reading a heap page's bytes as
    // values would silently return garbage where this returns Corruption.
    if (Status s = storage::ValidatePageHeader(bytes.value().bytes(), PageType::kVarHeap); !s.ok()) {
        return s;
    }
    // The returned span points into the frame, so the caller owns the pin
    // for as long as it reads (MG03's Shape C: this function returned a
    // span into an unpinned frame, and a second Fetch in the same loop
    // could evict the first one's page - the MG05 ASan run caught exactly
    // that in ResolveSpills' two-spill row).
    std::span<std::byte, kPageSize> page = bytes.value().bytes();
    pin = std::move(bytes.value());
    return PageRead(page, ptr.slot);
}

StatusOr<std::uint32_t> ChainLength(storage::PageStore& store, PageId root) {
    if (root == kInvalidPageId) return 0u;

    std::uint32_t pages = 0;
    PageId current = root;
    for (;; ++pages) {
        if (pages >= kMaxChainPages) {
            return Status::Corruption("var-heap chain from page " + std::to_string(root) +
                                       " exceeds the maximum length; the links may form a cycle");
        }
        auto bytes = store.GetForRead(current);
        if (!bytes.ok()) return bytes.status();
        const PageId next = PageNextPageId(bytes.value().bytes());
        if (next == kInvalidPageId) return pages + 1;
        current = next;
    }
}

}  // namespace kds::varheap
