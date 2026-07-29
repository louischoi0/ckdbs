#include "kds/storage/crc32c.hpp"

#include <array>
#include <cstring>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define KDS_CRC32C_X86 1
#include <nmmintrin.h>
#else
#define KDS_CRC32C_X86 0
#endif

namespace kds::storage {
namespace {

// Castagnoli polynomial 0x1EDC6F41, bit-reflected for the right-shifting
// (LSB-first) table algorithm - the same convention the SSE4.2 crc32
// instruction implements in hardware, which is why the two paths agree.
constexpr std::uint32_t kCastagnoliReflected = 0x82F63B78u;

// One CRC per possible byte value, so the inner loop is a lookup, an xor
// and a shift. 256 entries chosen over the 8 x 256 "slicing-by-8" variant
// because the hardware path already covers the throughput case; this one
// only has to be obviously correct.
constexpr std::array<std::uint32_t, 256> MakeTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0 ? (kCastagnoliReflected ^ (crc >> 1)) : (crc >> 1);
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = MakeTable();

#if KDS_CRC32C_X86
// Probed once; __builtin_cpu_supports compiles to a load from the GCC
// runtime's already-initialized feature word, but caching keeps the
// per-page cost at a predictable branch.
const bool kHasSse42 = __builtin_cpu_supports("sse4.2");

__attribute__((target("sse4.2"))) std::uint32_t HardwareExtend(std::uint32_t crc,
                                                               const std::byte* data,
                                                               std::size_t size) {
    // The instruction consumes the register value in native (little-endian
    // on x86-64) byte order, matching the byte-at-a-time table loop.
    std::uint64_t wide = ~crc;
    while (size >= sizeof(std::uint64_t)) {
        std::uint64_t chunk;
        std::memcpy(&chunk, data, sizeof(chunk));
        wide = _mm_crc32_u64(wide, chunk);
        data += sizeof(chunk);
        size -= sizeof(chunk);
    }
    // crc32 q-form leaves the result zero-extended in the low 32 bits.
    std::uint32_t narrow = static_cast<std::uint32_t>(wide);
    while (size > 0) {
        narrow = _mm_crc32_u8(narrow, std::to_integer<std::uint8_t>(*data));
        ++data;
        --size;
    }
    return ~narrow;
}
#endif

}  // namespace

std::uint32_t Crc32cExtendSoftware(std::uint32_t crc, std::span<const std::byte> data) {
    std::uint32_t state = ~crc;
    for (std::byte b : data) {
        state = kTable[(state ^ std::to_integer<std::uint8_t>(b)) & 0xFFu] ^ (state >> 8);
    }
    return ~state;
}

bool Crc32cHardwareAvailable() noexcept {
#if KDS_CRC32C_X86
    return kHasSse42;
#else
    return false;
#endif
}

std::uint32_t Crc32cExtendHardware(std::uint32_t crc, std::span<const std::byte> data) {
#if KDS_CRC32C_X86
    if (kHasSse42) {
        return HardwareExtend(crc, data.data(), data.size());
    }
#endif
    return Crc32cExtendSoftware(crc, data);
}

std::uint32_t Crc32cExtend(std::uint32_t crc, std::span<const std::byte> data) {
#if KDS_CRC32C_X86
    if (kHasSse42) {
        return HardwareExtend(crc, data.data(), data.size());
    }
#endif
    return Crc32cExtendSoftware(crc, data);
}

}  // namespace kds::storage
