#include "kds/storage/btree/btree_page.hpp"

#include <cstring>
#include <string>

// rules.md #2: all access to on-disk page bytes goes through field-wise
// memcpy helpers at explicit offsets; the mirror structs are never
// reinterpret_cast onto the buffer. Same discipline as heap_page.cpp.

namespace kds::btree {

namespace {

// Matches the Keystone column's 40-bit id width. Duplicated rather than
// pulled in from keystone.hpp for the reason heap_page.cpp duplicates it:
// the page layer stays decoupled from the row-encoding format.
inline constexpr std::uint64_t kMaxTupleId = (std::uint64_t{1} << 40) - 1;

std::size_t EntryOffset(std::uint16_t idx) {
    return kInternalEntriesOffset + static_cast<std::size_t>(idx) * kInternalEntrySize;
}

}  // namespace

BtreeInternalHeaderFields InternalView::ReadHeader() const {
    BtreeInternalHeaderFields h{};
    const std::byte* base = page_.data() + kInternalHeaderOffset;
    std::memcpy(&h.flags, base + kInternalFlagsOffset, sizeof(h.flags));
    std::memcpy(&h.nr_entries, base + kInternalNrEntriesOffset, sizeof(h.nr_entries));
    std::memcpy(&h.level, base + kInternalLevelOffset, sizeof(h.level));
    std::memcpy(&h.reserved0, base + kInternalReserved0Offset, sizeof(h.reserved0));
    std::memcpy(&h.leftmost_child, base + kInternalLeftmostChildOffset,
                sizeof(h.leftmost_child));
    std::memcpy(&h.reserved1, base + kInternalReserved1Offset, sizeof(h.reserved1));
    return h;
}

void InternalView::WriteHeader(const BtreeInternalHeaderFields& h) {
    std::byte* base = page_.data() + kInternalHeaderOffset;
    std::memcpy(base + kInternalFlagsOffset, &h.flags, sizeof(h.flags));
    std::memcpy(base + kInternalNrEntriesOffset, &h.nr_entries, sizeof(h.nr_entries));
    std::memcpy(base + kInternalLevelOffset, &h.level, sizeof(h.level));
    std::memcpy(base + kInternalReserved0Offset, &h.reserved0, sizeof(h.reserved0));
    std::memcpy(base + kInternalLeftmostChildOffset, &h.leftmost_child,
                sizeof(h.leftmost_child));
    std::memcpy(base + kInternalReserved1Offset, &h.reserved1, sizeof(h.reserved1));
}

BtreeInternalEntryFields InternalView::ReadEntry(std::uint16_t idx) const {
    BtreeInternalEntryFields e{};
    const std::byte* p = page_.data() + EntryOffset(idx);
    std::memcpy(&e.sep_key, p + kInternalEntrySepKeyOffset, sizeof(e.sep_key));
    std::memcpy(&e.child, p + kInternalEntryChildOffset, sizeof(e.child));
    return e;
}

void InternalView::WriteEntry(std::uint16_t idx, const BtreeInternalEntryFields& e) {
    std::byte* p = page_.data() + EntryOffset(idx);
    std::memcpy(p + kInternalEntrySepKeyOffset, &e.sep_key, sizeof(e.sep_key));
    std::memcpy(p + kInternalEntryChildOffset, &e.child, sizeof(e.child));
}

StatusOr<InternalView> InternalView::CreateEmpty(std::span<std::byte, kPageSize> page,
                                                  std::uint16_t level, PageId leftmost_child,
                                                  std::uint64_t owner_oid) {
    if (level == 0) {
        return Status::InvalidArgument("level 0 is a leaf, not an internal node");
    }

    // Zeroes the page and writes the common header (page_type
    // kBtreeInternal, page_lsn 0, checksum 0 - stamped at flush time,
    // page.md section 8). Everything below lives above that header.
    storage::FormatPage(page, PageType::kBtreeInternal, /*flags=*/0, owner_oid);

    InternalView view(page);

    BtreeInternalHeaderFields h{};
    h.flags = kInternalFlagInitialized;
    h.nr_entries = 0;
    h.level = level;
    h.reserved0 = 0;
    h.leftmost_child = leftmost_child;
    h.reserved1 = 0;
    view.WriteHeader(h);

    return view;
}

std::uint16_t InternalView::level() const { return ReadHeader().level; }
std::uint16_t InternalView::entry_count() const { return ReadHeader().nr_entries; }
PageId InternalView::leftmost_child() const { return ReadHeader().leftmost_child; }

StatusOr<BtreeInternalEntryFields> InternalView::Entry(std::uint16_t idx) const {
    const std::uint16_t n = entry_count();
    if (idx >= n) {
        return Status::OutOfRange("internal entry " + std::to_string(idx) + " is past the node's " +
                                  std::to_string(n) + " entries");
    }
    return ReadEntry(idx);
}

int InternalView::FindSlot(std::uint64_t key) const {
    // Last index with sep_key <= key. Upper-bound search, then step back:
    // `lo` ends as the count of entries with sep_key <= key, so lo - 1 is
    // that index and -1 correctly means "all separators are above key,
    // follow leftmost_child".
    std::uint16_t lo = 0;
    std::uint16_t hi = entry_count();
    while (lo < hi) {
        const std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);
        if (ReadEntry(mid).sep_key <= key) {
            lo = static_cast<std::uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return static_cast<int>(lo) - 1;
}

PageId InternalView::ChildFor(std::uint64_t key) const {
    const int idx = FindSlot(key);
    if (idx < 0) return leftmost_child();
    return ReadEntry(static_cast<std::uint16_t>(idx)).child;
}

Status InternalView::InsertEntry(std::uint64_t sep_key, PageId child) {
    if (sep_key > kMaxTupleId) {
        return Status::InvalidArgument("separator key exceeds 40-bit tuple id range");
    }

    BtreeInternalHeaderFields h = ReadHeader();
    if (h.nr_entries >= kInternalMaxEntries) {
        return Status::OutOfSpace("internal node is full (" +
                                  std::to_string(kInternalMaxEntries) + " entries)");
    }

    const int at = FindSlot(sep_key);
    if (at >= 0 && ReadEntry(static_cast<std::uint16_t>(at)).sep_key == sep_key) {
        // Two subtrees with the same low key would make the descent's
        // choice between them arbitrary, and one of them permanently
        // unreachable. A caller hitting this has a key-space bug, not a
        // full node.
        return Status::AlreadyExists("separator key " + std::to_string(sep_key) +
                                      " is already present in this internal node");
    }

    // Shift the tail up one stride to open the hole. Insertion is at the
    // end for a monotonically-issued pk (invariant 10), so this moves zero
    // bytes on the path that actually runs today - but the ordered form is
    // what makes the node's sortedness a property of the code rather than
    // of its one current caller.
    const std::uint16_t insert_at = static_cast<std::uint16_t>(at + 1);
    for (std::uint16_t i = h.nr_entries; i > insert_at; --i) {
        WriteEntry(i, ReadEntry(static_cast<std::uint16_t>(i - 1)));
    }
    WriteEntry(insert_at, BtreeInternalEntryFields{sep_key, child});

    h.nr_entries = static_cast<std::uint16_t>(h.nr_entries + 1);
    WriteHeader(h);
    return Status::OK();
}

}  // namespace kds::btree
