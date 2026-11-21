#pragma once

#include <cstdint>

#include "kds/base/common.hpp"

// A heap relation's rowid (DN-D2, DN-Q4 (a); `docs/spec/heap-and-tuple.md`
// §3.1b): where a row is - its data page and its slot - packed into 48 bits
// and unpacked in one step. Not a tuple key and not visible to SQL: a row's
// identity is its Keystone, and a rowid is bound to many rows over a
// relation's life, one at a time, as a released slot is taken again
// (DN-D1, DN-R2).
//
// Layout, explicit shift and mask (invariant 6's rule for a persisted
// format): `page_id << 16 | slot`. A page id is 32 bits and a slot index 16,
// so every rowid fits the 48 a Keystone id has, and the upper 16 bits of
// the stored word are 0 (invariant 7's zero-extension).
namespace kds::heap {

inline constexpr unsigned kRowidSlotBits = 16;
inline constexpr std::uint64_t kRowidSlotMask = (std::uint64_t{1} << kRowidSlotBits) - 1;

struct Rowid {
    PageId page_id = kInvalidPageId;
    std::uint16_t slot = 0;
    friend bool operator==(const Rowid&, const Rowid&) = default;
};

inline constexpr std::uint64_t EncodeRowid(Rowid r) noexcept {
    return (static_cast<std::uint64_t>(r.page_id) << kRowidSlotBits) | r.slot;
}

inline constexpr Rowid DecodeRowid(std::uint64_t word) noexcept {
    return Rowid{static_cast<PageId>(word >> kRowidSlotBits),
                 static_cast<std::uint16_t>(word & kRowidSlotMask)};
}

static_assert(DecodeRowid(EncodeRowid(Rowid{0xFFFFFFFEu, 0xFFFFu})) ==
              Rowid{0xFFFFFFFEu, 0xFFFFu});
static_assert(EncodeRowid(Rowid{0xFFFFFFFFu, 0xFFFFu}) < (std::uint64_t{1} << 48));

}  // namespace kds::heap
