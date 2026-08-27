#pragma once

#include <cstdint>

// The 128-bit integer the wide decimal rides on (docs/spec/types.md TY2:
// `p > 18` is a separate 16-byte type, never a widening of the 8-byte one).
//
// `__int128` is a GCC/Clang builtin rather than standard C++, and that is a
// toolchain this codebase already pinned the day it used
// `__builtin_add_overflow` for the SUM accumulator - so this alias adds no
// new dependency, only a name the rest of the engine can spell. What the
// builtin does **not** decide is any persisted byte: invariant 6 requires
// explicit shift/mask helpers for every on-disk and on-wire encoding, so
// the composition into and out of two uint64 halves is written out below
// and nothing ever memcpy's an `Int128` into a page or a frame.
//
// The value model mirrors the narrow decimal exactly: an unscaled signed
// integer whose scale lives beside it (`AstValue::scale`, the catalog's
// packed `(p, s)`), two's complement, so the halves are "low 64 bits" and
// "high 64 bits including the sign".

namespace kds {

using Int128 = __int128;

// The halves an AstValue carries (`int_val` low, `dec_hi` high) and every
// persisted encoding stores. Two functions, used by every reader and
// writer, so there is exactly one answer to which half is which.
constexpr Int128 Int128FromHalves(std::int64_t hi, std::int64_t lo) noexcept {
    return (static_cast<Int128>(hi) << 64) |
           static_cast<Int128>(static_cast<std::uint64_t>(lo));
}

constexpr std::int64_t Int128High(Int128 v) noexcept {
    return static_cast<std::int64_t>(v >> 64);
}

constexpr std::int64_t Int128Low(Int128 v) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(v));
}

}  // namespace kds
