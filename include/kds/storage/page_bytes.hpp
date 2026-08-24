#pragma once

#include <cstring>
#include <span>

#include "kds/base/common.hpp"

// The one home of field-wise page access (invariant 6's discipline: every
// persisted encoding moves through explicit offsets and memcpy, never an
// overlay struct or a compiler bitfield). Before this header each page
// class kept a private copy of the same two templates; the anchor page was
// the third, which is when a copy stops being an idiom and starts being a
// divergence risk (the 3f07eda review's S1). page_header.cpp adopted it
// with the anchor; the waystone page keeps its private pair until a change
// next touches that file, per the deferred-cleanup rule.

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
