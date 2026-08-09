#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "kds/stats/optimizer_signals.hpp"

// The cabin optimizer's decision core (docs/feat-physical-optimizer.md
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
    Fix16 amort_windows = kFixOne;      // T_amort in decay half-lives
    sched::MonoTimeNs half_life_ns = 600'000'000'000ULL;  // for cooldown time
    // CREATE's admission estimate: P_rel / this, floored at 1, until
    // NoteCreated reports the built size. `[PROPOSED]` 8.
    std::uint64_t create_estimate_divisor = 8;
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

    // PO6's invariant, exposed so the budget test asserts the fact rather
    // than re-deriving it: estimated + built pages currently committed.
    std::uint64_t pages_committed() const noexcept;
    std::size_t managed_count() const noexcept { return managed_.size(); }

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
    };

    CabinOptimizerConfig config_;
    std::map<CandidateKey, Managed> managed_;
};

}  // namespace kds::stats
