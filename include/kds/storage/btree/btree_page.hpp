#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// B+ tree **internal** node (PageType::kBtreeInternal). Leaves are not
// here: a leaf is a heap page body under a kBtreeLeaf header, read and
// written through heap::PageView (heap_page.hpp's CreateEmptyAs), so this
// file owns exactly one thing - the separator/child array that routes a
// descent.
//
// Layout within one kPageSize buffer:
//
//   [ common page header      ]  <- offset 0, 32 bytes (page_header.hpp)
//   [ BtreeInternalHeader     ]  <- offset kInternalHeaderOffset (32), 16 B
//   [ entry array             ]  <- offset kInternalEntriesOffset (48),
//                                   nr_entries * kInternalEntrySize, sorted
//                                   ascending by sep_key
//   [ ... free space ...      ]
//
// ---- Routing rule -------------------------------------------------------
//
// `leftmost_child` covers every key strictly below entries[0].sep_key;
// entries[i] covers [entries[i].sep_key, entries[i+1].sep_key). A
// separator is the **low key of the subtree it points at**, which is the
// same thing a leaf's min_key is - so a separator copied up from a leaf is
// literally that leaf's min_key, and the two can never disagree about
// where a key belongs. That is why the separator is a low key and not the
// "highest key of the left subtree" some texts use: this engine already
// has one immutable per-page low key and adding a second, differently
// derived boundary is how a descent lands one page off.
//
// ---- Why the keys are uint64 and not the tuples' 40-bit ids ------------
//
// Invariant 6: an id stored outside the tuple header is a zero-extended
// uint64 with the upper 24 bits 0. Same convention as min_key, so no
// separate width to reason about.
//
// Concurrency: InternalView is a thin view over caller-owned bytes with no
// synchronization of its own, exactly like heap::PageView. The caller
// holds the pin/latch (CLAUDE.md's page-latch consistency model).

namespace kds::btree {

// ---- Page header --------------------------------------------------------

inline constexpr std::size_t kInternalHeaderOffset = storage::kPageBodyOffset;

struct BtreeInternalHeaderFields {
    std::uint16_t flags;
    std::uint16_t nr_entries;
    // Distance to the leaf level; a leaf is level 0, so an internal node is
    // always >= 1. Stored rather than derived because a descent has to know
    // when the next hop lands on a heap-bodied leaf instead of another
    // internal node, and reading the child's header byte to find out costs
    // the page fetch this field avoids being wrong about.
    std::uint16_t level;
    std::uint16_t reserved0;
    // The subtree holding keys below entries[0].sep_key. Kept out of the
    // entry array (rather than stored as an entry with sep_key 0) so that
    // "entries[i].sep_key is the low key of child i" holds without an
    // exception, and so an empty-but-valid root has somewhere to point.
    PageId leftmost_child;
    std::uint32_t reserved1;
};

inline constexpr std::size_t kInternalFlagsOffset = 0;
inline constexpr std::size_t kInternalNrEntriesOffset = 2;
inline constexpr std::size_t kInternalLevelOffset = 4;
inline constexpr std::size_t kInternalReserved0Offset = 6;
inline constexpr std::size_t kInternalLeftmostChildOffset = 8;
inline constexpr std::size_t kInternalReserved1Offset = 12;
// 2+2+2+2+4+4 = 16, every field naturally aligned and the total a multiple
// of 4, so there is no interior or tail padding and sizeof() is safe to
// assert directly.
inline constexpr std::size_t kInternalHeaderSize = 16;

static_assert(offsetof(BtreeInternalHeaderFields, flags) == kInternalFlagsOffset);
static_assert(offsetof(BtreeInternalHeaderFields, nr_entries) == kInternalNrEntriesOffset);
static_assert(offsetof(BtreeInternalHeaderFields, level) == kInternalLevelOffset);
static_assert(offsetof(BtreeInternalHeaderFields, reserved0) == kInternalReserved0Offset);
static_assert(offsetof(BtreeInternalHeaderFields, leftmost_child) ==
              kInternalLeftmostChildOffset);
static_assert(offsetof(BtreeInternalHeaderFields, reserved1) == kInternalReserved1Offset);
static_assert(sizeof(BtreeInternalHeaderFields) == kInternalHeaderSize);

inline constexpr std::uint16_t kInternalFlagInitialized = 0x1;

// ---- Entry --------------------------------------------------------------

struct BtreeInternalEntryFields {
    std::uint64_t sep_key;  // low key of `child`'s subtree, zero-extended
    PageId child;
};

inline constexpr std::size_t kInternalEntrySepKeyOffset = 0;
inline constexpr std::size_t kInternalEntryChildOffset = 8;
// 8+4 = 12 bytes actually read/written. sizeof() rounds up to 16 for the
// uint64's alignment; that padding is never touched, so the on-disk size
// is 12 and the array is packed at that stride.
inline constexpr std::size_t kInternalEntrySize = 12;

static_assert(offsetof(BtreeInternalEntryFields, sep_key) == kInternalEntrySepKeyOffset);
static_assert(offsetof(BtreeInternalEntryFields, child) == kInternalEntryChildOffset);

inline constexpr std::size_t kInternalEntriesOffset =
    kInternalHeaderOffset + kInternalHeaderSize;  // 48

// (8192 - 48) / 12 = 678, with 8 bytes of slack at the page tail. Not
// rounded down to a power of two: nothing addresses an internal entry by
// shift/mask - the array is binary-searched - so the only thing a power of
// two would buy is fewer usable entries.
inline constexpr std::uint16_t kInternalMaxEntries =
    static_cast<std::uint16_t>((kPageSize - kInternalEntriesOffset) / kInternalEntrySize);
static_assert(kInternalMaxEntries == 678);

// ---- InternalView -------------------------------------------------------

class InternalView {
public:
    // Wraps already-initialized internal-node bytes. Does no validation,
    // for the same reason heap::PageView's constructor does not (rules.md
    // #1: constructors must not fail).
    explicit InternalView(std::span<std::byte, kPageSize> page) noexcept : page_(page) {}

    // Formats a brand-new internal node holding a single child and no
    // separators - the shape a root split produces before the new sibling
    // is added. Fails with InvalidArgument if `level` is 0 (that is a leaf,
    // which this file does not own).
    static StatusOr<InternalView> CreateEmpty(std::span<std::byte, kPageSize> page,
                                               std::uint16_t level, PageId leftmost_child,
                                               std::uint64_t owner_oid);

    std::uint16_t level() const;
    std::uint16_t entry_count() const;
    PageId leftmost_child() const;
    bool IsFull() const { return entry_count() >= kInternalMaxEntries; }

    // The entry at `idx`. Fails with OutOfRange past entry_count().
    StatusOr<BtreeInternalEntryFields> Entry(std::uint16_t idx) const;

    // The child a descent for `key` must follow: the child of the last
    // entry whose sep_key <= key, or leftmost_child if there is none.
    // Never fails - an internal node always has a leftmost child, and a
    // node with zero entries routes everything there.
    PageId ChildFor(std::uint64_t key) const;

    // Inserts (sep_key -> child) in sorted position. Fails with OutOfSpace
    // if the node is full, AlreadyExists if `sep_key` is already a
    // separator here (two subtrees cannot share a low key), and
    // InvalidArgument if `sep_key` exceeds the 40-bit id range.
    Status InsertEntry(std::uint64_t sep_key, PageId child);

private:
    BtreeInternalHeaderFields ReadHeader() const;
    void WriteHeader(const BtreeInternalHeaderFields& header);
    BtreeInternalEntryFields ReadEntry(std::uint16_t idx) const;
    void WriteEntry(std::uint16_t idx, const BtreeInternalEntryFields& entry);

    // Index of the last entry with sep_key <= key, or -1 for "none, use
    // leftmost_child". Binary search; the array is sorted by construction.
    int FindSlot(std::uint64_t key) const;

    std::span<std::byte, kPageSize> page_;
};

}  // namespace kds::btree
