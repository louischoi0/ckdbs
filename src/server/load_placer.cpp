#include "kds/server/load_placer.hpp"

#include <array>

#include "kds/server/session.hpp"

namespace kds::server {

std::uint32_t LoadPlacer::Decide(std::span<const CoreView> cores, std::uint32_t core,
                                 std::uint64_t now_ns, std::uint64_t moved_at_ns,
                                 bool left_this_window) noexcept {
    // Compared as sums, not differences: another core's word may be newer
    // than this core's, and a move time later than this window's end.
    if (core >= cores.size() || left_this_window || now_ns < moved_at_ns + kDwellNs) return core;
    std::uint32_t least = core;
    for (std::uint32_t k = 0; k < cores.size(); ++k) {
        if (k == core || cores[k].window_end_ns + kStaleNs < now_ns) continue;
        if (cores[k].busy_ppm < cores[least].busy_ppm) least = k;
    }
    const CoreView& here = cores[core];
    const std::uint32_t gap = here.busy_ppm - cores[least].busy_ppm;
    const std::uint32_t share =
        here.busy_ppm / (here.active_sessions > 0 ? here.active_sessions : 1);
    return gap > kMarginPpm && share < gap ? least : core;
}

std::uint32_t LoadPlacer::Place(std::uint32_t core, Session& session) {
    const std::uint32_t n = sched::CoreLoad::Cores();
    std::array<CoreView, sched::CoreLoad::kSlots> view{};
    for (std::uint32_t k = 0; k < n; ++k) {
        const sched::CoreLoad::Slot& s = sched::CoreLoad::Of(k);
        view[k] = {s.busy_ppm.load(std::memory_order_relaxed),
                   s.active_sessions.load(std::memory_order_relaxed),
                   s.window_end_ns.load(std::memory_order_relaxed)};
    }
    // This core's words are its own, written on this reactor
    // (`core_load.hpp`): its latest window end is the clock the dwell and
    // the staleness are read on, and the per-window guard's mark.
    sched::CoreLoad::Slot& mine = sched::CoreLoad::Of(core);
    const std::uint64_t now = view[core].window_end_ns;
    const std::uint32_t there =
        Decide(std::span<const CoreView>(view.data(), n), core, now, session.moved_at_ns(),
               mine.moved_out_in.load(std::memory_order_relaxed) == now);
    if (there != core) {
        mine.moved_out_in.store(now, std::memory_order_relaxed);
        session.set_moved_at_ns(now);
    }
    return there;
}

}  // namespace kds::server
