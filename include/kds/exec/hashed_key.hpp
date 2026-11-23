#pragma once

// A hashed primary key's home (DR-S3f; `docs/spec/heap-and-tuple.md` §3.1c).
//
// A btree whose declared primary key is neither the Keystone nor embeddable
// keeps the key in the unique index `$pkey_<oid>` over a hidden Keystone -
// and the Keystone id a row is first placed at is **chosen from its key**:
// its home, `Hash47` of the key's full values, in `[1, 2^47)`. A read that
// pins the whole key goes to the home in one descent and takes the row there
// if it is visible and carries the key; every other outcome is the index's
// (DR-R11). A row whose home is bound - by another key that hashed to it, by
// a deleted row of the same key until a `PURGE`, by a row whose key moved
// away - is issued an id in `[2^47, 2^48)`, which no home ever is.
//
// **The home is a fast path and never an authority** (`[quiet-wrong]`,
// DR-R11): uniqueness is the index's check on full values (BX), a missing
// row at a home says nothing, and a row found there answers only through
// the statement's whole residual. So a home computed differently for two
// spellings of one key value costs a second descent and no answer.
//
// **A persisted format** (DR-R12): the canonical bytes and `Hash47` are
// pinned by vectors (`tests/hashed_key_test.cpp`). A change would leave
// every existing row off its home - slower, never wrong - and is a format
// event all the same.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kds/catalog/schema.hpp"
#include "kds/parser/ast.hpp"

namespace kds::exec {

// Homes are `[1, kHashedKeyHomes]`; `catalog::kHashedKeyIssuedBase` is the
// first id above them, where such a relation's issue mark starts.
inline constexpr std::uint64_t kHashedKeyHomes = (std::uint64_t{1} << 47) - 1;
static_assert(kHashedKeyHomes + 1 == catalog::kHashedKeyIssuedBase);

// FNV-1a over `canonical`, finished by MurmurHash3's 64-bit mix, folded to
// `[1, kHashedKeyHomes]`.
std::uint64_t Hash47(std::span<const std::byte> canonical) noexcept;

// The home of the key whose columns are `key_cols` (positions in `schema`,
// key order) and whose values are `values`, each coerced to its column as
// an `INSERT`'s is. The canonical bytes are, per column, a tag byte, the
// value's length as four little-endian bytes and the value: a string's
// whole text (tag 2), any other type's index key encoding (tag 1), which
// is the value in full for every type but a string. nullopt where a value
// is NULL, does not coerce or does not encode: the row is then issued an id
// and the read takes the index.
std::optional<std::uint64_t> HashedKeyHome(const catalog::Schema& schema,
                                           std::span<const std::uint16_t> key_cols,
                                           std::span<const parser::AstValue> values);

}  // namespace kds::exec
