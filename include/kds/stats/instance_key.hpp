#pragma once

#include <cstdint>
#include <functional>

// The identity of one **pattern instance**: a statement shape with its
// arguments bound (docs/spec/waystone-concpets.md sections 1 and 5).
//
//   pattern_id   the shape, from parse (parser/fingerprint.hpp)
//   arg_hash     the arguments bound into it
//
// A waystone is keyed on the pair, and neither half means anything alone: a
// `pattern_id` names a shape that may have millions of instances, and an
// `arg_hash` is a hash of some arguments with no statement attached.
//
// ---- Why this is a type and not two parameters ---------------------------
//
// Both halves are `std::uint64_t`, so every signature that took them
// positionally - `WaystonePageHolds(page, pattern_id, arg_hash)`,
// `FormatWaystonePage(page, pattern_id, arg_hash, ts)`, the directory walk,
// and every recorder/replayer entry point still to be written - accepted
// them in either order and compiled cleanly when swapped. The failure that
// produces is quiet in exactly the wrong way: a swapped pair still hashes,
// still resolves to *a* page, and the identity check that exists to catch a
// wrong page catches this one too - so the bug reads as a permanent trail
// miss, a pure performance fault with no error anywhere, on the one
// subsystem whose entire purpose is not costing anything.
//
// Making the pair a struct makes that mistake fail to compile. That is the
// whole point of the type; it has no behaviour and is not meant to grow any.
//
// **Identity only, no state.** Nothing about where a trail lives, how hot it
// is, or whether it has been recorded belongs here - those are properties of
// a waystone, and this is the name of one.

namespace kds::stats {

struct InstanceKey {
    std::uint64_t pattern_id = 0;
    std::uint64_t arg_hash = 0;

    // Defaulted rather than written out: the identity of an instance is
    // exactly the identity of its two fields, and a hand-written comparison
    // could only ever differ from that by being wrong.
    bool operator==(const InstanceKey&) const noexcept = default;
};

// For the core-local hash tables the recorder and the sighting counter will
// key by instance. Mixed with the same odd-multiplier fold the standard
// library's own pair hashing uses in spirit, rather than XOR: `pattern_id ^
// arg_hash` collapses to 0 for the (impossible today, but not guarded)
// case where they are equal, and more usefully it loses the ordering that
// makes this type worth having.
//
// Deliberately **not** the FNV-1a that produces the two halves. This value
// never goes on disk - it selects a bucket in memory - so it carries none of
// fingerprint.hpp's stability obligations, and tying it to the persisted
// hash would invite someone to persist it later.
struct InstanceKeyHash {
    std::size_t operator()(const InstanceKey& key) const noexcept {
        std::uint64_t h = key.pattern_id * 0x9E3779B97F4A7C15ull;
        h ^= key.arg_hash + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

}  // namespace kds::stats

namespace std {

template <>
struct hash<kds::stats::InstanceKey> {
    std::size_t operator()(const kds::stats::InstanceKey& key) const noexcept {
        return kds::stats::InstanceKeyHash{}(key);
    }
};

}  // namespace std
