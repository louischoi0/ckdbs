#include "kds/exec/trail_replay.hpp"

#include <unordered_map>

namespace kds::exec {

namespace {

// Every step of a chain by its global step id, sub-chains included.
//
// Walked once at build rather than searched per entry: a chain has a
// handful of steps and a trail has up to 253 entries, so the map is the
// cheaper direction even at this size - and it makes "the chain does not
// have this step" a lookup instead of a scan.
void IndexSteps(const std::vector<Step>& steps,
                std::unordered_map<std::uint32_t, const Step*>& out);

void IndexSubChains(const std::vector<SubChain>& subs,
                    std::unordered_map<std::uint32_t, const Step*>& out) {
    for (const SubChain& sub : subs) IndexSteps(sub.steps, out);
}

void IndexSteps(const std::vector<Step>& steps,
                std::unordered_map<std::uint32_t, const Step*>& out) {
    for (const Step& step : steps) {
        out.emplace(step.step_id, &step);
        IndexSubChains(step.sub_chains, out);
    }
}

}  // namespace

void TrailReplay::Build(const StepChain& chain,
                        std::span<const stats::WaystoneEntry> entries) {
    by_key_.clear();
    if (entries.empty()) return;

    std::unordered_map<std::uint32_t, const Step*> steps;
    IndexSteps(chain.steps, steps);
    IndexSubChains(chain.hoisted, steps);

    by_key_.reserve(entries.size());
    for (const stats::WaystoneEntry& entry : entries) {
        // A never-written entry reads back all-zero, which decodes to a
        // plausible-looking pk 0 at page 0 (waystone.hpp). The flag is the
        // only safe test.
        if ((entry.flags & stats::kWaystoneEntryValid) == 0) continue;

        auto found = steps.find(entry.step_id);
        if (found == steps.end()) continue;
        const Step& step = *found->second;

        // Spec section 2 rule 1, relation half. Checked here, once, so no
        // consultation can reach an entry recorded against another
        // relation - which is the mistake that would decode a tuple with
        // the wrong schema.
        if (entry.rel_oid != step.rel_oid) continue;

        // A search step's rows may never be replayed (invariant 9). The
        // recorder does not write them, so this drops nothing today; it is
        // here because "the trail said so" must never be able to turn a
        // scan into a lookup.
        if (!IsTrailReplayable(step.kind)) continue;

        // **Last entry wins for a repeated key**, and it does not matter
        // which: the entries were recorded in one execution, and one
        // execution locates one tuple per (step, pk) - a probe for the same
        // key twice found the same row both times. Stated because
        // `insert_or_assign` looks like a choice and is not.
        by_key_.insert_or_assign(PackKey(entry.step_id, entry.pk),
                                 TrailLocation{entry.page_id, entry.page_epoch, entry.slot});
    }
}

const TrailLocation* TrailReplay::Find(std::uint32_t step_id, std::uint64_t pk) const noexcept {
    auto found = by_key_.find(PackKey(step_id, pk));
    return found == by_key_.end() ? nullptr : &found->second;
}

}  // namespace kds::exec
