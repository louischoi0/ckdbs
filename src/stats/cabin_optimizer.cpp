#include "kds/stats/cabin_optimizer.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace kds::stats {

namespace {

// Saturating 16.16 multiply: (a × b) >> 16 without overflow surprises.
// The model's quantities are decayed scores and page counts, so a product
// near 2^64 means the inputs were already saturated; pinning at the
// ceiling keeps the comparison ordering sane, which is all a threshold
// rule needs.
Fix16 MulFix(Fix16 a, Fix16 b) noexcept {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return std::numeric_limits<std::uint64_t>::max() >> 16;
    }
    return (a * b) >> 16;
}

// (a / b) in 16.16; b == 0 answers 0, the harmless direction for every
// ratio here (no lookups -> no failure rate, no executions -> no mean).
Fix16 DivFix(Fix16 a, Fix16 b) noexcept {
    if (b == 0) return 0;
    if (a > std::numeric_limits<std::uint64_t>::max() / kFixOne) {
        a = std::numeric_limits<std::uint64_t>::max() / kFixOne;
    }
    return (a * kFixOne) / b;
}

// Q24.8 (the snapshot's decayed scores) to 16.16.
Fix16 FromQ8(std::uint32_t q8) noexcept { return std::uint64_t{q8} << 8; }

// ---- The log-domain shadow of the model (2026-08-10) ---------------------
//
// 16.16 underflows at the same place Q24.8 does, ~16 half-lives into a
// silence, and below that floor every cold candidate reads exactly 0 -
// so the rule table cannot tell a Cabin that was worth 2^30 from one
// worth 2^10, and both fall out of ACTIVE at the same moment. The fix is
// to carry the same quantities in the log domain, where decay is a
// subtraction that never bottoms out, and to consult them **only where
// the linear form has run out of resolution**. Linear stays authoritative
// wherever it still has any, which is what keeps every measured
// threshold decision and PHY07's golden traces exactly as they were.
//
// Everything below is "log2 of the real value" - the Q24.8 and 16.16
// scalings divided out - so the three sources compose by addition.

// log2 of a 16.16 quantity, as a real number.
Log2Q16 LogOfFix(Fix16 v) noexcept {
    if (v == 0) return kLog2NegInf;
    return Log2OfQ16(v) - (Log2Q16{16} << 16);
}

// log2 of a snapshot's Q24.8 score, as a real number. The snapshot's
// `*_log2` fields are log2 of the *scaled* integer, and one whole point
// is 256 of those.
Log2Q16 LogOfQ8Score(Log2Q16 scaled_log2) noexcept {
    if (scaled_log2 == kLog2NegInf) return kLog2NegInf;
    return scaled_log2 - (Log2Q16{8} << 16);
}

// a + b in the log domain, either side possibly negative infinity.
Log2Q16 LogAdd(Log2Q16 a, Log2Q16 b) noexcept {
    if (a == kLog2NegInf || b == kLog2NegInf) return kLog2NegInf;
    return a + b;
}

Fix16 FromWhole(std::uint64_t whole) noexcept {
    if (whole > (std::numeric_limits<std::uint64_t>::max() >> 16)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole << 16;
}

// The DECAYING dwell in nanoseconds: whole half-lives x the half-life.
// **Both operands are integers and neither passes through 16.16**, which
// is the point - this used to be `2 x amort_windows` in fixed point, and
// lifting a nanosecond count into 16.16 saturated for every half-life
// above about two seconds, collapsing a configured 20-minute cooldown to
// 4.29 s. Saturating rather than wrapping: a cooldown so long it
// overflows u64 is a configuration asking never to drop, which is the
// direction it should err.
sched::MonoTimeNs CooldownNs(std::uint32_t half_lives, sched::MonoTimeNs half_life_ns) noexcept {
    if (half_lives != 0 && half_life_ns > std::numeric_limits<std::uint64_t>::max() / half_lives) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return half_life_ns * half_lives;
}

}  // namespace

const char* CabinActionName(CabinAction action) noexcept {
    switch (action) {
        case CabinAction::kCreate: return "CREATE";
        case CabinAction::kExtend: return "EXTEND";
        case CabinAction::kHeal: return "HEAL";
        case CabinAction::kDrop: return "DROP";
    }
    return "?";
}

const char* ActionReasonName(ActionReason reason) noexcept {
    switch (reason) {
        case ActionReason::kSustainedBenefit: return "sustained-benefit";
        case ActionReason::kCoverageExpansion: return "coverage-expansion";
        case ActionReason::kQualityHeal: return "quality-heal";
        case ActionReason::kQualityCollapse: return "quality-collapse";
        case ActionReason::kSustainedDecay: return "sustained-decay";
        case ActionReason::kBudgetSwap: return "budget-swap";
    }
    return "?";
}

const char* CabinOptimizer::StateName(State state) noexcept {
    switch (state) {
        case State::kCandidate: return "CANDIDATE";
        case State::kBuilding: return "BUILDING";
        case State::kActive: return "ACTIVE";
        case State::kDecaying: return "DECAYING";
    }
    return "?";
}

std::vector<ManagedEntryView> CabinOptimizer::ManagedEntries() const {
    std::vector<ManagedEntryView> out;
    out.reserve(managed_.size());
    for (const auto& [key, managed] : managed_) {
        ManagedEntryView view;
        view.rel_oid = key.rel_oid;
        view.col_pos = key.col_pos;
        view.state = StateName(managed.state);
        view.cabin_id = managed.cabin_id;
        view.pages = managed.pages;
        view.confirm_streak = managed.confirm_streak;
        view.benefit = managed.last_benefit;
        view.cost = managed.last_cost;
        out.push_back(view);
    }
    return out;
}

std::vector<DecisionRecord> CabinOptimizer::DecisionLog() const {
    std::vector<DecisionRecord> out;
    out.reserve(log_.size());
    // Oldest first: once the ring is full, log_head_ names the oldest.
    for (std::size_t i = 0; i < log_.size(); ++i) {
        out.push_back(log_[(log_head_ + i) % log_.size()]);
    }
    return out;
}

void CabinOptimizer::RecordDecision(const OptimizerSnapshot& snapshot, const ActionItem& item) {
    if (config_.decision_log_capacity == 0) return;
    DecisionRecord record{snapshot.version, snapshot.decay_epoch, item};
    if (log_.size() < config_.decision_log_capacity) {
        log_.push_back(record);
        return;
    }
    log_[log_head_] = record;
    log_head_ = (log_head_ + 1) % log_.size();
}

std::uint64_t CabinOptimizer::pages_committed() const noexcept {
    std::uint64_t pages = 0;
    for (const auto& [key, managed] : managed_) {
        if (managed.state != State::kCandidate) pages += managed.pages;
    }
    return pages;
}

void CabinOptimizer::NoteCreated(std::uint64_t rel_oid, std::uint16_t col_pos,
                                 std::uint64_t cabin_id, std::uint64_t pages) {
    // Insert-if-absent, deliberately: Execute reports a Cabin that exists,
    // and a controller that no longer tracks the candidate must adopt it
    // rather than let its pages fall out of the budget's accounting -
    // whoever forgot whom, the frames are real.
    auto found = managed_.emplace(CandidateKey{rel_oid, col_pos}, Managed{}).first;
    found->second.state = State::kActive;
    found->second.cabin_id = cabin_id;
    found->second.pages = pages;
    found->second.heal_attempted = false;
}

void CabinOptimizer::NoteBuildFailed(std::uint64_t rel_oid, std::uint16_t col_pos) {
    auto found = managed_.find(CandidateKey{rel_oid, col_pos});
    if (found == managed_.end()) return;
    // PO5: BUILDING -> discard, back to CANDIDATE from scratch. Demand, if
    // real, re-confirms.
    found->second = Managed{};
}

void CabinOptimizer::NoteExtended(std::uint64_t cabin_id, std::uint64_t pages) {
    for (auto& [key, managed] : managed_) {
        if (managed.cabin_id == cabin_id) {
            managed.pages = pages;
            return;
        }
    }
    // Unknown id: the entry was dropped between the decision and the
    // report, and its pages left the accounting with it. Nothing to update.
}

void CabinOptimizer::NoteDropped(std::uint64_t cabin_id) {
    for (auto it = managed_.begin(); it != managed_.end(); ++it) {
        if (it->second.cabin_id == cabin_id) {
            // DROPPED erases the entry entirely: re-nomination starts from
            // scratch as CANDIDATE (PO5), which a fresh insertion is.
            managed_.erase(it);
            return;
        }
    }
}

ActionSet CabinOptimizer::Decide(const OptimizerSnapshot& snapshot) {
    // ---- Aggregate the snapshot per candidate (the Σ_i of §II.4) --------
    struct Evidence {
        Fix16 benefit = 0;       // Σ f_i × max(0, mean_i − P_cabin), live means
        Fix16 p_rel = 0;         // max mean_i: the observed scan == build cost
        Fix16 freq = 0;          // Σ f_i, for the frozen-baseline re-pricing
        Fix16 lookups = 0;       // S3, when an owned cabin serves it
        Fix16 fail_rate = 0;
        Fix16 coverage_share = 0;
        bool served_by_unowned = false;

        // Σ f_i in the log domain, approximated by its **largest term**.
        // A sum is not a log-domain operation without a second LUT, and
        // the approximation costs at most log2(n) - under one threshold
        // margin for any n a core tracks, and irrelevant to the thing
        // this field exists for, which is ordering two cold shapes
        // decades of half-lives after the linear form gave up.
        Log2Q16 freq_log = kLog2NegInf;
    };
    std::map<CandidateKey, Evidence> evidence;

    const Fix16 p_cabin = FromWhole(config_.p_cabin_pages);
    for (const SnapshotFingerprint& fp : snapshot.fingerprints) {
        if (!fp.candidate.valid()) continue;
        const CandidateKey key{fp.candidate.rel_oid, fp.candidate.col_pos};
        Evidence& e = evidence[key];

        // Jurisdiction (PO1): a shape an existing Cabin serves is only this
        // controller's business if that Cabin is its own.
        if (fp.candidate.cabin_id != 0) {
            auto owned = managed_.find(key);
            if (owned == managed_.end() || owned->second.cabin_id != fp.candidate.cabin_id) {
                e.served_by_unowned = true;
                continue;
            }
        }

        const Fix16 f = FromQ8(fp.frequency_q8);
        const Fix16 mean = DivFix(FromQ8(fp.pages_q8), FromQ8(fp.frequency_q8));
        if (mean > p_cabin) e.benefit += MulFix(f, mean - p_cabin);
        e.p_rel = std::max(e.p_rel, mean);
        e.freq += f;
        e.freq_log = std::max(e.freq_log, LogOfQ8Score(fp.frequency_log2));
    }

    // Every managed candidate gets an evidence entry even when its shape
    // has vanished from the snapshot (evicted as cold, or simply gone):
    // zero evidence is itself evidence, and an ACTIVE Cabin nobody probes
    // must be able to decay rather than be forgotten alive.
    for (const auto& [key, managed] : managed_) {
        (void)managed;
        evidence[key];
    }

    // S3 quality joins by owned cabin id.
    for (const auto& [key, managed] : managed_) {
        if (managed.cabin_id == 0) continue;
        for (const SnapshotCabin& cabin : snapshot.cabins) {
            if (cabin.cabin_id != managed.cabin_id) continue;
            Evidence& e = evidence[key];
            e.lookups = FromQ8(cabin.lookups_q8);
            e.fail_rate = DivFix(FromQ8(cabin.hint_failures_q8), FromQ8(cabin.lookups_q8));
            e.coverage_share =
                DivFix(FromQ8(cabin.coverage_misses_q8), FromQ8(cabin.lookups_q8));
        }
    }

    // ---- The rule table, one candidate at a time, in key order ----------
    ActionSet actions;
    const auto emit = [&](const ActionItem& item) {
        RecordDecision(snapshot, item);
        actions.push_back(item);
    };
    // The DECAYING dwell, from its own parameter rather than from
    // T_amort: how much silence proves death is not the same question as
    // how long a build is believed to pay for itself.
    const sched::MonoTimeNs cooldown_ns =
        CooldownNs(config_.cooldown_half_lives, config_.half_life_ns);

    // The effective score for one entry: live means for a candidate, the
    // frozen pre-Cabin baseline once one exists - an ACTIVE Cabin makes
    // the scans it replaced cheap, and pricing it by the cheapness it
    // caused would drop its own success (Managed::frozen_p_scan).
    struct Scored {
        Fix16 benefit = 0;
        Fix16 cost = 0;
        // The same pair in the log domain. Consulted only where `benefit`
        // has underflowed to zero, and always monotone in it, so ordering
        // by `benefit_log` orders by benefit - including among candidates
        // the linear form has flattened to a tie at zero.
        Log2Q16 benefit_log = kLog2NegInf;
        Log2Q16 cost_log = kLog2NegInf;
    };
    const auto score = [&](const Evidence& e, const Managed* m) {
        Scored s;
        const Fix16 quality_upkeep = MulFix(MulFix(e.fail_rate, e.lookups),
                                            FromWhole(config_.k_heal_pages));
        Fix16 basis = e.p_rel;
        s.benefit = e.benefit;
        if (m != nullptr &&
            (m->state == State::kActive || m->state == State::kDecaying) &&
            m->frozen_p_scan != 0) {
            basis = m->frozen_p_scan;
            s.benefit = basis > p_cabin ? MulFix(e.freq, basis - p_cabin) : 0;
        }
        s.cost = DivFix(basis, config_.amort_windows) + quality_upkeep;
        // The log-domain twin. The saved-pages factor is a *ratio* of
        // co-decaying quantities (or, for an ACTIVE entry, the frozen
        // baseline), so it never underflows and can be read from the
        // linear side; only the frequency needed rescuing.
        if (basis > p_cabin) {
            s.benefit_log = LogAdd(e.freq_log, LogOfFix(basis - p_cabin));
        }
        s.cost_log = LogOfFix(s.cost);
        return s;
    };

    // "Is benefit below `theta x cost`?" - answered in whichever domain
    // still has resolution. Linear is authoritative whenever it has any,
    // which is what leaves every measured decision untouched; the log
    // domain answers only the question linear cannot, having flattened
    // both sides to zero.
    const auto below = [](const Scored& s, Fix16 theta) {
        if (s.benefit != 0) return s.benefit < MulFix(theta, s.cost);
        if (s.benefit_log == kLog2NegInf || s.cost_log == kLog2NegInf) return true;
        return s.benefit_log < LogOfFix(theta) + s.cost_log;
    };

    // "At most one action per candidate per call" is stated in the header,
    // and the budget swap is the one rule that emits against a key other
    // than the one being decided. Without this test its victim - when it
    // sorts *after* the candidate in key order - reaches the switch below
    // still carrying an emitted DROP, and either emits a second one for the
    // same cabin (a fabricated `sustained-decay`, since the cooldown is
    // measured from a `decaying_since` the swap had not stamped) or rebounds
    // to ACTIVE against the DROP the same ActionSet already carries.
    const auto acted_on_this_pass = [&actions](const CandidateKey& key) {
        return std::any_of(actions.begin(), actions.end(), [&key](const ActionItem& item) {
            return item.rel_oid == key.rel_oid && item.col_pos == key.col_pos;
        });
    };

    for (auto& [key, e] : evidence) {
        if (e.served_by_unowned) continue;
        if (acted_on_this_pass(key)) continue;

        auto found = managed_.find(key);
        if (found == managed_.end()) {
            if (e.benefit == 0) continue;
            found = managed_.emplace(key, Managed{}).first;
        }
        Managed& m = found->second;
        const Scored scored = score(e, &m);
        const Fix16 cost = scored.cost;
        // PHY06's view reads these and nothing else does: the rule table
        // below works from `scored` directly, so stashing cannot feed a
        // decision anything the snapshot did not.
        m.last_benefit = scored.benefit;
        m.last_cost = cost;

        switch (m.state) {
            case State::kCandidate: {
                if (scored.benefit > MulFix(config_.theta_create, cost)) {
                    ++m.confirm_streak;
                } else {
                    m.confirm_streak = 0;
                    break;
                }
                if (m.confirm_streak < config_.confirm_snapshots) break;

                // Budget admission (PO6): estimate until the build reports.
                const std::uint64_t estimate = std::max<std::uint64_t>(
                    1, (e.p_rel >> 16) / config_.create_estimate_divisor);
                if (pages_committed() + estimate > config_.page_budget) {
                    // The replacement rule (PO6): evict the weakest ACTIVE
                    // iff the candidate beats it by θ_swap. Weakest by
                    // *effective* benefit - a frozen baseline prices an
                    // incumbent the same way it defends one.
                    const CandidateKey* victim = nullptr;
                    Fix16 victim_net = std::numeric_limits<std::uint64_t>::max();
                    Log2Q16 victim_log = std::numeric_limits<std::int64_t>::max();
                    for (auto& [vkey, ve] : evidence) {
                        auto vm = managed_.find(vkey);
                        if (vm == managed_.end() || vm->second.state != State::kActive) {
                            continue;
                        }
                        const Scored vscored = score(ve, &vm->second);
                        // Ordered in the log domain, which is monotone in
                        // the linear benefit and - unlike it - still
                        // separates two incumbents that have both decayed
                        // past zero. Picking among them by linear benefit
                        // was a tie broken by map order, so the swap
                        // evicted an arbitrary cold Cabin rather than the
                        // coldest one.
                        if (vscored.benefit_log < victim_log) {
                            victim_log = vscored.benefit_log;
                            victim_net = vscored.benefit;
                            victim = &vkey;
                        }
                    }
                    if (victim == nullptr ||
                        scored.benefit <= MulFix(config_.theta_swap, victim_net)) {
                        break;  // the candidate waits (PO6)
                    }
                    Managed& vm = managed_.find(*victim)->second;
                    assert(vm.cabin_id != 0);
                    emit(ActionItem{CabinAction::kDrop, ActionReason::kBudgetSwap,
                                    vm.cabin_id, victim->rel_oid, victim->col_pos,
                                    victim_net, cost});
                    vm.state = State::kDecaying;  // dropped on execution edge
                    // Stamped, like every other entry into DECAYING. An
                    // unstamped victim measures its cooldown from epoch 0,
                    // so a later pass that never saw the drop executed would
                    // find the whole cooldown already elapsed.
                    vm.decaying_since = snapshot.decay_epoch;
                }
                m.state = State::kBuilding;
                m.pages = estimate;
                m.confirm_streak = 0;
                // §II.4's carried baseline, frozen at the decision: the
                // pre-Cabin scan cost this CREATE was justified by.
                m.frozen_p_scan = e.p_rel;
                emit(ActionItem{CabinAction::kCreate, ActionReason::kSustainedBenefit, 0,
                                key.rel_oid, key.col_pos, scored.benefit, cost});
                break;
            }
            case State::kBuilding:
                break;  // Execute owns the transition; nothing to decide
            case State::kActive: {
                assert(m.cabin_id != 0);
                // The onset test, and the one place the log domain earns
                // its keep. It used to read "zero benefit decays whatever
                // the cost", because a linear zero made the comparison
                // vacuous - and that flattened every deeply-idle Cabin
                // into the same instant, so a shape worth 2^30 left ACTIVE
                // exactly when one worth 2^10 did. In the log domain the
                // margin survives, so time-to-DECAYING is proportional to
                // demonstrated value again. A score that is *truly* zero
                // still decays: its log is negative infinity.
                if (below(scored, config_.theta_drop)) {
                    m.state = State::kDecaying;
                    m.decaying_since = snapshot.decay_epoch;
                    break;
                }
                if (e.fail_rate > config_.theta_heal) {
                    if (m.heal_attempted) {
                        // PO7: HEAL did not recover quality. Discard and
                        // re-observe beats repair at any cost.
                        emit(ActionItem{CabinAction::kDrop, ActionReason::kQualityCollapse,
                                        m.cabin_id, key.rel_oid, key.col_pos,
                                        scored.benefit, cost});
                        break;
                    }
                    m.heal_attempted = true;
                    emit(ActionItem{CabinAction::kHeal, ActionReason::kQualityHeal,
                                    m.cabin_id, key.rel_oid, key.col_pos, scored.benefit,
                                    cost});
                    break;
                }
                m.heal_attempted = false;  // quality recovered

                // EXTEND: the missed share's marginal pair, both sides
                // scaled by the same share (the v1 model choice the header
                // states).
                if (e.coverage_share > config_.theta_extend &&
                    MulFix(scored.benefit, e.coverage_share) >
                        MulFix(config_.theta_create, MulFix(cost, e.coverage_share))) {
                    emit(ActionItem{CabinAction::kExtend, ActionReason::kCoverageExpansion,
                                    m.cabin_id, key.rel_oid, key.col_pos, scored.benefit,
                                    cost});
                }
                break;
            }
            case State::kDecaying: {
                if (scored.benefit > MulFix(config_.theta_create, cost)) {
                    m.state = State::kActive;  // recovery on score rebound
                    break;
                }
                if (m.cabin_id != 0 &&
                    snapshot.decay_epoch >= m.decaying_since &&
                    snapshot.decay_epoch - m.decaying_since >= cooldown_ns) {
                    emit(ActionItem{CabinAction::kDrop, ActionReason::kSustainedDecay,
                                    m.cabin_id, key.rel_oid, key.col_pos, scored.benefit,
                                    cost});
                }
                break;
            }
        }
    }

#ifndef NDEBUG
    // PO1's jurisdiction, asserted at the emission edge: every action
    // targets this controller's own table.
    for (const ActionItem& action : actions) {
        auto found = managed_.find(CandidateKey{action.rel_oid, action.col_pos});
        assert(found != managed_.end());
        assert(action.action == CabinAction::kCreate ||
               found->second.cabin_id == action.cabin_id);
    }
#endif
    return actions;
}

}  // namespace kds::stats
