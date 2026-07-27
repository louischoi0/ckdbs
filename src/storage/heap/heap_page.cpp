#include "kds/storage/heap/heap_page.hpp"

#include <cstring>

// rules.md #2: all access to on-disk page bytes goes through field-wise
// memcpy helpers; reinterpret_cast of struct types onto page buffers is
// forbidden. Every read/write below copies one field at a time through an
// explicit byte offset into page_, never the mirror structs directly.

namespace kds::heap {

namespace {

// Matches the Keystone column's 40-bit id width (kKeystoneIdBits in
// keystone.hpp), duplicated here rather than including that header,
// to keep the heap page layer decoupled from row-encoding format (see the
// file-level layering note in heap_page.hpp).
inline constexpr std::uint64_t kMaxTupleId = (std::uint64_t{1} << 40) - 1;

std::size_t SlotOffset(std::uint16_t idx) {
    return kHeaderSize + static_cast<std::size_t>(idx) * kSlotOnDiskSize;
}

}  // namespace

HeapPageHeaderFields PageView::ReadHeader() const {
    HeapPageHeaderFields h{};
    const std::byte* base = page_.data();
    std::memcpy(&h.flags, base + kHeaderFlagsOffset, sizeof(h.flags));
    std::memcpy(&h.nr_slots, base + kHeaderNrSlotsOffset, sizeof(h.nr_slots));
    std::memcpy(&h.lower, base + kHeaderLowerOffset, sizeof(h.lower));
    std::memcpy(&h.upper, base + kHeaderUpperOffset, sizeof(h.upper));
    std::memcpy(&h.min_key, base + kHeaderMinKeyOffset, sizeof(h.min_key));
    return h;
}

void PageView::WriteHeader(const HeapPageHeaderFields& h) {
    std::byte* base = page_.data();
    std::memcpy(base + kHeaderFlagsOffset, &h.flags, sizeof(h.flags));
    std::memcpy(base + kHeaderNrSlotsOffset, &h.nr_slots, sizeof(h.nr_slots));
    std::memcpy(base + kHeaderLowerOffset, &h.lower, sizeof(h.lower));
    std::memcpy(base + kHeaderUpperOffset, &h.upper, sizeof(h.upper));
    std::memcpy(base + kHeaderMinKeyOffset, &h.min_key, sizeof(h.min_key));
}

HeapSlotFields PageView::ReadSlot(std::uint16_t idx) const {
    HeapSlotFields s{};
    const std::byte* p = page_.data() + SlotOffset(idx);
    std::memcpy(&s.offset, p + kSlotOffsetOffset, sizeof(s.offset));
    std::memcpy(&s.length, p + kSlotLengthOffset, sizeof(s.length));
    std::memcpy(&s.flags, p + kSlotFlagsOffset, sizeof(s.flags));
    return s;
}

void PageView::WriteSlot(std::uint16_t idx, const HeapSlotFields& s) {
    std::byte* p = page_.data() + SlotOffset(idx);
    std::memcpy(p + kSlotOffsetOffset, &s.offset, sizeof(s.offset));
    std::memcpy(p + kSlotLengthOffset, &s.length, sizeof(s.length));
    std::memcpy(p + kSlotFlagsOffset, &s.flags, sizeof(s.flags));
}

StatusOr<PageView> PageView::CreateEmpty(std::span<std::byte, kPageSize> page,
                                          std::uint64_t min_key) {
    if (min_key > kMaxTupleId) {
        return Status::InvalidArgument("min_key exceeds 40-bit tuple id range");
    }

    PageView view(page);

    HeapPageHeaderFields h{};
    h.flags = kHeaderFlagInitialized;
    h.nr_slots = 0;
    h.lower = static_cast<std::uint16_t>(kHeaderSize);
    // Stops short of kPageSize by sizeof(PageId): that tail is permanently
    // reserved for the next_page_id link, never counted as free space.
    h.upper = static_cast<std::uint16_t>(kNextPageIdOffset);
    h.min_key = min_key;
    view.WriteHeader(h);

    // kInvalidPageId (not 0) marks "no next page": unlike the legacy
    // kernel code, this design reserves 0xFFFFFFFF rather than 0 as the
    // invalid page id sentinel (common.hpp), so that's the "no next page"
    // value here too.
    view.set_next_page_id(kInvalidPageId);

    return view;
}

std::uint64_t PageView::min_key() const { return ReadHeader().min_key; }

std::uint16_t PageView::slot_count() const { return ReadHeader().nr_slots; }

std::uint16_t PageView::free_space() const {
    HeapPageHeaderFields h = ReadHeader();
    return h.upper > h.lower ? static_cast<std::uint16_t>(h.upper - h.lower) : 0;
}

bool PageView::HasSpaceFor(std::uint16_t payload_len) const {
    // Guard against uint16_t wraparound in the `needed` computation for
    // pathological payload_len values; kPageSize is well within uint16_t
    // range, so anything larger can never fit regardless.
    if (payload_len > kPageSize) {
        return false;
    }
    std::size_t needed = kSlotOnDiskSize + kTupleHeaderOnDiskSize + payload_len;
    return free_space() >= needed;
}

PageId PageView::next_page_id() const {
    PageId id;
    std::memcpy(&id, page_.data() + kNextPageIdOffset, sizeof(id));
    return id;
}

void PageView::set_next_page_id(PageId next) {
    std::memcpy(page_.data() + kNextPageIdOffset, &next, sizeof(next));
}

StatusOr<std::uint16_t> PageView::InsertTuple(std::span<const std::byte> payload,
                                               std::uint64_t xmin, std::uint64_t xmax,
                                               std::uint64_t undo_ptr) {
    if (payload.size() > kPageSize) {
        return Status::InvalidArgument("payload larger than a page");
    }

    auto payload_len = static_cast<std::uint16_t>(payload.size());
    std::size_t needed = kSlotOnDiskSize + kTupleHeaderOnDiskSize + payload_len;

    HeapPageHeaderFields h = ReadHeader();
    std::size_t avail = h.upper > h.lower ? (h.upper - h.lower) : 0;
    if (avail < needed) {
        return Status::OutOfSpace("heap page has no room for this tuple");
    }

    auto new_upper = static_cast<std::uint16_t>(h.upper - (kTupleHeaderOnDiskSize + payload_len));
    std::uint16_t new_slot_idx = h.nr_slots;

    std::byte* tuple_base = page_.data() + new_upper;
    std::memcpy(tuple_base + kTupleXminOffset, &xmin, sizeof(xmin));
    std::memcpy(tuple_base + kTupleXmaxOffset, &xmax, sizeof(xmax));
    std::memcpy(tuple_base + kTupleUndoPtrOffset, &undo_ptr, sizeof(undo_ptr));
    std::memcpy(tuple_base + kTupleDataLenOffset, &payload_len, sizeof(payload_len));
    std::uint8_t tuple_flags = 0;
    std::uint8_t tuple_reserved = 0;
    std::memcpy(tuple_base + kTupleFlagsOffset, &tuple_flags, sizeof(tuple_flags));
    std::memcpy(tuple_base + kTupleReservedOffset, &tuple_reserved, sizeof(tuple_reserved));
    if (payload_len > 0) {
        std::memcpy(tuple_base + kTupleHeaderOnDiskSize, payload.data(), payload_len);
    }

    HeapSlotFields slot{};
    slot.offset = new_upper;
    slot.length = static_cast<std::uint16_t>(kTupleHeaderOnDiskSize + payload_len);
    slot.flags = 0;
    WriteSlot(new_slot_idx, slot);

    h.nr_slots = static_cast<std::uint16_t>(h.nr_slots + 1);
    h.lower = static_cast<std::uint16_t>(h.lower + kSlotOnDiskSize);
    h.upper = new_upper;
    WriteHeader(h);

    return new_slot_idx;
}

StatusOr<PageView::Tuple> PageView::ReadTuple(std::uint16_t slot_idx) const {
    HeapPageHeaderFields h = ReadHeader();
    if (slot_idx >= h.nr_slots) {
        return Status::NotFound("slot index out of range");
    }

    HeapSlotFields slot = ReadSlot(slot_idx);
    if ((slot.flags & kSlotFlagDead) != 0 || slot.length == 0) {
        return Status::NotFound("slot is dead");
    }

    const std::byte* tuple_base = page_.data() + slot.offset;
    Tuple t{};
    std::memcpy(&t.xmin, tuple_base + kTupleXminOffset, sizeof(t.xmin));
    std::memcpy(&t.xmax, tuple_base + kTupleXmaxOffset, sizeof(t.xmax));
    std::memcpy(&t.undo_ptr, tuple_base + kTupleUndoPtrOffset, sizeof(t.undo_ptr));

    std::uint16_t data_len;
    std::memcpy(&data_len, tuple_base + kTupleDataLenOffset, sizeof(data_len));

    t.payload = std::span<const std::byte>(tuple_base + kTupleHeaderOnDiskSize, data_len);
    return t;
}

StatusOr<std::uint16_t> PageView::SlotCapacity(std::uint16_t slot_idx) const {
    HeapPageHeaderFields h = ReadHeader();
    if (slot_idx >= h.nr_slots) {
        return Status::NotFound("slot index out of range");
    }

    HeapSlotFields slot = ReadSlot(slot_idx);
    if ((slot.flags & kSlotFlagDead) != 0 || slot.length == 0) {
        return Status::NotFound("slot is dead");
    }

    return static_cast<std::uint16_t>(slot.length - kTupleHeaderOnDiskSize);
}

Status PageView::OverwriteTuple(std::uint16_t slot_idx, std::span<const std::byte> payload,
                                std::uint64_t xmin, std::uint64_t xmax, std::uint64_t undo_ptr) {
    HeapPageHeaderFields h = ReadHeader();
    if (slot_idx >= h.nr_slots) {
        return Status::NotFound("slot index out of range");
    }

    HeapSlotFields slot = ReadSlot(slot_idx);
    if ((slot.flags & kSlotFlagDead) != 0 || slot.length == 0) {
        return Status::NotFound("slot is dead");
    }

    auto payload_len = static_cast<std::uint16_t>(payload.size());
    if (static_cast<std::size_t>(kTupleHeaderOnDiskSize + payload_len) > slot.length) {
        return Status::OutOfSpace("new payload does not fit the slot's existing reservation");
    }

    std::byte* tuple_base = page_.data() + slot.offset;
    std::memcpy(tuple_base + kTupleXminOffset, &xmin, sizeof(xmin));
    std::memcpy(tuple_base + kTupleXmaxOffset, &xmax, sizeof(xmax));
    std::memcpy(tuple_base + kTupleUndoPtrOffset, &undo_ptr, sizeof(undo_ptr));
    std::memcpy(tuple_base + kTupleDataLenOffset, &payload_len, sizeof(payload_len));
    if (payload_len > 0) {
        std::memcpy(tuple_base + kTupleHeaderOnDiskSize, payload.data(), payload_len);
    }

    return Status::OK();
}

Status PageView::RetireSlot(std::uint16_t slot_idx) {
    HeapPageHeaderFields h = ReadHeader();
    if (slot_idx >= h.nr_slots) {
        return Status::NotFound("slot index out of range");
    }

    HeapSlotFields slot = ReadSlot(slot_idx);
    if ((slot.flags & kSlotFlagDead) != 0 || slot.length == 0) {
        return Status::NotFound("slot is already dead");
    }

    slot.flags |= kSlotFlagDead;
    slot.length = 0;
    WriteSlot(slot_idx, slot);
    return Status::OK();
}

}  // namespace kds::heap
