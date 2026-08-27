#pragma once

#include <cstdint>
#include <span>

#include "kds/base/status.hpp"
#include "kds/catalog/rows.hpp"
#include "kds/parser/ast.hpp"

// The secondary index key encoding (docs/spec/index.md §5, workplan IX01).
//
// One index key is **one order-preserving byte string**: the concatenation
// of its key columns' encodings, compared with `memcmp` and nothing else.
// That is the whole design, and everything below exists to make it true.
//
// ---- Why order-preserving bytes rather than a typed comparator ----------
//
// The index tree (storage/index/) routes a descent and binary-searches a
// leaf. If it compared *values* it would need the schema, a per-type
// dispatch, and a cross-scale/cross-width rule at every node - four things
// that can disagree with `CompareValues`. Encoding the order into the bytes
// once, at the boundary, leaves the tree with a single `memcmp` and no
// opinion about types at all: a key is an opaque byte string to every layer
// below this one.
//
// Big-endian with the sign bit flipped is what buys it. For a
// two's-complement signed integer, XORing the top bit maps the signed order
// onto the unsigned one (INT_MIN -> 0x00.., 0 -> 0x80.., INT_MAX ->
// 0xFF..), and big-endian makes `memcmp` compare the most significant byte
// first. Concatenating columns then yields lexicographic-by-column
// ordering, which is exactly tuple order.
//
// Note this is a **different encoding from `MakeCabinKey` and the aggregate
// group key**, and the difference is the point: those encode *identity*
// (two NULLs are one group), this encodes *order*. One function doing both
// would do one of them badly.
//
// ---- The coercion rule, which is the one that has already cost rows -----
//
// **Every value reaching this file must already be in its column's storage
// form.** A `DATE` arrives as an epoch-day integer, a `DECIMAL(10,2)` as an
// unscaled integer carrying scale 2 - never as the text the client wrote.
// The one path from a written literal to a storage value is
// `exec::CoerceLiteralToColumn` (docs/spec/types.md §3.1), and this file
// **refuses** anything else rather than parsing it itself.
//
// That refusal is deliberate and load-bearing. There were briefly two
// coercion paths in this engine: the Cabin's write hook keyed on the raw
// literal while its read path keyed on the coerced one, so an observed DATE
// value silently stopped seeing rows inserted after it was observed - fewer
// rows, a plausible answer, and no error anywhere. A second parser here
// would reproduce that failure with an index's blast radius. So the
// encoder takes storage values only, and a caller that has a literal
// coerces first or gets an InvalidArgument naming the type it was handed.
//
// ---- Truncation (spec §6) ----------------------------------------------
//
// A string column contributes at most `kIndexStringKeyBytes`, zero-padded.
// That makes an equality on the encoded key a **prefix test**: a superset
// of the true matches, so false positives only and never a false negative.
// A superset is what the read path already receives and already subtracts,
// by re-checking the predicate against the MVCC-resolved base row
// (spec §1) - so truncation adds nothing to the read path at all.
//
// Two collapses follow from the same padding, both benign for the same
// reason and both stated so they are not discovered as bugs:
//
//   - two strings sharing a `kIndexStringKeyBytes`-byte prefix encode
//     identically;
//   - `"a"` and `"a\0"` encode identically, because a short value is
//     zero-padded and an embedded NUL is not escaped.
//
// Neither can lose a row. Escaping would buy precision at the cost of a
// variable-width key, which is the fixed-width entry surrendered for one
// column type.
//
// Concurrency: pure functions over caller-owned buffers. No page, no store,
// no latch.

namespace kds::exec {

// How many bytes of a string column's value ride in the key.
//
// `[PROPOSED] 32` and an **open decision** (spec §13), on the same axis as
// `kds.inline_cell_width` and settled by the same measurement: it trades
// entry width - and so leaf fan-out - against how often a probe pays a base
// descent it did not have to. Nothing may depend on the number; every
// caller reads the width back from `IndexKeyColumnWidth`.
inline constexpr std::uint32_t kIndexStringKeyBytes = 32;

// Each column's encoding is preceded by one discriminator byte.
//
// It is always `kIndexKeyPresent` today, because the row codec refuses to
// encode a NULL and so no NULL can reach a key. The byte is spent anyway,
// deliberately: when NULLs become storable, giving them a place in the key
// must be a *semantic* change (which end they sort at) and not a **format**
// change that invalidates every index page ever written.
inline constexpr std::uint32_t kIndexKeyDiscriminatorSize = 1;
inline constexpr std::uint8_t kIndexKeyNull = 0x00;
inline constexpr std::uint8_t kIndexKeyPresent = 0x01;

// The encoded width of one key column, discriminator included.
//
// Fails with Unsupported for `float` (nothing settled its encoding, so it
// is not declarable either) and InvalidArgument for an unrecognized
// `type_val` or a zero-width `char`.
StatusOr<std::uint32_t> IndexKeyColumnWidth(const catalog::SysColumnRow& col);

// The encoded width of a whole composite key: the sum of the above.
//
// This is an index's `key_width` schema constant - computed once at
// `CREATE INDEX`, stored on the catalog row, and checked against every page
// the tree touches. Fails with InvalidArgument for an empty column list.
StatusOr<std::uint32_t> IndexKeyWidth(std::span<const catalog::SysColumnRow> key_cols);

// Encodes one column's value into `out`, which must be exactly
// `IndexKeyColumnWidth(col)` bytes.
//
// Fails with:
//   InvalidArgument  `value` is not in `col`'s storage form (see the
//                    coercion rule above), `out` is the wrong size, or the
//                    value carries a scale the column does not
//   OutOfRange       an integer value outside the column's width. Reachable
//                    only from a written literal - a decoded row cannot
//                    produce one - and a *compile-time* answer for the
//                    caller, never a per-row one
//   Unsupported      a NULL, whose position in the key order is undecided
//                    (see kIndexKeyNull), or a float column
Status EncodeIndexKeyColumn(const catalog::SysColumnRow& col, const parser::AstValue& value,
                            std::span<std::byte> out);

// Encodes a whole composite key. `values` is positionally aligned with
// `key_cols`, and `out` must be exactly `IndexKeyWidth(key_cols)` bytes.
//
// A caller holding fewer values than columns - a probe on a key *prefix* -
// passes the prefix and a correspondingly shorter `out`; the encoding of a
// prefix is a byte-prefix of the whole key by construction, which is what
// makes prefix seeks work with no second code path.
Status EncodeIndexKey(std::span<const catalog::SysColumnRow> key_cols,
                      std::span<const parser::AstValue> values, std::span<std::byte> out);

}  // namespace kds::exec
