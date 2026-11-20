#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/sched/clock.hpp"
#include "kds/stats/decay.hpp"

// The cabin optimizer's input signals, S1-S3, and the snapshot they are
// read through (docs/spec/cabin-controller.md §II.2/§II.3, workplan
// PHY01).
//
// One collector for the instance (AT-S8), three signals:
//
//   S1  execution frequency - a decayed count, touched once per
//       successful fingerprinted SELECT - **per fingerprint and candidate**
//       (`SignalKey`, DI-S2): a fingerprint is a text, and one text is as
//       many statements as the relations it resolves to.
//   S2  pages scanned per execution, on the same key - a decayed *sum*
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
// The maps are **bounded**: a full signal table evicts its coldest
// entry (lowest decayed score) to admit a hotter newcomer, which merely
// restarts that signal's evidence - a performance event, the
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
// Construction is one step under the collector's latch (the concurrency
// note below). PHY02's `Decide` takes the snapshot and nothing else.
//
// Concurrency: **the instance's, latched above one core** (AT-S8,
// `SetLatch`). Every core's dispatcher notes executions into it and the one
// Cabin store forwards every core's lookups. Each public method holds the
// latch for its body; it is innermost, taken holding nothing but the controller view's
// latch (`OptimizerSurface::view_latch`, outer, across a tick).

namespace kds::stats {

// Execution signals tracked before the coldest is evicted to admit a new
// one. `[PROPOSED]` 4096 - the sibling caps' number (kMaxSightings,
// cabin_max_values), for the sibling reason: a backstop against an
// unbounded axis, not a tuning target. **It bounds signals, not patterns**
// (DI-S2): a pattern has one signal per candidate it was seen to target.
inline constexpr std::size_t kMaxTrackedFingerprints = 4096;

// Cabins tracked. Cabin ids are catalog-bounded in practice; the cap is
// the same backstop shape.
inline constexpr std::size_t kMaxTrackedCabins = 4096;

// Which (relation, column) a fingerprint's cabin-servable predicate
// targets - the linkage §II.4's Σ_i needs, since B(c) sums fingerprints
// *served by candidate c* and a bare pattern_id names no column. Derived
// by the dispatcher from the compiled chain (a kCabinProbe step, or the
// first kFilterScan's filtered column, each first in written-position order
// since CC-Q15 - a planned join's executed order never decides which; the
// order still decides each step's kind); `cabin_id` is non-zero when an
// existing Cabin already serves the shape. Zero rel_oid means the shape
// has no cabin candidacy (a tuple key lookup, a bare scan) - still counted, so
// S1 stays the workload's whole heat map, but no CREATE prices it.
struct CandidateRef {
    std::uint64_t rel_oid = 0;
    std::uint16_t col_pos = 0;
    std::uint64_t cabin_id = 0;

    bool valid() const noexcept { return rel_oid != 0; }
};

// **What an execution signal is keyed by: the pattern and the candidate it
// targeted, together** (DI-S2,
// `instructions/v3.2.0/workorder-di-shape-identity.md` DI-Q12 (a)). A
// pattern id is a hash of the statement's text and holds no namespace and
// no relation oid, so one pattern is two statements wherever a name in it
// resolves to two relations - under two namespaces, or over a relation
// dropped and created again. Keyed by the pattern alone, the heat of both
// was one sum credited to whichever candidate ran last, and the controller
// priced one relation's Cabin with another relation's executions. With the
// candidate in the key each `(relation, column)` is credited the
// executions that targeted it and no others. `{0, 0}` is a pattern's
// executions with no candidacy: pure heat, priced by no decision.
struct SignalKey {
    std::uint64_t pattern_id = 0;
    std::uint64_t rel_oid = 0;
    std::uint16_t col_pos = 0;

    bool operator==(const SignalKey&) const = default;
};

struct SignalKeyHash {
    std::size_t operator()(const SignalKey& k) const noexcept;
};

// One signal's S1/S2 state: the decayed execution count and the decayed
// page sum. Their ratio is the decayed mean pages-per-execution.
struct FingerprintSignal {
    DecayState executions;
    DecayState pages;
    // The candidate the key names, with the Cabin that serves it as last
    // observed: `cabin_id` moves when a Cabin on the column is created or
    // dropped, and then the newest answer is the right one.
    CandidateRef candidate;
};

// S4 (CJ's WE13, `[EXPERIMENTAL]`), and how many signals a pattern has.
// **A wait sample is a pattern's, not a candidate's**: the sampler knows
// the session's published pattern and nothing the statement bound (DI-Q9
// (a)), so the sample is kept beside the pattern's signals and credited to
// none of them. Read by nothing that decides.
struct PatternWaits {
    DecayState wait_samples;
    std::uint32_t signals = 0;
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

// S4: the decayed count of ASH samples of a pattern that were WAITING, one
// row a pattern with a signal. `Decide` does not read it (no cost model
// prices a wait), so it moves no decision and no digest
// (`{version, decay_epoch}`).
struct SnapshotPatternWaits {
    std::uint64_t pattern_id = 0;
    std::uint32_t wait_samples_q8 = 0;
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
    // Sorted by (pattern_id, candidate relation, candidate column).
    std::vector<SnapshotFingerprint> fingerprints;
    std::vector<SnapshotPatternWaits> waits;        // sorted by pattern_id
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
    // candidacy - the signal of `pattern_id` and that candidate. Never
    // fails; a full table evicts its coldest signal to admit this one.
    void NoteExecution(std::uint64_t pattern_id, std::uint64_t pages_fetched,
                       CandidateRef candidate = {});

    // S4 (CJ's WE13): one ASH sample of `pattern_id` found WAITING. Counted
    // only for a pattern with a signal (a wait never admits or evicts one)
    // and only while `gate` is true: the controller's own switch.
    void NoteWaitSample(std::uint64_t pattern_id) noexcept;
    void set_wait_gate(const std::atomic<bool>* gate) noexcept { wait_gate_ = gate; }

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

    std::size_t tracked_signals() const noexcept {
        LatchGuard hold(latch_);
        return fingerprints_.size();
    }
    std::size_t tracked_cabins() const noexcept {
        LatchGuard hold(latch_);
        return cabins_.size();
    }

    // The latch every method holds (the header's concurrency note); null,
    // the default, at one core. `latch` must outlive this.
    void SetLatch(Latch* latch) noexcept { latch_ = latch; }

private:
    // The entry for `key`, evicting the coldest when the map is full and
    // `key` is new. O(n) scan on the full-and-new case only - the map is
    // capped at kMaxTracked*, and the common case is a hash probe.
    FingerprintSignal* FingerprintFor(const SignalKey& key);
    CabinQualitySignal* CabinFor(std::uint64_t cabin_id);

    const sched::Clock* clock_;
    sched::MonoTimeNs half_life_ns_;
    Latch* latch_ = nullptr;
    const std::atomic<bool>* wait_gate_ = nullptr;
    std::uint64_t version_ = 0;
    std::unordered_map<SignalKey, FingerprintSignal, SignalKeyHash> fingerprints_;
    // One entry per pattern with a signal in `fingerprints_`.
    std::unordered_map<std::uint64_t, PatternWaits> patterns_;
    std::unordered_map<std::uint64_t, CabinQualitySignal> cabins_;
};

}  // namespace kds::stats
