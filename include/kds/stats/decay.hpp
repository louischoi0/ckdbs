#pragma once

#include <cstdint>

#include "kds/sched/clock.hpp"

// The lazy-decay score (docs/feat-physical-optimizer.md R1, §3).
//
// One `{score, last_bump}` pair per scored thing; exponential half-life
// decay computed *lazily* — a touch decays-then-increments, a read decays
// without storing, and there is no background decay pass. Idle data costs
// nothing and is never visited; that is the design, not an optimization.
//
// **This is the one decay implementation.** Declared consumers: hot/cold
// classification in the physical-optimizer planner (workplan PX05),
// trail-retention ordering (docs/spec-pattern-tracking-levels.md), and
// EV1's experimental temperature hook (docs/spec-eviction.md). A second
// decay formula anywhere is the same defect a second literal-coercion path
// was (docs/spec-types.md §3.1). **Nothing consumes this yet** — the
// planner (PX05) is the first caller, and the header exists ahead of it
// for the reason the eviction sweep does: so the property is testable now.
//
// Time comes from the injected `sched::Clock` (rules.md #4 — no
// std::chrono in engine logic). **With no clock the score degrades to a
// raw counter** — the same best-effort stance `sys.access_stats.last_seen`
// takes — so a caller without a clock still gets a usable ordering, just
// not a time-weighted one.
//
// Precision contract: exact at whole half-lives (halving is a shift), and
// between them the fractional factor is bucketed (16 buckets per
// half-life), so a read may *overestimate* by at most 2^(1/16) - 1, about
// 4.4%. This is a ranking score; the tolerance is deliberate and the
// acceptance tests pin the exact points, not the buckets.
//
// Concurrency: pure functions over a caller-owned pair. Core-local like
// every stats structure — no atomics, no locks; the owning core's event
// loop is the serialization.

namespace kds::stats {

// Fixed-point scale for the score: Q24.8, i.e. 8 fractional bits, so one
// touch adds `kDecayScoreScale` and the fractional decay factor keeps
// resolution without floating point (rules.md: none on statement paths).
// 256 because the factor table below is itself Q8, making the multiply
// `(score * factor) >> 8` with no intermediate overflow: score < 2^32,
// factor <= 2^8, product < 2^40, comfortably inside uint64.
inline constexpr std::uint32_t kDecayScoreScale = 256;

// How many buckets one half-life's fraction is quantized into. 16 as a
// power of two so the bucket index is one multiply and one divide, and
// because 16 puts the worst-case overestimate at 2^(1/16) - 1 ≈ 4.4% —
// under the noise floor of anything a ranking consumer would act on.
inline constexpr std::uint32_t kDecayFractionBuckets = 16;

// Q8 factors for the fractional part: round(256 * 2^(-k/16)) for
// k = 0..15. Entry 0 is exactly 256 (no fraction, no error), which is what
// makes whole-half-life reads exact.
inline constexpr std::uint16_t kDecayFractionQ8[kDecayFractionBuckets] = {
    256, 245, 235, 225, 215, 206, 197, 189,
    181, 173, 166, 159, 152, 146, 140, 134,
};

// The score's bit width. A score shifted by this many halvings is zero,
// and the guard exists because >> by >= the width is undefined behaviour,
// not merely zero.
inline constexpr std::uint32_t kDecayScoreBits = 32;

struct DecayState {
    std::uint32_t scaled = 0;            // score in Q24.8
    sched::MonoTimeNs last_bump = 0;     // clock reading at the last Touch
};

// The decayed score at `now`, in Q24.8. Pure; stores nothing.
//
// Defensive by documented choice: `half_life_ns == 0` means no decay (the
// config layer refuses 0, so this arm is unreachable through a real
// server), and `now <= last_bump` reads as no elapsed time — a monotonic
// clock does not go backwards, and a manual one that does must not make a
// score *grow*.
constexpr std::uint32_t DecayedScaledAt(const DecayState& s, sched::MonoTimeNs now,
                                        sched::MonoTimeNs half_life_ns) {
    if (half_life_ns == 0) return s.scaled;
    if (now <= s.last_bump) return s.scaled;
    const std::uint64_t elapsed = now - s.last_bump;
    const std::uint64_t halvings = elapsed / half_life_ns;
    if (halvings >= kDecayScoreBits) return 0;
    const std::uint64_t whole = std::uint64_t{s.scaled} >> halvings;
    // Fractional bucket: frac * buckets / half_life is exact integer math
    // while `half_life_ns * buckets` fits uint64; past that (a half-life
    // over ~36 years) the fraction is dropped rather than overflowed.
    std::uint64_t bucket = 0;
    const std::uint64_t frac = elapsed % half_life_ns;
    if (half_life_ns <= UINT64_MAX / kDecayFractionBuckets) {
        bucket = (frac * kDecayFractionBuckets) / half_life_ns;
    }
    return static_cast<std::uint32_t>((whole * kDecayFractionQ8[bucket]) >> 8);
}

// Read: the decayed score now, in Q24.8. Null clock = raw counter.
inline std::uint32_t ValueAt(const DecayState& s, const sched::Clock* clock,
                             sched::MonoTimeNs half_life_ns) {
    if (clock == nullptr) return s.scaled;
    return DecayedScaledAt(s, clock->Now(), half_life_ns);
}

// Touch: decay to now, add one point, store. Saturating at the top —
// a score that cannot go higher stays at the ceiling rather than wrapping
// to cold, for the reason every aggregate in this engine checks: a wrap
// is a wrong answer wearing a plausible one's clothes.
inline void Touch(DecayState& s, const sched::Clock* clock, sched::MonoTimeNs half_life_ns) {
    std::uint32_t decayed = s.scaled;
    if (clock != nullptr) {
        const sched::MonoTimeNs now = clock->Now();
        decayed = DecayedScaledAt(s, now, half_life_ns);
        // A backwards clock keeps the later stamp: decaying future reads
        // from the earlier of the two points would double-charge the gap.
        if (now > s.last_bump) s.last_bump = now;
    }
    s.scaled = (decayed <= UINT32_MAX - kDecayScoreScale) ? decayed + kDecayScoreScale
                                                          : UINT32_MAX;
}

}  // namespace kds::stats
