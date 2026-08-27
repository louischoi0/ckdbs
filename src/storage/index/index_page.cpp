#include "kds/storage/index/index_page.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "kds/storage/insert_placement.hpp"  // kMaxBtreeDepth: one depth bound, not two

// rules.md #2: every scalar field of an on-disk page goes through a
// field-wise memcpy at a named offset; the mirror structs are never
// reinterpret_cast onto the buffer. Key bytes are the one thing copied
// verbatim - they arrive already encoded (exec/index_key.hpp) and this
// layer never interprets one.

namespace kds::index {

namespace {

// Compares two sort keys. Always full width - see the header's note on why
// a partial-key probe is zero-padded rather than compared short.
int CompareKeys(std::span<const std::byte> a, std::span<const std::byte> b) {
    return std::memcmp(a.data(), b.data(), a.size());
}

}  // namespace

// ---- Layout and sort keys ----------------------------------------------

Status CheckIndexLayout(const IndexLayout& layout) {
    if (layout.key_width == 0) {
        return Status::InvalidArgument("an index key must occupy at least one byte");
    }
    const std::uint32_t leaf = layout.leaf_entry_width();
    const std::uint32_t internal = layout.internal_entry_width();
    if (leaf > kMaxIndexEntryWidth || internal > kMaxIndexEntryWidth) {
        return Status::InvalidArgument(
            "index entry is " + std::to_string(std::max(leaf, internal)) +
            " bytes, above the " + std::to_string(kMaxIndexEntryWidth) +
            "-byte limit; a page must hold two entries or a full page has no split to make "
            "(key " + std::to_string(layout.key_width) + " + pk " + std::to_string(kIndexPkWidth) +
            " + covered " + std::to_string(layout.covered_width) + ")");
    }
    return Status::OK();
}

void PutIndexPk(std::span<std::byte> out, std::uint64_t pk) {
    for (std::uint32_t i = 0; i < kIndexPkWidth; ++i) {
        out[i] = static_cast<std::byte>((pk >> (8 * (kIndexPkWidth - 1 - i))) & 0xFF);
    }
}

std::uint64_t GetIndexPk(std::span<const std::byte> in) {
    std::uint64_t pk = 0;
    for (std::uint32_t i = 0; i < kIndexPkWidth; ++i) {
        pk = (pk << 8) | static_cast<std::uint64_t>(in[i]);
    }
    return pk;
}

Status EncodeIndexSortKey(const IndexLayout& layout, std::span<const std::byte> key,
                          std::uint64_t pk, std::span<std::byte> out) {
    if (key.size() != layout.key_width || out.size() != layout.sort_key_width()) {
        return Status::InvalidArgument("index sort key expects a " +
                                       std::to_string(layout.key_width) + "-byte key into a " +
                                       std::to_string(layout.sort_key_width()) + "-byte buffer");
    }
    std::memcpy(out.data(), key.data(), key.size());
    PutIndexPk(out.subspan(layout.key_width), pk);
    return Status::OK();
}

// ---- IndexLeafView ------------------------------------------------------

IndexLeafHeaderFields IndexLeafView::ReadHeader() const {
    IndexLeafHeaderFields h{};
    const std::byte* base = page_.data() + kIndexHeaderOffset;
    std::memcpy(&h.flags, base + kIndexLeafFlagsOffset, sizeof(h.flags));
    std::memcpy(&h.nr_entries, base + kIndexLeafNrEntriesOffset, sizeof(h.nr_entries));
    std::memcpy(&h.key_width, base + kIndexLeafKeyWidthOffset, sizeof(h.key_width));
    std::memcpy(&h.entry_width, base + kIndexLeafEntryWidthOffset, sizeof(h.entry_width));
    std::memcpy(&h.right_sibling, base + kIndexLeafRightSiblingOffset, sizeof(h.right_sibling));
    std::memcpy(&h.reserved0, base + kIndexLeafReserved0Offset, sizeof(h.reserved0));
    return h;
}

void IndexLeafView::WriteHeader(const IndexLeafHeaderFields& h) {
    std::byte* base = page_.data() + kIndexHeaderOffset;
    std::memcpy(base + kIndexLeafFlagsOffset, &h.flags, sizeof(h.flags));
    std::memcpy(base + kIndexLeafNrEntriesOffset, &h.nr_entries, sizeof(h.nr_entries));
    std::memcpy(base + kIndexLeafKeyWidthOffset, &h.key_width, sizeof(h.key_width));
    std::memcpy(base + kIndexLeafEntryWidthOffset, &h.entry_width, sizeof(h.entry_width));
    std::memcpy(base + kIndexLeafRightSiblingOffset, &h.right_sibling, sizeof(h.right_sibling));
    std::memcpy(base + kIndexLeafReserved0Offset, &h.reserved0, sizeof(h.reserved0));
}

std::size_t IndexLeafView::EntryOffset(std::uint16_t idx) const {
    return kIndexLeafEntriesOffset + static_cast<std::size_t>(idx) * ReadHeader().entry_width;
}

StatusOr<IndexLeafView> IndexLeafView::CreateEmpty(std::span<std::byte, kPageSize> page,
                                                    const IndexLayout& layout,
                                                    std::uint64_t owner_oid) {
    if (Status s = CheckIndexLayout(layout); !s.ok()) return s;

    // Zeroes the page and writes the common header (page_type kIndexLeaf,
    // page_lsn 0, checksum 0 - stamped at flush time, page.md section 8).
    storage::FormatPage(page, PageType::kIndexLeaf, /*flags=*/0, owner_oid);

    IndexLeafView view(page);
    IndexLeafHeaderFields h{};
    h.flags = kIndexFlagInitialized;
    h.nr_entries = 0;
    h.key_width = layout.key_width;
    h.entry_width = static_cast<std::uint16_t>(layout.leaf_entry_width());
    h.right_sibling = kInvalidPageId;
    h.reserved0 = 0;
    view.WriteHeader(h);
    return view;
}

std::uint16_t IndexLeafView::entry_count() const { return ReadHeader().nr_entries; }
std::uint16_t IndexLeafView::key_width() const { return ReadHeader().key_width; }
std::uint16_t IndexLeafView::entry_width() const { return ReadHeader().entry_width; }
PageId IndexLeafView::right_sibling() const { return ReadHeader().right_sibling; }

void IndexLeafView::set_right_sibling(PageId page_id) {
    IndexLeafHeaderFields h = ReadHeader();
    h.right_sibling = page_id;
    WriteHeader(h);
}

bool IndexLeafView::IsFull() const {
    const IndexLeafHeaderFields h = ReadHeader();
    if (h.entry_width == 0) return true;  // a malformed page has no room by definition
    return h.nr_entries >= static_cast<std::uint16_t>(kIndexEntrySpace / h.entry_width);
}

StatusOr<std::span<const std::byte>> IndexLeafView::Entry(std::uint16_t idx) const {
    const IndexLeafHeaderFields h = ReadHeader();
    if (idx >= h.nr_entries) {
        return Status::OutOfRange("index leaf entry " + std::to_string(idx) + " is past the " +
                                  std::to_string(h.nr_entries) + " it holds");
    }
    return std::span<const std::byte>(page_.data() + EntryOffset(idx), h.entry_width);
}

StatusOr<std::span<const std::byte>> IndexLeafView::SortKey(std::uint16_t idx) const {
    auto entry = Entry(idx);
    if (!entry.ok()) return entry.status();
    const IndexLeafHeaderFields h = ReadHeader();
    return entry.value().subspan(0, static_cast<std::size_t>(h.key_width) + kIndexPkWidth);
}

std::uint16_t IndexLeafView::LowerBound(std::span<const std::byte> sort_key) const {
    const IndexLeafHeaderFields h = ReadHeader();
    const std::size_t stored_len = static_cast<std::size_t>(h.key_width) + kIndexPkWidth;

    std::uint16_t lo = 0;
    std::uint16_t hi = h.nr_entries;
    while (lo < hi) {
        const std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);
        const std::span<const std::byte> stored(page_.data() + EntryOffset(mid), stored_len);
        if (CompareKeys(stored, sort_key) < 0) {
            lo = static_cast<std::uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return lo;
}

StatusOr<std::uint16_t> IndexLeafView::InsertEntry(std::span<const std::byte> entry) {
    IndexLeafHeaderFields h = ReadHeader();
    if (entry.size() != h.entry_width) {
        return Status::InvalidArgument("index entry is " + std::to_string(entry.size()) +
                                       " bytes, this leaf holds " +
                                       std::to_string(h.entry_width));
    }
    if (IsFull()) {
        return Status::OutOfSpace("index leaf is full (" + std::to_string(h.nr_entries) +
                                  " entries)");
    }

    const std::uint16_t at =
        LowerBound(entry.subspan(0, static_cast<std::size_t>(h.key_width) + kIndexPkWidth));

    // Open the hole with one move of the tail. Unlike the clustered tree's
    // internal node - where a monotonic pk means the insert is always at the
    // end - a secondary key arrives in arbitrary order, so this move is the
    // normal case rather than a formality.
    std::byte* base = page_.data();
    const std::size_t stride = h.entry_width;
    const std::size_t from = kIndexLeafEntriesOffset + static_cast<std::size_t>(at) * stride;
    const std::size_t moved = static_cast<std::size_t>(h.nr_entries - at) * stride;
    if (moved > 0) std::memmove(base + from + stride, base + from, moved);
    std::memcpy(base + from, entry.data(), stride);

    h.nr_entries = static_cast<std::uint16_t>(h.nr_entries + 1);
    WriteHeader(h);
    return at;
}

Status IndexLeafView::SplitInto(IndexLeafView& into) {
    IndexLeafHeaderFields h = ReadHeader();
    const IndexLeafHeaderFields target = into.ReadHeader();

    if (target.entry_width != h.entry_width || target.key_width != h.key_width) {
        return Status::InvalidArgument("index leaf split target has different entry widths");
    }
    if (target.nr_entries != 0) {
        return Status::InvalidArgument("index leaf split target is not empty");
    }
    if (h.nr_entries < 2) {
        return Status::InvalidArgument("an index leaf holding " + std::to_string(h.nr_entries) +
                                       " entries has no split to make");
    }

    // The midpoint (docs/spec/index.md IX4a, `[PROPOSED]`). A parameter of
    // this function in the sense that changing it changes nothing else: the
    // only property the tree depends on is that both halves end non-empty.
    const std::uint16_t keep = static_cast<std::uint16_t>(h.nr_entries / 2);
    const std::uint16_t move = static_cast<std::uint16_t>(h.nr_entries - keep);

    std::memcpy(into.page_.data() + kIndexLeafEntriesOffset,
                page_.data() + kIndexLeafEntriesOffset +
                    static_cast<std::size_t>(keep) * h.entry_width,
                static_cast<std::size_t>(move) * h.entry_width);

    IndexLeafHeaderFields new_target = target;
    new_target.nr_entries = move;
    into.WriteHeader(new_target);

    // The vacated bytes are left as they are. Nothing reads past
    // `nr_entries`, and zeroing them would be a page-sized write on every
    // split to hide bytes no reader can reach - the same bargain a heap
    // page's retired slot strikes.
    h.nr_entries = keep;
    WriteHeader(h);
    return Status::OK();
}

Status IndexLeafView::CheckAgainst(const IndexLayout& layout, PageId page_id) const {
    const IndexLeafHeaderFields h = ReadHeader();
    if ((h.flags & kIndexFlagInitialized) == 0) {
        return Status::Corruption("index leaf " + std::to_string(page_id) +
                                  " was never initialized");
    }
    if (h.key_width != layout.key_width ||
        h.entry_width != static_cast<std::uint16_t>(layout.leaf_entry_width())) {
        return Status::Corruption(
            "index leaf " + std::to_string(page_id) + " stores key_width " +
            std::to_string(h.key_width) + "/entry_width " + std::to_string(h.entry_width) +
            ", but this index declares " + std::to_string(layout.key_width) + "/" +
            std::to_string(layout.leaf_entry_width()));
    }
    if (h.entry_width > 0 && h.nr_entries > kIndexEntrySpace / h.entry_width) {
        return Status::Corruption("index leaf " + std::to_string(page_id) + " claims " +
                                  std::to_string(h.nr_entries) + " entries, more than fit");
    }
    return Status::OK();
}

// ---- IndexInternalView --------------------------------------------------

IndexInternalHeaderFields IndexInternalView::ReadHeader() const {
    IndexInternalHeaderFields h{};
    const std::byte* base = page_.data() + kIndexHeaderOffset;
    std::memcpy(&h.flags, base + kIndexInternalFlagsOffset, sizeof(h.flags));
    std::memcpy(&h.nr_entries, base + kIndexInternalNrEntriesOffset, sizeof(h.nr_entries));
    std::memcpy(&h.key_width, base + kIndexInternalKeyWidthOffset, sizeof(h.key_width));
    std::memcpy(&h.level, base + kIndexInternalLevelOffset, sizeof(h.level));
    std::memcpy(&h.leftmost_child, base + kIndexInternalLeftmostChildOffset,
                sizeof(h.leftmost_child));
    std::memcpy(&h.reserved0, base + kIndexInternalReserved0Offset, sizeof(h.reserved0));
    return h;
}

void IndexInternalView::WriteHeader(const IndexInternalHeaderFields& h) {
    std::byte* base = page_.data() + kIndexHeaderOffset;
    std::memcpy(base + kIndexInternalFlagsOffset, &h.flags, sizeof(h.flags));
    std::memcpy(base + kIndexInternalNrEntriesOffset, &h.nr_entries, sizeof(h.nr_entries));
    std::memcpy(base + kIndexInternalKeyWidthOffset, &h.key_width, sizeof(h.key_width));
    std::memcpy(base + kIndexInternalLevelOffset, &h.level, sizeof(h.level));
    std::memcpy(base + kIndexInternalLeftmostChildOffset, &h.leftmost_child,
                sizeof(h.leftmost_child));
    std::memcpy(base + kIndexInternalReserved0Offset, &h.reserved0, sizeof(h.reserved0));
}

std::uint32_t IndexInternalView::entry_stride() const {
    return static_cast<std::uint32_t>(ReadHeader().key_width) + kIndexPkWidth +
           static_cast<std::uint32_t>(sizeof(PageId));
}

std::size_t IndexInternalView::EntryOffset(std::uint16_t idx) const {
    return kIndexInternalEntriesOffset + static_cast<std::size_t>(idx) * entry_stride();
}

StatusOr<IndexInternalView> IndexInternalView::CreateEmpty(std::span<std::byte, kPageSize> page,
                                                            const IndexLayout& layout,
                                                            std::uint16_t level,
                                                            PageId leftmost_child,
                                                            std::uint64_t owner_oid) {
    if (Status s = CheckIndexLayout(layout); !s.ok()) return s;
    if (level == 0) {
        return Status::InvalidArgument("level 0 is a leaf, not an index internal node");
    }

    storage::FormatPage(page, PageType::kIndexInternal, /*flags=*/0, owner_oid);

    IndexInternalView view(page);
    IndexInternalHeaderFields h{};
    h.flags = kIndexFlagInitialized;
    h.nr_entries = 0;
    h.key_width = layout.key_width;
    h.level = level;
    h.leftmost_child = leftmost_child;
    h.reserved0 = 0;
    view.WriteHeader(h);
    return view;
}

std::uint16_t IndexInternalView::entry_count() const { return ReadHeader().nr_entries; }
std::uint16_t IndexInternalView::key_width() const { return ReadHeader().key_width; }
std::uint16_t IndexInternalView::level() const { return ReadHeader().level; }
PageId IndexInternalView::leftmost_child() const { return ReadHeader().leftmost_child; }

void IndexInternalView::set_leftmost_child(PageId page_id) {
    IndexInternalHeaderFields h = ReadHeader();
    h.leftmost_child = page_id;
    WriteHeader(h);
}

StatusOr<std::span<const std::byte>> IndexInternalView::Separator(std::uint16_t idx) const {
    const IndexInternalHeaderFields h = ReadHeader();
    if (idx >= h.nr_entries) {
        return Status::OutOfRange("index separator " + std::to_string(idx) + " is past the " +
                                  std::to_string(h.nr_entries) + " this node holds");
    }
    return std::span<const std::byte>(page_.data() + EntryOffset(idx),
                                      static_cast<std::size_t>(h.key_width) + kIndexPkWidth);
}

StatusOr<PageId> IndexInternalView::Child(std::uint16_t idx) const {
    const IndexInternalHeaderFields h = ReadHeader();
    if (idx >= h.nr_entries) {
        return Status::OutOfRange("index child " + std::to_string(idx) + " is past the " +
                                  std::to_string(h.nr_entries) + " this node holds");
    }
    PageId child = kInvalidPageId;
    std::memcpy(&child,
                page_.data() + EntryOffset(idx) + static_cast<std::size_t>(h.key_width) +
                    kIndexPkWidth,
                sizeof(child));
    return child;
}

bool IndexInternalView::IsFull(const IndexLayout& layout) const {
    return entry_count() >= MaxInternalEntries(layout);
}

int IndexInternalView::FindSlot(std::span<const std::byte> sort_key) const {
    // Last index with separator <= sort_key. Upper-bound search then step
    // back, so `lo` ends as the count of separators <= the probe and -1
    // correctly means "all separators are above it, follow leftmost_child".
    const IndexInternalHeaderFields h = ReadHeader();
    const std::size_t sep_len = static_cast<std::size_t>(h.key_width) + kIndexPkWidth;

    std::uint16_t lo = 0;
    std::uint16_t hi = h.nr_entries;
    while (lo < hi) {
        const std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);
        const std::span<const std::byte> sep(page_.data() + EntryOffset(mid), sep_len);
        if (CompareKeys(sep, sort_key) <= 0) {
            lo = static_cast<std::uint16_t>(mid + 1);
        } else {
            hi = mid;
        }
    }
    return static_cast<int>(lo) - 1;
}

PageId IndexInternalView::ChildFor(std::span<const std::byte> sort_key) const {
    const int idx = FindSlot(sort_key);
    if (idx < 0) return leftmost_child();
    auto child = Child(static_cast<std::uint16_t>(idx));
    return child.ok() ? child.value() : leftmost_child();
}

Status IndexInternalView::InsertEntry(const IndexLayout& layout,
                                      std::span<const std::byte> separator, PageId child) {
    IndexInternalHeaderFields h = ReadHeader();
    const std::size_t sep_len = static_cast<std::size_t>(h.key_width) + kIndexPkWidth;
    if (separator.size() != sep_len) {
        return Status::InvalidArgument("index separator is " + std::to_string(separator.size()) +
                                       " bytes, this node holds " + std::to_string(sep_len));
    }
    if (IsFull(layout)) {
        return Status::OutOfSpace("index internal node is full (" +
                                  std::to_string(h.nr_entries) + " entries)");
    }

    const int at = FindSlot(separator);
    if (at >= 0) {
        auto existing = Separator(static_cast<std::uint16_t>(at));
        if (!existing.ok()) return existing.status();
        if (CompareKeys(existing.value(), separator) == 0) {
            return Status::AlreadyExists(
                "this separator is already present in the index node; two subtrees cannot "
                "share a low key");
        }
    }

    const std::uint16_t insert_at = static_cast<std::uint16_t>(at + 1);
    const std::size_t stride = entry_stride();
    std::byte* base = page_.data();
    const std::size_t from = kIndexInternalEntriesOffset + static_cast<std::size_t>(insert_at) * stride;
    const std::size_t moved = static_cast<std::size_t>(h.nr_entries - insert_at) * stride;
    if (moved > 0) std::memmove(base + from + stride, base + from, moved);
    std::memcpy(base + from, separator.data(), sep_len);
    std::memcpy(base + from + sep_len, &child, sizeof(child));

    h.nr_entries = static_cast<std::uint16_t>(h.nr_entries + 1);
    WriteHeader(h);
    return Status::OK();
}

Status IndexInternalView::SplitInto(IndexInternalView& into, std::span<std::byte> sep_out) {
    IndexInternalHeaderFields h = ReadHeader();
    const std::size_t sep_len = static_cast<std::size_t>(h.key_width) + kIndexPkWidth;

    if (into.ReadHeader().key_width != h.key_width) {
        return Status::InvalidArgument("index node split target has a different key width");
    }
    if (into.entry_count() != 0) {
        return Status::InvalidArgument("index node split target is not empty");
    }
    if (sep_out.size() != sep_len) {
        return Status::InvalidArgument("index separator buffer is " +
                                       std::to_string(sep_out.size()) + " bytes, expected " +
                                       std::to_string(sep_len));
    }
    if (h.nr_entries < 2) {
        return Status::InvalidArgument("an index node holding " + std::to_string(h.nr_entries) +
                                       " entries has no split to make");
    }

    // The median entry is what gets pushed up: its separator leaves this
    // node, and its child becomes the new node's leftmost subtree. That is
    // consistent with the routing rule rather than an exception to it - the
    // pushed separator *is* the low key of the subtree `into` now covers.
    const std::uint16_t mid = static_cast<std::uint16_t>(h.nr_entries / 2);
    const std::size_t stride = entry_stride();
    const std::byte* mid_entry = page_.data() + EntryOffset(mid);

    std::memcpy(sep_out.data(), mid_entry, sep_len);
    PageId mid_child = kInvalidPageId;
    std::memcpy(&mid_child, mid_entry + sep_len, sizeof(mid_child));

    const std::uint16_t move = static_cast<std::uint16_t>(h.nr_entries - mid - 1);
    if (move > 0) {
        std::memcpy(into.page_.data() + kIndexInternalEntriesOffset,
                    page_.data() + EntryOffset(static_cast<std::uint16_t>(mid + 1)),
                    static_cast<std::size_t>(move) * stride);
    }

    IndexInternalHeaderFields target = into.ReadHeader();
    target.nr_entries = move;
    target.level = h.level;
    target.leftmost_child = mid_child;
    into.WriteHeader(target);

    h.nr_entries = mid;
    WriteHeader(h);

    // Both halves end able to route, which the `nr_entries >= 2` guard above
    // already gives: mid >= 1 leaves this node with a separator, and `into`
    // always has its leftmost child.
    return Status::OK();
}

Status IndexInternalView::CheckAgainst(const IndexLayout& layout, PageId page_id) const {
    const IndexInternalHeaderFields h = ReadHeader();
    if ((h.flags & kIndexFlagInitialized) == 0) {
        return Status::Corruption("index node " + std::to_string(page_id) +
                                  " was never initialized");
    }
    if (h.key_width != layout.key_width) {
        return Status::Corruption("index node " + std::to_string(page_id) + " stores key_width " +
                                  std::to_string(h.key_width) + ", but this index declares " +
                                  std::to_string(layout.key_width));
    }
    if (h.level == 0 || h.level >= storage::kMaxBtreeDepth) {
        return Status::Corruption("index node " + std::to_string(page_id) + " reports level " +
                                  std::to_string(h.level));
    }
    if (h.nr_entries > MaxInternalEntries(layout)) {
        return Status::Corruption("index node " + std::to_string(page_id) + " claims " +
                                  std::to_string(h.nr_entries) + " entries, more than fit");
    }
    return Status::OK();
}

}  // namespace kds::index
