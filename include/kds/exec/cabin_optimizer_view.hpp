#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/exec/cabin_optimizer_exec.hpp"
#include "kds/stats/cabin_optimizer.hpp"
#include "kds/stats/optimizer_signals.hpp"

// **What the Cabin controller's inspection surface reads: a published copy**
// (`docs/rules/rules.md` §3a; DL-S3, the work order's W5).
//
// The controller, its executor and the collector's folded state are **the
// tick's alone**: core 0's cadence mutates them, across a Cabin's creation
// and its whole seeded build. Until DL-S3 a `SHOW CABIN_OPTIMIZER` or a
// `SHOW META` on any core read them under a latch the tick held for all of
// that, and slept its reactor - and every session on it - for the build.
//
// Now the tick builds an immutable copy of what the views show when it is
// done, and publishes one pointer; a view reads the copy. Nothing a view
// takes is held across a tick, and the tick takes nothing a view holds for
// longer than a pointer's copy - which it only tries (DL-Q17): the tick
// runs on a reactor, and a publish that meets a view's copy is made again
// at the next tick.
//
// The quality columns are the collector's folded state as that tick left
// it, so they are as of the last tick; a ratio of two pairs that decay on
// one clock does not move while nothing is noted.

namespace kds::exec {

// The controller's and the executor's state as the views show it. Immutable
// once built.
struct CabinOptimizerViewData {
    bool has_controller = false;
    bool has_executor = false;
    std::vector<stats::DecisionRecord> log;         // oldest first
    std::vector<stats::ManagedEntryView> entries;   // in key order
    // One an entry: empty where the entry has no Cabin yet or no collector
    // was there to ask, and the quality columns then read NULL.
    std::vector<std::optional<stats::SnapshotCabin>> quality;
    std::size_t managed = 0;
    std::uint64_t pages_committed = 0;
    std::uint64_t page_budget = 0;
    CabinOptimizerExecutor::Counters counters;
};

// The view of these three as they stand. **For the thread that owns them**:
// the tick, or a fixture's one thread. Any of them may be null.
std::shared_ptr<const CabinOptimizerViewData> BuildCabinOptimizerView(
    const stats::CabinOptimizer* controller, const CabinOptimizerExecutor* executor,
    const stats::OptimizerSignals* signals);

// Where the tick publishes and every core's views read. One for the
// instance.
class CabinOptimizerViewBoard {
public:
    // The tick's: swaps the published copy for `view`, trying the latch.
    // False, and nothing published, where a reader's copy of the pointer
    // holds it this instant - the caller publishes again at its next tick.
    bool Publish(std::shared_ptr<const CabinOptimizerViewData> view) noexcept;

    // The published copy, or null before the first publish. Waits for a
    // swap or a copy of the pointer and nothing longer. Any thread.
    std::shared_ptr<const CabinOptimizerViewData> Current() const;

    // The latch, for a test that holds it as a reader's copy does.
    Latch* LatchForTest() const noexcept { return &latch_; }

private:
    // Held for `view_`'s swap and for a copy of it, and nothing else.
    mutable Latch latch_{LatchKind::kOptimizer};
    std::shared_ptr<const CabinOptimizerViewData> view_;
};

}  // namespace kds::exec
