#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kds/base/int128.hpp"
#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/parser/ast.hpp"
#include "kds/storage/page_store.hpp"
#include "kds/storage/varheap.hpp"

// Encodes/decodes one tuple payload to/from a row of parsed SQL values
// (parser::AstValue), positionally aligned with a catalog::Schema's columns
// (sorted by `pos`), typed by sys.types' kTypeVal* tags (well_known.hpp).
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
// Scope:
//   - NULL lives in the row's tail bitmap, the sole authority
//     (docs/spec/null.md §3): EncodeRow sets the bit for a nullable column
//     and refuses NULL into a NOT NULL one; a NULL tagged cell is filler.
//   - kTypeValFloat cannot be declared: catalog::RowLayout::Build() refuses
//     it (types.md TY1). DATE, TIMESTAMP and DECIMAL/DECIMAL128 are fixed
//     width (4, 8, 8/16 bytes - row_layout.cpp).
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
// docs/spec/parser-v2.md I15's rule R1, "decode before descending": no
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

    // What the append had to change structurally, carried straight through
    // from `varheap::ChainAppendResult` - the page it created and the tail it
    // linked, both kInvalidPageId when the tail had room. The caller logs
    // these *before* this spill's own VARHEAP_APPEND: a PAGE_INIT for the
    // created page and a full page image for the linked one. Without them a
    // replay meets an append naming a page nothing creates, or a value page
    // no chain walk reaches.
    PageId created_page_id = kInvalidPageId;
    PageId linked_page_id = kInvalidPageId;

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
    // The owning relation's oid, stamped into any page the spill creates
    // (page.md §2a). A chain is per-relation, so the caller that knows
    // `root` knows this too; 0 only where no relation exists to name.
    std::uint64_t owner_oid = 0;
    // **Logs each spill while its page is still held** (AT-S21, its
    // review's C1). Called right after the append, under the holds
    // `varheap::ChainAppend` hands back and before they drop: the caller
    // writes the spill's undo record and then its `VARHEAP_APPEND` there -
    // RV3's order - so the page is never held past this call. When set, the
    // spill is **not** pushed to `appended`: it is fully noted and logged.
    //
    // Why not hold the page until the row is logged: an insert would then
    // hold the var-heap tail and ask for the clustered leaf, and a reader
    // holds the leaf and asks for the var-heap page to resolve the spill -
    // two cores deadlocked on page latches that never time out.
    const std::function<Status(const AppendedSpill&)>* on_append = nullptr;

    bool usable() const noexcept { return store != nullptr && root != kInvalidPageId; }
};

// Fetches every pending spill and writes it into `out`, which must be the
// same decode target the spills came from. Call it *after* releasing any
// span into the tuple's page - that ordering is the whole reason the
// pending list exists.
Status ResolveSpills(storage::PageStore& store, const std::vector<PendingSpill>& spills,
                     std::span<parser::AstValue> out);

// "Decode everything", spelled where the decoders live rather than where
// the compiler does: `Step::kAllColumns` is the same value, and a
// static_assert in step_vm.cpp - the one translation unit that holds
// both headers - pins the two together so neither can move alone. row_codec does not depend on step_chain, and should not -
// a decoder needs no opinion about chains.
inline constexpr std::uint64_t kAllColumnsMask = ~std::uint64_t{0};

// **A slot a partial decode deliberately left out** (BN-R2,
// `workorder-bn-opt-read-update-path.md`): an UPDATE with
// no hook decodes only the columns its SET expressions read and fills every
// other slot with this, so an expression the mask missed is refused
// `Corruption` by the SET evaluator rather than handed a default value. A
// hook (Cabin, index, assertion) never sees one, because a relation with any
// of them decodes whole, and the encoder never does since BN-R3 encodes only
// the assigned values. A `kParam`, because no other one reaches a decoded
// row since declared patterns were withdrawn.
inline const parser::AstValue kUndecoded = [] {
    parser::AstValue v;
    v.type = parser::ValueType::kParam;
    v.str_val = "undecoded";
    return v;
}();

// Decode `columns` of one row into `out`, then resolve whatever spilled.
//
// **One rule, one place**: kAllColumns takes the whole-row decoder, any
// other mask the masked one, and the pending spills are fetched once
// nothing spans the page. A caller spelling this itself and drifting hands
// back a slot still holding the **previous row's value**.
//
// `scratch` is the caller's pending-spill buffer, hoisted so a row loop
// allocates once rather than per row; the decoders clear it themselves. It
// is left filled on return, which is what lets a caller count what it
// fetched (the step VM's `spill_fetches`).
// **Which of the decode's front checks a call runs** (BM-R6,
// `workorder-bm-scan-row-kernel.md`). Three of the four depend only on the
// schema, the layout and the target span, which a walk binds once; the
// fourth, the payload's length, depends on the row (invariant 13).
// `kPayloadOnly` is for a caller that has already passed
// `CheckDecodeShape` for this schema, layout and span - the step VM, once
// per step per statement - and keeps the per-row check.
enum class DecodeChecks : std::uint8_t { kAll, kPayloadOnly };

// The schema-and-layout half of the decode's checks, with the messages the
// decoders give: the Keystone column, the layout against the schema, the
// target span's size.
Status CheckDecodeShape(const catalog::Schema& schema, const catalog::RowLayout& layout,
                        std::span<const parser::AstValue> out);

Status DecodeAndResolve(storage::PageStore& store, const catalog::Schema& schema,
                        const catalog::RowLayout& layout, std::span<const std::byte> payload,
                        std::span<parser::AstValue> out, std::uint64_t columns,
                        std::vector<PendingSpill>& scratch,
                        DecodeChecks checks = DecodeChecks::kAll);

// Rewrites a literal into the storage form of `col`'s type, in place
// (docs/spec/types.md §3.1).
//
// A `'2026-08-07'` against a `DATE` column becomes the epoch integer
// 20672; a `'12.34'` against a `DECIMAL(10,2)` becomes the unscaled 1234
// carrying scale 2. Every other column type leaves the value untouched,
// once its kind is the one the column's encode takes: a string against an
// integer column, or an integer against a text one, is refused rather than
// compared - it can match no row, and an empty answer would hide the typo
// (types.md §3.1).
// Errors carry **no byte position** - this has no idea where the literal
// was written - so a caller holding an offset adds it with
// `Status::WithContext`.
//
// **Any path that turns a written literal into a value the engine compares
// or keys on must come through here.** Ground: a second coercion (the
// Cabin write hook keying on the raw literal) once made a DATE read key on
// 20672 and a write on `"2026-08-07"`, so a Cabin silently returned fewer
// rows than exist - the failure `docs/spec/cabin.md` §5 calls invisible.
Status CoerceLiteralToColumn(const catalog::SysColumnRow& col, parser::AstValue& value);

// Whether an integer `value` fits the integer or `uint64` column `col`, by
// the gate a row's encode applies (`EncodeOneValue`), so a compile-time check
// of a literal and the per-row write cannot disagree about which integers
// fit (BJ-S3). `value` is not changed. Any other column type is a caller
// error and answers `InvalidArgument`.
Status CheckIntegerLiteralFits(const catalog::SysColumnRow& col, const parser::AstValue& value);

// The family a column type belongs to. Two columns compare only inside one
// family (docs/spec/types.md §3.1a) - every integer width and signedness is
// one family, compared by value - and a `CAST` crosses families only where
// `set_expr`'s table admits it.
enum class TypeFamily : std::uint8_t { kInt, kDec, kText, kDate, kTimestamp, kBool, kOther };
TypeFamily TypeFamilyOf(std::uint32_t type_val);

// Rejects a schema that cannot carry a Keystone word: no columns at all,
// or a first column whose declared type is not an integer one. Exposed so
// CREATE TABLE can refuse such a table at definition time rather than at
// the first INSERT.
Status CheckKeystoneColumn(const catalog::Schema& schema);

// Encodes one row's values into a tuple payload of exactly
// `layout.row_size` bytes.
//
// The payload is `[Keystone word][columns 1..n-1]`: the first schema
// column IS the tuple key and is carried by the Keystone word's id
// field, so `values` supplies only the columns after it. `id` is the
// row's Keystone id, issued or named - the tuple key is never encoded into the
// body as well, since two copies of one value is how the two come to
// disagree.
//
// `layout` must have been built for `schema` (catalog::RowLayout::Build);
// a mismatched pair is InvalidArgument rather than a silently wrong row.
//
// Fails with InvalidArgument if the schema has no usable Keystone column,
// values.size() != schema.columns.size() - 1, `id` exceeds the Keystone
// word's id space, a NULL meets a NOT NULL column, any int value doesn't
// fit its column's width, or a string value doesn't fit a fixed-width
// `char` column; and with Unsupported for a varchar value too long to
// store inline when no var-heap sink is given (see the spilling note).
StatusOr<std::vector<std::byte>> EncodeRow(const catalog::Schema& schema,
                                            const catalog::RowLayout& layout, std::uint64_t id,
                                            const std::vector<parser::AstValue>& values,
                                            const VarHeapSink& varheap = {});

// One assignment `EncodeAssigned` writes: a column of the schema (never the
// tuple key, column 0) and the value it takes.
struct AssignedCell {
    std::size_t column = 0;
    const parser::AstValue* value = nullptr;
};

// **An UPDATE's new image: the row's own bytes with only its assigned cells
// re-encoded** (BN-R2, BN-R3, `instructions/v3.0.0/workorder-bn-opt-read-update-path.md`).
// `before` is the version being replaced, whole; each assigned cell is
// zeroed, its NULL bit cleared, and encoded as `EncodeRow` would encode it,
// spilling through `varheap` if it must. **Every other cell is carried as
// it stands** - an inline value byte for byte, and a spilled cell as the
// pointer it holds, so the new version names the old version's var-heap
// value rather than a copy of it. Two versions then share that value, which
// is what binds every release of a superseded value: it may go only once
// the newest version naming it is reclaimed (`docs/spec/heap-and-tuple.md`
// §3.4). The Keystone word is rewritten as `EncodeRow` writes it.
//
// Fails with Corruption if `before` is not exactly `layout.row_size` bytes
// or a cell names no body column, and otherwise as `EncodeRow` fails for
// the assigned values.
StatusOr<std::vector<std::byte>> EncodeAssigned(const catalog::Schema& schema,
                                                 const catalog::RowLayout& layout,
                                                 std::uint64_t id,
                                                 std::span<const std::byte> before,
                                                 std::span<const AssignedCell> cells,
                                                 const VarHeapSink& varheap = {});

// Decodes a tuple payload back into one value per `schema.columns` entry,
// tuple key first, in the same order EncodeRow() wrote them. Fails with
// Corruption if `payload.size()` is not exactly `layout.row_size` - see
// the file comment on checked redundancy.
StatusOr<std::vector<parser::AstValue>> DecodeRow(const catalog::Schema& schema,
                                                   const catalog::RowLayout& layout,
                                                   std::span<const std::byte> payload,
                                                   std::vector<PendingSpill>* spills = nullptr);

// DecodeRowInto: the same decode, into storage the caller already owns
// (V16) - `out` has exactly one slot per schema column, so a chain scan
// allocates O(1) rather than O(rows). DecodeRow() is a thin wrapper over it.
// `spills`, when given, is cleared and filled with the spilled cells this
// row carries; the caller resolves them once its page span is released.
// When null, a spilled cell is refused with Unsupported - a caller that
// cannot resolve one must not silently return an empty value for it.
//
// DecodeColumnsInto decodes only the columns `columns` selects - a bit per
// `col_pos` - leaving every other slot of `out` untouched. It skips
// *building* AstValues, which is where a decode's cost is, for rows a
// filter then rejects.
//
// The caller is responsible for what it left behind: a slot outside the mask
// holds whatever it held before, so a row that *matches* must be completed
// with the complement mask before anything reads the rest of it.
//
// `spills` collects only the masked columns' spilled values, for the same
// reason DecodeRowInto's does - resolving one is a page fetch and must
// happen after the caller's span is released.
Status DecodeColumnsInto(const catalog::Schema& schema, const catalog::RowLayout& layout,
                         std::span<const std::byte> payload, std::span<parser::AstValue> out,
                         std::uint64_t columns, std::vector<PendingSpill>* spills,
                         DecodeChecks checks = DecodeChecks::kAll);

Status DecodeRowInto(const catalog::Schema& schema, const catalog::RowLayout& layout,
                     std::span<const std::byte> payload, std::span<parser::AstValue> out,
                     std::vector<PendingSpill>* spills = nullptr,
                     DecodeChecks checks = DecodeChecks::kAll);

// One column, from a cell the caller located itself.
//
// The two above find their cells at the row layout's offsets. This one takes
// the cell directly, because a secondary index entry carries its covered
// columns **concatenated in the index's declared order** rather than at the
// relation's offsets (docs/spec/index.md §7) - and re-deriving what a cell
// means from an entry would be a second decoder for the same bytes.
//
// `column_index` is only what a reported spill names, so an entry-side caller
// passes the covered column's schema position.
Status DecodeOneValueInto(const catalog::SysColumnRow& col, std::span<const std::byte> cell,
                          std::size_t column_index, parser::AstValue& out,
                          std::vector<PendingSpill>* spills);

// Renders one decoded value for display in a SELECT response line.
// Prefers AstValue::raw_int_text when set (exact literal/uint64 text),
// falling back to int_val otherwise.
//
// `type_val` is the **column's** catalog type, and rendering is the one
// place it is needed (docs/spec/types.md §3.3). A `DATE` and a
// `TIMESTAMP` decode to the integers they are - epoch days and epoch
// microseconds - so the value alone cannot say which, or that either is a
// date at all. That is deliberate: decode does not format, and the string
// exists only for rows actually emitted, which is what keeps a scan from
// building text for every row it rejects.
//
// **Pass 0 when there is no column type** (a plan's compiled literal, a
// catalog view's value, an aggregate's group label). No defaulted overload
// exists on purpose: a caller that should pass a type would silently render
// an epoch day as `20672`.
//
// A `DECIMAL` needs no `type_val`: it is the one kind whose value carries
// its own scale, so `12.30` renders from the value alone.
std::string FormatValue(std::uint32_t type_val, const parser::AstValue& value);

// `LIKE` over bytes (BU-Q6): `%` any run, `_` any one byte, case-sensitive.
bool LikeMatch(std::string_view value, std::string_view pattern);

// The unsigned reading of a decoded integer value.
//
// **The one place that knows the `raw_int_text` rule**, which is worth
// having exactly once: the text carries the value only when `int_val`
// cannot - a uint64 above INT64_MAX - and is empty otherwise, because
// writing it cost a string conversion per integer column per row decoded.
// A caller that reads the text directly gets an empty string for every
// ordinary value and silently reads zero, which is how this rule breaks.
StatusOr<std::uint64_t> ValueAsUint64(const parser::AstValue& value);

// Compares two decoded values under one operator, with `type_val` saying
// how to read them - uint64 columns compare through their digit text,
// since int_val cannot represent the upper half of the range.
//
// A value-kind mismatch is a non-match rather than an error, and so is any
// comparison with a NULL operand (three-valued collapse, null.md §4);
// IS NULL / IS NOT NULL read the lhs alone. This is the comparison the
// whole engine uses.
bool CompareValues(std::uint32_t type_val, const parser::AstValue& lhs,
                   const parser::AstValue& rhs, parser::CompareOp op);

// ---- The scan kernel (workorder-bm-scan-row-kernel.md BM-R1..R4) ----------
//
// One conjunct `column <op> literal`, compiled to the comparison
// `CompareValues` would make on the decoded column, made instead on the
// cell's bytes. **A pre-filter, never the answer** (BM-R1): a row it
// rejects is one the residual rejects too, and every row it passes still
// goes through the residual unchanged. So the only wrong answer it can
// introduce is a dropped row, and every arm below compares in exactly the
// domain the decoder produces (BM-R4):
//   kInt       the int widths, date, timestamp and narrow decimal,
//              sign-extended at the cell's width, and bool, zero-extended -
//              then int64 against the literal's `int_val`, never narrowed
//              to the cell's width (an out-of-range literal compares as the
//              int64 it is, as `CompareValues` does);
//   kUint64    the cell against `ValueAsUint64(literal)`, unsigned;
//   kWide      the 128-bit pair, high half signed and low half unsigned;
//   kKeystone  column 0, as the 48-bit id `KeystoneIdOfPayload` reads -
//              never as its 8-byte cell, whose top two bytes are not the id;
//   kChar      a `char(N)` cell read back to its first NUL, as the decoder
//              trims it, then bytes against the literal's;
//   kVarchar   a `varchar` cell through `DecodeCell` only, and only when it
//              answers an inline value: a spilled value, a NULL tag and a
//              refusal all pass to the residual, which answers or refuses
//              them as it does today (BM-S4).
// A NULL operand (its bit set) is passed, never rejected: `CompareValues`'
// NULL arm stays the one place UNKNOWN collapses (BM-Q3 (a)).
struct ScanKernelTerm {
    enum class Kind : std::uint8_t { kInt, kUint64, kWide, kKeystone, kChar, kVarchar };
    Kind kind = Kind::kInt;
    parser::CompareOp op = parser::CompareOp::kEq;
    std::uint16_t column = 0;
    std::uint8_t width = 0;        // kInt: the cell's bytes
    bool sign_extend = true;       // kInt: false for bool alone
    std::int64_t lit = 0;          // kInt, kKeystone; kWide's low half
    std::int64_t lit_hi = 0;       // kWide's high half
    std::uint64_t ulit = 0;        // kUint64
    std::string text;              // kChar, kVarchar
};

using ScanKernel = std::vector<ScanKernelTerm>;

// The term for `schema.columns[column] <op> literal`, or nothing when the
// pair is outside the kernel's domain: an operator other than the six, a
// literal of another value kind or scale than the column compares in, a
// `uint64` literal `ValueAsUint64` refuses, or a `uint64` tuple key. Nothing is
// refused here; a pair the kernel does not hold is the residual's, as it
// always was.
std::optional<ScanKernelTerm> CompileScanKernelTerm(const catalog::Schema& schema,
                                                    const catalog::RowLayout& layout,
                                                    std::uint16_t column, parser::CompareOp op,
                                                    const parser::AstValue& literal);

// Whether some term rejects the row in `payload`. The caller has passed
// `CheckDecodeShape` for this layout and checked `payload` is the layout's
// row size (BM-R3: the length check comes first, invariant 13).
bool ScanKernelRejects(const ScanKernel& kernel, const catalog::RowLayout& layout,
                       std::span<const std::byte> payload);

// ---- Ordering (docs/workplan-order-by.md OB2) ----------------------------
//
// One decoded value, normalized to the form the engine orders it in. This
// is where "which value sorts first" is decided, once.
//
// **Why a normalized key and not a three-way `CompareValues`.** A sort
// compares each row O(log n) times and decodes it once, so the type
// dispatch belongs on the once side. Normalizing also removes the trap that
// makes `CompareValues` unusable as a sort predicate: it answers "no match"
// to anything it cannot compare, so `<` and `>` can both be false, which is
// not a strict weak ordering and which `std::sort` may read off the end of
// its range for. An `OrderKey` either exists and orders totally, or was
// refused at the boundary where a `Status` still has somewhere to go.
//
// Every numeric type lands in `num`: `Int128` holds an int64, a uint64 and
// a wide decimal's unscaled value exactly, so ordering is one comparison
// with no per-type branch. DATE and TIMESTAMP need no case of their own -
// an ordering on the encoded integer *is* the ordering on the value (TY5).
//
// **Keys are comparable only within one column.** `scale` is dropped, so
// two decimals of different scale would compare as their unscaled
// integers - safe because every value of a column shares that column's
// (p, s), and the reason `CompareValues` beside this asserts the scales
// agree rather than rescaling.
struct OrderKey {
    Int128 num = 0;
    std::string str;
    bool is_str = false;
    // D3 (`docs/spec/null.md`): NULL orders above every value, so ASC puts
    // NULLs last and the ordinary descending flip puts them first - no
    // NULLS FIRST/LAST grammar exists to override it. Two NULLs compare
    // equal, and the sort's `seq` tiebreak keeps the order total.
    bool is_null = false;

    // Negative, zero, positive - the `strcmp` convention. Total and
    // infallible: everything that could fail was refused by `OrderKeyOf`.
    int Compare(const OrderKey& rhs) const noexcept;
};

// Normalizes one decoded value of a column of type `type_val`, or refuses.
//
// Refuses rather than defaults in two cases, each of which is a decode or
// compile bug reaching a comparator - and a comparator that answers "equal"
// to a value it could not read produces a plausible order out of a bug:
//
//   - a uint64 whose digits do not read (the `raw_int_text` rule, which
//     `ValueAsUint64` is the single owner of).
//   - a value kind that has no order.
//
// A NULL is not one of them: D3 ratified where it sorts (largest), so it
// normalizes to an `is_null` key instead of being refused.
//
// Note what this is *not*: the index key's order, which compares a 32-byte
// truncation of a string and so is a prefix order rather than a total one
// (one of four reasons an index cannot serve an `ORDER BY` -
// `docs/workplan-order-by.md`).
StatusOr<OrderKey> OrderKeyOf(std::uint32_t type_val, const parser::AstValue& value);

}  // namespace kds::exec
