#include "kds/exec/trail_replay.hpp"

#include <unordered_map>

namespace kds::exec {

void TrailReplay::Build(const StepChain& chain,
                        std::span<const stats::WaystoneEntry> entries) {
    by_key_.clear();
    if (entries.empty()) return;

    // Every step of the chain by its global step id, sub-chains included.
    // Walked once at build rather than searched per entry: a chain has a
    // handful of steps and a trail has up to 253 entries, so the map is the
    // cheaper direction even at this size - and it makes "the chain does not
    // have this step" a lookup instead of a scan.
    std::unordered_map<std::uint32_t, const Step*> steps;
    ForEachStep(chain, [&](const Step& step) {
        steps.emplace(step.step_id, &step);
        return true;
    });

    by_key_.reserve(entries.size());
    for (const stats::WaystoneEntry& entry : entries) {
        // A never-written entry reads back all-zero, which decodes to a
        // plausible-looking tuple key 0 at page 0 (waystone.hpp). The flag is the
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
        // execution locates one tuple per (step, tuple key) - a probe for the same
        // key twice found the same row both times. Stated because
        // `insert_or_assign` looks like a choice and is not.
        by_key_.insert_or_assign(PackKey(entry.step_id, entry.tuple_key),
                                 TrailLocation{entry.page_id, entry.page_epoch, entry.slot});
    }
}

const TrailLocation* TrailReplay::Find(std::uint32_t step_id, std::uint64_t tuple_key) const noexcept {
    // BK-R3: the entry format holds 16-bit step ids, so no wider one was
    // ever recorded - a miss, never a narrowing onto another step's key.
    if (step_id > 0xFFFF) return nullptr;
    auto found = by_key_.find(PackKey(static_cast<std::uint16_t>(step_id), tuple_key));
    return found == by_key_.end() ? nullptr : &found->second;
}

}  // namespace kds::exec
