#pragma once

#include <cstdint>

// The cycle guard of a walk over links: a chain of pages, an undo chain.
//
// **A cycle check, never a length limit.** A legal chain may be longer than
// any budget a walk would set itself - a var-heap chain, a relation's leaf
// chain and a transaction's undo chain all grow with the data - and a
// budget refuses such a chain as `Corruption` on every walk, a capacity
// limit read as damage (the audit's G2, G16 and G19). Brent's check
// instead: the id reached at each power-of-two step is remembered, and
// meeting it again is a cycle, found within about three times the cycle's
// length plus its tail. No acyclic chain revisits an id, so a legal chain
// is never refused, whatever its length, and every cycle is.
//
// The guard is sound over any walk that never legally reaches an id it has
// already visited - a chain that grows only at its tail, a chain fixed
// under the walk, a leaf chain whose splits link only new pages. Each user
// states why its walk is one.
//
// Concurrency: none; one walk owns one guard.

namespace kds {

template <typename Id>
class CycleGuard {
public:
    // `start` is the id the walk begins at.
    explicit constexpr CycleGuard(Id start) noexcept : mark_(start) {}

    // True when the hop to `next` meets the remembered id: the links form a
    // cycle. Called once per hop, with the id the walk is about to visit.
    constexpr bool Revisits(Id next) noexcept {
        if (next == mark_) return true;
        if (++lap_ == power_) {
            mark_ = next;
            power_ <<= 1;
            lap_ = 0;
        }
        return false;
    }

private:
    Id mark_;
    std::uint64_t lap_ = 0;
    std::uint64_t power_ = 1;
};

}  // namespace kds
