#include "kds/stats/waystone.hpp"

#include <cstring>

namespace kds::stats {

namespace {

// Byte offset of entry `index` within the page. Kept in one place so the
// read and write paths cannot drift, and expressed as a multiply rather
// than a shift because kWaystoneEntrySize is not addressed as a power of
// two here (see the derivation in waystone.hpp).
constexpr std::size_t EntryOffset(std::size_t index) noexcept {
    return kWaystoneBodyOffset + index * kWaystoneEntrySize;
}

template <typename T>
T Load(std::span<const std::byte, kPageSize> page, std::size_t offset) noexcept {
    T v{};
    std::memcpy(&v, page.data() + offset, sizeof(T));
    return v;
}

template <typename T>
void Store(std::span<std::byte, kPageSize> page, std::size_t offset, const T& v) noexcept {
    std::memcpy(page.data() + offset, &v, sizeof(T));
}

}  // namespace

void FormatWaystonePage(std::span<std::byte, kPageSize> page, const InstanceKey& key,
                        std::uint64_t recorded_ts) {
    // Zeroes the whole page and writes the common header for this build's
    // current format version; the body is left zeroed for us.
    storage::FormatPage(page, PageType::kWaystone);

    WaystoneHeader header{};
    header.pattern_id = key.pattern_id;
    header.arg_hash = key.arg_hash;
    header.recorded_ts = recorded_ts;
    header.next_page_id = kInvalidPageId;
    header.use_count = 0;
    header.entry_count = 0;
    header.flags = 0;
    header.reserved = 0;

    // Cannot fail: entry_count is 0, the only value WriteWaystoneHeader
    // rejects a header for is one it cannot hold.
    (void)WriteWaystoneHeader(page, header);
}

WaystoneHeader ReadWaystoneHeader(std::span<const std::byte, kPageSize> page) {
    const std::size_t base = storage::kPageBodyOffset;
    WaystoneHeader h{};
    h.pattern_id = Load<std::uint64_t>(page, base + kWaystoneHeaderPatternIdOffset);
    h.arg_hash = Load<std::uint64_t>(page, base + kWaystoneHeaderArgHashOffset);
    h.recorded_ts = Load<std::uint64_t>(page, base + kWaystoneHeaderRecordedTsOffset);
    h.next_page_id = Load<PageId>(page, base + kWaystoneHeaderNextPageOffset);
    h.use_count = Load<std::uint32_t>(page, base + kWaystoneHeaderUseCountOffset);
    h.entry_count = Load<std::uint16_t>(page, base + kWaystoneHeaderEntryCountOffset);
    h.flags = Load<std::uint16_t>(page, base + kWaystoneHeaderFlagsOffset);
    h.reserved = Load<std::uint32_t>(page, base + kWaystoneHeaderReservedOffset);
    return h;
}

Status WriteWaystoneHeader(std::span<std::byte, kPageSize> page, const WaystoneHeader& header) {
    if (header.entry_count > kEntriesPerWaystonePage) {
        return Status::InvalidArgument("waystone: entry_count " +
                                       std::to_string(header.entry_count) + " exceeds the " +
                                       std::to_string(kEntriesPerWaystonePage) +
                                       " a page can hold");
    }

    const std::size_t base = storage::kPageBodyOffset;
    Store<std::uint64_t>(page, base + kWaystoneHeaderPatternIdOffset, header.pattern_id);
    Store<std::uint64_t>(page, base + kWaystoneHeaderArgHashOffset, header.arg_hash);
    Store<std::uint64_t>(page, base + kWaystoneHeaderRecordedTsOffset, header.recorded_ts);
    Store<PageId>(page, base + kWaystoneHeaderNextPageOffset, header.next_page_id);
    Store<std::uint32_t>(page, base + kWaystoneHeaderUseCountOffset, header.use_count);
    Store<std::uint16_t>(page, base + kWaystoneHeaderEntryCountOffset, header.entry_count);
    Store<std::uint16_t>(page, base + kWaystoneHeaderFlagsOffset, header.flags);
    Store<std::uint32_t>(page, base + kWaystoneHeaderReservedOffset, header.reserved);
    return Status::OK();
}

StatusOr<WaystoneEntry> ReadWaystoneEntry(std::span<const std::byte, kPageSize> page,
                                          std::size_t index) {
    if (index >= kEntriesPerWaystonePage) {
        return Status::OutOfRange("waystone: entry index past the end of the page");
    }

    const std::size_t base = EntryOffset(index);
    WaystoneEntry e{};
    e.pk = Load<std::uint64_t>(page, base + kWaystoneEntryPkOffset);
    e.rel_oid = Load<std::uint64_t>(page, base + kWaystoneEntryRelOidOffset);
    e.page_id = Load<PageId>(page, base + kWaystoneEntryPageIdOffset);
    e.page_epoch = Load<std::uint32_t>(page, base + kWaystoneEntryPageEpochOffset);
    e.slot = Load<std::uint16_t>(page, base + kWaystoneEntrySlotOffset);
    e.flags = Load<std::uint16_t>(page, base + kWaystoneEntryFlagsOffset);
    e.step_id = Load<std::uint16_t>(page, base + kWaystoneEntryStepIdOffset);
    e.reserved = Load<std::uint16_t>(page, base + kWaystoneEntryReservedOffset);
    return e;
}

Status WriteWaystoneEntry(std::span<std::byte, kPageSize> page, std::size_t index,
                          const WaystoneEntry& entry) {
    if (index >= kEntriesPerWaystonePage) {
        return Status::OutOfRange("waystone: entry index past the end of the page");
    }

    const std::size_t base = EntryOffset(index);
    Store<std::uint64_t>(page, base + kWaystoneEntryPkOffset, entry.pk);
    Store<std::uint64_t>(page, base + kWaystoneEntryRelOidOffset, entry.rel_oid);
    Store<PageId>(page, base + kWaystoneEntryPageIdOffset, entry.page_id);
    Store<std::uint32_t>(page, base + kWaystoneEntryPageEpochOffset, entry.page_epoch);
    Store<std::uint16_t>(page, base + kWaystoneEntrySlotOffset, entry.slot);
    Store<std::uint16_t>(page, base + kWaystoneEntryFlagsOffset, entry.flags);
    Store<std::uint16_t>(page, base + kWaystoneEntryStepIdOffset, entry.step_id);
    Store<std::uint16_t>(page, base + kWaystoneEntryReservedOffset, entry.reserved);
    return Status::OK();
}

bool WaystonePageHolds(std::span<const std::byte, kPageSize> page,
                       const InstanceKey& key) noexcept {
    // Type and version first: an unformatted page reads as page_type 0,
    // and a page written by a newer build must be refused rather than
    // misparsed. Both come back from here as "not this instance", which is
    // the answer a caller can act on.
    if (!storage::ValidatePageHeader(page, PageType::kWaystone).ok()) return false;

    const WaystoneHeader h = ReadWaystoneHeader(page);
    return InstanceKey{h.pattern_id, h.arg_hash} == key;
}

}  // namespace kds::stats
