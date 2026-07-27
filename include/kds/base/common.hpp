#pragma once

#include <cstddef>
#include <cstdint>

// Shared primitive types/constants (KDS-DESIGN.md invariants 1, 6, 7).

namespace kds {

// Page IDs are unsigned 32-bit, never signed (16 TB target capacity =
// 2^31 pages, half the u32 space; 0xFFFFFFFF is reserved invalid).
using PageId = std::uint32_t;
inline constexpr PageId kInvalidPageId = 0xFFFFFFFFu;

inline constexpr std::size_t kPageSize = 8192;

}  // namespace kds
