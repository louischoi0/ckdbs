#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// The two page formats of a secondary index (docs/spec/index.md §4,
// workplan IX02): the leaf that holds entries and the internal node that
// routes a descent.
//
// ---- What an index page is not ------------------------------------------
//
// It is not a heap page. A leaf here holds a **sorted array of fixed-width
// entries**, binary-searched directly - no slot directory, because nothing
// of variable length needs addressing; no `min_key`, no Keystone word, no
// MVCC header, because it holds no tuple. That is why dividing one on a
// split decides nothing about the heap page split policy CLAUDE.md leaves
// open, and why invariants 2 and 3 have nothing to be about here.
//
// It is also not `btree_page.hpp`. That file routes on a `uint64` Keystone
// id; this one routes on an opaque byte string of a width the *index*
// fixes. The shape is deliberately the same - separator array,
// `leftmost_child`, `level`, binary search - because the shape was right;
// the key type is what differs, and it differs all the way down.
//
// ---- The sort key is (key, pk), not the key --------------------------
//
// A secondary key is not unique, so two entries can share one key. If the
// tree routed on the key alone, a separator equal to a duplicated key would
// let a descent land past entries that belong to it - the duplicates would
// straddle a boundary and half of them would be unreachable.
//
// So the ordering key of every entry, and every separator, is the full
// `key || pk` pair: `kIndexPkWidth` bytes of primary key, **big-endian**,
// appended to the encoded key. Big-endian for the same reason the key
// itself is (exec/index_key.hpp): it is compared with `memcmp` and by
// nothing else. A seek for key K then uses `(K, 0)`, which sorts at or
// below every entry carrying K, and the walk crosses `right_sibling` links
// while the key part still matches.
//
// Invariant 7 is satisfied by the value, not the byte order: what is stored
// is a zero-extended `uint64` whose upper 24 bits are 0.
//
// ---- A probe is padded, never shortened ---------------------------------
//
// Every comparison in this file is over a **full** `key_width +
// kIndexPkWidth` bytes, and a caller that knows less than a whole sort key
// zero-pads the rest. That is not a convenience: a short comparison would be
// wrong. A separator whose key equals a probe's would compare *equal* under
// a truncated memcmp and route the descent right, past entries in the left
// subtree carrying that same key with a lower pk - which is exactly the
// duplicate-straddling failure the (key, pk) sort key exists to prevent.
//
// Zero-padding gives the true lower bound instead, and it does so for free:
// a key column's leading discriminator byte is `kIndexKeyPresent` (1) for
// every value that exists, so a zero byte in that position sorts below all
// of them. `WHERE a = 3` on an index over `(a, b)` therefore encodes column
// `a`, leaves `b` and the pk zero, and lands on the first entry that can
// match.
//
// ---- Key bytes are opaque here ------------------------------------------
//
// Nothing in this file or in index_tree.hpp interprets a key byte. Ordering
// is `memcmp` over `key_width + kIndexPkWidth` bytes, and what those bytes
// mean is `exec::EncodeIndexKey`'s business. That is what keeps the tree
// free of the schema, of a per-type dispatch, and of any chance of
// disagreeing with `CompareValues`.
//
// ---- Checked redundancy -------------------------------------------------
//
// Every page stores its own `key_width` (and a leaf its `entry_width`),
// duplicating what the catalog's index row already says. They carry no new
// information and exist to be **checked**: a page whose widths disagree
// with the index definition is `Corruption`, never interpreted. Same rule
// invariant 13 applies to a tuple's `data_len`.
//
// Encoding rules per rules.md sections 2 and 5: mirror structs with
// offsetof static_asserts, field-wise memcpy through named offsets,
// fixed-width little-endian for every *scalar* field, no bitfields, no
// reinterpret_cast onto page bytes. The key bytes are the one thing written
// verbatim, because they are already an encoding.
//
// Concurrency: both views are thin views over caller-owned bytes with no
// synchronization of their own, exactly like heap::PageView and
// btree::InternalView. The caller holds the pin/latch (CLAUDE.md's
// page-latch consistency model).

namespace kds::index {

// The primary key an entry carries, in bytes. Not derived from
// `sizeof(std::uint64_t)`: it is a stored width, and a stored width that
// tracks a C++ type is a format that changes when a type does.
inline constexpr std::uint32_t kIndexPkWidth = 8;

// The shape of one index's entries - its schema constant, in the sense
// `catalog::RowLayout::row_size` is a relation's.
struct IndexLayout {
    // The encoded composite key, from exec::IndexKeyWidth().
    std::uint16_t key_width = 0;
    // The covered columns' inline cells, concatenated. 0 for an index with
    // no COVERING clause, which is every index until the read path can use
    // one.
    std::uint16_t covered_width = 0;

    // What the tree orders by, and what a separator is.
    constexpr std::uint32_t sort_key_width() const {
        return static_cast<std::uint32_t>(key_width) + kIndexPkWidth;
    }
    constexpr std::uint32_t leaf_entry_width() const {
        return sort_key_width() + covered_width;
    }
    // A separator plus the child it routes to.
    constexpr std::uint32_t internal_entry_width() const {
        return sort_key_width() + static_cast<std::uint32_t>(sizeof(PageId));
    }
};

// ---- Page headers -------------------------------------------------------

inline constexpr std::size_t kIndexHeaderOffset = storage::kPageBodyOffset;  // 32

struct IndexLeafHeaderFields {
    std::uint16_t flags;
    std::uint16_t nr_entries;
    std::uint16_t key_width;
    std::uint16_t entry_width;
    // The next leaf in key order, or kInvalidPageId at the right edge. The
    // clustered tree's `next_page_id` under a different name and for the
    // identical purpose: a range walk crosses leaves without re-descending.
    PageId right_sibling;
    std::uint32_t reserved0;
};

inline constexpr std::size_t kIndexLeafFlagsOffset = 0;
inline constexpr std::size_t kIndexLeafNrEntriesOffset = 2;
inline constexpr std::size_t kIndexLeafKeyWidthOffset = 4;
inline constexpr std::size_t kIndexLeafEntryWidthOffset = 6;
inline constexpr std::size_t kIndexLeafRightSiblingOffset = 8;
inline constexpr std::size_t kIndexLeafReserved0Offset = 12;
// 2+2+2+2+4+4 = 16: every field naturally aligned, the total a multiple of
// 4, so there is no interior or tail padding and sizeof() is assertable.
inline constexpr std::size_t kIndexLeafHeaderSize = 16;

static_assert(offsetof(IndexLeafHeaderFields, flags) == kIndexLeafFlagsOffset);
static_assert(offsetof(IndexLeafHeaderFields, nr_entries) == kIndexLeafNrEntriesOffset);
static_assert(offsetof(IndexLeafHeaderFields, key_width) == kIndexLeafKeyWidthOffset);
static_assert(offsetof(IndexLeafHeaderFields, entry_width) == kIndexLeafEntryWidthOffset);
static_assert(offsetof(IndexLeafHeaderFields, right_sibling) == kIndexLeafRightSiblingOffset);
static_assert(offsetof(IndexLeafHeaderFields, reserved0) == kIndexLeafReserved0Offset);
static_assert(sizeof(IndexLeafHeaderFields) == kIndexLeafHeaderSize);

struct IndexInternalHeaderFields {
    std::uint16_t flags;
    std::uint16_t nr_entries;
    std::uint16_t key_width;
    // Distance to the leaf level; a leaf is level 0, so an internal node is
    // always >= 1. Stored rather than derived for btree_page.hpp's reason:
    // a descent has to know whether the next hop is another internal node,
    // and reading the child's header to find out costs the fetch this field
    // avoids being wrong about.
    std::uint16_t level;
    // The subtree holding sort keys below entries[0]. Kept out of the entry
    // array so that "entries[i]'s separator is the low key of child i" holds
    // with no exception, and so an empty-but-valid root has somewhere to
    // point.
    PageId leftmost_child;
    std::uint32_t reserved0;
};

inline constexpr std::size_t kIndexInternalFlagsOffset = 0;
inline constexpr std::size_t kIndexInternalNrEntriesOffset = 2;
inline constexpr std::size_t kIndexInternalKeyWidthOffset = 4;
inline constexpr std::size_t kIndexInternalLevelOffset = 6;
inline constexpr std::size_t kIndexInternalLeftmostChildOffset = 8;
inline constexpr std::size_t kIndexInternalReserved0Offset = 12;
inline constexpr std::size_t kIndexInternalHeaderSize = 16;  // same derivation as the leaf's

static_assert(offsetof(IndexInternalHeaderFields, flags) == kIndexInternalFlagsOffset);
static_assert(offsetof(IndexInternalHeaderFields, nr_entries) == kIndexInternalNrEntriesOffset);
static_assert(offsetof(IndexInternalHeaderFields, key_width) == kIndexInternalKeyWidthOffset);
static_assert(offsetof(IndexInternalHeaderFields, level) == kIndexInternalLevelOffset);
static_assert(offsetof(IndexInternalHeaderFields, leftmost_child) ==
              kIndexInternalLeftmostChildOffset);
static_assert(offsetof(IndexInternalHeaderFields, reserved0) == kIndexInternalReserved0Offset);
static_assert(sizeof(IndexInternalHeaderFields) == kIndexInternalHeaderSize);

inline constexpr std::uint16_t kIndexFlagInitialized = 0x1;

// Where each page's entry array starts: 32 + 16.
inline constexpr std::size_t kIndexLeafEntriesOffset =
    kIndexHeaderOffset + kIndexLeafHeaderSize;  // 48
inline constexpr std::size_t kIndexInternalEntriesOffset =
    kIndexHeaderOffset + kIndexInternalHeaderSize;  // 48

// Bytes available to either entry array.
inline constexpr std::size_t kIndexEntrySpace = kPageSize - kIndexLeafEntriesOffset;  // 8144
static_assert(kIndexLeafEntriesOffset == kIndexInternalEntriesOffset,
              "both arrays start at the same offset, so one constant sizes both");

// The widest entry a page can hold and still split.
//
// A split divides a full page's entries, so a page that cannot hold **two**
// has no division to make and the tree cannot make progress. That is the
// real bound on an index's declared width, and it is where a large COVERING
// clause is refused - by arithmetic, at declaration, rather than by an
// insert that fails much later.
inline constexpr std::uint32_t kMaxIndexEntryWidth = kIndexEntrySpace / 2;  // 4072

// Rejects a layout no tree could be built from, naming the offending width.
// Checked at CREATE INDEX and again whenever a page is opened.
Status CheckIndexLayout(const IndexLayout& layout);

// How many entries of `layout` fit in one page of each kind.
constexpr std::uint16_t MaxLeafEntries(const IndexLayout& layout) {
    return static_cast<std::uint16_t>(kIndexEntrySpace / layout.leaf_entry_width());
}
constexpr std::uint16_t MaxInternalEntries(const IndexLayout& layout) {
    return static_cast<std::uint16_t>(kIndexEntrySpace / layout.internal_entry_width());
}

// ---- Sort keys ----------------------------------------------------------

// Writes the `kIndexPkWidth`-byte big-endian primary key at the front of
// `out`. The one spelling of the pk's byte order, so no reader and writer
// can disagree about it.
void PutIndexPk(std::span<std::byte> out, std::uint64_t pk);
std::uint64_t GetIndexPk(std::span<const std::byte> in);

// Composes an entry's sort key - `key || pk` - into `out`, which must be
// exactly `layout.sort_key_width()` bytes.
Status EncodeIndexSortKey(const IndexLayout& layout, std::span<const std::byte> key,
                          std::uint64_t pk, std::span<std::byte> out);

// ---- Views --------------------------------------------------------------

class IndexLeafView {
public:
    // Wraps already-initialized leaf bytes. Does no validation, for the
    // reason heap::PageView's constructor does not (rules.md #1:
    // constructors must not fail). `CheckAgainst` is the validation.
    explicit IndexLeafView(std::span<std::byte, kPageSize> page) noexcept : page_(page) {}

    // Formats a brand-new empty leaf with no right sibling. `owner_oid`
    // (page.md §2a) is the owning *index*'s oid — the immediate-owner rule.
    static StatusOr<IndexLeafView> CreateEmpty(std::span<std::byte, kPageSize> page,
                                                const IndexLayout& layout,
                                                std::uint64_t owner_oid);

    std::uint16_t entry_count() const;
    std::uint16_t key_width() const;
    std::uint16_t entry_width() const;
    PageId right_sibling() const;
    void set_right_sibling(PageId page_id);

    // The whole entry, and its `key || pk` prefix. Both fail with OutOfRange
    // past `entry_count()`.
    StatusOr<std::span<const std::byte>> Entry(std::uint16_t idx) const;
    StatusOr<std::span<const std::byte>> SortKey(std::uint16_t idx) const;

    bool IsFull() const;

    // Index of the first entry whose sort key is >= `sort_key`, or
    // `entry_count()` if there is none. `sort_key` is a full
    // `key_width + kIndexPkWidth` bytes; a caller holding less pads with
    // zeros, which is the lower bound - see the header.
    std::uint16_t LowerBound(std::span<const std::byte> sort_key) const;

    // Inserts `entry` at its sorted position. Fails with OutOfSpace when
    // the leaf is full and InvalidArgument on a width mismatch.
    StatusOr<std::uint16_t> InsertEntry(std::span<const std::byte> entry);

    // Moves the upper half of this leaf's entries into `into`, which must be
    // freshly created with the same widths and empty. The sibling links are
    // **not** touched: the caller publishes them after the entry is in, so a
    // walk never reaches a half-built leaf.
    Status SplitInto(IndexLeafView& into);

    // Rejects a page whose stored widths disagree with `layout`, or that was
    // never initialized. The checked half of the redundancy above.
    Status CheckAgainst(const IndexLayout& layout, PageId page_id) const;

private:
    IndexLeafHeaderFields ReadHeader() const;
    void WriteHeader(const IndexLeafHeaderFields& header);
    std::size_t EntryOffset(std::uint16_t idx) const;

    std::span<std::byte, kPageSize> page_;
};

class IndexInternalView {
public:
    explicit IndexInternalView(std::span<std::byte, kPageSize> page) noexcept : page_(page) {}

    // Formats a brand-new internal node holding one child and no
    // separators - the shape a root split produces before its sibling is
    // added. Fails with InvalidArgument if `level` is 0 (that is a leaf).
    static StatusOr<IndexInternalView> CreateEmpty(std::span<std::byte, kPageSize> page,
                                                    const IndexLayout& layout,
                                                    std::uint16_t level, PageId leftmost_child,
                                                    std::uint64_t owner_oid);

    std::uint16_t entry_count() const;
    std::uint16_t key_width() const;
    std::uint16_t level() const;
    PageId leftmost_child() const;
    void set_leftmost_child(PageId page_id);

    StatusOr<std::span<const std::byte>> Separator(std::uint16_t idx) const;
    StatusOr<PageId> Child(std::uint16_t idx) const;

    bool IsFull(const IndexLayout& layout) const;

    // The child a descent for `sort_key` must follow: the child of the last
    // entry whose separator is <= `sort_key`, or `leftmost_child` if there
    // is none. Never fails - a node always has a leftmost child, and one
    // with no entries routes everything there.
    PageId ChildFor(std::span<const std::byte> sort_key) const;

    // Inserts (separator -> child) in sorted position. Fails with
    // OutOfSpace when full and AlreadyExists if the separator is already
    // present, since two subtrees sharing a low key would make the descent's
    // choice between them arbitrary and one of them unreachable.
    Status InsertEntry(const IndexLayout& layout, std::span<const std::byte> separator,
                       PageId child);

    // Splits this node, moving its upper entries into `into` and writing the
    // separator that must be **pushed up** into `sep_out`.
    //
    // Pushed up, not copied up: the median separator is already the low key
    // of the subtree that becomes `into`'s leftmost child, so it leaves this
    // node entirely. A leaf split copies its boundary up instead, because a
    // leaf's first entry has to stay in the leaf. The two are different
    // operations and this is the one place that difference lives.
    Status SplitInto(IndexInternalView& into, std::span<std::byte> sep_out);

    Status CheckAgainst(const IndexLayout& layout, PageId page_id) const;

private:
    IndexInternalHeaderFields ReadHeader() const;
    void WriteHeader(const IndexInternalHeaderFields& header);
    std::size_t EntryOffset(std::uint16_t idx) const;
    std::uint32_t entry_stride() const;

    // Index of the last entry with separator <= `sort_key`, or -1 for
    // "none, follow leftmost_child".
    int FindSlot(std::span<const std::byte> sort_key) const;

    std::span<std::byte, kPageSize> page_;
};

}  // namespace kds::index
