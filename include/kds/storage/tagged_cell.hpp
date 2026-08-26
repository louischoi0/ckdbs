#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"

// The tagged cell: the fixed-width slot every variable-width value
// occupies inside a tuple (docs/rules/rule-fixed-length-tuple.md section 3,
// docs/spec/heap-and-tuple.md section 3.3).
//
// ---- Why a fixed cell at all --------------------------------------------
//
// Invariant 13: every tuple is fixed-length. A relation's row size is a
// schema constant, so a `TEXT`/`varchar` value cannot be allowed to decide
// how many bytes it occupies - it gets exactly `W = kds.inline_cell_width`
// bytes whatever it holds. The point is tuple *mobility*: with fixed cells
// an UPDATE can never migrate a tuple, so combined with the immutable
// min_key a tuple's address is stable for life until relayout moves it on
// purpose. That is what stops an UPDATE from burning Waystone trail
// entries through epoch churn.
//
// ---- Why a tag byte rather than sentinels -------------------------------
//
// NULL, the empty string, and a spilled value must be distinguishable
// *without reading the var-heap* - a length of 0 cannot say which of the
// first two it is, and no in-cell sentinel can say the third without
// stealing a length value. The tag is also where future cell kinds land
// without a format bump (prefix-inlining, section 9's V4 revisit).
//
//   | tag (u8 @0) | after the tag                        | meaning       |
//   |-------------|--------------------------------------|---------------|
//   | kNull       | zeros                                | SQL NULL      |
//   | kInline     | len u16, len bytes, zero padding     | len <= W - 3  |
//   | kSpilled    | len u32, varheap_ptr u64             | in the var-heap|
//
// ---- What this layer does and does not decide ---------------------------
//
// A kSpilled cell carries a *pointer*, and this file neither writes the
// value it points at nor resolves it: that is storage/varheap.hpp, which
// needs a PageStore where this needs only the cell's bytes. Keeping the two
// apart is what lets the cell codec stay a pure function.
//
// kNull is carried by the format and reported by DecodeCell(), but the row
// codec still refuses to *encode* a NULL (row_codec.hpp). NULL semantics
// reach into CompareValues() and the NOT IN tri-state collapse, which is a
// different change.
//
// Encoding rules per rules.md sections 2 and 5: fixed-width little-endian,
// explicit byte-at-a-time codec, no bitfields, no reinterpret_cast onto the
// cell bytes.
//
// Concurrency: pure functions over a caller-owned span. The caller holds
// whatever pin/latch the underlying page needs; this file takes none.

namespace kds::storage {

// ---- The instance-pinned cell width -------------------------------------
//
// `kds.inline_cell_width` is configuration-referenced but instance-pinned:
// read once at bootstrap, written into the superblock, validated at every
// startup (spec section 4). On-disk tuple layout depends on it, so it can
// never be hot-changed and changing it for existing data is Unsupported.
//
// The default is **[PROPOSED], not settled** - CLAUDE.md carries it as an
// open decision, to be measured against the string-length distributions of
// target schemas. Nothing may depend on the number: it is a configured
// value threaded through catalog::RowLayout, never a constant compiled
// into a layout computation.
inline constexpr std::uint32_t kDefaultInlineCellWidth = 64;

// Floor: a kSpilled cell is 1 + 4 + 8 = 13 bytes, and a width that cannot
// hold the widest tag's payload would make spilling impossible to
// represent. Rounded up to the next multiple of 8 so a cell never
// straddles an 8-byte boundary it did not have to.
inline constexpr std::uint32_t kMinInlineCellWidth = 16;

// Ceiling: an arbitrary but stated bound, well under the ~8 KiB a heap
// page can hold, so a single cell can never be the reason a row does not
// fit a page. RowLayout::Build() checks the actual row against the actual
// page; this only keeps a typo in a config file from being interesting.
inline constexpr std::uint32_t kMaxInlineCellWidth = 4096;

// Rejects a width outside [kMin, kMax], naming the offending value and the
// bounds. Shared by the config parser and the superblock validator so the
// two can never disagree about what a legal width is.
Status CheckInlineCellWidth(std::uint32_t width);

// ---- Cell layout ---------------------------------------------------------

enum class CellTag : std::uint8_t {
    kNull = 0,
    kInline = 1,
    kSpilled = 2,
};

inline constexpr std::uint8_t kMaxAssignedCellTag = 2;

// Offsets within one cell, from its first byte.
inline constexpr std::size_t kCellTagOffset = 0;
inline constexpr std::size_t kCellInlineLenOffset = 1;   // u16
inline constexpr std::size_t kCellInlineBytesOffset = 3;
inline constexpr std::size_t kCellSpilledLenOffset = 1;  // u32
inline constexpr std::size_t kCellSpilledPtrOffset = 5;  // u64
inline constexpr std::size_t kCellSpilledSize = 13;      // 1 + 4 + 8

static_assert(kCellSpilledSize <= kMinInlineCellWidth,
              "the narrowest legal cell must still be able to hold a spilled descriptor");

// How many value bytes fit inline in a `width`-byte cell: everything after
// the tag and the u16 length. The spill decision is a pure function of
// value length - no heuristics, no per-row variance.
constexpr std::uint32_t InlineCapacity(std::uint32_t width) noexcept {
    return width - static_cast<std::uint32_t>(kCellInlineBytesOffset);
}

static_assert(InlineCapacity(kDefaultInlineCellWidth) == 61);

// ---- Codec ---------------------------------------------------------------

// Writes `value` into `cell` as a kInline cell. `cell.size()` is the
// relation's pinned width and must be in range; anything shorter than
// kMinInlineCellWidth is InvalidArgument.
//
// **The whole cell is zeroed first.** A cell is overwritten in place by an
// UPDATE, so without this the tail of a longer previous value would survive
// underneath a shorter new one - bytes a reader never sees but that leak
// into a page image, an undo record, and a checksum.
//
// Fails with Unsupported if value.size() > InlineCapacity(cell.size()):
// that value belongs in the var-heap, which does not exist yet. The error
// names both the length and the limit.
Status EncodeInlineCell(std::span<std::byte> cell, std::string_view value);

// Writes a kNull cell: the tag, then zeros. Nothing calls this yet (the row
// codec still refuses NULL); it exists so the format's three tags all have
// exactly one writer when NULLs land.
Status EncodeNullCell(std::span<std::byte> cell);

// Writes a kSpilled cell: the tag, the value's full length, and the
// var-heap pointer that resolves it (storage/varheap.hpp encodes the word).
//
// `len` is the length of the value in the var-heap, kept in the cell so a
// reader knows the size without fetching - which is what lets a length-only
// question be answered for a spilled value at the same cost as an inline
// one. Zero-fills the cell first, for the same reason EncodeInlineCell()
// does.
Status EncodeSpilledCell(std::span<std::byte> cell, std::uint32_t len,
                         std::uint64_t varheap_ptr);

// A decoded cell.
//
// For kInline, `bytes` is a view into the cell itself - valid only as long
// as the underlying page bytes stay alive and untouched. Bytes rather than
// a string_view, deliberately: this layer knows a cell holds `len` opaque
// bytes and nothing about what they spell. Turning them into a value's text
// is the row codec's business, and keeping the span typed as std::byte
// means no cast happens here at all.
//
// For kSpilled, `bytes` is empty and `len`/`varheap_ptr` say where the
// value actually lives. Resolving it is varheap::Fetch()'s job.
//
// For kNull, everything is zero.
struct CellValue {
    CellTag tag = CellTag::kNull;
    std::span<const std::byte> bytes;
    std::uint32_t len = 0;
    std::uint64_t varheap_ptr = 0;
};

// Reads the cell at the front of `cell`.
//
// Fails with Corruption for a tag byte beyond kMaxAssignedCellTag or a
// kInline length past the cell's capacity - both mean the bytes are not a
// cell this build wrote, and a length read out of a corrupt page must never
// be used to size a read.
//
// A kSpilled cell decodes successfully here and is *not* resolved: the
// caller reads `varheap_ptr` and fetches. A cell codec that reached for a
// page store would be a cell codec no test could call.
StatusOr<CellValue> DecodeCell(std::span<const std::byte> cell);

}  // namespace kds::storage
