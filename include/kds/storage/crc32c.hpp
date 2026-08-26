#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// CRC32C (Castagnoli, polynomial 0x1EDC6F41) - the page checksum function
// confirmed in docs/spec/page.md section 10. Used to detect torn or corrupted
// pages: computed at flush, verified on every load from disk, never on a
// buffer hit.
//
// Two implementations are exported deliberately. The software table
// version is the *definition* of the function and always available; the
// x86-64 SSE4.2 version is a performance twin that must agree with it bit
// for bit (docs/spec/page.md section 18-5 makes that equivalence a test
// requirement). Crc32c() dispatches to the hardware path once per process
// when the CPU supports it. Keeping the software path reachable by name
// also keeps the deterministic simulator (rules.md section 4) independent
// of the host CPU.
//
// The hardware path is the only architecture-specific code here, and it is
// compiled behind a target attribute rather than global -msse4.2, so the
// platform pin (rules.md section 7) stays open: on any non-x86-64 target
// the software path simply is the implementation.
//
// Concurrency: pure functions over caller-owned bytes; no state beyond a
// function-local one-time CPU feature probe. Safe to call from any core.

namespace kds::storage {

// Seed for a fresh computation. Extend() applies the standard CRC32C
// pre/post inversion internally, so
//   Crc32cExtend(Crc32cExtend(kCrc32cInit, a), b)
// equals the CRC32C of a followed by b - which is what lets the page
// checksum skip over its own field without copying the page.
inline constexpr std::uint32_t kCrc32cInit = 0;

// Known-answer value for the ASCII string "123456789", the standard
// CRC32C check vector.
inline constexpr std::uint32_t kCrc32cCheckValue = 0xE3069283u;

// Continues `crc` over `data`. Dispatches to the hardware path when
// available.
std::uint32_t Crc32cExtend(std::uint32_t crc, std::span<const std::byte> data);

// One-shot CRC32C of `data`.
inline std::uint32_t Crc32c(std::span<const std::byte> data) {
    return Crc32cExtend(kCrc32cInit, data);
}

// The portable table-driven implementation, always available.
std::uint32_t Crc32cExtendSoftware(std::uint32_t crc, std::span<const std::byte> data);

// True when this build and this CPU can use the SSE4.2 path. Always false
// off x86-64. Exposed so the equivalence test can skip (rather than
// silently pass) where there is nothing to compare against.
bool Crc32cHardwareAvailable() noexcept;

// The SSE4.2 implementation. Only call it when Crc32cHardwareAvailable()
// is true; on other builds it falls back to the software path so callers
// (i.e. the equivalence test) need no #ifdef.
std::uint32_t Crc32cExtendHardware(std::uint32_t crc, std::span<const std::byte> data);

}  // namespace kds::storage
