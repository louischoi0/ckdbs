#pragma once

// sim/rng.hpp — the harness's single entropy source (bench/workplan-teststrategy
// SIM01).
//
// Every piece of nondeterminism in the simulation harness — operation choice,
// values, crash points, fault schedules, session interleaving — derives from
// one `--seed` through label-forked streams:
//
//     Rng root(seed);
//     Rng workload = root.Fork("workload");
//     Rng faults   = root.Fork("faults");
//
// Two properties carry the whole regression story, and both are testable:
//
//  1. **Label stability.** A fork is a pure function of (seed, label), never
//     of how much any other stream has consumed. Adding a new consumer with
//     a new label shifts no existing stream, so a seed committed to
//     tests/testdata/sim_seeds.txt reproduces its run forever.
//
//  2. **Cross-platform byte-identity.** std::mt19937_64's output sequence is
//     pinned by the standard, but the <random> *distributions* are not —
//     libstdc++ and libc++ disagree on uniform_int_distribution — so this
//     header does its own bounded sampling and never touches a standard
//     distribution.

#include <cstdint>
#include <random>
#include <string_view>

namespace kds::sim {

// FNV-1a over the seed's eight bytes then the label's bytes. Nothing here
// needs cryptographic strength; it needs to be spelled out in full so the
// mapping (seed, label) -> stream can never drift with a library update.
inline std::uint64_t ForkSeed(std::uint64_t seed, std::string_view label) {
    constexpr std::uint64_t kOffset = 0xcbf29ce484222325ull;
    constexpr std::uint64_t kPrime = 0x100000001b3ull;
    std::uint64_t h = kOffset;
    for (int i = 0; i < 8; ++i) {
        h ^= (seed >> (i * 8)) & 0xff;
        h *= kPrime;
    }
    for (const char c : label) {
        h ^= static_cast<std::uint8_t>(c);
        h *= kPrime;
    }
    // splitmix64 finalizer: FNV alone is weak in its low bits, and the low
    // bits are exactly what seeds mt19937_64's state initialization.
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

class Rng {
public:
    explicit Rng(std::uint64_t seed) : seed_(seed), gen_(seed) {}

    // Forks derive from the *seed*, never from consumed generator state, so
    // fork order and consumption order are both irrelevant to the child's
    // stream. Sub-forking (root -> "iteration/3" -> "workload") composes.
    Rng Fork(std::string_view label) const { return Rng(ForkSeed(seed_, label)); }

    std::uint64_t seed() const { return seed_; }

    // Raw 64 bits — the standard-pinned mt19937_64 sequence.
    std::uint64_t Next() { return gen_(); }

    // Uniform in [0, n). Rejection sampling, not modulo: bias-free and
    // implementation-independent. n = 0 is a caller bug answered with 0
    // rather than a division fault mid-simulation.
    std::uint64_t Below(std::uint64_t n) {
        if (n == 0) return 0;
        const std::uint64_t limit = ~0ull - ~0ull % n;
        std::uint64_t v;
        do {
            v = gen_();
        } while (v >= limit);
        return v % n;
    }

    // Uniform in [lo, hi], inclusive both ends.
    std::int64_t Range(std::int64_t lo, std::int64_t hi) {
        return lo + static_cast<std::int64_t>(
                        Below(static_cast<std::uint64_t>(hi - lo) + 1));
    }

    // True with probability percent/100.
    bool Chance(std::uint32_t percent) { return Below(100) < percent; }

private:
    std::uint64_t seed_;
    std::mt19937_64 gen_;
};

}  // namespace kds::sim
