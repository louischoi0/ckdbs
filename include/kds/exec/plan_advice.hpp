#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "kds/base/latch.hpp"
#include "kds/exec/captured_plans.hpp"

// **`sys.plan_advice`** (`instructions/v3.2.0/workorder-cx-managed-plans.md`
// CX-S7, CX-Q30 (a), CX-Q31 (a)): what `ADVISE PLAN` proposed, one row a
// statement shape - its exact key, CX-R3's - the newest advice for a shape
// replacing the older. **Never read by a compile**: the engine applies no
// advice (CX-R5); a DBA or an agent reads the row and issues `CREATE PLAN`.
//
// **Memory, one table for the instance**, emptied by a restart: an advice
// is the reading of a moment's statistics, which are unlogged themselves.
// Bounded at `kMaxShapes`; past it the oldest shape's row is dropped. A row
// keeps the statement's shape text and no literal (`sys.plans`' rule); the
// `CREATE PLAN` text with the example's literals is the statement's reply
// alone.
//
// **Latch protocol** (`docs/rules/rules.md` §3): one latch guards the map,
// held for one record or one snapshot, across no I/O; nothing is taken
// under it.

namespace kds::exec {

struct PlanAdvice {
    CapturedKey key;
    // Where the plan in force comes from (CX-Q32): `admin <plan>/<variant>`,
    // `captured`, `uncaptured` (a `fixed` shape the next compile captures),
    // `searched` (`planned`), or `written`.
    std::string in_force;
    // Whether the search ran; false for a block it never orders (CX-Q33),
    // whose proposal is its written order.
    bool searched = false;
    CapturedOrder order{};       // the proposal, written positions in executed order
    double cost_ratio = 1.0;     // the proposal's cost over the plan in force's
    std::uint64_t generation = 0;  // the statistics generation the search read
    // `CREATE PLAN ...` over the shape text; empty where `CREATE PLAN` would
    // refuse the statement (CX-Q33 (a)).
    std::string proposal;
    std::int64_t advised_at_us = 0;
};

// A cost ratio as the reply and the view print it: three decimals.
std::string RatioText(double ratio);

class PlanAdvices {
public:
    // `[provisional]`, CX-Q30 (a), as `CapturedPlans::kMaxShapes` is.
    static constexpr std::size_t kMaxShapes = 1024;

    // Installs `advice` for its key, replacing the key's older one; past
    // the bound, the oldest shape's row is dropped first.
    void Record(PlanAdvice advice);

    // Every row, oldest first.
    std::vector<PlanAdvice> Snapshot();

private:
    struct Entry {
        PlanAdvice advice;
        std::uint64_t seq = 0;
    };
    Latch latch_{LatchKind::kPlanTable};
    std::unordered_map<CapturedKey, Entry, CapturedKeyHash> rows_;
    std::uint64_t next_seq_ = 0;
};

}  // namespace kds::exec
