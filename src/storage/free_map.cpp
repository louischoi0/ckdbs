#include "kds/storage/free_map.hpp"

#include <bit>

namespace kds::storage {
namespace {

constexpr std::size_t ByteOffset(std::uint32_t index) noexcept {
    return kPageBodyOffset + (index >> 3);
}

constexpr std::uint8_t BitMask(std::uint32_t index) noexcept {
    return static_cast<std::uint8_t>(1u << (index & 7u));
}

std::uint8_t ByteAt(std::span<const std::byte, kPageSize> page, std::uint32_t index) noexcept {
    return static_cast<std::uint8_t>(page[ByteOffset(index)]);
}

}  // namespace

void FormatFreeMapPage(std::span<std::byte, kPageSize> page, PageType type) {
    // FormatPage zeroes the page, which is already an empty bitmap.
    FormatPage(page, type);
}

Status ValidateFreeMapPage(std::span<const std::byte, kPageSize> page, PageType type) {
    if (Status s = VerifyPageChecksum(page); !s.ok()) return s;
    return ValidatePageHeader(page, type);
}

bool FreeMapIsAllocated(std::span<const std::byte, kPageSize> page, std::uint32_t index) noexcept {
    if (index >= kFreeMapBitsPerPage) return true;
    return (ByteAt(page, index) & BitMask(index)) != 0;
}

void FreeMapAllocate(std::span<std::byte, kPageSize> page, std::uint32_t index) noexcept {
    if (index >= kFreeMapBitsPerPage) return;
    std::byte& byte = page[ByteOffset(index)];
    byte = static_cast<std::byte>(static_cast<std::uint8_t>(byte) | BitMask(index));
}

std::optional<std::uint32_t> FreeMapFindFirstFree(std::span<const std::byte, kPageSize> page,
                                                  std::uint32_t from) noexcept {
    if (from >= kFreeMapBitsPerPage) return std::nullopt;

    // Bit-at-a-time to the next byte boundary, then whole bytes: 0xFF is
    // skipped in one comparison, and the first byte that is not gives its
    // lowest clear bit as countr_one.
    std::uint32_t index = from;
    while (index < kFreeMapBitsPerPage && (index & 7u) != 0) {
        if (!FreeMapIsAllocated(page, index)) return index;
        ++index;
    }

    for (; index < kFreeMapBitsPerPage; index += 8) {
        const std::uint8_t byte = ByteAt(page, index);
        if (byte == 0xFFu) continue;
        return index + static_cast<std::uint32_t>(std::countr_one(byte));
    }
    return std::nullopt;
}

std::uint32_t FreeMapCountAllocated(std::span<const std::byte, kPageSize> page) noexcept {
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < kPageBodySize; ++i) {
        count += static_cast<std::uint32_t>(
            std::popcount(static_cast<std::uint8_t>(page[kPageBodyOffset + i])));
    }
    return count;
}

}  // namespace kds::storage
