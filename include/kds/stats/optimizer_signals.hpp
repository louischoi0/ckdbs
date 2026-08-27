#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "kds/sched/clock.hpp"
#include "kds/stats/decay.hpp"

// The cabin optimizer's input signals, S1-S3, and the snapshot they are
// read through (docs/spec/physical-optimizer.md §II.2/§II.3, workplan
// PHY01).
//
// One core-local collector, three signals:
//
//   S1  per-fingerprint execution frequency - a decayed count, touched
//       once per successful fingerprinted SELECT.
//   S2  pages scanned per execution, per fingerprint - a decayed *sum*
//       kept beside S1's decayed count, so the decayed mean is their
//       ratio and both halves idle on the same clock. The pages come from
//       `StepStats::pages_fetched`, the executor counter built for this.
//   S3  Cabin quality, per cabin - decayed lookups, hint failures and
//       coverage misses, forwarded by `CabinStore` from the read path's
//       existing counting sites ("some exist via hint-heal accounting;
//       unify and expose" - this is the unification).
//
// ---- What this must never do ---------------------------------------------
//
// Fail a statement. Every recording call returns void and drops on any
// internal refusal - the same contract every stats collector here honors.
// The maps are **bounded**: a full fingerprint table evicts its coldest
// entry (lowest decayed score) to admit a hotter newcomer, which merely
// restarts that fingerprint's evidence - a performance event, the
// TrailRecorder-sightings stance. The caps are `[PROPOSED]` and nothing
// may depend on the numbers, only on the rule: **eviction restarts
// counting, it never fabricates it.**
//
// ---- The snapshot (PO10's determinism seam) -------------------------------
//
// `Snapshot()` is the **only** read the decision core will ever perform:
// an immutable by-value aggregation, versioned, stamped with the decay
// epoch (the clock reading every decayed value was computed at), entries
// sorted by id so identical states produce byte-identical snapshots.
// Construction is a single home-core step - core-local data, cooperative
// scheduling, no locks (rules.md #3). PHY02's `Decide` takes the snapshot
// and nothing else.
//
// Concurrency: core-local, no synchronization of its own.

namespace kds::stats {

// Fingerprints tracked before the coldest is evicted to admit a new one.
// `[PROPOSED]` 4096 - the sibling caps' number (kMaxSightings,
// cabin_max_values), for the sibling reason: a backstop against an
// unbounded axis, not a tuning target.
inline constexpr std::size_t kMaxTrackedFingerprints = 4096;

// Cabins tracked. Cabin ids are catalog-bounded in practice; the cap is
// the same backstop shape.
inline constexpr std::size_t kMaxTrackedCabins = 4096;

// Which (relation, column) a fingerprint's cabin-servable predicate
// targets - the linkage §II.4's Σ_i needs, since B(c) sums fingerprints
// *served by candidate c* and a bare pattern_id names no column. Derived
// by the dispatcher from the compiled chain (a kCabinProbe step, or the
// first kFilterScan's filtered column); `cabin_id` is non-zero when an
// existing Cabin already serves the shape. Zero rel_oid means the shape
// has no cabin candidacy (a pk lookup, a bare scan) - still counted, so
// S1 stays the workload's whole heat map, but no CREATE prices it.
struct CandidateRef {
    std::uint64_t rel_oid = 0;
    std::uint16_t col_pos = 0;
    std::uint64_t cabin_id = 0;

    bool valid() const noexcept { return rel_oid != 0; }
};

// One fingerprint's S1/S2 state: the decayed execution count and the
// decayed page sum. Their ratio is the decayed mean pages-per-execution.
struct FingerprintSignal {
    DecayState executions;
    DecayState pages;
    // Last-observed candidacy. A shape's target is a function of the
    // shape, so overwriting is idempotent in the steady state; it moves
    // only when the catalog under the shape moved (a Cabin created or
    // dropped), and then the newest answer is the right one.
    CandidateRef candidate;
};

// One Cabin's S3 state.
struct CabinQualitySignal {
    DecayState lookups;           // every probe, served or not
    DecayState hint_failures;     // C6 hints that did not verify
    DecayState coverage_misses;   // probes for a value nothing observed
};

// ---- The snapshot ---------------------------------------------------------

struct SnapshotFingerprint {
    std::uint64_t pattern_id = 0;
    std::uint32_t frequency_q8 = 0;  // decayed executions, Q24.8
    std::uint32_t pages_q8 = 0;      // decayed page sum, Q24.8

    // The same two values in the log domain (decay.hpp's `Log2Q16`),
    // carried beside the linear pair rather than replacing it. The linear
    // fields keep every existing consumer and every measured threshold
    // decision exactly as they were; these two exist because the linear
    // ones **underflow to zero after ~16 half-lives** of silence, and a
    // controller deciding retirement needs to tell one cold shape from
    // another long after both read 0. Decay is a subtraction here, so
    // they stay ordered for as long as anyone will ever wait.
    Log2Q16 frequency_log2 = kLog2NegInf;
    Log2Q16 pages_log2 = kLog2NegInf;

    CandidateRef candidate;          // zero rel_oid = no cabin candidacy
};

struct SnapshotCabin {
    std::uint64_t cabin_id = 0;
    std::uint32_t lookups_q8 = 0;
    std::uint32_t hint_failures_q8 = 0;
    std::uint32_t coverage_misses_q8 = 0;
};

// Immutable by construction of its use: handed out by value, and the
// decision core takes it by const reference. The division S2's mean needs
// is deliberately left to the consumer - PHY02's core works in its own
// fixed point (16.16), and dividing here would round twice.
struct OptimizerSnapshot {
    std::uint64_t version = 0;         // increments per Snapshot() call
    sched::MonoTimeNs decay_epoch = 0; // the clock reading of every value
    std::vector<SnapshotFingerprint> fingerprints;  // sorted by pattern_id
    std::vector<SnapshotCabin> cabins;              // sorted by cabin_id
};

class OptimizerSignals {
public:
    // The clock and half-life bind at construction - the one site config
    // threads them through (expeditor.cpp). A null clock degrades every
    // signal to a raw counter, decay.hpp's stated stance.
    OptimizerSignals(const sched::Clock* clock, sched::MonoTimeNs half_life_ns) noexcept
        : clock_(clock), half_life_ns_(half_life_ns) {}

    // S1 + S2: one successful fingerprinted execution that read
    // `pages_fetched` pages, targeting `candidate` if the shape has cabin
    // candidacy. Never fails; a full table evicts its coldest fingerprint
    // to admit this one.
    void NoteExecution(std::uint64_t pattern_id, std::uint64_t pages_fetched,
                       CandidateRef candidate = {});

    // S3, forwarded by CabinStore: a probe against `cabin_id`. `served`
    // false is a coverage miss - the value was not observed.
    void NoteCabinLookup(std::uint64_t cabin_id, bool served);

    // S3: a C6 location hint's verification outcome.
    void NoteCabinHint(std::uint64_t cabin_id, bool ok);

    // The one read (see the header). A single home-core step.
    OptimizerSnapshot Snapshot();

    // A single Cabin's decayed S3 state, for the inspection surface
    // (workplan PHY06's `SHOW CABIN_OPTIMIZER`). Const and version-silent
    // where `Snapshot()` is neither: a SHOW must not advance the version
    // sequence the decision log's digests are named in, or two operator
    // reads would make two ticks' records look one snapshot apart. An
    // untracked id answers zeros - indistinguishable from "tracked and
    // fully decayed", which is the honest reading either way.
    SnapshotCabin QualityOf(std::uint64_t cabin_id) const;

    std::size_t tracked_fingerprints() const noexcept { return fingerprints_.size(); }
    std::size_t tracked_cabins() const noexcept { return cabins_.size(); }

private:
    // The entry for `id`, evicting the coldest when the map is full and
    // `id` is new. O(n) scan on the full-and-new case only - the map is
    // capped at kMaxTracked*, and the common case is a hash probe.
    FingerprintSignal* FingerprintFor(std::uint64_t pattern_id);
    CabinQualitySignal* CabinFor(std::uint64_t cabin_id);

    const sched::Clock* clock_;
    sched::MonoTimeNs half_life_ns_;
    std::uint64_t version_ = 0;
    std::unordered_map<std::uint64_t, FingerprintSignal> fingerprints_;
    std::unordered_map<std::uint64_t, CabinQualitySignal> cabins_;
};

}  // namespace kds::stats
