#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/varheap.hpp"

// Encodes/decodes one heap tuple payload to/from a row of parsed SQL
// values (parser::AstValue), positionally aligned with a
// catalog::Schema's columns (which BuildSchemaFromColumns() now returns
// sorted by `pos` - see catalog.cpp). This is the missing piece the
// project's not-yet-ported type registry was blocking (see ast.hpp's and
// command_dispatcher.hpp's file comments): rather than wait on that
// subsystem, this resolves against whatever Catalog::ResolveTypeByName()
// can already look up in sys.types (well_known.hpp's kTypeVal* tags).
//
// ---- Every tuple is fixed-length (invariant 13) --------------------------
//
// A relation's row size is a schema constant, held in the
// catalog::RowLayout every function below takes alongside the schema
// (schema.hpp). Nothing here appends: a payload is a buffer of exactly
// `layout.row_size` bytes, zero-filled, into which each column is written
// at its static `layout.offsets[i]`. Decode reads back from the same
// offsets.
//
// This is what makes an UPDATE unable to migrate a tuple: a new version is
// the same size as the old one whatever its values, so
// PageView::OverwriteTuple() always fits and the row keeps its
// (page_id, slot) for life. Combined with the immutable min_key that is a
// stable tuple address, which is what Waystone trails are recorded against.
//
// A payload whose length disagrees with the constant is **Corruption,
// never interpreted**: the slot's `length` and the tuple header's
// `data_len` carry no information the schema does not already give, so
// they are checked redundancy (heap-and-tuple.md section 3.2).
//
// Scope, deliberately narrow:
//   - No NULLs. AstValue{type=kNull} is rejected by EncodeRow(); every
//     column must have a value on INSERT. The tagged cell format already
//     carries a kNull tag (storage/tagged_cell.hpp) and DecodeRow()
//     reports it, so the day NULLs land the *format* does not change -
//     what changes is CompareValues() and the NOT IN tri-state collapse,
//     which is a different piece of work.
//   - kTypeValFloat/kTypeValDecimal columns can no longer be declared at
//     all: catalog::RowLayout::Build() refuses them, because a fixed row
//     size has to reserve a width for every column and reserving one for
//     an undecided encoding is half of deciding it (see schema.hpp).
//   - Fixed-width columns (int8/16/32/64, uint64, bool, char) occupy their
//     natural widths, each field written byte-by-byte (rules.md #5: no
//     bitfields, no reinterpret_cast of the buffer). `varchar` occupies
//     one **tagged cell** of kds.inline_cell_width bytes whatever it
//     holds; a value too long to fit inline spills to the var-heap and the
//     cell carries a pointer instead. See the spilling note below.
//   - uint64 columns round-trip through AstValue::raw_int_text (the
//     literal's original digit text, preserved by the lexer/parser
//     specifically for this - see ast.hpp) rather than int_val, since
//     int_val is a signed int64_t and cannot represent the upper half of
//     the unsigned 64-bit range. DecodeRow() sets raw_int_text on the
//     values it produces for the same reason - callers formatting a
//     decoded uint64 column should read raw_int_text, not int_val.

namespace kds::exec {

// ---- Spilling: where the var-heap meets the codec ------------------------
//
// A value too long for its tagged cell lives in the relation's var-heap
// (storage/varheap.hpp) and the cell holds a pointer. That splits this
// file's two directions, and they are *not* symmetric:
//
// **Encode spills eagerly.** EncodeRow() takes a VarHeapSink; when a value
// does not fit inline it is appended to the chain and the cell is written
// as kSpilled. Without a sink, an over-long value is refused - which is
// what a caller with no relation context (a test, a hand-built schema)
// should get rather than a silent truncation.
//
// **Decode does not resolve.** DecodeRowInto() reports a spilled cell as a
// *pending* spill and leaves the value empty; the caller fetches afterwards
// through ResolveSpills(). That is not fastidiousness - it is
// docs/parser-v2.md I15's rule R1, "decode before descending": no
// page-frame span may be live across a nested page fetch, and decoding runs
// with a span into the heap page. Resolving inline would fetch a var-heap
// page while that span is live, which is exactly the shape the step VM's
// PageSpanGuard exists to catch. A row with nothing spilled costs nothing
// for the split, since the pending list stays empty.

// A var-heap value a decode found but deliberately did not fetch.
struct PendingSpill {
    std::size_t column = 0;  // index into the decode target
    varheap::VarHeapPtr ptr;
    std::uint32_t len = 0;
};

// A value EncodeRow() actually appended to the var-heap, reported back so
// the caller can log it. The bytes are copied rather than referenced: the
// caller writes the WAL record after encoding returns, by which time the
// AstValue they came from may be gone.
struct AppendedSpill {
    varheap::VarHeapPtr ptr;
    std::vector<std::byte> value;
};

// Where EncodeRow() puts a value that does not fit inline: one relation's
// var-heap chain. `root` is TableAccess::varheap_page_id.
//
// `appended`, when given, collects one entry per value that spilled. It is
// how the WAL ordering of spec section 5 is achievable from here: the
// caller logs a VARHEAP_APPEND for each *before* the HEAP_INSERT or
// HEAP_OVERWRITE whose cell points at it, in the same transaction. Without
// that ordering a replay could reach a tuple whose pointer resolves to
// nothing.
struct VarHeapSink {
    storage::PageStore* store = nullptr;
    PageId root = kInvalidPageId;
    std::vector<AppendedSpill>* appended = nullptr;

    bool usable() const noexcept { return store != nullptr && root != kInvalidPageId; }
};

// Fetches every pending spill and writes it into `out`, which must be the
// same decode target the spills came from. Call it *after* releasing any
// span into the tuple's page - that ordering is the whole reason the
// pending list exists.
Status ResolveSpills(storage::PageStore& store, const std::vector<PendingSpill>& spills,
                     std::span<parser::AstValue> out);

// Rejects a schema that cannot carry a Keystone word: no columns at all,
// or a first column whose declared type is not an integer one. Exposed so
// CREATE TABLE can refuse such a table at definition time rather than at
// the first INSERT.
Status CheckKeystoneColumn(const catalog::Schema& schema);

// Encodes one row's values into a tuple payload of exactly
// `layout.row_size` bytes.
//
// The payload is `[Keystone word][columns 1..n-1]`: the first schema
// column IS the primary key and is carried by the Keystone word's id
// field, so `values` supplies only the columns after it. `id` is the
// system-generated key (catalog::Catalog::AllocateRowId) - the pk is
// never encoded into the body as well, since two copies of one value is
// how the two come to disagree.
//
// `layout` must have been built for `schema` (catalog::RowLayout::Build);
// a mismatched pair is InvalidArgument rather than a silently wrong row.
//
// Fails with InvalidArgument if the schema has no usable Keystone column,
// values.size() != schema.columns.size() - 1, `id` exceeds the Keystone
// word's 40-bit range, any value is NULL, any int value doesn't fit its
// column's width, or a string value doesn't fit a fixed-width `char`
// column; and with Unsupported for a varchar value too long to store
// inline (see the file comment).
StatusOr<std::vector<std::byte>> EncodeRow(const catalog::Schema& schema,
                                            const catalog::RowLayout& layout, std::uint64_t id,
                                            const std::vector<parser::AstValue>& values,
                                            const VarHeapSink& varheap = {});

// Reads just the primary key out of a tuple payload, without decoding the
// body. This is what a duplicate-key scan compares, and it is why the pk
// lives in a fixed-offset word: finding it costs no schema walk.
StatusOr<std::uint64_t> RowKeystoneId(std::span<const std::byte> payload);

// Decodes a tuple payload back into one value per `schema.columns` entry,
// pk first, in the same order EncodeRow() wrote them. Fails with
// Corruption if `payload.size()` is not exactly `layout.row_size` - see
// the file comment on checked redundancy.
StatusOr<std::vector<parser::AstValue>> DecodeRow(const catalog::Schema& schema,
                                                   const catalog::RowLayout& layout,
                                                   std::span<const std::byte> payload,
                                                   std::vector<PendingSpill>* spills = nullptr);

// The same decode, writing into storage the caller already owns (V16).
//
// `out` must have exactly one slot per schema column; the values are
// written in place. This is what makes a chain scan allocate O(1) rather
// than O(rows): a chain frame sizes its buffer once when the statement
// opens and every row decodes into the same slots, where DecodeRow()
// returns a fresh vector - one allocation per row per step, on the hot
// path of every join.
//
// DecodeRow() above is now a thin wrapper over this, so there is one
// decoder and no possibility of the two drifting.
// `spills`, when given, is cleared and filled with the spilled cells this
// row carries; the caller resolves them once its page span is released.
// When null, a spilled cell is refused with Unsupported - a caller that
// cannot resolve one must not silently return an empty value for it.
Status DecodeRowInto(const catalog::Schema& schema, const catalog::RowLayout& layout,
                     std::span<const std::byte> payload, std::span<parser::AstValue> out,
                     std::vector<PendingSpill>* spills = nullptr);

// Renders one decoded value for display in a SELECT response line.
// Prefers AstValue::raw_int_text when set (exact literal/uint64 text),
// falling back to int_val otherwise.
std::string FormatValue(const parser::AstValue& value);

// Compares two decoded values under one operator, with `type_val` saying
// how to read them - uint64 columns compare through their digit text,
// since int_val cannot represent the upper half of the range.
//
// A type mismatch is a non-match rather than an error, which is what SQL's
// NULL-comparison semantics would give here in the absence of real NULLs.
//
// This is the comparison the *whole engine* uses. It used to be reachable
// only through MatchesWhere(), which resolved columns by name against one
// schema; V16 deleted that evaluator (see chain_frame.hpp for why) and
// kept the comparison, which was never the problem.
bool CompareValues(std::uint32_t type_val, const parser::AstValue& lhs,
                   const parser::AstValue& rhs, parser::CompareOp op);

}  // namespace kds::exec
