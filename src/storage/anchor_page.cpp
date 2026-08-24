#include "kds/storage/anchor_page.hpp"

#include <cstring>

namespace kds::storage {
namespace {

template <typename T>
T Load(std::span<const std::byte, kPageSize> page, std::size_t offset) {
    T out{};
    std::memcpy(&out, page.data() + offset, sizeof(T));
    return out;
}

template <typename T>
void Store(std::span<std::byte, kPageSize> page, std::size_t offset, T value) {
    std::memcpy(page.data() + offset, &value, sizeof(T));
}

std::size_t EntryOffset(std::size_t i) {
    return kAnchorEntriesOffset + i * kAnchorEntrySize;
}

// The index of `index_oid`'s entry, or nr when absent.
std::size_t FindEntry(std::span<const std::byte, kPageSize> page, std::uint64_t index_oid,
                      std::uint16_t nr) {
    for (std::size_t i = 0; i < nr; ++i) {
        if (Load<std::uint64_t>(page, EntryOffset(i)) == index_oid) return i;
    }
    return nr;
}

}  // namespace

void FormatAnchorPage(std::span<std::byte, kPageSize> page, std::uint64_t owner_oid,
                      PageId clustered_root) {
    FormatPage(page, PageType::kAnchor, /*flags=*/0, owner_oid);
    Store<std::uint32_t>(page, kAnchorClusteredRootOffset, clustered_root);
    Store<std::uint16_t>(page, kAnchorNrIndexOffset, 0);
}

PageId AnchorClusteredRoot(std::span<const std::byte, kPageSize> page) {
    return Load<std::uint32_t>(page, kAnchorClusteredRootOffset);
}

void SetAnchorClusteredRoot(std::span<std::byte, kPageSize> page, PageId root) {
    Store<std::uint32_t>(page, kAnchorClusteredRootOffset, root);
}

PageId AnchorIndexRoot(std::span<const std::byte, kPageSize> page, std::uint64_t index_oid) {
    const auto nr = Load<std::uint16_t>(page, kAnchorNrIndexOffset);
    const std::size_t i = FindEntry(page, index_oid, nr);
    if (i == nr) return kInvalidPageId;
    return Load<std::uint32_t>(page, EntryOffset(i) + sizeof(std::uint64_t));
}

Status SetAnchorIndexRoot(std::span<std::byte, kPageSize> page, std::uint64_t index_oid,
                          PageId root) {
    const auto nr = Load<std::uint16_t>(page, kAnchorNrIndexOffset);
    std::size_t i = FindEntry(page, index_oid, nr);
    if (i == nr) {
        if (nr >= kAnchorMaxIndexEntries) {
            return Status::ResourceExhausted(
                "anchor page holds " + std::to_string(nr) +
                " index entries already; the table is full");
        }
        Store<std::uint64_t>(page, EntryOffset(i), index_oid);
        Store<std::uint16_t>(page, kAnchorNrIndexOffset, static_cast<std::uint16_t>(nr + 1));
    }
    Store<std::uint32_t>(page, EntryOffset(i) + sizeof(std::uint64_t), root);
    return Status::OK();
}

void RemoveAnchorIndexRoot(std::span<std::byte, kPageSize> page, std::uint64_t index_oid) {
    const auto nr = Load<std::uint16_t>(page, kAnchorNrIndexOffset);
    const std::size_t i = FindEntry(page, index_oid, nr);
    if (i == nr) return;
    const std::size_t last = nr - 1;
    if (i != last) {
        Store<std::uint64_t>(page, EntryOffset(i), Load<std::uint64_t>(page, EntryOffset(last)));
        Store<std::uint32_t>(
            page, EntryOffset(i) + sizeof(std::uint64_t),
            Load<std::uint32_t>(page, EntryOffset(last) + sizeof(std::uint64_t)));
    }
    Store<std::uint16_t>(page, kAnchorNrIndexOffset, static_cast<std::uint16_t>(last));
    return;
}

}  // namespace kds::storage
