#pragma once

#include <cstdint>
#include <limits>

#include "kds/sched/clock.hpp"

// The lazy-decay score (docs/spec/physical-optimizer.md R1, §3).
//
// One `{score, last_bump}` pair per scored thing; exponential half-life
// decay computed *lazily* — a touch decays-then-increments, a read decays
// without storing, and there is no background decay pass. Idle data costs
// nothing and is never visited; that is the design, not an optimization.
//
// **This is the one decay implementation.** Declared consumers: hot/cold
// classification in the physical-optimizer planner (workplan PX05),
// trail-retention ordering (docs/spec/pattern-tracking-levels.md), and
// EV1's experimental temperature hook (docs/spec/eviction.md). A second
// decay formula anywhere is the same defect a second literal-coercion path
// was (docs/spec/types.md §3.1). **Nothing consumes this yet** — the
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

// Accumulate: decay to now, add `points` whole points, store. Saturating
// at the top — a score that cannot go higher stays at the ceiling rather
// than wrapping to cold, for the reason every aggregate in this engine
// checks: a wrap is a wrong answer wearing a plausible one's clothes.
//
// The N-point form exists for the quantity signals (feat-physical-optimizer
// §II.2 S2: pages scanned per execution) — a decayed *sum* kept beside a
// decayed *count* yields a decayed mean as their ratio, both halves
// decaying on the same clock so the ratio is stable under idleness.
inline void Accumulate(DecayState& s, const sched::Clock* clock, sched::MonoTimeNs half_life_ns,
                       std::uint32_t points) {
    std::uint32_t decayed = s.scaled;
    if (clock != nullptr) {
        const sched::MonoTimeNs now = clock->Now();
        decayed = DecayedScaledAt(s, now, half_life_ns);
        // A backwards clock keeps the later stamp: decaying future reads
        // from the earlier of the two points would double-charge the gap.
        if (now > s.last_bump) s.last_bump = now;
    }
    const std::uint64_t added =
        std::uint64_t{decayed} + std::uint64_t{points} * kDecayScoreScale;
    s.scaled = added > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(added);
}

// Touch: Accumulate's one-point form, the common case.
inline void Touch(DecayState& s, const sched::Clock* clock, sched::MonoTimeNs half_life_ns) {
    Accumulate(s, clock, half_life_ns, 1);
}

// ---- The range-preserving read (2026-08-10) ------------------------------
//
// `ValueAt` answers in Q24.8, so a score **underflows to zero after about
// 16 half-lives** of silence at realistic magnitudes (a single touch is
// 256 scaled units, and 256 >> 8 is 1). Past that point every idle score
// reads exactly 0 and no comparison can order them - which cost the cabin
// optimizer the thing it most wanted from a decayed score: a Cabin worth
// 2^30 should take ~30 half-lives to fall below a threshold, not 16.
// Measured as "for 89% of the DECAYING dwell the DROP is a timeout, not a
// judgement" (`bench/results-cabin-optimizer-days.md`).
//
// **The information was never lost in the state** - only in the read.
// `scaled` and `last_bump` still hold the whole history; it is the
// `>> halvings` inside `DecayedScaledAt` that flushes it. So the fix is an
// accessor, not a format: `Log2Q16` answers in the log domain, where decay
// is a **subtraction** and therefore exact and unbounded - a score stays
// ordered against its peers for thousands of half-lives.
//
// Consumers that only rank live data keep using `ValueAt` unchanged; this
// is for the ones that must still tell two cold things apart.

// Signed log2 of a decayed score, in Q16.16 (16 fractional bits). The
// value is log2 of the **scaled** (Q24.8) score, so 0 means one scaled
// unit and 8*65536 means one whole point - callers compare these against
// each other and against log2 of their own thresholds, never against a
// linear score.
using Log2Q16 = std::int64_t;

// A zero score's log2 is negative infinity; this stands in for it and is
// below every representable result (the most negative real answer at u32
// magnitudes and a u64 elapsed is far above it). Comparing it works
// naturally: nothing is colder than a score that was never touched.
inline constexpr Log2Q16 kLog2NegInf = std::numeric_limits<std::int64_t>::min() / 2;

// log2(1 + k/64) in Q16, k = 0..63 - the fractional part of a log2, LUT'd
// on the six bits below the leading one. Worst-case error is 0.78% in the
// ratio it represents, which is under a tenth of the narrowest threshold
// margin any consumer applies (theta_drop = 0.5 against theta_create = 3).
inline constexpr std::uint32_t kLog2FractionQ16[64] = {
    0, 1466, 2909, 4331, 5732, 7112, 8473, 9814,
    11136, 12440, 13727, 14996, 16248, 17484, 18704, 19909,
    21098, 22272, 23433, 24579, 25711, 26830, 27936, 29029,
    30109, 31178, 32234, 33279, 34312, 35334, 36346, 37346,
    38336, 39316, 40286, 41246, 42196, 43137, 44068, 44990,
    45904, 46809, 47705, 48593, 49472, 50344, 51207, 52063,
    52911, 53751, 54584, 55410, 56229, 57040, 57845, 58643,
    59434, 60219, 60997, 61769, 62534, 63294, 64047, 64794,
};

// log2 of a positive integer, Q16.16. Zero answers kLog2NegInf.
inline Log2Q16 Log2OfQ16(std::uint64_t value) noexcept {
    if (value == 0) return kLog2NegInf;
    // Index of the leading one, then the six bits under it select the
    // fractional bucket - the mantissa normalized into [1, 2).
    int msb = 63;
    while (((value >> msb) & 1u) == 0) --msb;
    const int shift = 63 - msb;
    const std::uint64_t mantissa = value << shift;  // in [2^63, 2^64)
    const std::uint32_t bucket = static_cast<std::uint32_t>((mantissa >> 57) & 63u);
    return (static_cast<Log2Q16>(msb) << 16) + kLog2FractionQ16[bucket];
}

// The decayed score at `now`, in the log domain: log2(scaled) minus the
// elapsed half-lives. **Never underflows** - decay is a subtraction here,
// so two scores idle for a century still order correctly by how large
// they were when the silence began. Null clock, or a clock that went
// backwards, reads as no elapsed time, matching `DecayedScaledAt`.
inline Log2Q16 DecayedLog2At(const DecayState& s, sched::MonoTimeNs now,
                             sched::MonoTimeNs half_life_ns) noexcept {
    if (s.scaled == 0) return kLog2NegInf;
    Log2Q16 result = Log2OfQ16(s.scaled);
    if (half_life_ns == 0 || now <= s.last_bump) return result;
    const std::uint64_t elapsed = now - s.last_bump;
    // Whole and fractional half-lives separately: the whole part would
    // overflow a Q16.16 shift for long idles, and the fraction needs the
    // division anyway.
    const std::uint64_t whole = elapsed / half_life_ns;
    const std::uint64_t frac_ns = elapsed % half_life_ns;
    const Log2Q16 frac_q16 =
        static_cast<Log2Q16>((frac_ns << 16) / half_life_ns);
    // Saturate rather than wrap on an absurd idle: still colder than
    // anything real, which is the only property that matters.
    if (whole > (1ULL << 40)) return kLog2NegInf;
    return result - (static_cast<Log2Q16>(whole) << 16) - frac_q16;
}

// Read: the decayed log2 score now. Null clock = the raw score's log2,
// the counter degradation `ValueAt` also performs.
inline Log2Q16 Log2ValueAt(const DecayState& s, const sched::Clock* clock,
                           sched::MonoTimeNs half_life_ns) noexcept {
    if (clock == nullptr) return Log2OfQ16(s.scaled);
    return DecayedLog2At(s, clock->Now(), half_life_ns);
}

}  // namespace kds::stats
