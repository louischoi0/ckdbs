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
    if (depth >= 1 && depth <= kMaxPatternDirDepth) return Status::OK();
    return Status::InvalidArgument("waystone: directory depth " + std::to_string(depth) +
                                   " is outside 1.." + std::to_string(kMaxPatternDirDepth));
}

}  // namespace

StatusOr<PageId> CreateDirPage(storage::PageStore& store) {
    // Headerless: 2048 x 4 bytes tiles the page exactly, so a common
    // header would cost a child slot and a checksum stamped at byte 4
    // would overwrite child 1.
    auto created = store.CreateNewHeaderless();
    if (!created.ok()) return created.status();
    auto& [page_id, bytes_ref] = created.value();
    const std::span<std::byte, kPageSize> bytes = bytes_ref.bytes();
    FormatDirPage(bytes);
    return page_id;
}

StatusOr<PageId> LookupWaystonePage(storage::PageStore& store, PageId root, int depth,
                                    const InstanceKey& key) {
    if (Status s = CheckDepth(depth); !s.ok()) return s;

    // No range check on the key, unlike the pk directory this replaces: a
    // hash has no coverage to exceed, and the digits above 11*depth fold
    // (waystone_dir.hpp).
    PageId current = root;
    for (int level = 0; level < depth; ++level) {
        auto bytes = store.GetForRead(current);
        if (!bytes.ok()) return bytes.status();

        const PageId child = LoadChild(bytes.value().bytes(), DirIndexAt(key.arg_hash, depth, level));
        if (child == kEmptyDirSlot) {
            // Never populated. The ordinary answer on the replay path, not
            // an error: most instances of a pattern have no trail.
            return kInvalidPageId;
        }
        current = child;
    }
    return current;
}

StatusOr<PageId> LookupOrCreateWaystonePage(storage::PageStore& store, PageId root, int depth,
                                            const InstanceKey& key) {
    if (Status s = CheckDepth(depth); !s.ok()) return s;

    PageId current = root;
    for (int level = 0; level < depth; ++level) {
        const std::size_t index = DirIndexAt(key.arg_hash, depth, level);

        auto bytes = store.Get(current);
        if (!bytes.ok()) return bytes.status();
        const PageId child = LoadChild(bytes.value().bytes(), index);
        if (child != kEmptyDirSlot) {
            current = child;
            continue;
        }

        // Missing link. The last level's child is the waystone page
        // itself, headered like every page class that is not addressed by
        // shift and mask; every level above it is another interior page.
        const bool leaf = (level == depth - 1);
        PageId fresh = kInvalidPageId;
        if (leaf) {
            // Left zeroed, i.e. PageType::kInvalid: the caller formats it
            // for the instance it is about to record. Until then it reads
            // as "not this instance" and costs a reader a miss, which is
            // the same thing an unlinked slot costs.
            auto created = store.CreateNew();
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
        StoreChild(parent.value().bytes(), index, fresh);
        current = fresh;
    }
    return current;
}

StatusOr<PageId> GrowPatternDirectory(storage::PageStore& store, PageId root, int depth) {
    if (Status s = CheckDepth(depth); !s.ok()) return s;
    if (depth == kMaxPatternDirDepth) {
        return Status::OutOfRange("waystone: directory is already at the maximum depth " +
                                  std::to_string(kMaxPatternDirDepth) +
                                  ", which addresses the whole 64-bit arg_hash");
    }

    auto created = CreateDirPage(store);
    if (!created.ok()) return created.status();

    auto bytes = store.Get(created.value());
    if (!bytes.ok()) return bytes.status();
    StoreChild(bytes.value().bytes(), 0, root);
    return created.value();
}

}  // namespace kds::stats
