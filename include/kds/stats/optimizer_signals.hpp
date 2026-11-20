#pragma once

#include <atomic>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/sched/clock.hpp"
#include "kds/stats/decay.hpp"

// The cabin optimizer's input signals, S1-S3, and the snapshot they are
// read through (docs/spec/cabin-controller.md §II.2/§II.3, workplan
// PHY01).
//
// One collector for the instance (AT-S8), a part a core (DL-S1), three
// signals:
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
// Fail a statement, **or make one wait** (`docs/rules/rules.md` §3a). Every
// recording call returns void and drops on any internal refusal - the same
// contract every stats collector here honors - and takes nothing a
// statement on another core can hold.
//
// ---- Where a note goes: a part a core, folded by the consumer (DL-S1) ------
//
// **A note writes its own core's part and nothing else.** A part is three
// fixed tables - execution signals, a pattern's wait samples, Cabins -
// allocated whole at the core's first note and never grown, each entry the
// decayed pairs of what the core noted since the last fold. The part's
// latch is taken by the **try** form: the one other taker is the consumer's
// fold, and a note that meets it is dropped and counted, never waited out.
// A new key past half a table is dropped and counted the same way; the fold
// empties the table, so the bound is on the signals one core notes between
// two folds.
//
// **The consumer folds.** `Fold()` - the controller's tick, through
// `Snapshot()` - takes each part in core order, copies its entries into
// memory it reserved beforehand, empties the part and lets it go; then,
// holding nothing a statement takes, merges the copies into the folded
// state in key order (`MergeDecay`, `decay.hpp`). The folded maps are
// **bounded** and the fold is where they evict: a full signal table drops
// its coldest entry (lowest decayed score) to admit a hotter newcomer,
// which merely restarts that signal's evidence - a performance event, the
// TrailRecorder-sightings stance. The caps are `[PROPOSED]` and nothing may
// depend on the numbers, only on the rule: **eviction restarts counting, it
// never fabricates it.** Room for a part's newcomers is made in one pass
// over the map - the coldest go together - so a fold's cost is a scan a
// part, not a scan a newcomer: the tick runs on a reactor. For the same
// reason a part the fold finds busy is left to the next fold (DL-Q17): the
// tick waits for nothing either.
//
// **A switched-off consumer collects nothing** (`rules.md` §3a's second
// rule): every note asks the gate - the controller's own switch, one
// relaxed load - before anything else, and the dispatcher asks it before it
// builds an identity or a candidate on the collector's behalf. What was
// folded before a switch-off stays and decays by the clock (DL-Q13 (a)).
//
// ---- The snapshot (PO10's determinism seam) -------------------------------
//
// `Snapshot()` is the **only** read the decision core will ever perform:
// an immutable by-value aggregation, versioned, stamped with the decay
// epoch (the clock reading every decayed value was computed at), entries
// sorted by id so identical states produce byte-identical snapshots. It
// folds first, so the same notes in the same per-core order give the same
// snapshot (DL-R5). PHY02's `Decide` takes the snapshot and nothing else.
//
// Concurrency: the notes as above. **The folded state has no latch: it is
// the consumer's alone**, touched by `Fold`, `Snapshot`, `QualityOf` and
// the `tracked_*` counts on the tick's thread and by nothing else - a view
// reads the quality the tick published (`exec/cabin_optimizer_view.hpp`,
// DL-S3), never this state.

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
    // An order: what makes a fold's merge, and an eviction's tie, the same
    // whatever a hash table's iteration yields (DL-R5).
    auto operator<=>(const SignalKey&) const = default;
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
    // What one core's part holds between two folds, live: half its slots.
    // `[PROPOSED]`, a backstop as the caps above are.
    static constexpr std::size_t kNotedSignalsPerCore = 1024;
    static constexpr std::size_t kNotedWaitsPerCore = 256;
    static constexpr std::size_t kNotedCabinsPerCore = 256;
    // A fold makes room for one part's newcomers at a time, so a part's
    // worth must fit the folded map it lands in.
    static_assert(kNotedSignalsPerCore <= kMaxTrackedFingerprints);
    static_assert(kNotedCabinsPerCore <= kMaxTrackedCabins);

    // The clock and half-life bind at construction - the one site config
    // threads them through (expeditor.cpp). A null clock degrades every
    // signal to a raw counter, decay.hpp's stated stance. `cores` is the
    // parts: a note names its core, and one past the last part is dropped.
    // **The part latches are armed above one core** (G2): at one core the
    // notes and the fold are one thread's.
    OptimizerSignals(const sched::Clock* clock, sched::MonoTimeNs half_life_ns,
                     std::uint32_t cores = 1);
    ~OptimizerSignals();
    OptimizerSignals(const OptimizerSignals&) = delete;
    OptimizerSignals& operator=(const OptimizerSignals&) = delete;

    // The consumer's switch (PO8). Null, the default, collects always: a
    // collector built with no controller's switch beside it.
    void set_gate(const std::atomic<bool>* gate) noexcept { gate_ = gate; }
    // Whether a note would be taken: what a caller asks before it builds
    // anything on the collector's behalf. One relaxed load (DL-R10).
    bool collecting() const noexcept {
        return gate_ == nullptr || gate_->load(std::memory_order_relaxed);
    }

    // ---- A statement's path: `core`'s part only ----------------------------

    // S1 + S2: one successful fingerprinted execution that read
    // `pages_fetched` pages, targeting `candidate` if the shape has cabin
    // candidacy - the signal of `pattern_id` and that candidate (DI-S2).
    // Never fails and never waits.
    void NoteExecution(std::uint32_t core, std::uint64_t pattern_id, std::uint64_t pages_fetched,
                       CandidateRef candidate = {}) noexcept;

    // S4 (CJ's WE13): one ASH sample of `pattern_id` found WAITING. A
    // pattern's, not a candidate's (DI-Q9 (a)). Folded only for a pattern
    // with a signal by the time its part is folded - one the folded state
    // tracks, or that this part noted an execution of since the last fold:
    // a wait alone never admits or evicts one. Parts fold in core order, so
    // a wait noted on a core below the one that first ran its pattern in the
    // same interval is not counted. A waiting pattern takes a slot of its
    // part's wait table whether or not it ever has a signal - the sampler
    // notes a waiting write's too - and one past the table's bound is a
    // `full` drop.
    void NoteWaitSample(std::uint32_t core, std::uint64_t pattern_id) noexcept;

    // S3, forwarded by CabinStore: a probe against `cabin_id`. `served`
    // false is a coverage miss - the value was not observed.
    void NoteCabinLookup(std::uint32_t core, std::uint64_t cabin_id, bool served) noexcept;

    // S3: a C6 location hint's verification outcome.
    void NoteCabinHint(std::uint32_t core, std::uint64_t cabin_id, bool ok) noexcept;

    // ---- The consumer: one thread at a time (the header) -------------------

    // Every part's notes merged into the folded state, parts in core order
    // and entries by key. A part found busy keeps its notes for the next
    // fold.
    void Fold();

    // The one read (see the header): folds, then aggregates.
    OptimizerSnapshot Snapshot();

    // A single Cabin's decayed S3 state as last folded, for the inspection
    // surface (workplan PHY06's `SHOW CABIN_OPTIMIZER`). Const and
    // version-silent where `Snapshot()` is neither: a SHOW must not advance
    // the version sequence the decision log's digests are named in, or two
    // operator reads would make two ticks' records look one snapshot apart
    // - and it folds nothing, for the same reason. An untracked id answers
    // zeros - indistinguishable from "tracked and fully decayed", which is
    // the honest reading either way.
    SnapshotCabin QualityOf(std::uint64_t cabin_id) const;

    std::size_t tracked_signals() const noexcept { return fingerprints_.size(); }

    // A part's latch, for a test that holds it as the fold does; null where
    // the parts are unarmed or there is no such part.
    Latch* PartLatchForTest(std::uint32_t core) noexcept {
        return armed_ && core < parts_.size() ? &parts_[core]->latch : nullptr;
    }

    // Notes lost, over every part (DL-R2): `busy`, a note that met the
    // fold's copy of its part; `full`, a new key past its part's bound, a
    // part whose tables could not be allocated, or a core with no part. Any
    // thread.
    struct Drops {
        std::uint64_t busy = 0;
        std::uint64_t full = 0;
    };
    Drops drops() const noexcept;

private:
    // One key's entry in a part: what its core noted since the last fold.
    template <class Key, class Signal>
    struct Noted {
        Key key{};
        Signal signal;
        bool used = false;
    };
    using SignalNote = Noted<SignalKey, FingerprintSignal>;
    using WaitNote = Noted<std::uint64_t, DecayState>;  // a pattern's wait samples
    using CabinNote = Noted<std::uint64_t, CabinQualitySignal>;
    // Twice the live bound, so a probe meets an empty slot.
    static constexpr std::size_t kSignalSlots = 2 * kNotedSignalsPerCore;
    static constexpr std::size_t kWaitSlots = 2 * kNotedWaitsPerCore;
    static constexpr std::size_t kCabinSlots = 2 * kNotedCabinsPerCore;

    struct Part {
        Latch latch{LatchKind::kStatsRing};
        // Under `latch`. Allocated whole at the part's first note.
        std::unique_ptr<SignalNote[]> signals;
        std::unique_ptr<WaitNote[]> waits;
        std::unique_ptr<CabinNote[]> cabins;
        std::size_t signals_used = 0;
        std::size_t waits_used = 0;
        std::size_t cabins_used = 0;
        bool failed = false;  // the allocation failed: every note drops
        // Any thread reads them; the part's core alone adds.
        std::atomic<std::uint64_t> busy_drops{0};
        std::atomic<std::uint64_t> full_drops{0};
    };

    // `core`'s part with its latch held by `hold`, or null - counted -
    // where there is no such part, the fold holds it, or its tables cannot
    // be had.
    Part* Enter(std::uint32_t core, std::optional<LatchGuard>& hold) noexcept;
    Latch* Armed(Part& part) const noexcept { return armed_ ? &part.latch : nullptr; }

    const sched::Clock* clock_;
    sched::MonoTimeNs half_life_ns_;
    const bool armed_;
    const std::atomic<bool>* gate_ = nullptr;
    std::vector<std::unique_ptr<Part>> parts_;
    // The notes of a core with no part, counted nowhere else.
    std::atomic<std::uint64_t> stray_drops_{0};

    // ---- The consumer's ----------------------------------------------------
    std::uint64_t version_ = 0;
    std::unordered_map<SignalKey, FingerprintSignal, SignalKeyHash> fingerprints_;
    // One entry per pattern with a signal in `fingerprints_`.
    std::unordered_map<std::uint64_t, PatternWaits> patterns_;
    std::unordered_map<std::uint64_t, CabinQualitySignal> cabins_;
    // The fold's copies, reserved at construction so a part's hold allocates
    // nothing.
    std::vector<SignalNote> taken_signals_;
    std::vector<WaitNote> taken_waits_;
    std::vector<CabinNote> taken_cabins_;
    // The fold's ranking of a full map's entries, kept between folds.
    std::vector<std::pair<Log2Q16, SignalKey>> evict_signals_;
    std::vector<std::pair<Log2Q16, std::uint64_t>> evict_cabins_;
};

}  // namespace kds::stats
