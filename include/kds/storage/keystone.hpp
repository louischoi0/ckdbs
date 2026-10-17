#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "kds/base/status.hpp"

// Every tuple's mandatory first column (heap-and-tuple.md section 4): one
// 64-bit word, the "Keystone column" / "Keystone word",
// `id:48 | flags:8 | reserved:8` - bytes 0-5 the id, byte 6 `flags`, byte
// 7 `reserved` (BK-R1, `instructions/v3.0.0/workorder-bk-keystone-48.md`).
// The id took eight bits of the reserved field at BK; that field was
// `meta_handle:16` back when a bounded metadata pool handed out handles,
// and the pool is gone and the handle retired: write 0, ignore on read,
// repurposing is [OPEN]. This file only does encode/decode of that word -
// it is a row-encoding concern, independent of the physical heap page
// layer, which stores whatever payload bytes it is given; an id is read
// back through `KeystoneIdOfPayload` alone.
//
// The id is the relation's tuple key. Per row, the `INSERT` names it or
// the catalog issues it from the relation's `next_id` mark (rows.hpp); its
// uniqueness is the clustered tree's descent's (heap-and-tuple.md section
// 4.1, invariant 11).
//
// Concurrency: callers holding a live tuple in a buffer-pool frame must
// update the Keystone word with a single atomic uint64_t CAS (rules.md
// #3) so the three fields never tear across a concurrent reader. **The
// lock byte is not part of that and is not used**: AO-R3 decided the lock
// family holds no persisted bit, so the byte stays zero and a row's
// exclusive tenancy is the header's `trx_id` stamp plus the in-memory
// table (`docs/spec/txn.md` §5). This
// header only provides the pure encode/decode math; the atomic exchange
// itself happens at the call site, once a frame/page abstraction exists
// to hold it.

namespace kds {

inline constexpr int kKeystoneIdBits = 48;
inline constexpr int kKeystoneFlagsBits = 8;
inline constexpr int kKeystoneReservedBits = 8;
static_assert(kKeystoneIdBits + kKeystoneFlagsBits + kKeystoneReservedBits == 64);

// Bytes the word occupies at the front of every tuple payload.
inline constexpr std::size_t kKeystoneWordSize = 8;
static_assert(kKeystoneWordSize * 8 ==
              kKeystoneIdBits + kKeystoneFlagsBits + kKeystoneReservedBits);

// Largest id representable in kKeystoneIdBits bits; Encode() rejects anything
// above this, and every refusal that names the ceiling names this value.
inline constexpr std::uint64_t kMaxKeystoneId = (std::uint64_t{1} << kKeystoneIdBits) - 1;

// One past the largest id: the exclusive upper bound of a relation's whole
// id space, so `[0, kIdSpaceEnd)` is "every row" wherever a half-open tuple key
// interval is declared (a read borrow, a range lock).
inline constexpr std::uint64_t kIdSpaceEnd = kMaxKeystoneId + 1;

// The decoded form of a tuple's Keystone column.
struct Keystone {
    std::uint64_t id;
    std::uint8_t flags;
    std::uint8_t reserved;

    // Packs the three fields into one Keystone word via explicit
    // shift/mask (rules.md #5: compiler bitfields are forbidden for
    // on-disk formats). Fails with InvalidArgument if `id` doesn't fit in
    // kKeystoneIdBits bits; `flags` and `reserved` always fit their full
    // width by type - a byte each, so neither can carry a bit the word has
    // no room for.
    static StatusOr<std::uint64_t> Encode(std::uint64_t id, std::uint8_t flags,
                                           std::uint8_t reserved);

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
// every reader of a stored id needs it - the heap chain, the btree, redo,
// the builds, the row codec, the dispatcher - and a second copy of a
// persisted format's decode is exactly what invariant 5 is guarding
// against.
StatusOr<std::uint64_t> KeystoneIdOfPayload(std::span<const std::byte> payload);

// `Corruption` unless the payload's Keystone word carries `id`. An insert is
// handed the id beside the payload that encodes it, and two disagreeing
// copies of a tuple's identity is the kind of defect that stays silent for
// months, so the heap chain and the btree check them once, where both are in
// hand.
Status RequirePayloadCarries(std::span<const std::byte> payload, std::uint64_t id);

}  // namespace kds
