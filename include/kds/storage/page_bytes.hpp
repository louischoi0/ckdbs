#pragma once

#include <cstring>
#include <span>

#include "kds/base/common.hpp"

// The one home of field-wise page access (invariant 6's discipline: every
// persisted encoding moves through explicit offsets and memcpy, never an
// overlay struct or a compiler bitfield). Used by page_header.cpp and
// anchor_page.cpp; `stats/waystone_page.cpp` still keeps a private pair.

namespace kds::storage {

template <typename T>
inline T LoadField(std::span<const std::byte, kPageSize> page, std::size_t offset) {
    T out{};
    std::memcpy(&out, page.data() + offset, sizeof(T));
    return out;
}

template <typename T>
inline void StoreField(std::span<std::byte, kPageSize> page, std::size_t offset, T value) {
    std::memcpy(page.data() + offset, &value, sizeof(T));
}

}  // namespace kds::storage
