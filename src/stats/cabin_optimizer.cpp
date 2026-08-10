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

Fix16 FromWhole(std::uint64_t whole) noexcept {
    if (whole > (std::numeric_limits<std::uint64_t>::max() >> 16)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole << 16;
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
    const sched::MonoTimeNs cooldown_ns = static_cast<sched::MonoTimeNs>(
        MulFix(2 * config_.amort_windows, FromWhole(config_.half_life_ns)) >> 16);

    // The effective score for one entry: live means for a candidate, the
    // frozen pre-Cabin baseline once one exists - an ACTIVE Cabin makes
    // the scans it replaced cheap, and pricing it by the cheapness it
    // caused would drop its own success (Managed::frozen_p_scan).
    struct Scored {
        Fix16 benefit = 0;
        Fix16 cost = 0;
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
        return s;
    };

    for (auto& [key, e] : evidence) {
        if (e.served_by_unowned) continue;

        auto found = managed_.find(key);
        if (found == managed_.end()) {
            if (e.benefit == 0) continue;
            found = managed_.emplace(key, Managed{}).first;
        }
        Managed& m = found->second;
        const Scored scored = score(e, &m);
        const Fix16 cost = scored.cost;

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
                    for (auto& [vkey, ve] : evidence) {
                        auto vm = managed_.find(vkey);
                        if (vm == managed_.end() || vm->second.state != State::kActive) {
                            continue;
                        }
                        const Fix16 vnet = score(ve, &vm->second).benefit;
                        if (vnet < victim_net) {
                            victim_net = vnet;
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
                // Zero benefit decays regardless of cost: with no observed
                // traffic both sides read 0 and the threshold comparison
                // goes vacuous, which must not keep a dead Cabin alive.
                if (scored.benefit == 0 ||
                    scored.benefit < MulFix(config_.theta_drop, cost)) {
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
