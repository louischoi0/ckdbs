#pragma once

#include <cstdint>

#include "kds/base/status.hpp"

// Every tuple's mandatory first column (KDS-DESIGN.md section 4): one
// 64-bit word, the "Keystone column" / "Keystone word",
// `id:40 | flags:8 | meta_handle:16`. This file only does encode/decode of
// that word - it is a row-encoding concern, independent of the physical
// heap page layer (heap_page.hpp does not know this struct exists; it
// just stores whatever payload bytes it's given).
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
inline constexpr int kKeystoneMetaHandleBits = 16;
static_assert(kKeystoneIdBits + kKeystoneFlagsBits + kKeystoneMetaHandleBits == 64);

// Largest id representable in 40 bits; Encode() rejects anything above this.
inline constexpr std::uint64_t kMaxKeystoneId = (std::uint64_t{1} << kKeystoneIdBits) - 1;

// The decoded form of a tuple's Keystone column.
struct Keystone {
    std::uint64_t id;
    std::uint8_t flags;
    std::uint16_t meta_handle;

    // Packs the three fields into one Keystone word via explicit
    // shift/mask (rules.md #5: compiler bitfields are forbidden for
    // on-disk formats). Fails with InvalidArgument if `id` doesn't fit in
    // 40 bits; `flags` and `meta_handle` always fit their full width by
    // type.
    static StatusOr<std::uint64_t> Encode(std::uint64_t id, std::uint8_t flags,
                                           std::uint16_t meta_handle);

    // Unpacks a Keystone word produced by Encode(). Never fails: every
    // bit pattern is a valid decoding.
    static Keystone Decode(std::uint64_t word) noexcept;
};

}  // namespace kds
