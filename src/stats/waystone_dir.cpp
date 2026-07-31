#include "kds/stats/waystone_dir.hpp"

#include <cstring>
#include <string>

namespace kds::stats {

namespace {

PageId LoadChild(std::span<const std::byte, kPageSize> page, std::size_t index) {
    PageId child;
    std::memcpy(&child, page.data() + index * sizeof(PageId), sizeof(PageId));
    return child;
}

void StoreChild(std::span<std::byte, kPageSize> page, std::size_t index, PageId child) {
    std::memcpy(page.data() + index * sizeof(PageId), &child, sizeof(PageId));
}

// Every slot kEmptyDirSlot. Written explicitly rather than relying on a
// zeroed page: kEmptyDirSlot is kInvalidPageId (0xFFFFFFFF), and a zeroed
// page would read as 2048 children all pointing at page 0.
void FormatDirPage(std::span<std::byte, kPageSize> page) {
    for (std::size_t i = 0; i < kDirFanout; ++i) {
        StoreChild(page, i, kEmptyDirSlot);
    }
}

Status CheckDepth(int depth) {
    if (depth >= 1 && depth <= kMaxDirDepth) return Status::OK();
    return Status::InvalidArgument("waystone: directory depth " + std::to_string(depth) +
                                   " is outside 1.." + std::to_string(kMaxDirDepth));
}

// A pk that a directory of this depth cannot address would silently alias
// onto another one: the walk masks each digit to 11 bits, so the bits
// above the top digit would simply be discarded and two different pks
// would land on one entry. Refused instead.
Status CheckAddressable(std::uint64_t pk, int depth) {
    if (pk < DirCoverageAtDepth(depth)) return Status::OK();
    return Status::InvalidArgument("waystone: pk " + std::to_string(pk) +
                                   " is past what a depth-" + std::to_string(depth) +
                                   " directory covers (" +
                                   std::to_string(DirCoverageAtDepth(depth)) + ")");
}

}  // namespace

StatusOr<int> DirDepthFor(std::uint64_t pk) {
    if (pk > kMaxPk) {
        return Status::InvalidArgument("waystone: pk " + std::to_string(pk) +
                                       " exceeds the 40-bit Keystone id range");
    }
    for (int depth = 1; depth <= kMaxDirDepth; ++depth) {
        if (pk < DirCoverageAtDepth(depth)) return depth;
    }
    // Unreachable: kMaxDirDepth covers 2^41 > kMaxPk, asserted in the
    // header. Kept as a Status rather than an assert because "unreachable"
    // arguments are exactly the ones that stop being so.
    return Status::OutOfRange("waystone: no directory depth covers pk " + std::to_string(pk));
}

StatusOr<PageId> CreateDirPage(storage::PageStore& store) {
    // Headerless: 2048 x 4 bytes tiles the page exactly, so a common
    // header would cost a child slot and a checksum stamped at byte 4
    // would overwrite child 1.
    auto created = store.CreateNewHeaderless();
    if (!created.ok()) return created.status();
    auto [page_id, bytes] = created.value();
    FormatDirPage(bytes);
    return page_id;
}

StatusOr<PageId> LookupEntryPage(storage::PageStore& store, PageId root, int depth,
                                 std::uint64_t pk) {
    if (Status s = CheckDepth(depth); !s.ok()) return s;
    if (Status s = CheckAddressable(pk, depth); !s.ok()) return s;

    PageId current = root;
    for (int level = 0; level < depth; ++level) {
        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();

        const PageId child = LoadChild(bytes.value(), DirIndexAt(pk, depth, level));
        if (child == kEmptyDirSlot) {
            // Never populated. A normal answer on the probe path, not an
            // error: most pks in a sparse space live here.
            return kInvalidPageId;
        }
        current = child;
    }
    return current;
}

StatusOr<PageId> LookupOrCreateEntryPage(storage::PageStore& store, PageId root, int depth,
                                         std::uint64_t pk) {
    if (Status s = CheckDepth(depth); !s.ok()) return s;
    if (Status s = CheckAddressable(pk, depth); !s.ok()) return s;

    PageId current = root;
    for (int level = 0; level < depth; ++level) {
        const std::size_t index = DirIndexAt(pk, depth, level);

        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();
        const PageId child = LoadChild(bytes.value(), index);
        if (child != kEmptyDirSlot) {
            current = child;
            continue;
        }

        // Missing link. The last level's child is a leaf entry page, which
        // is a plain zeroed page (256 entries with flags 0, i.e. not
        // kEntryLive); every level above holds another directory page.
        const bool leaf = (level == depth - 1);
        PageId fresh = kInvalidPageId;
        if (leaf) {
            // Headerless for the same reason: 256 x 32 tiles it exactly.
            auto created = store.CreateNewHeaderless();
            if (!created.ok()) return created.status();
            fresh = created.value().first;
        } else {
            auto created = CreateDirPage(store);
            if (!created.ok()) return created.status();
            fresh = created.value();
        }

        // Linked after it is formatted, and re-fetched first: CreateNew()
        // may have handed out a new frame, and a page store is free to
        // move its frames (today's do not; the buffer pool with eviction
        // will). Same ordering rule ChainInsert follows - publish the link
        // only once what it points at is whole.
        auto parent = store.Get(current);
        if (!parent.ok()) return parent.status();
        StoreChild(parent.value(), index, fresh);
        current = fresh;
    }
    return current;
}

StatusOr<PageId> GrowDirectory(storage::PageStore& store, PageId root, int depth) {
    if (Status s = CheckDepth(depth); !s.ok()) return s;
    if (depth == kMaxDirDepth) {
        return Status::OutOfRange("waystone: directory is already at the maximum depth " +
                                  std::to_string(kMaxDirDepth) +
                                  ", which covers the whole pk space");
    }

    auto created = CreateDirPage(store);
    if (!created.ok()) return created.status();

    auto bytes = store.Get(created.value());
    if (!bytes.ok()) return bytes.status();
    StoreChild(bytes.value(), 0, root);
    return created.value();
}

}  // namespace kds::stats
