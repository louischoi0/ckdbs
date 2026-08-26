#include "kds/storage/anchor_page.hpp"

#include <string>

#include "kds/storage/page_bytes.hpp"

namespace kds::storage {
namespace {

constexpr std::size_t EntryOffset(std::size_t i) noexcept {
    return kAnchorEntriesOffset + i * kAnchorEntrySize;
}

// The checked-redundancy read of nr_index (the header states the rule):
// a count the page cannot hold is Corruption, never a loop bound.
StatusOr<std::uint16_t> EntryCount(std::span<const std::byte, kPageSize> page) {
    const auto nr = LoadField<std::uint16_t>(page, kAnchorNrIndexOffset);
    if (nr > kAnchorMaxIndexEntries) {
        return Status::Corruption("anchor page names " + std::to_string(nr) +
                                  " index entries; the page holds at most " +
                                  std::to_string(kAnchorMaxIndexEntries));
    }
    return nr;
}

// The index of `index_oid`'s entry, or nr when absent. `nr` has passed
// EntryCount's bound.
std::size_t FindEntry(std::span<const std::byte, kPageSize> page, std::uint64_t index_oid,
                      std::uint16_t nr) {
    for (std::size_t i = 0; i < nr; ++i) {
        if (LoadField<std::uint64_t>(page, EntryOffset(i)) == index_oid) return i;
    }
    return nr;
}

// The one home of the full-table refusal, so the check that runs before a
// build and the write that runs after it cannot drift apart in wording or
// in bound.
Status RoomForANewEntry(std::uint16_t nr) {
    if (nr >= kAnchorMaxIndexEntries) {
        return Status::ResourceExhausted("anchor page holds " + std::to_string(nr) +
                                         " index entries already; the table is full");
    }
    return Status::OK();
}

}  // namespace

void FormatAnchorPage(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid,
                      PageId clustered_root) {
    FormatPage(page, PageType::kAnchor, /*flags=*/0, owner_oid);
    StoreField<std::uint32_t>(page, kAnchorClusteredRootOffset, clustered_root);
    StoreField<std::uint16_t>(page, kAnchorNrIndexOffset, 0);
}

PageId AnchorClusteredRoot(std::span<const std::byte, kPageSize> page) {
    return LoadField<std::uint32_t>(page, kAnchorClusteredRootOffset);
}

void SetAnchorClusteredRoot(std::span<std::byte, kPageSize> page, PageId root) {
    StoreField<std::uint32_t>(page, kAnchorClusteredRootOffset, root);
}

StatusOr<PageId> AnchorIndexRoot(std::span<const std::byte, kPageSize> page,
                                 std::uint64_t index_oid) {
    auto nr = EntryCount(page);
    if (!nr.ok()) return nr.status();
    const std::size_t i = FindEntry(page, index_oid, nr.value());
    if (i == nr.value()) return kInvalidPageId;
    return LoadField<std::uint32_t>(page, EntryOffset(i) + sizeof(std::uint64_t));
}

Status CheckAnchorRoomForIndex(std::span<const std::byte, kPageSize> page,
                               std::uint64_t index_oid) {
    auto nr = EntryCount(page);
    if (!nr.ok()) return nr.status();
    // An entry that exists is updated in place and needs no slot.
    if (FindEntry(page, index_oid, nr.value()) != nr.value()) return Status::OK();
    return RoomForANewEntry(nr.value());
}

Status SetAnchorIndexRoot(std::span<std::byte, kPageSize> page, std::uint64_t index_oid,
                          PageId root) {
    auto nr = EntryCount(page);
    if (!nr.ok()) return nr.status();
    std::size_t i = FindEntry(page, index_oid, nr.value());
    if (i == nr.value()) {
        if (Status s = RoomForANewEntry(nr.value()); !s.ok()) return s;
        StoreField<std::uint64_t>(page, EntryOffset(i), index_oid);
        StoreField<std::uint16_t>(page, kAnchorNrIndexOffset,
                                  static_cast<std::uint16_t>(nr.value() + 1));
    }
    StoreField<std::uint32_t>(page, EntryOffset(i) + sizeof(std::uint64_t), root);
    return Status::OK();
}

}  // namespace kds::storage
