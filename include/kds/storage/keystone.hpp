#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/status.hpp"

// Every tuple's mandatory first column (heap-and-tuple.md section 4): one
// 64-bit word, the "Keystone column" / "Keystone word",
// `id:40 | flags:8 | reserved:16` (the reserved field was
// `meta_handle:16` back when a bounded metadata pool handed out handles;
// that pool is gone and the handle is retired: write 0, ignore on read,
// repurposing is [OPEN]). This file only does encode/decode
// of that word - it is a row-encoding concern, independent of the physical
// heap page layer (heap_page.hpp does not know this struct exists; it
// just stores whatever payload bytes it's given).
//
// The id is the relation's primary key, and it is system-generated: the
// catalog issues it from the relation's `next_id` sequence (rows.hpp), so
// it is unique and monotonic by construction rather than by a uniqueness
// check. Callers do not supply it (CLAUDE.md invariant 10).
//
// Concurrency: callers holding a live tuple in a buffer-pool frame must
// update the Keystone word with a single atomic uint64_t CAS (rules.md
// #3) so the three fields never tear across a concurrent reader. This
// header only provides the pure encode/decode math; the atomic exchange
// itself happens at the call site, once a frame/page abstraction exists
// to hold it.

namespace kds {

inline constexpr int kKeystoneIdBits = 40;
inline constexpr int kKeystoneFlagsBits = 8;
inline constexpr int kKeystoneReservedBits = 16;
static_assert(kKeystoneIdBits + kKeystoneFlagsBits + kKeystoneReservedBits == 64);

// Bytes the word occupies at the front of every tuple payload.
inline constexpr std::size_t kKeystoneWordSize = 8;
static_assert(kKeystoneWordSize * 8 ==
              kKeystoneIdBits + kKeystoneFlagsBits + kKeystoneReservedBits);

// Largest id representable in 40 bits; Encode() rejects anything above this.
inline constexpr std::uint64_t kMaxKeystoneId = (std::uint64_t{1} << kKeystoneIdBits) - 1;

// The decoded form of a tuple's Keystone column.
struct Keystone {
    std::uint64_t id;
    std::uint8_t flags;
    std::uint16_t reserved;

    // Packs the three fields into one Keystone word via explicit
    // shift/mask (rules.md #5: compiler bitfields are forbidden for
    // on-disk formats). Fails with InvalidArgument if `id` doesn't fit in
    // 40 bits; `flags` and `reserved` always fit their full width by type.
    static StatusOr<std::uint64_t> Encode(std::uint64_t id, std::uint8_t flags,
                                           std::uint16_t reserved);

    // Unpacks a Keystone word produced by Encode(). Never fails: every
    // bit pattern is a valid decoding.
    static Keystone Decode(std::uint64_t word) noexcept;
};

// The id in the Keystone word at the front of a tuple payload: an explicit
// little-endian load (rules.md #5 - no reinterpret_cast over page bytes,
// no compiler bitfields) followed by Decode(). Fails with Corruption if
// the payload is too short to hold the word at all.
//
// This lives here rather than in each caller's anonymous namespace because
// it now has four of them - heap_chain.cpp, btree.cpp, heap_page.cpp's
// KeystoneIdAt(), and the row codec's pk read - and four copies of a
// persisted format's decode is exactly what invariant 5 is guarding
// against.
StatusOr<std::uint64_t> KeystoneIdOfPayload(std::span<const std::byte> payload);

}  // namespace kds
