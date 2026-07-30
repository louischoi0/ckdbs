#include "kds/stats/waystone.hpp"

#include <cstring>
#include <string>

// Waystone entry codec (waystone-concpets.md section 5). Field-wise memcpy
// through the named offsets in the header, exactly as page_header.cpp and
// record.cpp do it, and for the same reason (rules.md sections 2 and 5):
// the mirror struct pins the layout with static_asserts and is never
// memcpy'd whole onto page bytes, because struct padding and field order
// are the compiler's business and an on-disk format is not.
//
// There is no page header on an entry page and therefore no checksum, no
// page_lsn and no page_type: 256 entries of 32 bytes tile the 8 KiB
// exactly, and a header would cost an entry and break the shift/mask
// addressing that is the point of the layout. What detects damage here is
// the probe's Keystone-id check at the target (spec section 3.1), not a
// CRC - an entry is advisory, so a wrong one must be *survivable*, which
// is a stronger property than being detectable.

namespace kds::stats {

namespace {

template <typename T>
T Load(std::span<const std::byte, kPageSize> page, std::size_t offset) {
    T value;
    std::memcpy(&value, page.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void Store(std::span<std::byte, kPageSize> page, std::size_t offset, T value) {
    std::memcpy(page.data() + offset, &value, sizeof(T));
}

// Byte offset of an entry within its page. A shift, not a multiply, and
// that is why kEntrySize is a power of two.
constexpr std::size_t ByteOffsetOf(std::size_t slot) noexcept {
    return slot * kEntrySize;
}

Status CheckSlot(std::size_t slot) {
    if (slot < kEntriesPerPage) return Status::OK();
    return Status::OutOfRange("waystone: entry slot " + std::to_string(slot) +
                              " is past the " + std::to_string(kEntriesPerPage) +
                              " entries a page holds");
}

}  // namespace

StatusOr<WaystoneEntry> ReadEntry(std::span<const std::byte, kPageSize> page, std::size_t slot) {
    if (Status s = CheckSlot(slot); !s.ok()) return s;
    const std::size_t at = ByteOffsetOf(slot);

    WaystoneEntry entry;
    entry.pk = Load<std::uint64_t>(page, at + kEntryPkOffset);
    entry.page_id = Load<PageId>(page, at + kEntryPageIdOffset);
    entry.slot = Load<std::uint16_t>(page, at + kEntrySlotOffset);
    entry.flags = Load<std::uint16_t>(page, at + kEntryFlagsOffset);
    entry.use_count = Load<std::uint32_t>(page, at + kEntryUseCountOffset);
    entry.last_ts = Load<std::uint32_t>(page, at + kEntryLastTsOffset);
    entry.page_epoch = Load<std::uint32_t>(page, at + kEntryPageEpochOffset);
    entry.reserved = Load<std::uint32_t>(page, at + kEntryReservedOffset);
    return entry;
}

Status WriteEntry(std::span<std::byte, kPageSize> page, std::size_t slot,
                  const WaystoneEntry& entry) {
    if (Status s = CheckSlot(slot); !s.ok()) return s;
    if (entry.pk > kMaxPk) {
        return Status::InvalidArgument("waystone: pk " + std::to_string(entry.pk) +
                                       " exceeds the 40-bit Keystone id range");
    }
    const std::size_t at = ByteOffsetOf(slot);

    Store<std::uint64_t>(page, at + kEntryPkOffset, entry.pk);
    Store<PageId>(page, at + kEntryPageIdOffset, entry.page_id);
    Store<std::uint16_t>(page, at + kEntrySlotOffset, entry.slot);
    Store<std::uint16_t>(page, at + kEntryFlagsOffset, entry.flags);
    Store<std::uint32_t>(page, at + kEntryUseCountOffset, entry.use_count);
    Store<std::uint32_t>(page, at + kEntryLastTsOffset, entry.last_ts);
    Store<std::uint32_t>(page, at + kEntryPageEpochOffset, entry.page_epoch);
    Store<std::uint32_t>(page, at + kEntryReservedOffset, entry.reserved);
    return Status::OK();
}

}  // namespace kds::stats
