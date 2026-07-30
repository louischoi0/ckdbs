#include "kds/stats/waystone_hooks.hpp"

#include <string>

#include "kds/stats/waystone_dir.hpp"

namespace kds::stats {

namespace {

Status CheckRef(const WaystoneRef& ws, std::uint64_t pk) {
    if (!ws.valid()) {
        return Status::InvalidArgument(
            "waystone: relation has no usable directory (root " + std::to_string(ws.dir_root) +
            ", depth " + std::to_string(ws.depth) + ")");
    }
    if (pk > kMaxPk) {
        return Status::InvalidArgument("waystone: pk " + std::to_string(pk) +
                                       " exceeds the 40-bit Keystone id range");
    }
    if (!ws.covers(pk)) {
        // Separated from InvalidArgument deliberately: this is the normal
        // signal that the directory has to deepen, not a caller error.
        return Status::OutOfRange("waystone: pk " + std::to_string(pk) +
                                  " is past what a depth-" + std::to_string(ws.depth) +
                                  " directory covers; grow the directory and retry");
    }
    return Status::OK();
}

}  // namespace

Status OnInsert(storage::PageStore& store, const WaystoneRef& ws, std::uint64_t pk,
                PageId page_id, std::uint16_t slot, std::uint32_t epoch) {
    if (Status s = CheckRef(ws, pk); !s.ok()) return s;

    // Allocating is the point: a pk whose entry page does not exist yet is
    // the ordinary case for the first tuple in each 256-id range.
    auto leaf = LookupOrCreateEntryPage(store, ws.dir_root, ws.depth, pk);
    if (!leaf.ok()) return leaf.status();

    auto bytes = store.Get(leaf.value());
    if (!bytes.ok()) return bytes.status();

    WaystoneEntry entry{};
    entry.pk = pk;
    entry.page_id = page_id;
    entry.slot = slot;
    entry.flags = kEntryLive;
    // The tuple exists and nothing has read it. Heat is earned, not
    // granted - starting it above zero would make a bulk load look hot.
    entry.use_count = 0;
    entry.last_ts = 0;
    entry.page_epoch = epoch;
    entry.reserved = 0;
    return WriteEntry(bytes.value(), EntrySlotOf(pk), entry);
}

Status OnDelete(storage::PageStore& store, const WaystoneRef& ws, std::uint64_t pk) {
    if (Status s = CheckRef(ws, pk); !s.ok()) return s;

    // Never allocates: a delete that had to create an entry page would be
    // recording the absence of a tuple, which is not a fact this structure
    // keeps.
    auto leaf = LookupEntryPage(store, ws.dir_root, ws.depth, pk);
    if (!leaf.ok()) return leaf.status();
    if (leaf.value() == kInvalidPageId) {
        return Status::OK();  // no entry to clear; see the header
    }

    auto bytes = store.Get(leaf.value());
    if (!bytes.ok()) return bytes.status();

    auto entry = ReadEntry(std::span<const std::byte, kPageSize>(bytes.value()), EntrySlotOf(pk));
    if (!entry.ok()) return entry.status();
    if ((entry.value().flags & kEntryLive) == 0) {
        return Status::OK();  // already cleared; clearing twice is not an event
    }

    // Read-modify-write rather than a blind store: location and heat
    // survive a delete-mark (spec section 9), and only liveness changes.
    WaystoneEntry updated = entry.value();
    updated.flags = static_cast<std::uint16_t>(updated.flags & ~kEntryLive);
    return WriteEntry(bytes.value(), EntrySlotOf(pk), updated);
}

StatusOr<WaystoneEntry> LookupEntry(storage::PageStore& store, const WaystoneRef& ws,
                                    std::uint64_t pk) {
    if (Status s = CheckRef(ws, pk); !s.ok()) return s;

    auto leaf = LookupEntryPage(store, ws.dir_root, ws.depth, pk);
    if (!leaf.ok()) return leaf.status();
    if (leaf.value() == kInvalidPageId) {
        return Status::NotFound("waystone: no entry page for pk " + std::to_string(pk));
    }

    auto bytes = store.Get(leaf.value());
    if (!bytes.ok()) return bytes.status();
    return ReadEntry(std::span<const std::byte, kPageSize>(bytes.value()), EntrySlotOf(pk));
}

}  // namespace kds::stats
