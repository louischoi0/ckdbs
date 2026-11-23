#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "kds/base/status.hpp"
#include "kds/catalog/schema.hpp"
#include "kds/parser/ast.hpp"

// ---- The embedded primary key (DR-S3e, F1) ----------------------------------
//
// `instructions/v3.2.0/workorder-dr-primary-key-over-keystone.md` §2a. A
// btree relation whose declared primary key is made of columns whose
// order-preserving codes total 48 bits or less carries no index for it: the
// key *is* the Keystone id. The Keystone is left as it is (DR-R1) - a
// 48-bit id in a hidden column 0, placed where it sorts - and the engine
// chooses its value from the key:
//
//     id = code(key) + 1
//
// `code` is each key column's code, in key order, most significant first,
// as one unsigned integer of the key's total bits. A column's code is its
// index key encoding (`index_key.hpp`, IX5) without the discriminator byte:
// big-endian, a signed type's sign bit flipped. So ids ascend exactly as
// keys do - leaf order is key order - and one key names one id.
//
// The `+ 1` is because 0 is no id. A key whose codes fill all 48 bits has
// one combination too many: the largest value of every column together
// would be id 2^48, and it is refused `OutOfRange` by name.
//
// **A persisted format** (DR-R12): every row of such a relation sits at the
// id this file computes, for the relation's life. `tests/embedded_key_test.cpp`
// pins it by vectors; changing it, or the index key encoding it reads, is a
// format event (DR-R5).
//
// What may embed (DR-Q7 (a) as revised): `int8`, `int16`, `int32`, `date`
// and `bool`. A `char(N)` does not: its comparison against its code was not
// read, so such a key takes a unique index (P1).

namespace kds::exec {

inline constexpr std::uint32_t kEmbeddedKeyMaxBits = 48;

// The bits `type_val`'s code takes, or nullopt for a type that never embeds.
std::optional<std::uint32_t> EmbeddedKeyColumnBits(std::uint32_t type_val);

// Whether a primary key over `key_cols`, in key order, embeds: every column
// of an embeddable type, and at most `kEmbeddedKeyMaxBits` together. Decided
// once, at `CREATE TABLE`, from the declaration alone (DR-R12).
bool KeyEmbeds(std::span<const catalog::SysColumnRow> key_cols);

// The Keystone id of the key whose column values are `key_values`, one per
// column of `schema.embedded_key` in key order, each in its column's
// storage form (`CoerceLiteralToColumn`).
//
// Fails with the index key encoder's refusals for a value the column cannot
// hold - `OutOfRange` an integer outside its width, `InvalidArgument` a
// value of another type, `NotImplemented` a NULL - and with `OutOfRange`
// for the one key a 48-bit form cannot name.
StatusOr<std::uint64_t> EmbeddedKeyId(const catalog::Schema& schema,
                                      std::span<const parser::AstValue> key_values);

// The ids of every key that begins with `prefix` (the first columns' values,
// in key order, storage form) and whose next column lies in `[low, high]`,
// inclusive; `low` and `high` are both null for a bare prefix. The window is
// `[lo, hi]`, inclusive.
//
// nullopt where no key can match: `low` above `high`, or a window that
// holds only the one key a 48-bit form cannot name. A failure where the
// window cannot be told: a value that does not encode (the encoder's
// refusal - a refused value is not known to match no row), or bounds that
// are not this shape (one end alone, nothing bound, more values than the
// key has columns). A read leaves such conjuncts to its residual, which
// answers them on every row; `PURGE` refuses them.
struct EmbeddedKeyWindow {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
};
StatusOr<std::optional<EmbeddedKeyWindow>> EmbeddedKeyWindowOf(
    const catalog::Schema& schema, std::span<const parser::AstValue> prefix,
    const parser::AstValue* low, const parser::AstValue* high);

}  // namespace kds::exec
