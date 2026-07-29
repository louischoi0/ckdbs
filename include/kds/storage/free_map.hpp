#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "kds/base/common.hpp"
#include "kds/base/status.hpp"
#include "kds/storage/page_header.hpp"

// The allocation bitmap page (PageType::kFreeMap, docs/page.md section 5):
// one bit per page id, 1 = allocated. This is what makes "which page ids
// exist" a durable fact rather than an in-memory side table.
//
// Codec for one bitmap page only. Multi-page coverage (free-map pages at
// computable interval positions) and the ALLOC/FREE WAL records are
// page.md section 5's remaining work; DevicePageStore documents the
// single-page ceiling it lives under.
//
// Bit addressing is explicit shift/mask over the page body - a persisted
// format, so no bitfields (invariant 5):
//
//   byte = kPageBodyOffset + (index >> 3)
//   bit  = index & 7          (LSB-first, so "lowest free bit in a byte"
//                              is countr_one of that byte)

namespace kds::storage {

// (8192 - 32) * 8 = 65,280 page ids, i.e. 510 MiB of data file.
inline constexpr std::uint32_t kFreeMapBitsPerPage =
    static_cast<std::uint32_t>(kPageBodySize) * 8;
static_assert(kFreeMapBitsPerPage == 65280);

// Common header for kFreeMap, every bit clear. The checksum is stamped by
// the owner at write-out time (page.md section 8), not here.
void FormatFreeMapPage(std::span<std::byte, kPageSize> page);

// Checksum verifies and the header is a kFreeMap this build can parse.
// Either failure is Corruption.
Status ValidateFreeMapPage(std::span<const std::byte, kPageSize> page);

// An index at or above kFreeMapBitsPerPage reads as allocated and ignores
// writes: callers range-check and report OutOfRange with their own
// context, and a missed check cannot corrupt a neighbouring page's bit.
bool FreeMapIsAllocated(std::span<const std::byte, kPageSize> page, std::uint32_t index) noexcept;
void FreeMapAllocate(std::span<std::byte, kPageSize> page, std::uint32_t index) noexcept;

// Lowest clear bit at or above `from`, or nullopt if this page has none.
std::optional<std::uint32_t> FreeMapFindFirstFree(std::span<const std::byte, kPageSize> page,
                                                  std::uint32_t from) noexcept;

// Set bits. O(page); metering and tests, not a hot path.
std::uint32_t FreeMapCountAllocated(std::span<const std::byte, kPageSize> page) noexcept;

}  // namespace kds::storage
