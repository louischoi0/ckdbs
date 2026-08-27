#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "kds/base/int128.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/parser/ast.hpp"

// The KWP/1 row encoding (docs/spec/protocol.md D5 and §6): how a result row
// becomes bytes.
//
// ---- One encoder, two consumers -----------------------------------------
//
// This is deliberately **not** part of the wire path, and it is not part of
// the cross-core path either - it is below both. `docs/spec/crosscore.md` CC2
// requires a `STEP_BATCH` payload to be "rows in the KWP binary encoding -
// the same encoder the wire path uses... one encoder, two consumers; no
// second row format", and this file is what makes that literal rather than
// aspirational. It exists before either consumer does, which is the only
// way that rule survives contact with whichever one is built first.
//
// So: nothing here knows about frames, sockets, cores or rings. It turns a
// schema into a row description and decoded values into bytes, and back.
//
// ---- The format ----------------------------------------------------------
//
// Row description (`S_ROW_DESC`'s payload):
//
//     field_count  u16
//     fields[]     name_len u16, name bytes, type_oid u32,
//                  type_len i16 (-1 = variable), flags u16
//
// Row batch (`S_ROW_BATCH`'s payload):
//
//     row_count    u16
//     rows[]       per field: len i32 (-1 = NULL), then `len` bytes
//
// All integers little-endian (D5, "binary, little-endian end to end").
// The `{i32 len | -1 = NULL}` shape is §6's "one NULL convention
// everywhere" - the same encoding carries bound parameters, so a NULL going
// in and a NULL coming back are spelled identically.
//
// **A length prefix on fixed-width fields is redundant and is kept anyway.**
// Two reasons, both about who reads this: a consumer can skip a field it
// does not understand without a type table, and NULL needs a representation
// that is not "a value of the right width". Paying 4 bytes per field to
// make the stream self-describing is what lets the cross-core consumer and
// the wire consumer share a decoder.
//
// ---- Types ---------------------------------------------------------------
//
// Covers exactly what the engine can store (`src/exec/row_codec.cpp`): the
// signed ints, `uint64`, `bool`, `char`, `varchar`, and - since the types
// work (`docs/spec/types.md`) - `date`, `timestamp` and `decimal(p, s)`.
// `float` is refused at CREATE TABLE, so no column can hold one and
// nothing here encodes one.
//
// **DECIMAL on the wire** (the decision protocol.md §6 held `[OPEN]`,
// settled 2026-08-07): the value is the **unscaled int64, LE, 8 bytes** -
// the exact integer storage holds, in TIMESTAMP's shape - and the scale
// travels **in the row description, once**, never per value. A per-value
// scale would be 2 bytes of redundancy per field that can only ever agree
// with the column or be a defect; the description states the fact once,
// which is the same reasoning that put `(p, s)` in one catalog word
// (TY02). `FieldDescription::type_mod` carries that word - literally the
// catalog's packed `len`, read through `catalog::DecimalPrecisionOf` /
// `DecimalScaleOf`, so there is one packing with two readers and no
// second scheme. A `p > 18` decimal is a *future separate type* (TY2)
// with its own type_val and its own 16-byte width; nothing here forecloses
// it, which is what "scaled-int, width follows the type" buys over the
// rejected string encoding - text would cost a per-value parse on an
// all-binary protocol and reintroduce the two-readings drift the type
// system just removed.

namespace kds::wire {

// One field of a row description. `name` is copied, because a description
// outlives the catalog entry it was built from in every intended use.
struct FieldDescription {
    std::string name;
    std::uint32_t type_oid = 0;
    // Bytes on the wire for a fixed-width type, or -1 for a variable one.
    // Not the *storage* width: a varchar occupies `inline_cell_width` bytes
    // in a tuple and a variable number here, and conflating the two is how
    // a client ends up padding.
    std::int16_t type_len = -1;
    std::uint16_t flags = 0;
    // Type modifier. Zero for every type except `decimal`, where it is the
    // catalog's packed `(p, s)` word (`SysColumnRow::len`), read with
    // `catalog::DecimalPrecisionOf` / `DecimalScaleOf`. The scale a
    // decimal field's unscaled int64 means nothing without lives here,
    // once per result set - see the DECIMAL note in the file header.
    std::uint32_t type_mod = 0;
};

// Field 0 of every user relation is the Keystone id (protocol.md §6), which
// is a u64 and never NULL. Named so no call site re-derives it.
inline constexpr std::uint16_t kFieldFlagKeystone = 0x1;

// Wire bytes for a fixed-width column type, or -1 when it is variable.
// Answers -1 for a type this build does not know, which is the safe
// reading: a consumer then relies on the per-value length, which is always
// present.
std::int16_t WireTypeLen(std::uint32_t type_val) noexcept;

// Builds a row description from a relation's schema.
std::vector<FieldDescription> DescribeSchema(const catalog::Schema& schema);

// ---- Encoding ------------------------------------------------------------

// Appends the row description to `out`.
void EncodeRowDescription(const std::vector<FieldDescription>& fields,
                          std::vector<std::byte>& out);

// Appends one value in its `{len i32, bytes}` form.
//
// The column decides how the value is read, not what it is: a `uint64`
// column encodes from `AstValue::raw_int_text` because `int_val` cannot
// represent the upper half of the range - the same reason
// `exec::CompareValues` compares that column through its digit text - and
// a `decimal` column is why this takes the column row rather than a bare
// type_val: the value's scale is checked against the column's, because an
// unscaled integer written under the wrong scale is not a malformed byte,
// it is a different number.
//
// Fails with InvalidArgument if the value's kind cannot satisfy the column
// (an integer for a text column, a decimal whose scale disagrees), and
// with Unsupported for a `kParam`, which is a declaration's placeholder
// and never a result value.
Status EncodeValue(const catalog::SysColumnRow& col, const parser::AstValue& value,
                   std::vector<std::byte>& out);

// Accumulates rows into one batch payload.
//
// Row count is a u16 at the head, so the writer has to know how many rows
// there were before it can finish - which is why this is a class and not a
// free function. `Finish()` patches the count in and hands the buffer over.
class RowBatchWriter {
public:
    explicit RowBatchWriter(std::size_t reserve_bytes = 0);

    // Appends one row. `values` must be positionally aligned with the
    // schema the batch was described from - the encoding carries no field
    // names, exactly as the engine's own row codec carries none.
    Status AppendRow(const catalog::Schema& schema, std::span<const parser::AstValue> values);

    // Rows appended so far, and the payload's current size. Both are what a
    // caller batching to a size target reads between rows.
    std::uint16_t row_count() const noexcept { return row_count_; }
    std::size_t size_bytes() const noexcept { return buffer_.size(); }

    // Whether another row would exceed a u16 row count. A batch is capped
    // by the format, not only by a size target, and a caller that ignores
    // this gets a wrapped count rather than an error.
    bool full() const noexcept;

    // Writes the row count into the reserved header and returns the
    // payload. The writer is left empty and reusable, which is what keeps a
    // streaming caller from allocating a buffer per batch.
    std::vector<std::byte> Finish();

private:
    std::vector<std::byte> buffer_;
    std::uint16_t row_count_ = 0;
};

// ---- Decoding ------------------------------------------------------------
//
// Present in the same file as the encoder, and tested against it, because
// the format's only real guarantee is that the two agree.

StatusOr<std::vector<FieldDescription>> DecodeRowDescription(std::span<const std::byte> payload);

// One decoded field: a view into the payload, plus whether it was NULL.
// A view rather than a copy because the batch outlives the decode in every
// intended use, and copying every field is the cost this whole encoding is
// trying to avoid.
//
// **The payload must outlive the decode**, and nothing in the type system
// says so - `DecodeRowBatch(writer.Finish(), n)` compiles and hands back
// views into a destroyed temporary. The rvalue overload below is deleted to
// make that particular spelling a compile error rather than a garbage row;
// it cannot catch every way of getting this wrong, only the easy one.
struct DecodedField {
    std::span<const std::byte> bytes;
    bool is_null = false;
};

// Decodes a batch into `field_count`-sized rows. Fails with Corruption on a
// truncated payload, a length that runs past the end, or a row count the
// payload cannot supply - never a partial answer, because a caller cannot
// tell a short batch from a corrupt one.
StatusOr<std::vector<std::vector<DecodedField>>> DecodeRowBatch(
    std::span<const std::byte> payload, std::size_t field_count);

// Decoding an owning buffer that is about to die yields views into freed
// memory. Deleted so `DecodeRowBatch(writer.Finish(), n)` fails to compile.
StatusOr<std::vector<std::vector<DecodedField>>> DecodeRowBatch(std::vector<std::byte>&&,
                                                                std::size_t) = delete;

// Reads a fixed-width signed integer field back. The counterpart of
// EncodeValue for the integer types, so a test can round-trip without
// re-implementing the layout.
StatusOr<std::int64_t> DecodeInt(std::span<const std::byte> bytes);
StatusOr<std::uint64_t> DecodeUint64(std::span<const std::byte> bytes);
std::string_view DecodeText(std::span<const std::byte> bytes);

// The 16-byte wide-decimal field: two LE halves, low first. The scale is
// the row description's `type_mod`, not the field's, exactly as for the
// 8-byte decimal.
StatusOr<Int128> DecodeDecimalWide(std::span<const std::byte> bytes);

// A KWP-decoded field back into a value, by the schema column's type: the
// inverse of RowBatchWriter's per-type encoding, read back through the
// same width and scale facts the encoder used. Two consumers - the
// session rendering a remote reply (workplan P4c) and a consuming
// pipeline stage filling its upstream frame slot (P4d-4b) - and one
// inverse, for the reason this file holds one encoder.
parser::AstValue FieldToValue(const catalog::SysColumnRow& col, const DecodedField& field);

// FieldToValue behind invariant 13's rule: a non-NULL fixed-width field
// whose byte length disagrees with the column's wire width is Corruption,
// never interpreted - the bare form zero-extends a short field, which
// reads a truncated stream as a smaller number. Every cross-core edge
// decode goes through this (workplan P4d-4b-3): the consuming stage's
// input fill and both of the session's reply shapes.
StatusOr<parser::AstValue> FieldToValueChecked(const catalog::SysColumnRow& col,
                                               const DecodedField& field);

}  // namespace kds::wire
