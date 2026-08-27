#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "kds/stats/optimizer_signals.hpp"

// The cabin optimizer's decision core (docs/spec/physical-optimizer.md
// §II.4/PO3/PO5, workplan PHY02): `Decide(Snapshot) → ActionSet`.
//
// ---- The purity contract (PO3/PO10), stated exactly ----------------------
//
// **No I/O, no clock reads, no floating point, no engine resources.** Time
// is the snapshot's `decay_epoch`; arithmetic is unsigned 16.16 fixed
// point; the only state is the controller's own lifecycle and evidence
// table, which `Decide` advances deterministically. Identical controllers
// fed identical snapshot sequences produce bit-identical action traces -
// that is what PHY07's golden-trace replay stands on, and it is why the
// managed table is an ordered map: iteration order is part of determinism,
// and a hash map's is not.
//
// Decide (here, pure over its own state) and Execute (PHY04, effectful)
// are separate phases by construction: this header includes nothing that
// can touch a page, a socket, or a catalog. The build/drop *outcomes* flow
// back through the three Note* edges below, which PHY04 will call and
// PHY02's tests call directly.
//
// ---- The model (§II.4), and where v1 simplifies it -----------------------
//
// Benefit per candidate c: B(c) = Σ_i f_i × max(0, P_scan,i − P_cabin),
// summed over fingerprints the snapshot links to c (CandidateRef - the
// PHY02 amendment to PHY01's snapshot, since a bare pattern_id names no
// column). P_scan,i is the fingerprint's decayed mean pages per execution,
// computed here from the S2 pair - the one division, as promised.
//
// Cost: C(c) = P_rel / T_amort + h_fail × f_lookup × k_heal, with
// **P_rel estimated as the largest serving fingerprint's mean page cost**
// (a full filter-scan reads the relation, so the observed scan cost is the
// observed build cost) and T_amort expressed in decay half-lives
// (`amort_windows`, default 1 - build cost and benefit decay on the same
// clock, exactly as PO2 proposed).
//
// Three v1 model choices, stated so they can be revisited rather than
// discovered: EXTEND's marginal pair is modeled as the coverage-miss share
// of B against the same share of C; a CREATE's budget admission uses the
// page estimate P_rel / kCreateEstimateDivisor until NoteCreated reports
// the real count; and the ACTIVE baseline is the live recomputation rather
// than PHY03's frozen P_scan, until PHY03 exists to freeze it.
//
// ---- Jurisdiction (PO1) ---------------------------------------------------
//
// Bound Cabins are structurally absent - the snapshot is fed by CabinStore,
// which holds Observational Cabins only. Cabins the optimizer does not own
// (user-declared) appear in the snapshot's quality section and as
// CandidateRef.cabin_id; `Decide` excludes both at aggregation - a shape an
// existing Cabin already serves is not a candidate, and an unowned Cabin is
// never healed, extended, or dropped - and debug-asserts at emission that
// every action targets its own table.
//
// Concurrency: core-local, no synchronization of its own (rules.md #3).

namespace kds::stats {

// Unsigned 16.16 fixed point (PO3). All model quantities are non-negative;
// saturating multiplies live in the .cpp.
using Fix16 = std::uint64_t;
inline constexpr Fix16 kFixOne = std::uint64_t{1} << 16;

enum class CabinAction : std::uint8_t {
    kCreate = 0,
    kExtend = 1,
    kHeal = 2,
    kDrop = 3,
};

enum class ActionReason : std::uint8_t {
    kSustainedBenefit = 0,  // CREATE: B > θ_create × C for N_confirm snapshots
    kCoverageExpansion = 1, // EXTEND: coverage-miss share cleared its margin
    kQualityHeal = 2,       // HEAL: hint failures above θ_heal, still worth it
    kQualityCollapse = 3,   // DROP: HEAL did not recover quality (PO7)
    kSustainedDecay = 4,    // DROP: B < θ_drop × C for the whole cooldown
    kBudgetSwap = 5,        // DROP: evicted to admit a stronger candidate (PO6)
};

const char* CabinActionName(CabinAction action) noexcept;
const char* ActionReasonName(ActionReason reason) noexcept;

// One decision, as plain data - directly serializable into PHY03's
// decision log. `cabin_id` is 0 for a CREATE (no id exists yet); the
// candidate key names the column either way. `benefit`/`cost` are the
// score snapshot the decision was taken on (PO8's "logged with the inputs
// that produced it").
struct ActionItem {
    CabinAction action{};
    ActionReason reason{};
    std::uint64_t cabin_id = 0;
    std::uint64_t rel_oid = 0;
    std::uint16_t col_pos = 0;
    Fix16 benefit = 0;
    Fix16 cost = 0;
};

using ActionSet = std::vector<ActionItem>;

// §II.6's settings, defaults as proposed there. Every number is a
// parameter and nothing may depend on one - the tests pin the *rules*
// (hysteresis, budget invariant, determinism), which hold at any setting
// respecting θ_drop < 1 < θ_create.
struct CabinOptimizerConfig {
    Fix16 theta_create = 3 * kFixOne;
    Fix16 theta_drop = kFixOne / 2;
    Fix16 theta_swap = 2 * kFixOne;
    Fix16 theta_extend = kFixOne / 5;   // 20% coverage-miss share
    Fix16 theta_heal = kFixOne / 10;    // 10% hint-failure rate
    std::uint32_t confirm_snapshots = 3;
    std::uint64_t page_budget = 1024;   // per core, optimizer-managed pages
    std::uint64_t p_cabin_pages = 2;    // pages per Cabin lookup (PROPOSED)
    std::uint64_t k_heal_pages = 2;     // pages per heal event (PROPOSED)
    // T_amort in decay half-lives - the one number expressing one belief:
    // a Cabin's expected useful lifetime. It is deliberately fused into
    // both sides of the model (PO2's "build cost and benefit decay on the
    // same clock"): the cost C = P_rel / T_amort prices the build over
    // the window, and the DROP cooldown is 2 x T_amort half-lives.
    // **Default 64, operator-ratified 2026-08-10** from the business-days
    // finding (bench/results-cabin-optimizer-days.md): at T_amort = 1 the
    // lifecycle was a nightly rebuild loop by the model's own arithmetic
    // (survival = log2(B/C) + 2 half-lives of silence; a market overnight
    // at the default 600 s half-life is ~105). 64 makes the cooldown
    // alone 128 half-lives = 21 h 20 m at defaults, so a Cabin survives
    // any close-to-open gap and the morning rebound recovers it
    // DECAYING -> ACTIVE without a rebuild. Two consequences, stated:
    // a *weekend* still drops it - deliberate, since re-nomination costs
    // ~3 ticks and two silent days of staleness is what DROP is for -
    // and the admission bar falls by the same factor, which is the same
    // belief read forward (a structure serving for a day need only pay
    // for itself over a day); PO6's budget, not the bar, is what bounds
    // the population.
    Fix16 amort_windows = 64 * kFixOne;

    // T_cooldown: how long a DECAYING entry is given to rebound before it
    // is dropped, in **whole decay half-lives**. Its own parameter since
    // 2026-08-10; it was `2 x T_amort`, and the two are different
    // questions wearing one number - T_amort is how long a build is
    // believed to pay for itself, T_cooldown is how much silence is
    // proof of death. The default 128 is exactly what the old expression
    // yielded at the shipped T_amort = 64, so decoupling changed no
    // behaviour and `bench/results-cabin-optimizer-days.md`'s measured
    // 128.14 s cooldown still describes the shipped configuration.
    //
    // **It cannot usefully be set below a workload's longest expected
    // quiet period.** A dead Cabin and an overnight-quiet one emit the
    // same signal - no lookups - so the only thing that separates them is
    // waiting longer than the quiet period; a cooldown under ~105
    // half-lives reintroduces the nightly rebuild loop the T_amort
    // ratification removed (a market overnight is 17.5 h ~ 105 half-lives
    // at the default 600 s). Below that floor this knob does not retire
    // dead Cabins sooner, it retires *live* ones. A 24/7 workload with no
    // quiet period is the case that can safely lower it.
    //
    // Whole half-lives, and integer on purpose: the cooldown is a clock
    // quantity, and lifting nanoseconds into 16.16 is what once collapsed
    // a 20-minute cooldown to 4.3 seconds. 0 means no time patience at
    // all - the score hysteresis (theta_drop < 1 < theta_create) is then
    // the only anti-thrash left, which is a coherent choice and not a
    // recommended one.
    std::uint32_t cooldown_half_lives = 128;

    sched::MonoTimeNs half_life_ns = 600'000'000'000ULL;  // for cooldown time
    // CREATE's admission estimate: P_rel / this, floored at 1, until
    // NoteCreated reports the built size. `[PROPOSED]` 8.
    std::uint64_t create_estimate_divisor = 8;

    // PO8's decision log, bounded: the last K decisions per core.
    // `[PROPOSED]` 1024. Advisory data - memory-resident, lost on crash,
    // and the loss is documented as acceptable because the state machine
    // re-derives from re-observation (PHY03's stated crash posture).
    std::size_t decision_log_capacity = 1024;
};

// One logged decision (PO8: "every action is recorded with the inputs
// that produced it"). The snapshot digest is `{version, decay_epoch}` -
// the pair that names one snapshot uniquely per core - beside the
// ActionItem, which already carries the B/C score it was taken on.
struct DecisionRecord {
    std::uint64_t snapshot_version = 0;
    sched::MonoTimeNs decay_epoch = 0;
    ActionItem item;
};

// One managed candidate as the inspection surface sees it (PO9, workplan
// PHY06's `SHOW CABIN_OPTIMIZER`). A by-value projection of the internal
// `Managed` entry plus its key: the view is a report, and handing out a
// reference into the ordered map would let a reader outlive a Decide.
// `benefit`/`cost` are the scores of the **last Decide pass** - stamped
// every pass, not only when an action fired, so a quiet entry still shows
// the numbers keeping it quiet.
struct ManagedEntryView {
    std::uint64_t rel_oid = 0;
    std::uint16_t col_pos = 0;
    const char* state = "";       // CANDIDATE / BUILDING / ACTIVE / DECAYING
    std::uint64_t cabin_id = 0;   // 0 until NoteCreated
    std::uint64_t pages = 0;      // estimate while BUILDING, real after
    std::uint32_t confirm_streak = 0;
    Fix16 benefit = 0;
    Fix16 cost = 0;
};

class CabinOptimizer {
public:
    explicit CabinOptimizer(CabinOptimizerConfig config = {}) noexcept : config_(config) {}

    // The decision pass: advances evidence (confirm streaks, cooldowns,
    // heal attempts) and returns the actions this snapshot justifies. At
    // most one action per candidate per call.
    ActionSet Decide(const OptimizerSnapshot& snapshot);

    // PHY04's completion edges (PO5's lifecycle transitions). Tests drive
    // them directly; nothing here executes anything.
    void NoteCreated(std::uint64_t rel_oid, std::uint16_t col_pos, std::uint64_t cabin_id,
                     std::uint64_t pages);
    void NoteBuildFailed(std::uint64_t rel_oid, std::uint16_t col_pos);
    void NoteDropped(std::uint64_t cabin_id);

    // The EXTEND completion edge (PHY06, closing PHY04's recorded gap): a
    // widened Cabin's sets grew, so its page proxy moved, and an entry
    // whose `pages` stays at the CREATE-time count undercounts the budget
    // by every extend forever - PO6's invariant silently eroding. Executor
    // reports the *new total*, not a delta, so a repeated report is
    // idempotent. Unknown cabin_id is a no-op, NoteDropped's stance.
    void NoteExtended(std::uint64_t cabin_id, std::uint64_t pages);

    // PO6's invariant, exposed so the budget test asserts the fact rather
    // than re-deriving it: estimated + built pages currently committed.
    std::uint64_t pages_committed() const noexcept;
    std::size_t managed_count() const noexcept { return managed_.size(); }

    // The decision log, oldest first (PHY03). A copy: this is an
    // inspection surface (PHY06's view reads it), bounded by the
    // configured capacity, and never on a statement path.
    std::vector<DecisionRecord> DecisionLog() const;

    // The managed table, in key order (PHY06's view). A copy, for the
    // DecisionLog reason; never on a statement path.
    std::vector<ManagedEntryView> ManagedEntries() const;

    // The settings, for the view's budget line: utilization is
    // pages_committed() against config().page_budget, and re-deriving the
    // ceiling anywhere else would let the two drift.
    const CabinOptimizerConfig& config() const noexcept { return config_; }

private:
    enum class State : std::uint8_t { kCandidate, kBuilding, kActive, kDecaying };

    struct CandidateKey {
        std::uint64_t rel_oid = 0;
        std::uint16_t col_pos = 0;
        bool operator<(const CandidateKey& other) const noexcept {
            return rel_oid != other.rel_oid ? rel_oid < other.rel_oid
                                            : col_pos < other.col_pos;
        }
    };

    struct Managed {
        State state = State::kCandidate;
        std::uint64_t cabin_id = 0;      // non-zero from NoteCreated on
        std::uint64_t pages = 0;         // estimate while BUILDING, real after
        std::uint32_t confirm_streak = 0;
        sched::MonoTimeNs decaying_since = 0;
        bool heal_attempted = false;

        // §II.4's carried baseline (PHY03, closing PHY02's deferred
        // choice): the pre-Cabin scan cost, frozen at the CREATE decision.
        // Load-bearing, not bookkeeping - an ACTIVE Cabin makes the very
        // scans it replaced cheap, so a benefit recomputed from *live*
        // page costs collapses the moment the Cabin works and the
        // controller drops its own success. B and C for an ACTIVE or
        // DECAYING entry therefore price the frozen mean under the live
        // frequency; a dead shape still dies, because zero frequency
        // zeroes the benefit whatever the baseline says.
        Fix16 frozen_p_scan = 0;

        // The last Decide pass's scores, for PHY06's view only - nothing
        // in the rule table reads them back, so they cannot feed a
        // decision loop the snapshot did not.
        Fix16 last_benefit = 0;
        Fix16 last_cost = 0;
    };

    static const char* StateName(State state) noexcept;

    void RecordDecision(const OptimizerSnapshot& snapshot, const ActionItem& item);

    CabinOptimizerConfig config_;
    std::map<CandidateKey, Managed> managed_;

    // The bounded log: grows to capacity, then overwrites oldest-first.
    std::vector<DecisionRecord> log_;
    std::size_t log_head_ = 0;  // oldest entry, once the ring is full
};

}  // namespace kds::stats
